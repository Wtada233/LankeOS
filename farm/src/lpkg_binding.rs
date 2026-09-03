//! 唯一碰 lpkg 的接缝（§3.5 分层架构）。
//!
//! 逻辑层只依赖 `trait LpkgBinding`：
//! - `StubBinding`：返回 canned 结果，用于集成测试与 `--demo` 模式，绕开真实构建
//! - `RealBinding`：docker create/exec 编排（容器内 lpkg upgrade+build）。**仅容器构建**——
//!   宿主直接 lpkg build 会污染环境（装依赖、留产物），已禁止。
//!
//! 绑定优先（ADR #13）：除 lpkg 之外的低层能力（libarchive/下载/哈希/ELF）都应
//! 直接链接进进程内，不 exec 外部程序。

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::Stdio;

/// 一次构建的实际产物扫描结果 + 状态。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct BuildOutcome {
    pub ok: bool,
    pub needed_so: Vec<String>,
    pub provides: Vec<String>,
    pub deps: Vec<String>,
    pub failure_stage: Option<String>,
    /// 构建产物 .lpkg 的路径（RealBinding 填充；StubBinding 为 None）。
    /// 供调度器 publish（进 repo）与 repack（元数据漂移修正）。
    pub lpkg_path: Option<PathBuf>,
}

impl BuildOutcome {
    pub fn success(needed_so: &[&str], provides: &[&str], deps: &[&str]) -> Self {
        BuildOutcome {
            ok: true,
            needed_so: needed_so.iter().map(|s| s.to_string()).collect(),
            provides: provides.iter().map(|s| s.to_string()).collect(),
            deps: deps.iter().map(|s| s.to_string()).collect(),
            failure_stage: None,
            lpkg_path: None,
        }
    }

    pub fn failure(stage: &str) -> Self {
        BuildOutcome {
            ok: false,
            failure_stage: Some(stage.to_string()),
            ..Default::default()
        }
    }
}

/// 逻辑层与 lpkg 交互的唯一接口。
///
/// 只暴露 `build`：依赖拉取（`lpkg upgrade -y`）是每次构建的环境前置，属实现细节，
/// 由 RealBinding 在构建内部完成（容器模式内联在容器脚本里），不进调度器。
pub trait LpkgBinding {
    /// 在 fresh container 中构建 pkg，返回实际扫描结果。
    /// `ok == false` → 确定性构建失败，job 进入 BLOCKED（§8.5 零自动重试）。
    fn build(&mut self, pkg: &str) -> BuildOutcome;

    /// 设置仓库全部提供能力（扫描 not-found 判定用：needed_so 无 provider → 不进 needed_so）。
    /// 默认 no-op；RealBinding 覆盖以填充其 repo_provides 字段。
    fn set_repo_provides(&mut self, _provides: std::collections::HashSet<String>) {}
}

/// Stub：按预设 outcome 返回，不进行任何实际操作。
#[derive(Debug, Default)]
pub struct StubBinding {
    pub outcomes: HashMap<String, BuildOutcome>,
}

impl StubBinding {
    pub fn new(outcomes: HashMap<String, BuildOutcome>) -> Self {
        StubBinding { outcomes }
    }
}

impl LpkgBinding for StubBinding {
    fn build(&mut self, pkg: &str) -> BuildOutcome {
        self.outcomes.get(pkg).cloned().unwrap_or(BuildOutcome {
            ok: true,
            ..Default::default()
        })
    }
}

/// Real：docker cp 编排（仅容器构建——主机构建会污染宿主环境，已禁止）。
/// 构建产物先落 `out/.staging/<pkg>/`（打包完成 → SONAME 检测 → 漂移 repack → 才上传本地仓库，
/// 见 build.rs 调度）；容器易失，不映射宿主 pkgs（避免构建残留污染）。
pub struct RealBinding {
    pub base_image: String,
    pub repo_dir: PathBuf,
    pub out_dir: PathBuf,
    pub arch: String,
    /// 内嵌 repo 服务器端口：写进容器 `/etc/lpkg/mirror.conf`，容器内 `lpkg upgrade`
    /// 经 host 网络从 `127.0.0.1:{repo_port}` 拉本地最新依赖（增量语义的关键）。
    pub repo_port: u16,
    /// Ctrl+C 中断清理共享状态：当前在途容器/包（信号处理器据此删容器、删 DB 条目）。
    pub cleanup: std::sync::Arc<std::sync::Mutex<CleanupState>>,
    /// 仓库全部提供能力（SONAME/虚拟提供）：扫描 not-found 判定用——
    /// needed_so 条目无 provider → not found → 不进 needed_so。构建开始前从旧索引填充。
    pub repo_provides: std::collections::HashSet<String>,
}

/// docker 容器 RAII：作用域结束自动 `docker rm -f`，覆盖所有 `?` 提前返回与失败路径，
/// 不再依赖每处手动清理。
struct ContainerGuard(String);
impl Drop for ContainerGuard {
    fn drop(&mut self) {
        let _ = std::process::Command::new("docker")
            .args(["rm", "-f", &self.0])
            .status();
    }
}

/// 滚动基础镜像：每 `ROLL_LIMIT` 个 commit 后 export+import 扁平化一次（commit 叠加 overlay
/// 有性能损耗，需要周期性压平）。计数存 `<out_dir>/.build-roll`。
const ROLL_LIMIT: u32 = 25;

fn roll_counter_path(out_dir: &Path) -> PathBuf {
    out_dir.join(".build-roll")
}

/// 当前滚动 commit 计数（0..=ROLL_LIMIT；0 = 从原始 base 开始）。
fn read_roll_counter(out_dir: &Path) -> u32 {
    std::fs::read_to_string(roll_counter_path(out_dir))
        .ok()
        .and_then(|s| s.trim().parse().ok())
        .unwrap_or(0)
        .min(ROLL_LIMIT)
}

fn write_roll_counter(out_dir: &Path, c: u32) {
    let _ = std::fs::write(roll_counter_path(out_dir), c.to_string());
}

/// 清理悬空镜像（`<none>`）。来源：
/// - `docker import - <base>`（flatten_to_base）重新打 tag → 旧 base 镜像失去 tag 变 `<none>`
///   （每 ROLL_LIMIT 个 commit 扁平化一次，长期积累的主源）；
/// - 崩溃/重跑窗口：`docker commit` 覆盖已存在的 roll tag → 旧镜像变 `<none>`。
///
/// `docker image prune -f --filter dangling=true` 只删**悬空**（无 tag 且无子镜像引用），
/// 不碰在用镜像——安全、幂等。**时机是关键**：悬空镜像的层往往仍被活容器引用，prune 必须等
/// 相关容器删除**之后**才有效（finalize_roll 删临时容器后显式补；每次构建开头在清完孤儿容器后补），
/// 容器存活时调用恒 0B。
fn prune_dangling_images() {
    let _ = std::process::Command::new("docker")
        .args(["image", "prune", "-f", "--filter", "dangling=true"])
        .status();
}

/// `docker export <cid> | docker import - <base>`：把容器文件系统**扁平化**为单层镜像覆盖 base，
/// 再删掉 roll1..ROLL_LIMIT 编号镜像。gc_roll 与 finalize_roll 共用。
fn flatten_to_base(cid: &str, base_image: &str) -> Result<(), String> {
    let export = std::process::Command::new("docker")
        .args(["export", cid])
        .stdout(Stdio::piped())
        .spawn()
        .map_err(|e| format!("docker export 失败: {e}"))?;
    let import = std::process::Command::new("docker")
        .args(["import", "-", base_image])
        .stdin(Stdio::from(export.stdout.unwrap()))
        .output()
        .map_err(|e| format!("docker import 失败: {e}"))?;
    if !import.status.success() {
        return Err(format!(
            "docker import 失败: {}",
            String::from_utf8_lossy(&import.stderr)
        ));
    }
    // 只删存在的 roll 镜像：不存在的一律跳过（`docker rmi` 对 No such image 会报错刷屏）。
    for i in 1..=ROLL_LIMIT {
        let img = roll_image(base_image, i);
        let exists = std::process::Command::new("docker")
            .args(["images", "-q", &img])
            .output()
            .map(|o| o.status.success() && !o.stdout.is_empty())
            .unwrap_or(false);
        if exists {
            let _ = std::process::Command::new("docker")
                .args(["rmi", "-f", &img])
                .status();
        }
    }
    // 注意：此处**不** prune 悬空镜像——旧 base/roll 层仍被调用方未删的容器引用
    //（gc_roll 的构建容器、finalize_roll 的临时容器），此刻 prune 恒 0B；容器删除后的
    // prune 由调用方负责（finalize_roll 删临时容器后显式补，gc_roll 由下次构建开头的 prune 兜底）。
    Ok(())
}

/// roll 镜像 tag：`wtada233/lankeos:latest` → `wtada233/lankeos:roll{N}`（替换 tag 部分，
/// 不能拼成 `:latest:rollN`——docker 引用只允许一个 tag，两个冒号 = invalid reference format）。
fn roll_image(base_image: &str, n: u32) -> String {
    match base_image.rsplit_once(':') {
        Some((repo, _tag)) => format!("{repo}:roll{n}"),
        None => format!("{base_image}:roll{n}"),
    }
}

/// 滚动收尾（**正常构建结束 / Ctrl+C 共用**）：用最新 commit 起临时容器 → export+import
/// 扁平化覆盖 base → 删全部 roll 镜像 → 计数归零。roll==0（无 commit 链）为 no-op。
pub fn finalize_roll(out_dir: &Path, base_image: &str) -> Result<(), String> {
    let roll = read_roll_counter(out_dir);
    if roll == 0 {
        return Ok(());
    }
    let source = roll_image(base_image, roll);
    let tmp_name = format!("lankefarm-finalize-{}", std::process::id());
    let create = std::process::Command::new("docker")
        .args([
            "create",
            "--name",
            &tmp_name,
            &source,
            "sh",
            "-c",
            "tail -f /dev/null",
        ])
        .output()
        .map_err(|e| format!("docker create（finalize）失败: {e}"))?;
    if !create.status.success() {
        return Err(format!(
            "docker create（finalize）失败: {}",
            String::from_utf8_lossy(&create.stderr)
        ));
    }
    let cid = String::from_utf8_lossy(&create.stdout).trim().to_string();
    let _ = std::process::Command::new("docker")
        .args(["start", &cid])
        .status();
    let res = flatten_to_base(&cid, base_image);
    let _ = std::process::Command::new("docker")
        .args(["rm", "-f", &cid])
        .status();
    // 临时容器已删，roll/旧 base 层引用才释放、此刻才真正悬空——补一次 prune。
    // 时序很关键：flatten_to_base 内（容器存活时）prune 恒 0B，见该函数注释。
    prune_dangling_images();
    res?;
    write_roll_counter(out_dir, 0);
    Ok(())
}

/// 容器名只保留 `[a-zA-Z0-9._-]`（docker create --name 的合法字符集），其余转 `-`。
fn sanitize_name(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        if c.is_ascii_alphanumeric() || c == '.' || c == '_' || c == '-' {
            out.push(c);
        } else {
            out.push('-');
        }
    }
    if out.is_empty() {
        out.push('x');
    }
    out
}

/// 清理上次崩溃（SIGKILL/断电，`ContainerGuard::drop` 未执行）遗留的孤儿构建容器。
/// 容器名内嵌创建者 PID（`lankefarm-build-<pid>-<pkg>`）：只清理 PID 已不存在的容器，
/// 并发 build 进程的**活**容器不会被误杀（固定名方案下后启动者会 rm 掉前者的在途容器）。
fn cleanup_stale_build_containers(
    run_quiet: &dyn Fn(&[&str]) -> std::io::Result<std::process::ExitStatus>,
) {
    let Ok(out) = std::process::Command::new("docker")
        .args(["ps", "-aq", "--filter", "name=lankefarm-build-"])
        .output()
    else {
        return;
    };
    for line in String::from_utf8_lossy(&out.stdout).lines() {
        let name = line.trim();
        if name.is_empty() {
            continue;
        }
        let Some(pid_part) = name
            .strip_prefix("lankefarm-build-")
            .and_then(|s| s.split('-').next())
        else {
            continue;
        };
        let Ok(pid) = pid_part.parse::<u32>() else {
            continue;
        };
        if !std::path::Path::new(&format!("/proc/{pid}")).exists() {
            let _ = run_quiet(&["rm", "-f", name]);
        }
    }
}

/// Ctrl+C 中断清理共享状态：当前在途容器/包（给信号处理器删），out_dir/base_image/state_path（finalize 用）。
#[derive(Default)]
pub struct CleanupState {
    pub current_cid: Option<String>,
    pub current_pkg: Option<String>,
    pub out_dir: PathBuf,
    pub base_image: String,
    pub state_path: PathBuf,
}

impl RealBinding {
    pub fn new(
        base_image: impl Into<String>,
        repo_dir: impl Into<PathBuf>,
        out_dir: impl Into<PathBuf>,
        arch: impl Into<String>,
        repo_port: u16,
        cleanup: std::sync::Arc<std::sync::Mutex<CleanupState>>,
    ) -> Self {
        RealBinding {
            base_image: base_image.into(),
            repo_dir: repo_dir.into(),
            out_dir: out_dir.into(),
            arch: arch.into(),
            repo_port,
            cleanup,
            repo_provides: std::collections::HashSet::new(),
        }
    }

    /// docker cp 编排：配方拷进容器 → 容器内 lpkg upgrade+build → .lpkg 拷回 staging。
    /// 不 bind-mount pkgs（容器易失，残留随容器销毁）；容器经 host 网络从内嵌 repo 服务器拉依赖。
    fn docker_build(&self, pkg: &str, staging: &Path) -> Result<PathBuf, String> {
        // 唯一容器名（进程 PID + 包名）：并发 build 进程互不踩踏——固定名 `lankefarm-build`
        // 时后启动进程的 `rm -f`/`create --name` 会杀/撞前者的在途容器。
        // 静默命令（rm/start/cp/配置）：屏蔽 docker 的 cid 回显与 cp 进度噪音；构建 exec 单独流式。
        let run_quiet = |args: &[&str]| {
            std::process::Command::new("docker")
                .args(args)
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .status()
        };
        // 只清理"创建者 PID 已死"的孤儿容器（SIGKILL/断电，RAII 未执行）；活容器不动——
        // 否则并发进程的构建会被误杀。
        cleanup_stale_build_containers(&run_quiet);
        // 清上次崩溃/重跑遗留的悬空镜像（commit 覆盖 roll tag 的孤儿），保持长期不积累。
        prune_dangling_images();
        let name = format!(
            "lankefarm-build-{}-{}",
            std::process::id(),
            sanitize_name(pkg)
        );

        // 滚动基础镜像：commit 链（<base>:roll<1..25>）或原始 base。
        // `lpkg upgrade` 会把容器里所有"版本落后于当前仓库"的包全量更新——不滚动的话，仓库越攒
        // 越多，每次 upgrade 越慢（滚雪球）。每构建一次、upgrade 成功后 commit 快照，下次从最新
        // commit 起只升增量；达到 ROLL_LIMIT 个 commit 后扁平化（见 4.5）。
        let roll = read_roll_counter(&self.out_dir);
        let mut create_image = if roll == 0 {
            self.base_image.clone()
        } else {
            roll_image(&self.base_image, roll)
        };

        // 1. create + start 常驻容器。`sh -c "mkdir -p /work && tail -f /dev/null"`：
        //    mkdir 必须在 docker cp 前就位——对不存在的 /work，docker cp <dir> :/work/ 会把
        //    配方内容直接铺进 /work，而不是建 /work/<pkg>（实测）。tail -f 保活，busybox/coreutils 都支持。
        let mut create = std::process::Command::new("docker")
            // DooD：挂宿主 docker socket，容器内可 docker run（docker 包 build tini 静态需要）。
            .args([
                "create",
                "--network=host",
                "--name",
                &name,
                "-v",
                "/var/run/docker.sock:/var/run/docker.sock",
                &create_image,
                "sh",
                "-c",
                "mkdir -p /work && tail -f /dev/null",
            ])
            .output()
            .map_err(|e| format!("docker create 失败: {e}"))?;
        // 健壮性：roll 镜像缺失/已删（如 GC 后计数未及时归零的崩溃窗口）→ 回退原始 base 并重置计数。
        if !create.status.success() && roll > 0 {
            eprintln!("  [warn] 从 {create_image} 创建失败，回退原始 base 并重置滚动计数");
            create_image = self.base_image.clone();
            write_roll_counter(&self.out_dir, 0);
            create = std::process::Command::new("docker")
                .args([
                    "create",
                    "--network=host",
                    "--name",
                    &name,
                    "-v",
                    "/var/run/docker.sock:/var/run/docker.sock",
                    &create_image,
                    "sh",
                    "-c",
                    "mkdir -p /work && tail -f /dev/null",
                ])
                .output()
                .map_err(|e| format!("docker create 失败: {e}"))?;
        }
        if !create.status.success() {
            return Err(format!(
                "docker create 失败: {}",
                String::from_utf8_lossy(&create.stderr)
            ));
        }
        let cid = String::from_utf8_lossy(&create.stdout).trim().to_string();
        let _guard = ContainerGuard(cid.clone());
        // 记录在途容器 cid（Ctrl+C 清理用）；成功路径末尾清空，提前返回时 rm -f 幂等无害。
        self.cleanup.lock().unwrap().current_cid = Some(cid.clone());
        let ok = run_quiet(&["start", &cid])
            .map(|s| s.success())
            .unwrap_or(false);
        if !ok {
            return Err(format!("docker start 失败（{pkg}）"));
        }

        // 2. 容器内 lpkg mirror.conf 指向内嵌 repo 服务器（--network=host ⇒ 127.0.0.1 即宿主）。
        //    lpkg 的 repo URL 只从 /etc/lpkg/mirror.conf 读；不写则默认拉远端 lankerepo，
        //    本地刚构建的新依赖根本看不见，增量语义就断了。
        let conf = format!(
            "mkdir -p /etc/lpkg && echo 'http://127.0.0.1:{}/' > /etc/lpkg/mirror.conf",
            self.repo_port
        );
        let ok = run_quiet(&["exec", &cid, "sh", "-c", &conf])
            .map(|s| s.success())
            .unwrap_or(false);
        if !ok {
            return Err(format!("容器内写入 mirror.conf 失败（{pkg}）"));
        }

        // 4. 容器内构建——**实时流式日志**，不捕获（捕获 = 黑盒，构建完成才输出；
        //    stdout 末尾混入 ls 结果会污染文件名提取）。
        //    index.txt 现含**完整 needed_so**（单一真源），lpkg 的 SONAME 检查（前向/后向）在
        //    容器里真实运行——过渡期的缺失 SONAME 由 `--missing-so-no-error`（upgrade）与
        //    `--use-system-soname`（build，配合备份恢复的旧 .so）显式容忍，不再靠剥索引/清状态
        //    压制检查（那些 hack 已删）。
        //    流程：`lpkg install lpkg`（基础镜像里旧版无 force-solve-conflict）→ `lpkg upgrade`
        //    拉 127.0.0.1 内嵌 repo 最新依赖；upgrade 若仍报错，用确认短语喂 force-solve-conflict
        //    清理后重试（仅依赖环触发）。
        // force-solve-conflict 是显式破坏性操作，lpkg 在非交互（-y）下直接拒绝执行——
        // 它的确认短语从 stdin 读取，正确姿势是 `echo '...' | lpkg force-solve-conflict`
        // （不带 -y）。带 -y 会把短语机制废掉，兜底永远失败 → 构建被 BLOCKED。
        // 拆成两步：upgrade（成功后 commit 滚动快照）→ build。upgrade 失败时容器状态不可信，
        // 不 commit、不滚动，直接报错。
        // 顺序约束（**关键**）：docker commit/export 会把整个容器文件系统快照进镜像。备份的
        // 旧 .so 恢复、配方拷入都必须发生在 commit 之后，否则会滚进 roll 镜像、最终扁平化进
        // base——过渡期结束后 `cleanup_backups` 只清宿主 out/backups，镜像里残留的旧 lib 就
        // 永久留在 base（此前 base 被污染即由此而来）。因此 upgrade 脚本末尾 `rm -rf /backups`
        // 先把 /backups 白洞化（顺带清掉历史污染镜像里残留的 /backups），commit 之后（见 4.6）
        // 再重新注入并恢复——旧 .so 只活在本次临时容器，随容器销毁。配方同样在 commit 之后
        // 才拷入（见 4.7），否则每包源码会滚进镜像（滚雪球）。
        let upgrade_script = "lpkg install lpkg -y && \
             ( lpkg upgrade -y --missing-so-no-error || { echo 'I understand that this may break my system.' | lpkg force-solve-conflict && lpkg upgrade -y --missing-so-no-error; } ) || exit 1 ; \
             rm -rf /backups ; \
             exit 0";
        let status = std::process::Command::new("docker")
            .args(["exec", &cid, "sh", "-c", upgrade_script])
            .status()
            .map_err(|e| format!("docker exec 失败: {e}"))?;
        if !status.success() {
            return Err(format!("容器内 lpkg upgrade 失败（{pkg}）"));
        }

        // 4.5 滚动 commit / GC（仅 upgrade 成功）：commit → <base>:roll<N+1>；
        //     达到 ROLL_LIMIT 个 commit 后 export+import 扁平化覆盖 base、删编号镜像、计数归零。
        //     commit/GC 失败 → **硬报错**（滚动快照是性能核心，静默跳过会让下次 upgrade 重新滚雪球；
        //     且容器随后会被 rm，未 commit 的状态就丢了）。
        if roll < ROLL_LIMIT {
            let next = roll + 1;
            let tag = roll_image(&self.base_image, next);
            let ok = std::process::Command::new("docker")
                .args(["commit", &cid, &tag])
                .status()
                .map(|s| s.success())
                .unwrap_or(false);
            if !ok {
                return Err(format!("docker commit {tag} 失败（滚动快照未保存）"));
            }
            write_roll_counter(&self.out_dir, next);
        } else {
            self.gc_roll(&cid)?;
            write_roll_counter(&self.out_dir, 0);
        }

        // 4.6 旧 .so 恢复（**commit/GC 之后**）：重新注入备份 → cp 进 /usr/lib → ldconfig -X。
        //     旧二进制（如链旧 libxml2.so.2 的 gettext）靠它继续运行，新构建用新 .so。
        //     放 commit 后 ⇒ 恢复的旧 .so 只存在于本次临时容器，绝不进 roll/base 镜像
        //     （commit/GC/finalize 的快照都是干净的，见 4 的顺序约束）。
        //     **ldconfig 必须加 -X（只重建缓存、不更新符号链接）**：真 ABI 断裂时被移除的
        //     SONAME 链接本就在备份里（cp 已还原，无需 ldconfig 再造）；而伪 SONAME 备份
        //     （libvpx.so.12.0 → libvpx.so.12.0.0，实体真 SONAME 是 libvpx.so.12）会让无 -X 的
        //     ldconfig 在容器里**新建** /usr/lib/libvpx.so.12 —— 这个 lpkg 不追踪的文件会和
        //     新包安装冲突（"owned by package unknown (manual file)"）。-X 消除该合成。
        let backups = self.out_dir.join("backups");
        if backups.is_dir() {
            let _ = run_quiet(&[
                "cp",
                backups.to_string_lossy().as_ref(),
                &format!("{cid}:/backups"),
            ]);
        }
        let restore_script =
            "if [ -d /backups ]; then cp -a /backups/. /usr/lib/ && ldconfig -X; fi; true";
        let status = std::process::Command::new("docker")
            .args(["exec", &cid, "sh", "-c", restore_script])
            .status()
            .map_err(|e| format!("docker exec 恢复旧 .so 失败: {e}"))?;
        if !status.success() {
            return Err(format!("容器内恢复备份旧 .so 失败（{pkg}）"));
        }

        // 4.7 docker cp 配方进容器（/work/<pkg>）——必须放在 commit/GC 之后：
        //     快照时 /work 为空（见 4 的顺序约束），否则配方/源码会随 commit 滚进镜像。
        let src = self.repo_dir.join(pkg);
        let ok = run_quiet(&[
            "cp",
            src.to_string_lossy().as_ref(),
            &format!("{cid}:/work/"),
        ])
        .map(|s| s.success())
        .unwrap_or(false);
        if !ok {
            return Err(format!("docker cp {pkg} 配方进容器失败"));
        }

        // 5. 构建
        let build_script = format!("cd /work/{pkg} && lpkg build -y --use-system-soname");
        let status = std::process::Command::new("docker")
            .args(["exec", &cid, "sh", "-c", &build_script])
            .status()
            .map_err(|e| format!("docker exec 失败: {e}"))?;
        if !status.success() {
            return Err(format!("容器内 lpkg build 失败（{pkg}）"));
        }

        // 5. 取精确产物名（独立干净的小命令；docker cp **不支持 glob**）。
        //    注意 cd 进目录再 `ls *.lpkg` → 输出 basename（用绝对路径 glob 会输出完整路径，拼 remote 时重复）。
        let out = std::process::Command::new("docker")
            .args([
                "exec",
                &cid,
                "sh",
                "-c",
                &format!("cd /work/{pkg} && ls -1 *.lpkg 2>/dev/null | tail -1"),
            ])
            .output()
            .map_err(|e| format!("docker exec 取产物名失败: {e}"))?;
        let lpkg_name = String::from_utf8_lossy(&out.stdout).trim().to_string();
        if lpkg_name.is_empty() {
            return Err(format!("容器内未产出 .lpkg（{pkg}）"));
        }

        // 6. docker cp 回宿主 staging（精确文件名）
        let remote = format!("{cid}:/work/{pkg}/{lpkg_name}");
        let ok = run_quiet(&["cp", &remote, staging.to_string_lossy().as_ref()])
            .map(|s| s.success())
            .unwrap_or(false);
        if !ok {
            return Err(format!("docker cp {pkg} .lpkg 回宿主失败"));
        }
        self.cleanup.lock().unwrap().current_cid = None;
        Ok(staging.join(&lpkg_name))
    }

    /// 滚动 GC：把当前容器**扁平化**为单层镜像并覆盖原始 base（commit 叠加 overlay 有性能损耗），
    /// 再删掉 roll1..ROLL_LIMIT 编号镜像。计数归零由调用方负责。
    fn gc_roll(&self, cid: &str) -> Result<(), String> {
        flatten_to_base(cid, &self.base_image)
    }
}

impl LpkgBinding for RealBinding {
    fn set_repo_provides(&mut self, provides: std::collections::HashSet<String>) {
        self.repo_provides = provides;
    }

    fn build(&mut self, pkg: &str) -> BuildOutcome {
        // 仅容器构建：禁止主机 lpkg build（会污染宿主环境——装依赖、留产物）。
        if self.base_image.is_empty() {
            return BuildOutcome::failure("missing_image");
        }
        // 产物先落 staging（打包完成 → 后续 SONAME 检测 → 漂移 repack → 才上传本地仓库）
        let staging = self.out_dir.join(".staging").join(pkg);
        if std::fs::create_dir_all(&staging).is_err() {
            return BuildOutcome::failure("create_staging");
        }

        // 记录当前在途包（Ctrl+C 清理 DB 条目用）；docker_build 内同步记录在途容器 cid。
        self.cleanup.lock().unwrap().current_pkg = Some(pkg.to_string());

        // docker cp 编排
        let lpkg = match self.docker_build(pkg, &staging) {
            Ok(dest) => dest,
            Err(e) => {
                self.cleanup.lock().unwrap().current_pkg = None;
                return BuildOutcome::failure(&format!("docker build 失败: {e}"));
            }
        };
        self.cleanup.lock().unwrap().current_pkg = None;

        // scan（staging 的 .lpkg；解包目录供后续 repack 复用）
        let extract_dir = self.out_dir.join("extract").join(pkg);
        match crate::scan::scan_lpkg(&lpkg, &extract_dir, &self.repo_provides) {
            Ok(scan) => BuildOutcome {
                ok: true,
                needed_so: scan.needed_so,
                provides: scan.provides,
                // scan_lpkg 从 .lpkg 的 metadata.json 转述真实运行时依赖；之前丢成空 Vec
                // 导致 index.txt deps 恒空，容器 lpkg 无法解析运行时依赖（如 build→pyproject-hooks）。
                deps: scan.deps,
                failure_stage: None,
                lpkg_path: Some(lpkg),
            },
            Err(e) => {
                eprintln!("{}", crate::tr!("build.scan_fail", pkg, e));
                BuildOutcome::failure("scan")
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // 回归：roll tag 必须是 `<repo>:roll{N}`，不能拼成 `...:latest:roll{N}`（invalid reference format）。
    #[test]
    fn roll_image_replaces_tag() {
        assert_eq!(
            roll_image("wtada233/lankeos:latest", 1),
            "wtada233/lankeos:roll1"
        );
        assert_eq!(
            roll_image("wtada233/lankeos:latest", 25),
            "wtada233/lankeos:roll25"
        );
        // 无 tag 的 base
        assert_eq!(roll_image("wtada233/lankeos", 3), "wtada233/lankeos:roll3");
    }

    #[test]
    fn roll_counter_roundtrip_and_clamp() {
        let tmp = std::env::temp_dir().join(format!("farm-roll-test-{}", std::process::id()));
        std::fs::create_dir_all(&tmp).unwrap();

        // 缺文件 → 0
        assert_eq!(read_roll_counter(&tmp), 0);
        // 写入/读回
        write_roll_counter(&tmp, 7);
        assert_eq!(read_roll_counter(&tmp), 7);
        // 超过 ROLL_LIMIT 钳制（坏状态防御）
        write_roll_counter(&tmp, 999);
        assert_eq!(read_roll_counter(&tmp), ROLL_LIMIT);
        // ROLL_LIMIT 本身合法（触发 GC 的边界）
        write_roll_counter(&tmp, ROLL_LIMIT);
        assert_eq!(read_roll_counter(&tmp), ROLL_LIMIT);

        std::fs::remove_dir_all(&tmp).ok();
    }

    #[test]
    fn stub_returns_preset_and_default_success() {
        let mut outcomes = HashMap::new();
        outcomes.insert(
            "llvm".to_string(),
            BuildOutcome::success(&["libxml2.so.3"], &["libLLVM.so", "libLLVM.so.18"], &[]),
        );
        outcomes.insert("bad".to_string(), BuildOutcome::failure("lankebuild_build"));
        let mut b = StubBinding::new(outcomes);

        assert!(b.build("llvm").ok);
        assert_eq!(
            b.build("llvm").provides,
            vec!["libLLVM.so", "libLLVM.so.18"]
        );
        assert!(!b.build("bad").ok);
        assert_eq!(
            b.build("bad").failure_stage.as_deref(),
            Some("lankebuild_build")
        );
        let d = b.build("anything");
        assert!(d.ok);
        assert!(d.needed_so.is_empty());
    }

    #[test]
    fn build_outcome_failure_sets_stage() {
        let f = BuildOutcome::failure("configure");
        assert!(!f.ok);
        assert_eq!(f.failure_stage.as_deref(), Some("configure"));
    }
}
