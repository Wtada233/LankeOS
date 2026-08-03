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

impl RealBinding {
    pub fn new(
        base_image: impl Into<String>,
        repo_dir: impl Into<PathBuf>,
        out_dir: impl Into<PathBuf>,
        arch: impl Into<String>,
        repo_port: u16,
    ) -> Self {
        RealBinding {
            base_image: base_image.into(),
            repo_dir: repo_dir.into(),
            out_dir: out_dir.into(),
            arch: arch.into(),
            repo_port,
        }
    }

    /// docker cp 编排：配方拷进容器 → 容器内 lpkg upgrade+build → .lpkg 拷回 staging。
    /// 不 bind-mount pkgs（容器易失，残留随容器销毁）；容器经 host 网络从内嵌 repo 服务器拉依赖。
    fn docker_build(&self, pkg: &str, staging: &Path) -> Result<PathBuf, String> {
        // 固定容器名：启动前清掉上次中断（Ctrl-C/崩溃）残留的孤儿容器，也避免并发重名冲突。
        // 静默命令（rm/start/cp/配置）：屏蔽 docker 的 cid 回显与 cp 进度噪音；构建 exec 单独流式。
        const NAME: &str = "lankefarm-build";
        let run_quiet = |args: &[&str]| {
            std::process::Command::new("docker")
                .args(args)
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .status()
        };
        let _ = run_quiet(&["rm", "-f", NAME]);

        // 1. create + start 常驻容器。`sh -c "mkdir -p /work && tail -f /dev/null"`：
        //    mkdir 必须在 docker cp 前就位——对不存在的 /work，docker cp <dir> :/work/ 会把
        //    配方内容直接铺进 /work，而不是建 /work/<pkg>（实测）。tail -f 保活，busybox/coreutils 都支持。
        let create = std::process::Command::new("docker")
            // DooD：挂宿主 docker socket，容器内可 docker run（docker 包 build tini 静态需要）。
            .args(["create", "--network=host", "--name", NAME,
                   "-v", "/var/run/docker.sock:/var/run/docker.sock",
                   &self.base_image,
                   "sh", "-c", "mkdir -p /work && tail -f /dev/null"])
            .output()
            .map_err(|e| format!("docker create 失败: {e}"))?;
        if !create.status.success() {
            return Err(format!("docker create 失败: {}", String::from_utf8_lossy(&create.stderr)));
        }
        let cid = String::from_utf8_lossy(&create.stdout).trim().to_string();
        let _guard = ContainerGuard(cid.clone());
        let ok = run_quiet(&["start", &cid]).map(|s| s.success()).unwrap_or(false);
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
            .map(|s| s.success()).unwrap_or(false);
        if !ok {
            return Err(format!("容器内写入 mirror.conf 失败（{pkg}）"));
        }

        // 3. docker cp 配方进容器（/work/<pkg>）
        let src = self.repo_dir.join(pkg);
        let ok = run_quiet(&["cp", src.to_string_lossy().as_ref(), &format!("{cid}:/work/")])
            .map(|s| s.success()).unwrap_or(false);
        if !ok {
            return Err(format!("docker cp {pkg} 配方进容器失败"));
        }

        // 3.5 ABI 过渡：把备份的旧 SONAME .so 注入容器（/backups，构建脚本里 cp 到 /usr/lib）。
        //    旧二进制（如链旧 libxml2.so.2 的 gettext）靠它继续运行，新构建用新 .so。
        let backups = self.out_dir.join("backups");
        if backups.is_dir() {
            let _ = run_quiet(&["cp", backups.to_string_lossy().as_ref(), &format!("{cid}:/backups")]);
        }

        // 4. 容器内构建——**实时流式日志**，不捕获（捕获 = 黑盒，构建完成才输出；
        //    且 stdout 末尾混入 ls 结果会污染文件名提取）。
        //    先清空安装数据库（deps/needed_so/provides.db）——repo 索引已剥掉 needed_so，再清
        //    安装侧记录，lpkg 的 SONAME 一致性检查完全失效，ABI 断裂/bootstrap 环不再硬报错；
        //    farm 的 ABI 传播从宿主侧自己的 abidb 读，不受影响。
        //    然后 `lpkg install lpkg`（基础镜像里旧版无 force-solve-conflict）→ `lpkg upgrade` 拉
        //    127.0.0.1 内嵌 repo 最新依赖；upgrade 若仍报错，用确认短语喂 force-solve-conflict
        //    清理后重试（正常不触发，因索引已剥 needed_so）。
        let script = format!(
            "cd /work/{pkg} && \
             rm -rf /var/lib/lpkg/deps && rm -rf /var/lib/lpkg/needed_so && rm -rf /var/lib/lpkg/provides.db && \
             lpkg install lpkg -y && \
             ( lpkg upgrade -y --missing-so-no-error || {{ echo 'I understand that this may break my system.' | lpkg force-solve-conflict -y --missing-so-no-error && lpkg upgrade -y --missing-so-no-error; }} ) && \
             [ -d /backups ] && cp -a /backups/. /usr/lib/ ; \
             lpkg build -y --use-system-soname"
        );
        let status = std::process::Command::new("docker")
            .args(["exec", &cid, "sh", "-c", &script])
            .status()
            .map_err(|e| format!("docker exec 失败: {e}"))?;
        if !status.success() {
            return Err(format!("容器内 lpkg build 失败（{pkg}）"));
        }

        // 5. 取精确产物名（独立干净的小命令；docker cp **不支持 glob**）。
        //    注意 cd 进目录再 `ls *.lpkg` → 输出 basename（用绝对路径 glob 会输出完整路径，拼 remote 时重复）。
        let out = std::process::Command::new("docker")
            .args(["exec", &cid, "sh", "-c", &format!("cd /work/{pkg} && ls -1 *.lpkg 2>/dev/null | tail -1")])
            .output()
            .map_err(|e| format!("docker exec 取产物名失败: {e}"))?;
        let lpkg_name = String::from_utf8_lossy(&out.stdout).trim().to_string();
        if lpkg_name.is_empty() {
            return Err(format!("容器内未产出 .lpkg（{pkg}）"));
        }

        // 6. docker cp 回宿主 staging（精确文件名）
        let remote = format!("{cid}:/work/{pkg}/{lpkg_name}");
        let ok = run_quiet(&["cp", &remote, staging.to_string_lossy().as_ref()])
            .map(|s| s.success()).unwrap_or(false);
        if !ok {
            return Err(format!("docker cp {pkg} .lpkg 回宿主失败"));
        }
        Ok(staging.join(&lpkg_name))
    }
}

impl LpkgBinding for RealBinding {
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

        // docker cp 编排
        let lpkg = match self.docker_build(pkg, &staging) {
            Ok(dest) => dest,
            Err(e) => return BuildOutcome::failure(&format!("docker build 失败: {e}")),
        };

        // scan（staging 的 .lpkg；解包目录供后续 repack 复用）
        let extract_dir = self.out_dir.join("extract").join(pkg);
        match crate::scan::scan_lpkg(&lpkg, &extract_dir) {
            Ok(scan) => BuildOutcome {
                ok: true,
                needed_so: scan.needed_so,
                provides: scan.provides,
                deps: Vec::new(), // farm 不扫 deps（gen_deps/deprules 生成）
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
