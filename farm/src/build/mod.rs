//! build.rs — farm build 调度（§4/6/7/8）。
//!
//! 逻辑层：受影响集 → 拓扑分批（build_deps）→ 逐包 build → verify 三分支 →
//! repack（元数据漂移）/ 传播（provides 漂移）。lpkg 交互经 `LpkgBinding` 接缝；
//! .lpkg 解包/扫描（scan.rs）与重打（repack.rs）在本模块编排。
//!
//! 用户澄清的三条规则：
//! 1. **传播重建先 bump release**：被 ABI 断裂波及的包，构建前先 `release + 1`（§7.2 重编语义）；
//! 2. **元数据漂移双写**：既改 .lpkg 内 metadata.json（repack），也改仓库 LankeBUILD.json，
//!    确保源定义（真相）与包内元数据一致；
//! 3. **只比 needed_so/provides**：deps 由 gen_deps/deprules 规则生成，farm 不扫不比。

use std::collections::{HashSet, VecDeque};
use std::fs;
use std::path::{Path, PathBuf};

use crate::tr;
use crate::abi;
use crate::graph::RevMap;
use crate::lpkg_binding::{BuildOutcome, LpkgBinding};
use crate::state::{JobStatus, State};
use crate::ux;
mod sched;
mod repo;
pub(crate) use repo::{effective_version, needs_build, repack_if_drift, place_in_repo, update_repo_index, bump_release, update_lankebuild_metadata, load_old_index, sorted_pkg_names, sha256_file, recipe_hash, cleanup_backups};
mod prompt;
mod sources;
pub(crate) use sources::pre_download_sources;
pub(crate) use prompt::{prompt_blocked, PromptChoice};
pub(crate) use sched::{topo_order, reorder_queue};

/// farm build 输入。
pub struct BuildOptions {
    pub pkgs_dir: PathBuf,
    pub out_dir: PathBuf,
    /// 空 = pkgs 全部包。
    pub targets: Vec<String>,
    /// 架构（lpkg mirror URL 模式 `<repo>/<arch>/<pkg>/<ver>.lpkg`，§8）。
    pub arch: String,
    /// 基础镜像（docker 构建/交互 shell），BLOCKED 提示用。必填——仅容器构建，禁止主机构建。
    pub image: String,
    /// 源预下载网络重试次数（§8.6，默认 3）。
    pub download_retries: u32,
    /// 交互模式：stdin 为 tty 时构建计划预览需 operator 确认；非交互（CI/测试/脚本）跳过。
    pub interactive: bool,
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct BuildReport {
    pub built: Vec<String>,
    pub repacked: Vec<String>,
    pub abi_broken: Vec<String>,
    pub blocked: Vec<String>,
    pub skipped: Vec<String>,
    pub source_missing: Vec<String>,
}

/// 单包构建的终态（供进程内交互接管分发）。
enum BuildDone {
    Ok(BuildOutcome),
    Skipped,
    Blocked,
}

/// 交互提示的用户选择。
/// LankeBUILD.json 最小字段（build 调度用）。
#[derive(serde::Deserialize, Clone)]
pub struct LankeBuild {
    pub name: String,
    pub version: String,
    #[serde(default)]
    pub release: Option<u32>,
    #[serde(default)]
    pub deps: Vec<String>,
    #[serde(default)]
    pub provides: Vec<String>,
    #[serde(default)]
    pub needed_so: Vec<String>,
    #[serde(default)]
    pub build_deps: Vec<String>,
    #[serde(default)]
    pub sources: Vec<String>,
    #[serde(default)]
    pub work_sources: Vec<String>,
}

pub fn read_lankebuild(pkgs_dir: &Path, pkg: &str) -> Option<LankeBuild> {
    let path = pkgs_dir.join(pkg).join("LankeBUILD.json");
    let content = fs::read_to_string(&path).ok()?;
    serde_json::from_str(&content).ok()
}

/// 进程内交互接管（§8.5）：BLOCKED 时提示 operator 选择，不退出进程。
/// 主调度：返回构建报告（built/repacked/abi_broken/blocked）。
/// `state` 非空时记录 job 状态 + 配方 hash（§11 持久化，供续跑/差分）。
pub fn run_build(
    opts: &BuildOptions,
    binding: &mut dyn LpkgBinding,
    state: Option<&State>,
) -> Result<BuildReport, String> {
    // 1. 旧索引（§7.2 传播反图的锚）——必须由 seed 落地的本地 repo index.txt，缺失/为空直接报错
    //    （禁止无基线构建：needed_so provider 校验、ABI diff 都需要它）。
    let old = load_old_index(&opts.out_dir, &opts.arch)?;
    let revmap = RevMap::build(&old);

    // 2. 增量选择（用户规则）：effective_version 与本地 repo 旧索引一致的包跳过构建。
    //    LankeBUILD.json 的 version 是 raw；有 release 字段拼 version+release（如 1.1+2）。
    let all_pkgs = sorted_pkg_names(&opts.pkgs_dir);
    let initial: Vec<String> = if opts.targets.is_empty() {
        let v: Vec<String> = all_pkgs
            .iter()
            .filter(|p| needs_build(&opts.pkgs_dir, p, &old))
            .cloned()
            .collect();
        let skipped = all_pkgs.len() - v.len();
        if skipped > 0 {
            println!("{}", tr!("build.incremental_skip", skipped));
        }
        v
    } else {
        opts.targets.clone()
    };
    let mut queue: VecDeque<(String, bool)> = topo_order(&opts.pkgs_dir, &initial, &old)
        .into_iter()
        .map(|p| (p, false))
        .collect();

    // 2.5 构建计划预览：topo 顺序（仅"最开始能确认需要 build"的包；ABI 受害者随后动态入队）。
    // 交互模式 → 列出顺序并让 operator 确认才开始；确认后**只为确认集**预下载全部源。
    // ABI 受害者不预下载——构建时由 lpkg build 自己下载（URL 未知性 + 不浪费等待）。
    if !queue.is_empty() {
        prompt::print_build_plan(&queue, opts);
        if opts.interactive && !prompt::confirm_plan() {
            println!("{}", tr!("build.plan_cancel"));
            return Ok(BuildReport::default());
        }
        for (pkg, _) in &queue {
            if let Err(e) = pre_download_sources(&opts.pkgs_dir, pkg, opts.download_retries) {
                // 批量预下载是尽力而为：失败不阻塞，循环里每个确认集包会再走一次
                // 源就绪门（带交互接管），这里只先打警告。
                eprintln!("  {}", ux::yellow(&tr!("build.source_missing", pkg, e)));
            }
        }
    }

    let mut seen: HashSet<String> = HashSet::new();
    let mut report = BuildReport::default();

    while let Some((pkg, is_victim)) = queue.pop_front() {
        if !seen.insert(pkg.clone()) {
            continue;
        }
        let ver = effective_version(&opts.pkgs_dir, &pkg).unwrap_or_else(|| "?".into());
        // 传播重建（被 ABI 断裂波及）→ 先 bump release（用户规则 1），再构建
        if is_victim {
            bump_release(&opts.pkgs_dir, &pkg);
        }
        println!(
            "{}",
            // 整体 dim（灰）；不嵌套 bold——内层 \x1b[0m 会全重置掉外层 dim
            ux::dim(&tr!(
                "build.start",
                pkg,
                ver,
                if is_victim { tr!("build.victim") } else { "" }
            ))
        );
        let rhash = recipe_hash(&opts.pkgs_dir, &pkg);
        if let Some(st) = state {
            let _ = st.set_job(&pkg, JobStatus::Building, None, rhash.as_deref());
        }

        // 源预下载 + 构建 → 统一的进程内交互接管（§8.5，不退出进程）。
        // **任何失败都不允许自动跳过**——源预下载失败同样走提示：1) 开 shell 修复 2) 跳过 3) 结束。
        // 只有 operator 明确选"跳过"才跳过（否则依赖序会被打乱、后面的包基于缺失的依赖构建）。
        let (done, end_build) = 'pkg: loop {
            // §8.6 源预下载：宿主侧预取，源就绪才构建。
            // ABI 受害者跳过——预下载只给确认集（上面已 bulk 预取）；受害者构建时由 lpkg build 自己下载。
            if !is_victim {
                if let Err(e) = pre_download_sources(&opts.pkgs_dir, &pkg, opts.download_retries) {
                    eprintln!("  {}", ux::yellow(&tr!("build.source_missing", pkg, e)));
                    if !opts.interactive {
                        // 非交互：无 operator 可提示 → 标记 source-missing 阻塞继续（不静默丢弃）
                        eprintln!("{}", tr!("build.source_missing_ni", pkg));
                        report.source_missing.push(pkg.clone());
                        if let Some(st) = state {
                            let _ = st.set_job(&pkg, JobStatus::Blocked, Some("source-missing"), rhash.as_deref());
                        }
                        break 'pkg (BuildDone::Blocked, false);
                    }
                    match prompt_blocked(&pkg, opts, &format!("源预下载失败：{e}")) {
                        PromptChoice::Retry => continue, // 开 shell 手动放置源/修网络后重试
                        PromptChoice::Skip => {
                            report.source_missing.push(pkg.clone());
                            if let Some(st) = state {
                                let _ = st.set_job(&pkg, JobStatus::Skipped, Some("operator skip: source"), rhash.as_deref());
                            }
                            break 'pkg (BuildDone::Skipped, false);
                        }
                        PromptChoice::End => break 'pkg (BuildDone::Blocked, true),
                    }
                }
            }

            // 构建失败 → 交互接管
            let outcome = binding.build(&pkg);
            if outcome.ok {
                break 'pkg (BuildDone::Ok(outcome), false);
            }
            let stage = outcome
                .failure_stage
                .clone()
                .unwrap_or_else(|| "未知阶段".to_string());
            if let Some(st) = state {
                let _ = st.set_job(&pkg, JobStatus::Blocked, Some(&stage), rhash.as_deref());
            }
            if !opts.interactive {
                eprintln!("{}", tr!("build.blocked_ni", pkg, stage));
                break 'pkg (BuildDone::Blocked, false);
            }
            match prompt_blocked(&pkg, opts, &stage) {
                PromptChoice::Retry => {} // shell 修复后重试（继续内层 loop）
                PromptChoice::Skip => {
                    if let Some(st) = state {
                        let _ = st.set_job(&pkg, JobStatus::Skipped, Some("operator skip"), rhash.as_deref());
                    }
                    break 'pkg (BuildDone::Skipped, false);
                }
                PromptChoice::End => {
                    break 'pkg (BuildDone::Blocked, true);
                }
            }
        };
        let outcome = match done {
            BuildDone::Ok(o) => o,
            BuildDone::Skipped => {
                report.skipped.push(pkg.clone());
                continue;
            }
            BuildDone::Blocked => {
                report.blocked.push(pkg.clone());
                if end_build {
                    break;
                }
                continue;
            }
        };

        // 元数据漂移检测 + repack（打包完成 → SONAME 检测 → 与 .lpkg 内 metadata.json 比对 → 漂移才 repack）。
        // 只比 needed_so/provides；deps 由 gen_deps/deprules 生成，不读不改。
        let drifted = repack_if_drift(&outcome, opts, &pkg);
        if drifted {
            update_lankebuild_metadata(&opts.pkgs_dir, &pkg, &outcome);
            report.repacked.push(pkg.clone());
            println!(
                "  {}",
                ux::yellow(&tr!("build.repack", pkg))
            );
        }

        // 上传本地仓库（取代旧版本）+ 更新 index.txt —— **breaking 包必须先进仓库**，
        // 否则依赖它的包重建时仍用旧 ABI（用户规则：反哺仓库 / 中间上传流程）。
        let final_lpkg = match place_in_repo(&outcome, opts, &pkg) {
            Ok(p) => p,
            Err(e) => {
                eprintln!("{}", tr!("build.repo_fail", pkg, e));
                report.blocked.push(pkg.clone());
                continue;
            }
        };
        let version = effective_version(&opts.pkgs_dir, &pkg).unwrap_or_else(|| "?".into());
        let hash = sha256_file(&final_lpkg).unwrap_or_default();
        // index.txt：**写回完整 needed_so**（单一真源）。容器可见索引与 farm 的 ABI 传播共用，
        // 不再剥 needed_so、不再维护第二份 .abi.json；构建顺序/传播/备份清理都从这里读。
        // 容器的 SONAME 检查由 --missing-so-no-error / --use-system-soname 在过渡期容忍。
        if let Err(e) = update_repo_index(&opts.out_dir, &opts.arch, &pkg, &version, &hash,
                                          &outcome.provides, &outcome.needed_so) {
            eprintln!("{}", tr!("build.index_fail", pkg, e));
            report.blocked.push(pkg.clone());
            continue;
        }
        report.built.push(pkg.clone());
        println!(
            "  {}",
            ux::green(&tr!("build.repo", pkg, final_lpkg.display()))
        );

        // 临时目录清理：解包目录（scan/repack 共用，已用完）与 staging（产物已 rename 进 repo）。
        // 只清成功路径——构建失败时保留，供 operator 排查/重试（下次 scan 会先清空解包目录）。
        let _ = fs::remove_dir_all(opts.out_dir.join("extract").join(&pkg));
        let _ = fs::remove_dir_all(opts.out_dir.join(".staging").join(&pkg));

        // ABI 传播（§7.2）：**只有 SONAME 变化才触发**（removed SONAME → 直连受害者重建）。
        // 变化的 SONAME 无包直接 need → 改好元数据直接进仓库，不传播。
        let removed = abi::removed_sonames(&old, &pkg, &outcome.provides);
        if !removed.is_empty() {
            report.abi_broken.push(pkg.clone());
            let mut victims = abi::direct_victims(&revmap, &removed);
            victims.sort();
            for v in victims {
                if !seen.contains(&v) {
                    println!(
                        "  {}",
                        ux::yellow(&tr!("build.abi", pkg, removed.join(", "), v))
                    );
                    queue.push_back((v, true)); // 传播重建 → 触发 release bump
                }
            }
            // 受害者按**依赖算法**重排：被依赖的受害者先建，依赖它们的后建。
            // 否则按字母序先建 appstream 时，其构建依赖树里的 librsvg（同样是 libxml2 受害者，
            // 还引用旧 libxml2.so.2）未重建 → 装构建依赖时 SONAME 无 provider 硬报错。
            reorder_queue(&mut queue, &opts.pkgs_dir, &old);
        }

        if let Some(st) = state {
            let _ = st.set_job(&pkg, JobStatus::Done, None, rhash.as_deref());
            let _ = st.record_build(&pkg, &ver, true);
        }
    }

    // ABI 过渡备份清理：**整个 build 完成后**（而非单包完成）。此时所有引用旧 SONAME 的包
    // 都已重建（直连受害者 + 级联），备份的旧 .so 不再被当前 index.txt 任何 needed_so 引用 → 删除；
    // 仍有包被跳过 / BLOCKED 未重建则保留，留待下次 build 完成后再清。
    cleanup_backups(&opts.out_dir, &opts.arch);

    Ok(report)
}

/// repack .lpkg 的 metadata.json + 双写 LankeBUILD.json（规则 2）。共用一次解包。
/// 有效版本：LankeBUILD.json 的 version 是 raw；有 release 字段拼 version+release（如 1.1+2）。
#[cfg(test)]
mod tests {
    use super::*;
    use super::sources::sources_ready;
    use crate::graph::Index;
    use crate::lpkg_binding::{BuildOutcome, StubBinding};
    use std::collections::HashMap;

    fn write_pkg(pkgs: &Path, name: &str, provides: &[&str], needed: &[&str], build_deps: &[&str]) {
        let dir = pkgs.join(name);
        fs::create_dir_all(&dir).unwrap();
        let json = serde_json::json!({
            "name": name,
            "version": "1.0",
            "provides": provides,
            "needed_so": needed,
            "build_deps": build_deps,
        });
        fs::write(dir.join("LankeBUILD.json"), serde_json::to_string_pretty(&json).unwrap()).unwrap();
    }

    /// 合成一个真实可解包的 .lpkg（metadata.json + content/libfoo.so.1 假 ELF）。
    fn make_test_lpkg(path: &Path, name: &str, version: &str, needed: &[&str], provides: &[&str]) {
        let src = path.parent().unwrap().join("lpkg-src");
        let _ = fs::remove_dir_all(&src);
        fs::create_dir_all(src.join("content")).unwrap();
        fs::write(src.join("content/libfoo.so.1"), [0x7f, b'E', b'L', b'F', 2, 1, 1]).unwrap();
        let meta = serde_json::json!({
            "name": name,
            "version": version,
            "needed_so": needed,
            "provides": provides,
            "deps": [],
        });
        fs::write(src.join("metadata.json"), serde_json::to_string_pretty(&meta).unwrap()).unwrap();
        let f = fs::File::create(path).unwrap();
        let enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
        let mut b = tar::Builder::new(enc);
        b.append_dir_all(".", &src).unwrap();
        let enc = b.into_inner().unwrap();
        enc.finish().unwrap();
        fs::remove_dir_all(&src).ok();
    }

    /// 为包准备 staging .lpkg + 解包目录（模拟 RealBinding 产物），返回 lpkg 路径。
    fn stage_lpkg(out: &Path, pkg: &str, version: &str, needed: &[&str], provides: &[&str]) -> PathBuf {
        let staging = out.join(".staging").join(pkg);
        fs::create_dir_all(&staging).unwrap();
        let lpkg = staging.join(format!("{pkg}-{version}.lpkg"));
        make_test_lpkg(&lpkg, pkg, version, needed, provides);
        let extract = out.join("extract").join(pkg);
        crate::scan::extract_lpkg(&lpkg, &extract).unwrap();
        lpkg
    }

    /// 写带 sources/work_sources 的包（预下载测试用）。
    fn write_pkg_sources(pkgs: &Path, name: &str, sources: &[&str], work_sources: &[&str]) {
        let dir = pkgs.join(name);
        fs::create_dir_all(&dir).unwrap();
        let json = serde_json::json!({
            "name": name,
            "version": "1.0",
            "sources": sources,
            "work_sources": work_sources,
        });
        fs::write(dir.join("LankeBUILD.json"), serde_json::to_string_pretty(&json).unwrap()).unwrap();
    }

    /// 写带 sources + provides + needed_so 的包（victim 跳过预下载集成测试用）。
    fn write_pkg_full(
        pkgs: &Path,
        name: &str,
        provides: &[&str],
        needed: &[&str],
        sources: &[&str],
    ) {
        let dir = pkgs.join(name);
        fs::create_dir_all(&dir).unwrap();
        let json = serde_json::json!({
            "name": name,
            "version": "1.0",
            "provides": provides,
            "needed_so": needed,
            "sources": sources,
        });
        fs::write(dir.join("LankeBUILD.json"), serde_json::to_string_pretty(&json).unwrap()).unwrap();
    }

    /// 起一个本地 HTTP 服务器 fixture（serve.rs），serve 临时目录。返回 (handle, port, root)。
    /// 线程随测试进程退出；root 由调用方清理。**原子递增端口 + 按端口分 root**——
    /// 多个测试并行起服务器不会互相冲突（各自独立目录）。基数取 18100：避开
    /// net.rs 测试硬编码的 18080/18081（历史遗留固定端口）。
    fn spawn_test_server() -> (std::thread::JoinHandle<()>, u16, std::path::PathBuf) {
        use std::sync::atomic::{AtomicU16, Ordering};
        static NEXT_PORT: AtomicU16 = AtomicU16::new(18100);
        let port = NEXT_PORT.fetch_add(1, Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!("farm-serve-test-{}-{port}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();
        let r = root.clone();
        let h = std::thread::spawn(move || {
            let _ = crate::serve::serve("127.0.0.1", &r, port);
        });
        std::thread::sleep(std::time::Duration::from_millis(300));
        (h, port, root)
    }

    fn temp_dir(name: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    /// 写 seed 基线：index.txt（完整 needed_so，单一真源）。
    fn write_baseline(out: &Path, index_text: &str) {
        fs::create_dir_all(out.join("x86_64")).unwrap();
        fs::write(out.join("x86_64/index.txt"), index_text).unwrap();
    }

    // ── §8.6 源预下载测试矩阵 ────────────────────────────────────────────────
    #[test]
    fn pre_download_skips_file_and_git_sources() {
        // file:// 引用包本体文件、git+ 由 lpkg libgit2 处理 → 都不该触发 HTTP 下载
        let pkgs = temp_dir("farm-predl-skip");
        write_pkg_sources(
            &pkgs,
            "p",
            &["file:///x.patch", "git+https://github.com/a/b@v1"],
            &[],
        );
        pre_download_sources(&pkgs, "p", 3).unwrap();
        // 目录里只有 LankeBUILD.json，没有下载出的文件
        assert_eq!(fs::read_dir(pkgs.join("p")).unwrap().count(), 1);
        fs::remove_dir_all(&pkgs).ok();
    }

    #[test]
    fn pre_download_skips_existing_files() {
        // 源文件已存在（operator 放置 / 上次已取）→ 不再下载，即使 URL 不可达
        let pkgs = temp_dir("farm-predl-exist");
        write_pkg_sources(&pkgs, "p", &["http://127.0.0.1:1/src.tar.gz"], &[]);
        fs::write(pkgs.join("p/src.tar.gz"), b"exists").unwrap();
        pre_download_sources(&pkgs, "p", 1).unwrap();
        assert_eq!(fs::read(pkgs.join("p/src.tar.gz")).unwrap(), b"exists");
        fs::remove_dir_all(&pkgs).ok();
    }

    #[test]
    fn pre_download_fetches_sources_and_work_sources() {
        // 从本地 HTTP 服务器下载 sources + work_sources 两个源
        let (_h, port, root) = spawn_test_server();
        fs::write(root.join("src.tar.gz"), b"hello-src").unwrap();
        fs::write(root.join("font.otf"), b"font-data").unwrap();
        let pkgs = temp_dir("farm-predl-fetch");
        let s1 = format!("http://127.0.0.1:{port}/src.tar.gz");
        let s2 = format!("http://127.0.0.1:{port}/font.otf");
        write_pkg_sources(&pkgs, "p", &[&s1], &[&s2]);
        pre_download_sources(&pkgs, "p", 3).unwrap();
        assert_eq!(fs::read(pkgs.join("p/src.tar.gz")).unwrap(), b"hello-src");
        assert_eq!(fs::read(pkgs.join("p/font.otf")).unwrap(), b"font-data");
        fs::remove_dir_all(&pkgs).ok();
        fs::remove_dir_all(&root).ok();
        drop(_h);
    }

    #[test]
    fn pre_download_missing_url_is_source_missing() {
        // 端口 1 拒绝连接 → 重试耗尽 → Err（source-missing，非致命）
        let pkgs = temp_dir("farm-predl-miss");
        write_pkg_sources(&pkgs, "p", &["http://127.0.0.1:1/nope.tar.gz"], &[]);
        let err = pre_download_sources(&pkgs, "p", 1).unwrap_err();
        assert!(!err.is_empty(), "应返回 source-missing 错误");
        assert!(!pkgs.join("p/nope.tar.gz").exists());
        fs::remove_dir_all(&pkgs).ok();
    }

    #[test]
    fn sources_ready_tracks_network_files() {
        let pkgs = temp_dir("farm-src-ready");
        write_pkg_sources(
            &pkgs,
            "p",
            &["file:///x", "git+https://github.com/a/b@v1", "http://127.0.0.1:1/a.tar.gz"],
            &[],
        );
        // a.tar.gz 不存在 → 未就绪
        assert!(!sources_ready(&pkgs, "p"));
        fs::write(pkgs.join("p/a.tar.gz"), b"").unwrap();
        assert!(sources_ready(&pkgs, "p"));
        fs::remove_dir_all(&pkgs).ok();
    }

    #[test]
    fn topo_order_respects_build_deps() {
        let dir = std::env::temp_dir().join("farm-build-topo");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        write_pkg(&dir, "a", &["liba.so.1"], &[], &[]);
        write_pkg(&dir, "b", &["libb.so.1"], &["liba.so.1"], &["a"]);
        write_pkg(&dir, "c", &[], &["libb.so.1"], &["b"]);
        let old = index_of(&[
            ("a", vec!["liba.so.1"], vec![]),
            ("b", vec!["libb.so.1"], vec!["liba.so.1"]),
            ("c", vec![], vec!["libb.so.1"]),
        ]);
        let order = topo_order(&dir, &["a".into(), "b".into(), "c".into()], &old);
        let pos = |x: &str| order.iter().position(|n| n == x).unwrap();
        assert!(pos("a") < pos("b"));
        assert!(pos("b") < pos("c"));
        fs::remove_dir_all(&dir).ok();
    }

    /// 由 (name, provides, needed_so) 构造旧索引（link_deps 的 provider 解析输入）。
    fn index_of(pkgs: &[(&str, Vec<&str>, Vec<&str>)]) -> Index {
        let mut packages = HashMap::new();
        for (name, provides, needed) in pkgs {
            packages.insert(
                name.to_string(),
                crate::graph::PkgInfo {
                    name: name.to_string(),
                    version: "1.0".to_string(),
                    sha256: String::new(),
                    deps: Vec::new(),
                    provides: provides.iter().map(|s| s.to_string()).collect(),
                    needed_so: needed.iter().map(|s| s.to_string()).collect(),
                },
            );
        }
        Index::from_packages(packages)
    }

    #[test]
    fn topo_order_link_chain_bottom_up() {
        // 复现 chromium 痛点：链接依赖链 glibc → zlib → curl → chromium，
        // 必须自底向上建，否则依赖库后重建会让已建的叶子白跑。
        let dir = std::env::temp_dir().join("farm-build-topo-link");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        write_pkg(&dir, "glibc", &["libc.so.6"], &[], &[]);
        write_pkg(&dir, "zlib", &["libz.so.1"], &["libc.so.6"], &["glibc"]);
        write_pkg(&dir, "curl", &["libcurl.so.4"], &["libc.so.6", "libz.so.1"], &["glibc", "zlib"]);
        write_pkg(
            &dir,
            "chromium",
            &[],
            &["libc.so.6", "libz.so.1", "libcurl.so.4"],
            &["glibc", "zlib", "curl"],
        );
        let old = index_of(&[
            ("glibc", vec!["libc.so.6"], vec![]),
            ("zlib", vec!["libz.so.1"], vec!["libc.so.6"]),
            ("curl", vec!["libcurl.so.4"], vec!["libc.so.6", "libz.so.1"]),
            ("chromium", vec![], vec!["libc.so.6", "libz.so.1", "libcurl.so.4"]),
        ]);
        let targets: Vec<String> = ["glibc", "zlib", "curl", "chromium"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let order = topo_order(&dir, &targets, &old);
        let pos = |s: &str| order.iter().position(|n| n == s).unwrap();
        assert!(pos("glibc") < pos("zlib"), "链接链应自底向上: {order:?}");
        assert!(pos("zlib") < pos("curl"));
        assert!(pos("curl") < pos("chromium"), "chromium 的依赖库应全在其前: {order:?}");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn topo_order_breaks_cycle() {
        // A↔B 互链（needed_so 成环：a 需 libb、b 需 liba）：应警告并切断，仍给出覆盖全部包的
        // 完整顺序（不死循环、不丢包）。
        let dir = std::env::temp_dir().join("farm-build-topo-cycle");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        write_pkg(&dir, "a", &["liba.so.1"], &["libb.so.1"], &[]);
        write_pkg(&dir, "b", &["libb.so.1"], &["liba.so.1"], &[]);
        write_pkg(&dir, "c", &[], &["liba.so.1"], &[]);
        let old = index_of(&[
            ("a", vec!["liba.so.1"], vec!["libb.so.1"]),
            ("b", vec!["libb.so.1"], vec!["liba.so.1"]),
            ("c", vec![], vec!["liba.so.1"]),
        ]);
        let targets: Vec<String> = ["a", "b", "c"].iter().map(|s| s.to_string()).collect();
        let order = topo_order(&dir, &targets, &old);
        assert_eq!(order.len(), 3, "环切断后应覆盖所有包: {order:?}");
        for t in &targets {
            assert!(order.contains(t), "不应丢包 {t}: {order:?}");
        }
        let pos = |s: &str| order.iter().position(|n| n == s).unwrap();
        assert!(pos("a") < pos("c"), "c 依赖 a，应在其后: {order:?}");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn reorder_queue_puts_dependency_victims_first() {
        // 复现 appstream 痛点：appstream 的 build_deps 含 librsvg，两者都是 libxml2 受害者。
        // 字母序入队 appstream 先，但 appstream 需要重建后的 librsvg → 重排必须把被依赖者放前。
        // （build_deps 已不参与建图，这里用 needed_so 的 librsvg-2.so.2 → librsvg 边。）
        let dir = std::env::temp_dir().join("farm-reorder-queue");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        write_pkg(&dir, "appstream", &[], &["libxml2.so.2", "librsvg-2.so.2"], &["librsvg"]);
        write_pkg(&dir, "librsvg", &["librsvg-2.so.2"], &["libxml2.so.2"], &["libxml2"]);
        let old = index_of(&[
            ("libxml2", vec!["libxml2.so.2"], vec![]),
            ("librsvg", vec!["librsvg-2.so.2"], vec!["libxml2.so.2"]),
            ("appstream", vec![], vec!["libxml2.so.2", "librsvg-2.so.2"]),
        ]);
        let mut queue: VecDeque<(String, bool)> =
            vec![("appstream".to_string(), true), ("librsvg".to_string(), true)].into();
        reorder_queue(&mut queue, &dir, &old);
        let names: Vec<&str> = queue.iter().map(|(n, _)| n.as_str()).collect();
        let pos = |s: &str| names.iter().position(|n| *n == s).unwrap();
        assert!(pos("librsvg") < pos("appstream"), "被依赖的受害者应先建: {names:?}");
        assert!(queue.iter().all(|(_, v)| *v), "全部应保持 victim: {queue:?}");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn reorder_queue_dedups_multiple_abi_pushes() {
        // chromium 的依赖 libA/libB/libC 各断裂一次 → 各 push 一次 chromium，队列出现 4 个
        // chromium（1 初始 + 3 断裂）。不去重会让 topo_order 的 rev 污染（libA 弹出时 chromium
        // in_deg 被多减）→ 顺序错乱。去重后 chromium 只留一个、victim 标记保留、依赖全在前。
        let dir = std::env::temp_dir().join("farm-reorder-dedup");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        write_pkg(&dir, "chromium", &[], &["liba.so.1", "libb.so.1", "libc.so.1"], &[]);
        let old = index_of(&[
            ("libA", vec!["liba.so.1"], vec![]),
            ("libB", vec!["libb.so.1"], vec![]),
            ("libC", vec!["libc.so.1"], vec![]),
            ("chromium", vec![], vec!["liba.so.1", "libb.so.1", "libc.so.1"]),
        ]);
        let mut queue: VecDeque<(String, bool)> = vec![
            ("chromium".to_string(), false), // 初始（版本变更要重建）
            ("libA".to_string(), false),
            ("libB".to_string(), false),
            ("libC".to_string(), false),
            ("chromium".to_string(), true), // libA 断裂
            ("chromium".to_string(), true), // libB 断裂
            ("chromium".to_string(), true), // libC 断裂
        ]
        .into();
        reorder_queue(&mut queue, &dir, &old);
        let chromium_count = queue.iter().filter(|(n, _)| n == "chromium").count();
        assert_eq!(chromium_count, 1, "chromium 应只出现一次: {queue:?}");
        let chromium_flag = queue.iter().find(|(n, _)| n == "chromium").unwrap().1;
        assert!(chromium_flag, "chromium 应保持 victim（任一断裂入队）: {queue:?}");
        let pos = |s: &str| queue.iter().position(|(n, _)| n == s).unwrap();
        assert!(
            pos("libA") < pos("chromium")
                && pos("libB") < pos("chromium")
                && pos("libC") < pos("chromium"),
            "依赖应全在 chromium 前: {queue:?}"
        );
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn mid_chain_package_ordered_between_dep_and_dependents() {
        // 中链包 P：既链接 X（X 断裂 → P 是受害者），又被 A/B/C 链接（P 重建后 A/B/C 才可建）。
        // 重排必须把 P 放在 A/B/C 之前——否则 A/B/C 基于旧 libP 构建。
        let dir = std::env::temp_dir().join("farm-midchain-order");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        write_pkg(&dir, "X", &["libX.so.1"], &[], &[]);
        write_pkg(&dir, "P", &["libP.so.1"], &["libX.so.1"], &[]);
        write_pkg(&dir, "A", &[], &["libP.so.1"], &[]);
        write_pkg(&dir, "B", &[], &["libP.so.1"], &[]);
        write_pkg(&dir, "C", &[], &["libP.so.1"], &[]);
        let old = index_of(&[
            ("X", vec!["libX.so.1"], vec![]),
            ("P", vec!["libP.so.1"], vec!["libX.so.1"]),
            ("A", vec![], vec!["libP.so.1"]),
            ("B", vec![], vec!["libP.so.1"]),
            ("C", vec![], vec!["libP.so.1"]),
        ]);
        // X 已建（不在队列），P 及其 3 个依赖者都是受害者入队
        let mut queue: VecDeque<(String, bool)> = vec![
            ("A".to_string(), true),
            ("C".to_string(), true),
            ("B".to_string(), true),
            ("P".to_string(), true),
        ]
        .into();
        reorder_queue(&mut queue, &dir, &old);
        let pos = |s: &str| queue.iter().position(|(n, _)| n == s).unwrap();
        assert!(
            pos("P") < pos("A") && pos("P") < pos("B") && pos("P") < pos("C"),
            "中链包 P 应在 A/B/C 之前（它们链接 P）: {queue:?}"
        );
        // 全部保留 victim 标记（P 和 A/B/C 都是传播重建）
        assert!(queue.iter().all(|(_, v)| *v), "全部应为 victim: {queue:?}");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn leaf_stays_last_after_each_dep_break() {
        // 叶子 chromium 在队尾，依赖 libA/libB/libC 各断裂一次。
        // 每次断裂后 reorder：chromium 必须仍排在所有未建依赖之后（维持队尾），构建一次。
        let dir = std::env::temp_dir().join("farm-leaf-stays-last");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        write_pkg(&dir, "chromium", &[], &["liba.so.1", "libb.so.1", "libc.so.1"], &[]);
        let old = index_of(&[
            ("libA", vec!["liba.so.1"], vec![]),
            ("libB", vec!["libb.so.1"], vec![]),
            ("libC", vec!["libc.so.1"], vec![]),
            ("chromium", vec![], vec!["liba.so.1", "libb.so.1", "libc.so.1"]),
        ]);
        // libA 断裂后：队列 = [libB, libC, chromium(victim)]
        let mut q1: VecDeque<(String, bool)> =
            vec![("libB".to_string(), false), ("libC".to_string(), false), ("chromium".to_string(), true)].into();
        reorder_queue(&mut q1, &dir, &old);
        assert_eq!(q1.back().map(|(n, _)| n.as_str()), Some("chromium"), "libA 断裂后 chromium 应仍在队尾: {q1:?}");
        // libB 断裂后：队列 = [libC, chromium]
        let mut q2: VecDeque<(String, bool)> =
            vec![("libC".to_string(), false), ("chromium".to_string(), true)].into();
        reorder_queue(&mut q2, &dir, &old);
        assert_eq!(q2.back().map(|(n, _)| n.as_str()), Some("chromium"), "libB 断裂后 chromium 应仍在队尾: {q2:?}");
        // libC 断裂后：队列 = [chromium]（依赖全建完）
        let mut q3: VecDeque<(String, bool)> =
            vec![("chromium".to_string(), true)].into();
        reorder_queue(&mut q3, &dir, &old);
        assert_eq!(q3.len(), 1, "chromium 只应出现一次: {q3:?}");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn needed_drift_repacks_uploads_and_updates_index() {
        // 真实路径集成测试：合成 .lpkg → SONAME 检测漂移 → repack 重打包 → 上传本地仓库 + 更新 index。
        let dir = temp_dir("farm-build-repack");
        let out = temp_dir("farm-build-out");
        // seed 过的旧索引（§7.2 基线）：index.txt（完整 needed_so，单一真源）
        write_baseline(&out, "libfoo|1.0:oldhash::libfoo.so,libfoo.so.1:|\n");
        write_pkg(&dir, "libfoo", &["libfoo.so", "libfoo.so.1"], &["libc.so.6"], &[]);

        // staging .lpkg：metadata 缺 libm（陈旧）→ 扫描实际有 libm → 漂移
        let lpkg_path = stage_lpkg(
            &out, "libfoo", "1.0",
            &["libc.so.6"], &["libfoo.so", "libfoo.so.1"],
        );
        let mut outcomes = HashMap::new();
        outcomes.insert(
            "libfoo".into(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libc.so.6".into(), "libm.so.6".into()],
                provides: vec!["libfoo.so".into(), "libfoo.so.1".into()],
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(lpkg_path),
            },
        );
        let mut binding = StubBinding::new(outcomes);
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec!["libfoo".into()],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
        };
        let report = run_build(&opts, &mut binding, None).unwrap();
        assert!(report.built.contains(&"libfoo".to_string()));
        assert!(report.repacked.contains(&"libfoo".to_string()));

        // .lpkg 已上传本地仓库（取代旧版本）；文件名必须匹配 lpkg 的 `<version>.lpkg` 下载 URL
        let repo_lpkg = out.join("x86_64/libfoo/1.0.lpkg");
        assert!(repo_lpkg.exists(), ".lpkg 应上传本地仓库");
        assert_eq!(
            fs::read_dir(out.join("x86_64/libfoo")).unwrap().count(),
            1,
            "旧版本应被取代"
        );

        // index.txt 已更新（保留 version+deps，更新 provides；**写回完整 needed_so**——单一真源）
        let idx = fs::read_to_string(out.join("x86_64/index.txt")).unwrap();
        assert!(idx.starts_with("libfoo|1.0:"), "index 版本保留: {idx}");
        assert!(idx.contains("libm.so.6"), "index 应含完整 needed_so: {idx}");
        assert!(idx.contains("libfoo.so,libfoo.so.1"), "index 保留 provides: {idx}");

        // 仓库 .lpkg 的 metadata.json 已修正（repack 重打包，不 rebuild）
        let extract2 = out.join("extract").join("libfoo-check");
        crate::scan::extract_lpkg(&repo_lpkg, &extract2).unwrap();
        let meta = crate::scan::read_metadata_json(&extract2.join("metadata.json")).unwrap();
        let n: Vec<&str> = meta["needed_so"]
            .as_array()
            .unwrap()
            .iter()
            .filter_map(|v| v.as_str())
            .collect();
        assert!(n.contains(&"libm.so.6"), "repack 后 metadata.json 应有 libm: {n:?}");

        // LankeBUILD.json 同步（双写）
        let b = read_lankebuild(&dir, "libfoo").unwrap();
        assert!(b.needed_so.contains(&"libm.so.6".to_string()));

        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn abi_break_rebuilds_direct_victim_and_bumps_release() {
        // 集成验证 §7.2 传播（真实实现，非空壳）+ 反哺仓库顺序：
        // A SONAME 变（libfoo.so.1→.2）→ 直连受害者 B 自动重建 + release bump；
        // A 必须先进仓库（index 更新），B 才能按新 ABI 重建。
        let dir = temp_dir("farm-build-abi");
        let out = temp_dir("farm-build-abi-out");
        write_baseline(
            &out,
            "a|1.0:h::libfoo.so,libfoo.so.1:|\nb|1.0:h::libb.so:libfoo.so.1,libc.so.6|\n",
        );
        write_pkg(&dir, "a", &["libfoo.so", "libfoo.so.1"], &[], &[]);
        write_pkg(&dir, "b", &["libb.so"], &["libfoo.so.1", "libc.so.6"], &["a"]);

        // a 重建后 SONAME bump → libfoo.so.2（metadata 也变，repack 触发）
        let a_lpkg = stage_lpkg(
            &out, "a", "1.0", &["libc.so.6"],
            &["libfoo.so", "libfoo.so.1"],
        );
        // b 的 staging 产物：metadata 需要 libfoo.so.2（重建后按新 ABI）
        let b_lpkg = stage_lpkg(
            &out, "b", "1.0+1", &["libfoo.so.2", "libc.so.6"],
            &["libb.so"],
        );
        let mut outcomes = HashMap::new();
        outcomes.insert(
            "a".into(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libc.so.6".into()],
                provides: vec!["libfoo.so".into(), "libfoo.so.2".into()],
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(a_lpkg),
            },
        );
        outcomes.insert(
            "b".into(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libfoo.so.2".into(), "libc.so.6".into()],
                provides: vec!["libb.so".into()],
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(b_lpkg),
            },
        );
        let mut binding = StubBinding::new(outcomes);
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec!["a".into()],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
        };
        let report = run_build(&opts, &mut binding, None).unwrap();
        assert!(report.abi_broken.contains(&"a".to_string()));
        assert!(
            report.built.contains(&"b".to_string()),
            "ABI 断裂应自动重建直连受害者 b"
        );
        // 传播重建 → b 的 release bump（先 +1 再构建）
        let b = read_lankebuild(&dir, "b").unwrap();
        assert_eq!(b.release, Some(1), "ABI 传播重建应先 bump release");
        // 反哺仓库：a 和 b 都已进本地仓库 + index 更新（a 的 SONAME 已变 .2）。
        // 文件名按 lpkg 的 `<version>.lpkg` URL 约定（不是 `<pkg>-<version>.lpkg`）。
        assert!(out.join("x86_64/a/1.0.lpkg").exists(), "a 应进仓库");
        assert!(out.join("x86_64/b/1.0+1.lpkg").exists(), "b 应进仓库（version+release）");
        let idx = fs::read_to_string(out.join("x86_64/index.txt")).unwrap();
        assert!(idx.contains("libfoo.so.2"), "index 应反映 a 的新 SONAME: {idx}");
        assert!(idx.contains("b|1.0+1"), "index 应反映 b 的 release bump: {idx}");
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn build_requires_seeded_old_index() {
        // §7.2 基线强制：没有 seed 落地的本地 repo 索引 → 直接报错，禁止无基线构建（盲人摸象）。
        let dir = temp_dir("farm-build-nobaseline");
        let out = temp_dir("farm-build-nobaseline-out");
        write_pkg(&dir, "libfoo", &["libfoo.so", "libfoo.so.1"], &[], &[]);
        let mut binding = StubBinding::new(HashMap::new());
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec!["libfoo".into()],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
        };
        let err = run_build(&opts, &mut binding, None).unwrap_err();
        assert!(
            err.contains("farm seed"),
            "应明确提示先 seed（{err}），而非静默当首次构建"
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn abi_victim_skips_pre_download_confirmed_bulk_predownloaded() {
        // 预下载拆分（用户规则）：确认集（初始 target）开始构建前 bulk 预下载全部源；
        // ABI 受害者动态入队，**不预下载**——构建时由 lpkg build 自己下载。
        // 复现 gettext/bootstrap 场景：victim b 的源 URL 故意不可达——若没跳过，b 会进
        // source-missing；跳过后 StubBinding 直接成功。
        let (_h, port, root) = spawn_test_server();
        fs::write(root.join("asrc.tar.gz"), b"hello-src").unwrap();
        let dir = temp_dir("farm-victim-nopredl");
        let out = temp_dir("farm-victim-nopredl-out");
        write_baseline(
            &out,
            "a|1.0:h::libfoo.so,libfoo.so.1:|\nb|1.0:h::libb.so:libfoo.so.1,libc.so.6|\n",
        );
        let a_src = format!("http://127.0.0.1:{port}/asrc.tar.gz");
        write_pkg_full(&dir, "a", &["libfoo.so", "libfoo.so.1"], &[], &[&a_src]);
        // b 的源故意不可达：victim 不预下载，才不至于 source-missing
        write_pkg_full(
            &dir,
            "b",
            &["libb.so"],
            &["libfoo.so.1", "libc.so.6"],
            &["http://127.0.0.1:1/nope.tar.gz"],
        );

        // a 重建 → SONAME .1→.2（ABI 断裂）→ b 直连受害者
        let a_lpkg = stage_lpkg(
            &out, "a", "1.0", &["libc.so.6"],
            &["libfoo.so", "libfoo.so.2"],
        );
        let b_lpkg = stage_lpkg(
            &out, "b", "1.0+1", &["libfoo.so.2", "libc.so.6"],
            &["libb.so"],
        );
        let mut outcomes = HashMap::new();
        outcomes.insert(
            "a".into(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libc.so.6".into()],
                provides: vec!["libfoo.so".into(), "libfoo.so.2".into()],
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(a_lpkg),
            },
        );
        outcomes.insert(
            "b".into(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libfoo.so.2".into(), "libc.so.6".into()],
                provides: vec!["libb.so".into()],
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(b_lpkg),
            },
        );
        let mut binding = StubBinding::new(outcomes);
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec!["a".into()],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
        };
        let report = run_build(&opts, &mut binding, None).unwrap();

        assert!(report.built.contains(&"a".to_string()));
        assert!(
            report.built.contains(&"b".to_string()),
            "ABI 断裂应重建直连受害者 b: {:?}",
            report.built
        );
        assert!(
            report.source_missing.is_empty(),
            "victim 跳过预下载，不应 source-missing: {:?}",
            report.source_missing
        );
        assert!(
            dir.join("a/asrc.tar.gz").exists(),
            "确认集 a 应在构建前 bulk 预下载源"
        );
        assert!(
            !dir.join("b/nope.tar.gz").exists(),
            "victim b 不应预下载（靠 lpkg build 自己下载）"
        );

        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
        fs::remove_dir_all(&root).ok();
        drop(_h);
    }

    #[test]
    fn backup_cleaned_when_soname_unreferenced() {
        // 整个 build 完成后清理 ABI 过渡备份：备份的旧 SONAME 已不再被任何包 needed_so 引用
        // → 删除（含空根目录）。本用例：无可构建包（队列空），cleanup 仍执行。
        let dir = temp_dir("farm-backup-clean");
        let out = temp_dir("farm-backup-clean-out");
        // libfoo 需 libc.so.6（index.txt 要有 needed_so，cleanup 的"剥离时代遗留"守卫才放行）
        write_baseline(&out, "libfoo|1.0:h::libfoo.so,libfoo.so.1:libc.so.6|\n");
        write_pkg(&dir, "libfoo", &["libfoo.so", "libfoo.so.1"], &["libc.so.6"], &[]);
        // 预置一个"已无引用"的备份（libz.so.1 不在 index.txt 任何 needed_so 里）
        fs::create_dir_all(out.join("backups/libz")).unwrap();
        fs::write(out.join("backups/libz/libz.so.1"), b"").unwrap();
        fs::write(out.join("backups/libz/libz.so.1.2.3"), b"").unwrap();

        let mut binding = StubBinding::new(HashMap::new());
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec![],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
        };
        let _ = run_build(&opts, &mut binding, None).unwrap();
        assert!(
            !out.join("backups").exists(),
            "无引用的备份目录应整体清理"
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn backup_kept_when_soname_still_referenced() {
        // 反向：仍有包的 needed_so 引用旧 SONAME（该包被跳过/BLOCKED 未重建）→ 备份保留，
        // 等下次 build 完成后再清。本用例 gettext 需要 libxml2.so.2 → libxml2 备份保留。
        let dir = temp_dir("farm-backup-keep");
        let out = temp_dir("farm-backup-keep-out");
        write_baseline(&out, "gettext|1.0:h::libgettext.so:libxml2.so.2,libc.so.6|\n");
        write_pkg(&dir, "gettext", &["libgettext.so"], &["libxml2.so.2", "libc.so.6"], &[]);
        fs::create_dir_all(out.join("backups/libxml2")).unwrap();
        fs::write(out.join("backups/libxml2/libxml2.so.2"), b"").unwrap();
        fs::write(out.join("backups/libxml2/libxml2.so.2.9.14"), b"").unwrap();

        let mut binding = StubBinding::new(HashMap::new());
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec![],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
        };
        let _ = run_build(&opts, &mut binding, None).unwrap();
        assert!(
            out.join("backups/libxml2/libxml2.so.2").exists(),
            "仍有引用的备份应保留（过渡未完成）"
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }
}
