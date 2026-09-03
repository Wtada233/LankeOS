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

use crate::abi;
use crate::graph::{Index, RevMap};
use crate::lpkg_binding::{BuildOutcome, LpkgBinding};
use crate::state::{JobStatus, State};
use crate::tr;
use crate::ux;
mod repo;
mod sched;
pub(crate) use repo::{
    bump_release, cleanup_backups, effective_version, load_old_index, needs_build, place_in_repo,
    recipe_hash, repack_if_drift, sha256_file, sorted_pkg_names, update_lankebuild_metadata,
    update_repo_index,
};
mod farm_flags;
mod groups;
mod prompt;
mod sources;
pub(crate) use groups::RebuildGroups;
pub(crate) use prompt::{prompt_blocked, PromptChoice};
pub(crate) use sched::{reorder_queue, topo_order};
pub(crate) use sources::pre_download_sources;

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
    /// 声明式重建组目录（`data/build/*.yaml`，与 data/trackers 同模式）。
    pub build_data_dir: PathBuf,
    /// validate 模式：初始选择改为"所有没有 `.build_ok` 标记的包"（而非版本增量 skip）。
    /// 成功构建写 `.build_ok`；跳过/blocked 不写（下次 validate 会重试）。
    pub validate: bool,
    /// --manual-sort：严格按命令行传入的包名顺序构建（引导链/手工编排），不做 topo 重排。
    pub manual_sort: bool,
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
    /// farm metadata：给 build/validate 看的声明式标志（lpkg 构建不消费）。
    /// 格式见 `build/farm_flags.rs`（目前支持 `BUILD_AFTER_BUILD_DEPS`）。
    #[serde(default)]
    pub farm_flags: Vec<String>,
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

/// 源就绪门（§8.6）：第一次安装计划中能确定的 http/https 源**必须全部下载**（用户规则）。
///
/// - 交互模式：下载失败 → 开宿主 shell 让 operator 手动介入（放置源/修网络/改 URL），
///   退出后重试。**不许退出、不许跳过**——直到源就绪才放行。
/// - 非交互模式：无 operator 可介入 → 返回 Err（整个构建终止，不出现 source-missing 继续）。
/// - 唯一允许"跳过"的是 `git+`/`file://` 源（`is_skip_source`，git 构建时由 lpkg 处理）。
/// - ABI 受害者（is_victim）不在第一次安装计划内，不预下载（构建时由 lpkg build 自己下载）。
fn source_gate(pkg: &str, opts: &BuildOptions, is_victim: bool) -> Result<(), String> {
    if is_victim {
        return Ok(());
    }
    loop {
        match pre_download_sources(&opts.pkgs_dir, pkg, opts.download_retries) {
            Ok(()) => return Ok(()),
            Err(e) if opts.interactive => {
                eprintln!("  {}", ux::yellow(&tr!("build.source_missing", pkg, e)));
                // 开 shell 手动介入；退出后回到循环顶部重试（仍失败会再次开 shell）
                prompt::open_shell(pkg, opts);
            }
            Err(e) => return Err(tr!("build.source_missing_fatal", pkg, e)),
        }
    }
}

/// validate 标记：`.build_ok` **存在且内容 == 当前 LankeBUILD+LankeBUILD.json 的 sha256**
/// （recipe_hash）才算"已成功构建且配方未变"。配方变了 → 标记失效 → validate 会重建。
pub(crate) fn has_build_ok(pkgs_dir: &Path, pkg: &str) -> bool {
    let Some(expected) = recipe_hash(pkgs_dir, pkg) else {
        return false;
    };
    let Ok(stored) = std::fs::read_to_string(pkgs_dir.join(pkg).join(".build_ok")) else {
        return false;
    };
    stored.trim() == expected
}

/// validate 标记：成功构建后写 `.build_ok`（内容 = 当前 recipe_hash；跳过/blocked 不写）。
pub(crate) fn mark_build_ok(pkgs_dir: &Path, pkg: &str) -> std::io::Result<()> {
    let h = recipe_hash(pkgs_dir, pkg).unwrap_or_default();
    std::fs::write(pkgs_dir.join(pkg).join(".build_ok"), h.as_bytes())
}

/// abifix 修复清单：扫描 pkgs/ 的 LankeBUILD.json，返回 `needed_so` 引用「仓库 index 无任何
/// 包提供」的 SONAME 的包及其缺失清单（`(pkg, missing)`）。自提供不算缺失（scan 语义——
/// 包自身 SONAME 已从 needed_so 扣除）。判定以旧索引 `all_provided_capabilities` 为仓库能力
/// 真源（与 scan 的 not-found 过滤 / ABI 传播同源）。
///
/// 调用方（farm abifix）据此 bump release 后强制重建：重建时容器按当前仓库 provider 装依赖，
/// 孤儿 needed_so 若不再链接则重扫后自动消失；若仍真需要则构建失败（BLOCKED）→ 提示先更新
/// provider 配方（如 display-info 上游 soversion 变、下游还没跟上）。
pub(crate) fn abifix_targets(pkgs_dir: &Path, old: &Index) -> Vec<(String, Vec<String>)> {
    let provided = old.all_provided_capabilities();
    let mut out = Vec::new();
    for pkg in sorted_pkg_names(pkgs_dir) {
        let Some(b) = read_lankebuild(pkgs_dir, &pkg) else {
            continue;
        };
        let own: HashSet<&str> = b.provides.iter().map(String::as_str).collect();
        let missing: Vec<String> = b
            .needed_so
            .iter()
            .filter(|s| !provided.contains(s.as_str()) && !own.contains(s.as_str()))
            .cloned()
            .collect();
        if !missing.is_empty() {
            out.push((pkg, missing));
        }
    }
    out
}

/// abifix 全流程（CLI 入口调用的 pub 面）：载入旧索引 → 检测孤儿 → 打印 + 逐个 bump release →
/// 返回修复目标包名清单（cli 据此强制重建，ABI 传播在 run_build 内照常级联）。
/// **无目标返回空 Vec**——调用方绝不能落到空目标的增量构建。
pub fn abifix_plan(pkgs_dir: &Path, out_dir: &Path, arch: &str) -> Result<Vec<String>, String> {
    let old = load_old_index(out_dir, arch)?;
    let targets = abifix_targets(pkgs_dir, &old);
    if targets.is_empty() {
        println!("{}", tr!("abifix.none"));
        return Ok(Vec::new());
    }
    println!("{}", tr!("abifix.title", targets.len()));
    for (pkg, missing) in &targets {
        println!(
            "  {}",
            ux::yellow(&tr!("abifix.target", pkg, missing.join(", ")))
        );
    }
    // bump release = 重建信号（与 ABI 传播 victim 的 bump 规则一致），重建重扫 needed_so
    for (pkg, _) in &targets {
        bump_release(pkgs_dir, pkg);
    }
    Ok(targets.into_iter().map(|(p, _)| p).collect())
}

/// 进程内交互接管（§8.5）：BLOCKED 时提示 operator 选择，不退出进程。
/// 主调度：返回构建报告（built/repacked/abi_broken/blocked）。
/// `state` 非空时记录 job 状态 + 配方 hash（§11 持久化；读端/差分 requeue 尚未实现，
/// 仅作 operator 排查用）。失败路径（source 缺失 / repack / repo / index）也落 Blocked 库。
pub fn run_build(
    opts: &BuildOptions,
    binding: &mut dyn LpkgBinding,
    state: Option<&State>,
) -> Result<BuildReport, String> {
    // 1. 旧索引（§7.2 传播反图的锚）——必须由 seed 落地的本地 repo index.txt，缺失/为空直接报错
    //    （禁止无基线构建：needed_so provider 校验、ABI diff 都需要它）。
    let old = load_old_index(&opts.out_dir, &opts.arch)?;
    // 仓库全部提供能力 → binding 扫描 not-found 判定（needed_so 无 provider → 不进 needed_so）
    binding.set_repo_provides(old.all_provided_capabilities());
    let revmap = RevMap::build(&old);
    // 声明式重建组（data/build/*.yaml）：不链但 ABI 敏感的包（python 生态等）。
    let groups = RebuildGroups::load(&opts.build_data_dir);

    // 2. 增量选择（用户规则）：effective_version 与本地 repo 旧索引一致的包跳过构建。
    //    LankeBUILD.json 的 version 是 raw；有 release 字段拼 version+release（如 1.1+2）。
    //    validate 模式：选择改为"所有没有 `.build_ok` 标记的包"（成功构建才会写标记，
    //    跳过/blocked 不写 → 下次 validate 重试）。排序仍走同一 topo_order。
    let all_pkgs = sorted_pkg_names(&opts.pkgs_dir);
    let initial: Vec<String> = if opts.targets.is_empty() {
        let v: Vec<String> = all_pkgs
            .iter()
            .filter(|p| {
                if opts.validate {
                    !has_build_ok(&opts.pkgs_dir, p)
                } else {
                    needs_build(&opts.pkgs_dir, p, &old)
                }
            })
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
    // 组边参与初始排序：声明式组受害者（python-* 等）排在触发包之后——
    // `--all` 时它们已在初始队列，没有 needed_so 链接边，须靠组边强制 python 先建。
    let group_edges = groups.trigger_edges_in(&initial);
    // --manual-sort：严格按命令行传入的包名顺序构建（引导链/手工编排用），不做 topo 重排。
    // 否则纯 python 包（无 needed_so 边）会按字母序建，bootstrap（setuptools→flit-core→build）会断。
    let mut queue: VecDeque<(String, bool)> = if opts.manual_sort && !opts.targets.is_empty() {
        opts.targets.iter().cloned().map(|p| (p, false)).collect()
    } else {
        topo_order(&opts.pkgs_dir, &initial, &old, &group_edges)
            .into_iter()
            .map(|p| (p, false))
            .collect()
    };

    // 2.5 构建计划预览：topo 顺序（仅"最开始能确认需要 build"的包；ABI 受害者随后动态入队）。
    // 交互模式 → 列出顺序并让 operator 确认才开始；确认后**只为确认集**预下载全部源。
    // ABI 受害者不预下载——构建时由 lpkg build 自己下载（URL 未知性 + 不浪费等待）。
    if !queue.is_empty() {
        prompt::print_build_plan(&queue, opts);
        if opts.interactive && !prompt::confirm_plan() {
            println!("{}", tr!("build.plan_cancel"));
            return Ok(BuildReport::default());
        }
        // 确认集全部 http/https 源**必须预下载**（用户规则）：任何失败都不允许跳过/标记 missing
        // 继续——交互模式开宿主 shell 手动介入后重试，非交互则整个构建终止。
        for (pkg, _) in &queue {
            source_gate(pkg, opts, false)?;
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
        // 源预下载失败**不允许跳过 / 不允许标记 missing 继续**（用户规则）：交互模式开宿主
        // shell 手动介入后重试；非交互无 operator → 整个构建硬终止。
        // 构建失败仍走原菜单：1) 开 shell 修复 2) 跳过 3) 结束。
        let (done, end_build) = 'pkg: loop {
            // §8.6 源预下载：宿主侧预取，源就绪才构建。
            // ABI 受害者不在第一次安装计划内，不预下载（构建时由 lpkg build 自己下载）。
            // 非交互下源无法下载 → 构建终止（不允许 source-missing 状态继续）
            source_gate(&pkg, opts, is_victim)?;

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
                        let _ = st.set_job(
                            &pkg,
                            JobStatus::Skipped,
                            Some("operator skip"),
                            rhash.as_deref(),
                        );
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
        // **repack 失败 → BLOCK**（曾静默降级为"无漂移"照发陈旧 metadata，.lpkg 与 index 永久失配）。
        let drifted = match repack_if_drift(&outcome, opts, &pkg) {
            Ok(d) => d,
            Err(e) => {
                eprintln!("{}", tr!("build.repack_fail", pkg, e));
                report.blocked.push(pkg.clone());
                if let Some(st) = state {
                    let _ = st.set_job(&pkg, JobStatus::Blocked, Some("repack"), rhash.as_deref());
                }
                continue;
            }
        };
        if drifted {
            update_lankebuild_metadata(&opts.pkgs_dir, &pkg, &outcome);
            report.repacked.push(pkg.clone());
            println!("  {}", ux::yellow(&tr!("build.repack", pkg)));
        }

        // 上传本地仓库（取代旧版本）+ 更新 index.txt —— **breaking 包必须先进仓库**，
        // 否则依赖它的包重建时仍用旧 ABI（用户规则：反哺仓库 / 中间上传流程）。
        let final_lpkg = match place_in_repo(&outcome, opts, &pkg) {
            Ok(p) => p,
            Err(e) => {
                eprintln!("{}", tr!("build.repo_fail", pkg, e));
                report.blocked.push(pkg.clone());
                if let Some(st) = state {
                    let _ = st.set_job(&pkg, JobStatus::Blocked, Some("repo"), rhash.as_deref());
                }
                continue;
            }
        };
        let version = effective_version(&opts.pkgs_dir, &pkg).unwrap_or_else(|| "?".into());
        let hash = sha256_file(&final_lpkg).unwrap_or_default();
        // index.txt：**写回完整 needed_so**（单一真源）。容器可见索引与 farm 的 ABI 传播共用，
        // 不再剥 needed_so、不再维护第二份 .abi.json；构建顺序/传播/备份清理都从这里读。
        // 容器的 SONAME 检查由 --missing-so-no-error / --use-system-soname 在过渡期容忍。
        if let Err(e) = update_repo_index(
            &opts.out_dir,
            &opts.arch,
            &pkg,
            &version,
            &hash,
            &outcome.deps,
            &outcome.provides,
            &outcome.needed_so,
        ) {
            eprintln!("{}", tr!("build.index_fail", pkg, e));
            report.blocked.push(pkg.clone());
            if let Some(st) = state {
                let _ = st.set_job(&pkg, JobStatus::Blocked, Some("index"), rhash.as_deref());
            }
            continue;
        }
        report.built.push(pkg.clone());
        println!(
            "  {}",
            ux::green(&tr!("build.repo", pkg, final_lpkg.display()))
        );

        // validate 标记：构建 + repack + 进 repo + index 全部成功后，在包目录写 `.build_ok`。
        // farm validate 据此只重建没有标记的包；跳过/blocked 不进此分支（不写标记 → 下次重试）。
        let _ = mark_build_ok(&opts.pkgs_dir, &pkg);

        // 临时目录清理：解包目录（scan/repack 共用，已用完）与 staging（产物已 rename 进 repo）。
        // 只清成功路径——构建失败时保留，供 operator 排查/重试（下次 scan 会先清空解包目录）。
        // 解包目录含 root 属主树（sudo tar 保留所有权）→ 用 sudo 感知删除。
        let _ = crate::scan::remove_dir_tree(&opts.out_dir.join("extract").join(&pkg));
        let _ = crate::scan::remove_dir_tree(&opts.out_dir.join(".staging").join(&pkg));

        // ABI 传播（§7.2）：removed SONAME → 直连受害者重建；声明式重建组（data/build/*.yaml）
        // 额外重建"不链但 ABI/运行时敏感"的包。变化的 SONAME 无包直接 need → 改好元数据进仓库。
        //
        // 触发语义（用户规则）：
        //   - abichange 组（python…）：只在 SONAME 断裂时触发（removed 非空）
        //   - version-change 组（perl 等纯解释器，无 libperl.so 可断）：on 包本轮重建且有效版本
        //     与旧索引不同时，按 version-change-script 判定（OLD_VER/NEW_VER，如 minor 变才重建），
        //     独立于 ABI 断裂——不再有"任何重建都触发"的 script_interpreter 回退（patch 升级会
        //     无谓拖垮整个组，已删）。
        let removed = abi::removed_sonames(&old, &pkg, &outcome.provides);
        let group_trigger = !removed.is_empty();
        if !removed.is_empty() {
            report.abi_broken.push(pkg.clone());
        }
        // 直连受害者（链接被移除 SONAME 的包，只在真 ABI 断裂时）∪ 声明式重建组受害者 ∪ version-change 受害者
        let mut victims = abi::direct_victims(&revmap, &removed);
        if group_trigger {
            victims.extend(groups.victims_for(&pkg, &all_pkgs));
        }
        // version-change：on 包版本变化（旧索引 vs 本轮有效版本）→ 脚本判定 exit 0 才重建组受害者
        let version_victims: Vec<String> = match old.packages.get(&pkg) {
            Some(ov) if ov.version != version => {
                match groups.version_victims_if(&pkg, &ov.version, &version, &all_pkgs) {
                    Ok(v) => v,
                    Err(e) => {
                        eprintln!("{}", tr!("build.version_change_fail", pkg, e));
                        Vec::new()
                    }
                }
            }
            _ => Vec::new(),
        };
        victims.extend(version_victims.iter().cloned());
        if !victims.is_empty() {
            victims.sort();
            victims.dedup();
            for v in victims {
                if !seen.contains(&v) {
                    if !removed.is_empty() {
                        println!(
                            "  {}",
                            ux::yellow(&tr!("build.abi", pkg, removed.join(", "), v))
                        );
                    } else if version_victims.contains(&v) {
                        println!("  {}", ux::yellow(&tr!("build.version_rebuild", pkg, v)));
                    } else {
                        println!("  {}", ux::yellow(&tr!("build.group_rebuild", pkg, v)));
                    }
                    queue.push_back((v, true)); // 传播重建 → 触发 release bump
                }
            }
            // 受害者按**依赖算法**重排：被依赖的受害者先建，依赖它们的后建。
            // 否则按字母序先建 appstream 时，其构建依赖树里的 librsvg（同样是 libxml2 受害者，
            // 还引用旧 libxml2.so.2）未重建 → 装构建依赖时 SONAME 无 provider 硬报错。
            // --manual-sort 时跳过：严格保持手工传入顺序（引导链场景无 ABI 受害者）。
            if !opts.manual_sort {
                reorder_queue(&mut queue, &opts.pkgs_dir, &old, &groups);
            }
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
    use super::sources::sources_ready;
    use super::*;
    use crate::graph::Index;
    use crate::lpkg_binding::{BuildOutcome, StubBinding};
    use std::collections::HashMap;

    fn write_pkg(pkgs: &Path, name: &str, provides: &[&str], needed: &[&str], build_deps: &[&str]) {
        write_pkg_ver(pkgs, name, "1.0", provides, needed, build_deps);
    }

    /// 同 `write_pkg`，但版本可指定（version-change 传播测试用：old 索引 vs 配方版本对比）。
    fn write_pkg_ver(
        pkgs: &Path,
        name: &str,
        version: &str,
        provides: &[&str],
        needed: &[&str],
        build_deps: &[&str],
    ) {
        write_pkg_flags(pkgs, name, version, provides, needed, build_deps, &[]);
    }

    /// 同 `write_pkg_ver`，可额外写 `farm_flags`（farm metadata 排序测试用）。
    fn write_pkg_flags(
        pkgs: &Path,
        name: &str,
        version: &str,
        provides: &[&str],
        needed: &[&str],
        build_deps: &[&str],
        farm_flags: &[&str],
    ) {
        let dir = pkgs.join(name);
        fs::create_dir_all(&dir).unwrap();
        let json = serde_json::json!({
            "name": name,
            "version": version,
            "provides": provides,
            "needed_so": needed,
            "build_deps": build_deps,
            "farm_flags": farm_flags,
        });
        fs::write(
            dir.join("LankeBUILD.json"),
            serde_json::to_string_pretty(&json).unwrap(),
        )
        .unwrap();
        // 真实包两者都有：recipe_hash 读 LankeBUILD + LankeBUILD.json 两个文件
        fs::write(dir.join("LankeBUILD"), "lankebuild_build() { : }\n").unwrap();
    }

    // validate 标记：成功构建写 `.build_ok`，has_build_ok 据此判定；未写的包 validate 会重试。
    #[test]
    fn build_ok_marker_lifecycle() {
        let tmp = std::env::temp_dir().join(format!("farm-buildok-test-{}", std::process::id()));
        let pkgs = tmp.join("pkgs");
        let _ = fs::remove_dir_all(&pkgs);
        fs::create_dir_all(&pkgs).unwrap();
        write_pkg(&pkgs, "alpha", &[], &[], &[]);
        write_pkg(&pkgs, "beta", &[], &[], &[]);

        // 初始都没有标记
        assert!(!has_build_ok(&pkgs, "alpha"));
        assert!(!has_build_ok(&pkgs, "beta"));

        // 只有 alpha 构建成功 → 写标记；beta 跳过/blocked → 不写
        mark_build_ok(&pkgs, "alpha").unwrap();
        assert!(has_build_ok(&pkgs, "alpha"));
        assert!(!has_build_ok(&pkgs, "beta"));

        // 配方内容不变 → 标记仍有效（hash 匹配）
        write_pkg(&pkgs, "alpha", &[], &[], &[]);
        assert!(has_build_ok(&pkgs, "alpha"), "配方未变标记应仍有效");

        // 配方变了（version 1.0 → 2.0）→ hash 不匹配 → 标记失效 → validate 重建
        let j = serde_json::json!({
            "name": "alpha", "version": "2.0",
            "provides": [], "needed_so": [], "build_deps": [],
        });
        fs::write(
            pkgs.join("alpha").join("LankeBUILD.json"),
            serde_json::to_string_pretty(&j).unwrap(),
        )
        .unwrap();
        assert!(!has_build_ok(&pkgs, "alpha"), "配方变化后 .build_ok 应失效");

        let _ = fs::remove_dir_all(&tmp);
    }

    /// 合成一个真实可解包的 .lpkg（metadata.json + content/libfoo.so.1 假 ELF）。
    fn make_test_lpkg(path: &Path, name: &str, version: &str, needed: &[&str], provides: &[&str]) {
        let src = path.parent().unwrap().join("lpkg-src");
        let _ = fs::remove_dir_all(&src);
        fs::create_dir_all(src.join("content")).unwrap();
        fs::write(
            src.join("content/libfoo.so.1"),
            [0x7f, b'E', b'L', b'F', 2, 1, 1],
        )
        .unwrap();
        let meta = serde_json::json!({
            "name": name,
            "version": version,
            "needed_so": needed,
            "provides": provides,
            "deps": [],
        });
        fs::write(
            src.join("metadata.json"),
            serde_json::to_string_pretty(&meta).unwrap(),
        )
        .unwrap();
        let f = fs::File::create(path).unwrap();
        let enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
        let mut b = tar::Builder::new(enc);
        b.append_dir_all(".", &src).unwrap();
        let enc = b.into_inner().unwrap();
        enc.finish().unwrap();
        fs::remove_dir_all(&src).ok();
    }

    /// 为包准备 staging .lpkg + 解包目录（模拟 RealBinding 产物），返回 lpkg 路径。
    fn stage_lpkg(
        out: &Path,
        pkg: &str,
        version: &str,
        needed: &[&str],
        provides: &[&str],
    ) -> PathBuf {
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
        fs::write(
            dir.join("LankeBUILD.json"),
            serde_json::to_string_pretty(&json).unwrap(),
        )
        .unwrap();
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
        fs::write(
            dir.join("LankeBUILD.json"),
            serde_json::to_string_pretty(&json).unwrap(),
        )
        .unwrap();
    }

    /// 起一个本地 HTTP 服务器 fixture（serve.rs），serve 临时目录。返回 (handle, port, root)。
    /// 线程随测试进程退出；root 由调用方清理。**原子递增端口 + 按端口分 root**——
    /// 多个测试并行起服务器不会互相冲突（各自独立目录）。基数取 18100：避开
    /// net.rs 测试硬编码的 18080/18081（历史遗留固定端口）。
    fn spawn_test_server() -> (std::thread::JoinHandle<()>, u16, std::path::PathBuf) {
        use std::sync::atomic::{AtomicU16, Ordering};
        static NEXT_PORT: AtomicU16 = AtomicU16::new(18100);
        let port = NEXT_PORT.fetch_add(1, Ordering::Relaxed);
        let root =
            std::env::temp_dir().join(format!("farm-serve-test-{}-{port}", std::process::id()));
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
    fn source_missing_aborts_build_in_non_interactive() {
        // 用户规则：http/https 源必须下载，**不允许 source-missing 继续**。
        // 非交互无 operator 介入 → run_build 整体硬终止（Err），而不是标记 missing 跳过继续。
        let dir = temp_dir("farm-src-missing-abort");
        let out = temp_dir("farm-src-missing-abort-out");
        write_baseline(&out, "a|1.0:h::liba.so.1:|\n");
        write_pkg_full(
            &dir,
            "a",
            &["liba.so.1"],
            &[],
            &["http://127.0.0.1:1/nope.tar.gz"],
        );
        let mut binding = StubBinding::new(HashMap::new());
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec!["a".into()],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 1,
            interactive: false,
            build_data_dir: std::path::PathBuf::from("data/build"),
            validate: false,
            manual_sort: false,
        };
        let err = run_build(&opts, &mut binding, None).unwrap_err();
        assert!(
            err.contains("source-missing"),
            "非交互源缺失应硬终止：{err}"
        );
        assert!(!dir.join("a/nope.tar.gz").exists());
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn sources_ready_tracks_network_files() {
        let pkgs = temp_dir("farm-src-ready");
        write_pkg_sources(
            &pkgs,
            "p",
            &[
                "file:///x",
                "git+https://github.com/a/b@v1",
                "http://127.0.0.1:1/a.tar.gz",
            ],
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
        let order = topo_order(&dir, &["a".into(), "b".into(), "c".into()], &old, &[]);
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
        write_pkg(
            &dir,
            "curl",
            &["libcurl.so.4"],
            &["libc.so.6", "libz.so.1"],
            &["glibc", "zlib"],
        );
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
            (
                "chromium",
                vec![],
                vec!["libc.so.6", "libz.so.1", "libcurl.so.4"],
            ),
        ]);
        let targets: Vec<String> = ["glibc", "zlib", "curl", "chromium"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let order = topo_order(&dir, &targets, &old, &[]);
        let pos = |s: &str| order.iter().position(|n| n == s).unwrap();
        assert!(pos("glibc") < pos("zlib"), "链接链应自底向上: {order:?}");
        assert!(pos("zlib") < pos("curl"));
        assert!(
            pos("curl") < pos("chromium"),
            "chromium 的依赖库应全在其前: {order:?}"
        );
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
        let order = topo_order(&dir, &targets, &old, &[]);
        assert_eq!(order.len(), 3, "环切断后应覆盖所有包: {order:?}");
        for t in &targets {
            assert!(order.contains(t), "不应丢包 {t}: {order:?}");
        }
        let pos = |s: &str| order.iter().position(|n| n == s).unwrap();
        assert!(pos("a") < pos("c"), "c 依赖 a，应在其后: {order:?}");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn topo_order_group_victims_after_trigger() {
        // 用户 bug 复现：`--all` 模式下 python-cairo（不链 libpython，无 needed_so 边）
        // 必须排在触发包 python 之后——否则容器 upgrade 时本地 repo 还是旧 python。
        // 组边（victim → on）强制排序；python 不在 targets 则无约束。
        let dir = std::env::temp_dir().join("farm-build-topo-group");
        let _ = std::fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        write_pkg(
            &dir,
            "python",
            &["libpython3.14.so", "libpython3.14.so.1"],
            &["libc.so.6"],
            &[],
        );
        // python-cairo 只链 libcairo/libc，**不链 libpython**（组依赖的由来）
        write_pkg(
            &dir,
            "python-cairo",
            &["libpycairo.so"],
            &["libcairo.so.2", "libc.so.6"],
            &[],
        );
        write_pkg(&dir, "blueman", &[], &["libc.so.6"], &[]);
        let old = index_of(&[
            (
                "python",
                vec!["libpython3.14.so", "libpython3.14.so.1"],
                vec!["libc.so.6"],
            ),
            (
                "python-cairo",
                vec!["libpycairo.so"],
                vec!["libcairo.so.2", "libc.so.6"],
            ),
            ("blueman", vec![], vec!["libc.so.6"]),
        ]);
        let gdir =
            std::env::temp_dir().join(format!("farm-topo-group-data-{}", std::process::id()));
        let _ = fs::remove_dir_all(&gdir);
        fs::create_dir_all(&gdir).unwrap();
        fs::write(
            gdir.join("python.yaml"),
            "rebuild-on-abichange: python\npackages: python-* blueman\n",
        )
        .unwrap();
        let groups = RebuildGroups::load(&gdir);
        let targets: Vec<String> = ["python", "python-cairo", "blueman"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let edges = groups.trigger_edges_in(&targets);
        let order = topo_order(&dir, &targets, &old, &edges);
        let pos = |x: &str| order.iter().position(|n| n == x).unwrap();
        assert!(
            pos("python") < pos("python-cairo"),
            "组受害者 python-cairo 必须在 python 之后: {order:?}"
        );
        assert!(
            pos("python") < pos("blueman"),
            "blueman 也应在 python 之后: {order:?}"
        );

        // python 不在 targets（不重建）→ 无组边约束，python-cairo 可独立排
        let targets2: Vec<String> = ["python-cairo", "blueman"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let edges2 = groups.trigger_edges_in(&targets2);
        assert!(
            edges2.is_empty(),
            "python 不在 targets 时不应有组边: {edges2:?}"
        );
        let order2 = topo_order(&dir, &targets2, &old, &edges2);
        assert_eq!(order2.len(), 2, "排序仍覆盖全部包: {order2:?}");
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&gdir).ok();
    }

    #[test]
    fn build_after_build_deps_orders_foo_before_bar_in_python_group() {
        // python 重建组（rebuild-on-abichange: python，packages: python-*）：python ABI 变化时
        // python-foo / python-bar 都会重建。python-bar 构建依赖 python-foo 且声明
        // BUILD_AFTER_BUILD_DEPS → Kahn 排序必须把 python-foo 排在 python-bar 前。
        // 没有该 flag 时两者是同级（无 bar→foo 边），名字升序会是 python-bar 先建——构建时
        // 容器里还没有 python-foo 刚产出的产物，基于旧 foo 构建白跑。
        let dir = std::env::temp_dir().join("farm-build-after-builddeps");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        write_pkg(&dir, "python", &["libpython3.14.so.1"], &["libc.so.6"], &[]);
        write_pkg(
            &dir,
            "python-foo",
            &["libpyfoo.so"],
            &["libpython3.14.so.1", "libc.so.6"],
            &["python"],
        );
        write_pkg_flags(
            &dir,
            "python-bar",
            "1.0",
            &["libpybar.so"],
            &["libpython3.14.so.1", "libc.so.6"],
            &["python-foo"],
            &["BUILD_AFTER_BUILD_DEPS"],
        );
        let old = index_of(&[
            ("python", vec!["libpython3.14.so.1"], vec!["libc.so.6"]),
            (
                "python-foo",
                vec!["libpyfoo.so"],
                vec!["libpython3.14.so.1", "libc.so.6"],
            ),
            (
                "python-bar",
                vec!["libpybar.so"],
                vec!["libpython3.14.so.1", "libc.so.6"],
            ),
        ]);
        let gdir =
            std::env::temp_dir().join(format!("farm-after-builddeps-group-{}", std::process::id()));
        let _ = fs::remove_dir_all(&gdir);
        fs::create_dir_all(&gdir).unwrap();
        fs::write(
            gdir.join("python.yaml"),
            "rebuild-on-abichange: python\npackages: python-*\n",
        )
        .unwrap();
        let groups = RebuildGroups::load(&gdir);

        // --all 模式：python + 两个组受害者都在 targets。边 = 组边（foo/bar → python）+
        // 链接边（foo/bar 链 libpython）+ BUILD_AFTER_BUILD_DEPS 边（bar → foo）。
        let targets: Vec<String> = ["python", "python-foo", "python-bar"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let edges = groups.trigger_edges_in(&targets);
        let order = topo_order(&dir, &targets, &old, &edges);
        let pos = |x: &str| order.iter().position(|n| n == x).unwrap();
        assert!(
            pos("python") < pos("python-foo"),
            "组受害者 foo 应在 python 后: {order:?}"
        );
        assert!(
            pos("python-foo") < pos("python-bar"),
            "python-bar 声明 BUILD_AFTER_BUILD_DEPS，必须先建构建依赖 python-foo: {order:?}"
        );

        // ABI 受害者重排路径（reorder_queue）：python 已建完，foo/bar 作为组受害者动态入队 →
        // 同一 topo_order，bar 的 build_deps 边仍让 foo 先建（且受害者标记保留）。
        let mut queue: VecDeque<(String, bool)> = vec![
            ("python-bar".to_string(), true),
            ("python-foo".to_string(), true),
        ]
        .into();
        reorder_queue(&mut queue, &dir, &old, &groups);
        let names: Vec<&str> = queue.iter().map(|(n, _)| n.as_str()).collect();
        let posq = |s: &str| names.iter().position(|n| *n == s).unwrap();
        assert!(
            posq("python-foo") < posq("python-bar"),
            "受害者重排也必须 foo 先建: {names:?}"
        );
        assert!(queue.iter().all(|(_, v)| *v), "受害者标记应保留: {queue:?}");

        // 对照：去掉 BUILD_AFTER_BUILD_DEPS → bar/foo 同级（无 bar→foo 边），名字升序
        // python-bar 在 python-foo 前——证明 flag 才是 foo 先建的唯一原因。
        write_pkg_flags(
            &dir,
            "python-bar",
            "1.0",
            &["libpybar.so"],
            &["libpython3.14.so.1", "libc.so.6"],
            &["python-foo"],
            &[],
        );
        let order_noflag = topo_order(&dir, &targets, &old, &edges);
        let pos2 = |x: &str| order_noflag.iter().position(|n| n == x).unwrap();
        assert!(
            pos2("python-bar") < pos2("python-foo"),
            "无 flag 时同级按名字升序（python-bar 在前），flag 是 foo 先建的唯一原因: {order_noflag:?}"
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&gdir).ok();
    }

    #[test]
    fn build_after_build_deps_skips_dep_not_in_targets() {
        // python-bar 构建依赖 python-foo，但本轮 python-foo **不会被 rebuild**（不在 targets）。
        // BUILD_AFTER_BUILD_DEPS 只对 targets 内的包生效 → bar→foo 边丢弃，python-bar 直接
        // 构建不等待 foo（与无关包 zlib 同级，按名字升序排在其前）。
        let dir = std::env::temp_dir().join("farm-build-after-builddeps-skip");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        write_pkg(&dir, "python", &["libpython3.14.so.1"], &["libc.so.6"], &[]);
        write_pkg(
            &dir,
            "python-foo",
            &["libpyfoo.so"],
            &["libpython3.14.so.1", "libc.so.6"],
            &["python"],
        );
        write_pkg_flags(
            &dir,
            "python-bar",
            "1.0",
            &["libpybar.so"],
            &["libpython3.14.so.1", "libc.so.6"],
            &["python-foo"],
            &["BUILD_AFTER_BUILD_DEPS"],
        );
        // zlib：本轮要重建、但与 python-bar 无任何依赖的无关包（名字升序锚点）。
        write_pkg(&dir, "zlib", &["libz.so.1"], &["libc.so.6"], &[]);
        let old = index_of(&[
            ("python", vec!["libpython3.14.so.1"], vec!["libc.so.6"]),
            (
                "python-foo",
                vec!["libpyfoo.so"],
                vec!["libpython3.14.so.1", "libc.so.6"],
            ),
            (
                "python-bar",
                vec!["libpybar.so"],
                vec!["libpython3.14.so.1", "libc.so.6"],
            ),
            ("zlib", vec!["libz.so.1"], vec!["libc.so.6"]),
        ]);
        // 本轮只重建 python-bar + zlib：python-foo 不在 targets。若 build_deps 边没被过滤，
        // bar 会挂一个无人解析的依赖 → 兜底路径顺序变 zlib, bar；过滤正确 → bar 与 zlib 同级
        // 名字升序：bar, zlib。
        let targets: Vec<String> = vec!["python-bar".to_string(), "zlib".to_string()];
        let order = topo_order(&dir, &targets, &old, &[]);
        assert_eq!(
            order,
            vec!["python-bar".to_string(), "zlib".to_string()],
            "python-foo 不重建 → bar→foo 边丢弃，bar 直接构建（名字升序）: {order:?}"
        );
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn topo_order_siblings_name_ordered_and_deterministic() {
        // 用户规则：构建顺序必须**确定**——同级包（无相互依赖）固定按名字升序；
        // 输入乱序不影响结果，且两次运行逐位一致（绝不允许随机）。
        let dir = std::env::temp_dir().join("farm-build-topo-siblings");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        write_pkg(&dir, "a", &["liba.so.1"], &[], &[]);
        write_pkg(&dir, "m", &["libm.so.1"], &[], &[]);
        write_pkg(&dir, "z", &["libz.so.1"], &[], &[]);
        let old = index_of(&[
            ("a", vec!["liba.so.1"], vec![]),
            ("m", vec!["libm.so.1"], vec![]),
            ("z", vec!["libz.so.1"], vec![]),
        ]);
        // 故意乱序输入（z,m,a）：heap 按名字升序弹出，输出与输入无关
        let targets: Vec<String> = ["z", "m", "a"].iter().map(|s| s.to_string()).collect();
        let order = topo_order(&dir, &targets, &old, &[]);
        assert_eq!(order, vec!["a", "m", "z"], "同级应名字升序: {order:?}");
        assert_eq!(
            order,
            topo_order(&dir, &targets, &old, &[]),
            "两次运行必须逐位一致（确定性）"
        );
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
        write_pkg(
            &dir,
            "appstream",
            &[],
            &["libxml2.so.2", "librsvg-2.so.2"],
            &["librsvg"],
        );
        write_pkg(
            &dir,
            "librsvg",
            &["librsvg-2.so.2"],
            &["libxml2.so.2"],
            &["libxml2"],
        );
        let old = index_of(&[
            ("libxml2", vec!["libxml2.so.2"], vec![]),
            ("librsvg", vec!["librsvg-2.so.2"], vec!["libxml2.so.2"]),
            ("appstream", vec![], vec!["libxml2.so.2", "librsvg-2.so.2"]),
        ]);
        let mut queue: VecDeque<(String, bool)> = vec![
            ("appstream".to_string(), true),
            ("librsvg".to_string(), true),
        ]
        .into();
        reorder_queue(&mut queue, &dir, &old, &RebuildGroups::default());
        let names: Vec<&str> = queue.iter().map(|(n, _)| n.as_str()).collect();
        let pos = |s: &str| names.iter().position(|n| *n == s).unwrap();
        assert!(
            pos("librsvg") < pos("appstream"),
            "被依赖的受害者应先建: {names:?}"
        );
        assert!(
            queue.iter().all(|(_, v)| *v),
            "全部应保持 victim: {queue:?}"
        );
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
        write_pkg(
            &dir,
            "chromium",
            &[],
            &["liba.so.1", "libb.so.1", "libc.so.1"],
            &[],
        );
        let old = index_of(&[
            ("libA", vec!["liba.so.1"], vec![]),
            ("libB", vec!["libb.so.1"], vec![]),
            ("libC", vec!["libc.so.1"], vec![]),
            (
                "chromium",
                vec![],
                vec!["liba.so.1", "libb.so.1", "libc.so.1"],
            ),
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
        reorder_queue(&mut queue, &dir, &old, &RebuildGroups::default());
        let chromium_count = queue.iter().filter(|(n, _)| n == "chromium").count();
        assert_eq!(chromium_count, 1, "chromium 应只出现一次: {queue:?}");
        let chromium_flag = queue.iter().find(|(n, _)| n == "chromium").unwrap().1;
        assert!(
            chromium_flag,
            "chromium 应保持 victim（任一断裂入队）: {queue:?}"
        );
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
        reorder_queue(&mut queue, &dir, &old, &RebuildGroups::default());
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
        write_pkg(
            &dir,
            "chromium",
            &[],
            &["liba.so.1", "libb.so.1", "libc.so.1"],
            &[],
        );
        let old = index_of(&[
            ("libA", vec!["liba.so.1"], vec![]),
            ("libB", vec!["libb.so.1"], vec![]),
            ("libC", vec!["libc.so.1"], vec![]),
            (
                "chromium",
                vec![],
                vec!["liba.so.1", "libb.so.1", "libc.so.1"],
            ),
        ]);
        // libA 断裂后：队列 = [libB, libC, chromium(victim)]
        let mut q1: VecDeque<(String, bool)> = vec![
            ("libB".to_string(), false),
            ("libC".to_string(), false),
            ("chromium".to_string(), true),
        ]
        .into();
        reorder_queue(&mut q1, &dir, &old, &RebuildGroups::default());
        assert_eq!(
            q1.back().map(|(n, _)| n.as_str()),
            Some("chromium"),
            "libA 断裂后 chromium 应仍在队尾: {q1:?}"
        );
        // libB 断裂后：队列 = [libC, chromium]
        let mut q2: VecDeque<(String, bool)> =
            vec![("libC".to_string(), false), ("chromium".to_string(), true)].into();
        reorder_queue(&mut q2, &dir, &old, &RebuildGroups::default());
        assert_eq!(
            q2.back().map(|(n, _)| n.as_str()),
            Some("chromium"),
            "libB 断裂后 chromium 应仍在队尾: {q2:?}"
        );
        // libC 断裂后：队列 = [chromium]（依赖全建完）
        let mut q3: VecDeque<(String, bool)> = vec![("chromium".to_string(), true)].into();
        reorder_queue(&mut q3, &dir, &old, &RebuildGroups::default());
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
        write_pkg(
            &dir,
            "libfoo",
            &["libfoo.so", "libfoo.so.1"],
            &["libc.so.6"],
            &[],
        );

        // staging .lpkg：metadata 缺 libm（陈旧）→ 扫描实际有 libm → 漂移
        let lpkg_path = stage_lpkg(
            &out,
            "libfoo",
            "1.0",
            &["libc.so.6"],
            &["libfoo.so", "libfoo.so.1"],
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
            build_data_dir: std::path::PathBuf::from("data/build"),
            validate: false,
            manual_sort: false,
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
        assert!(
            idx.contains("libfoo.so,libfoo.so.1"),
            "index 保留 provides: {idx}"
        );

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
        assert!(
            n.contains(&"libm.so.6"),
            "repack 后 metadata.json 应有 libm: {n:?}"
        );

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
        write_pkg(
            &dir,
            "b",
            &["libb.so"],
            &["libfoo.so.1", "libc.so.6"],
            &["a"],
        );

        // a 重建后 SONAME bump → libfoo.so.2（metadata 也变，repack 触发）
        let a_lpkg = stage_lpkg(
            &out,
            "a",
            "1.0",
            &["libc.so.6"],
            &["libfoo.so", "libfoo.so.1"],
        );
        // b 的 staging 产物：metadata 需要 libfoo.so.2（重建后按新 ABI）
        let b_lpkg = stage_lpkg(
            &out,
            "b",
            "1.0+1",
            &["libfoo.so.2", "libc.so.6"],
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
            build_data_dir: std::path::PathBuf::from("data/build"),
            validate: false,
            manual_sort: false,
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
        assert!(
            out.join("x86_64/b/1.0+1.lpkg").exists(),
            "b 应进仓库（version+release）"
        );
        let idx = fs::read_to_string(out.join("x86_64/index.txt")).unwrap();
        assert!(
            idx.contains("libfoo.so.2"),
            "index 应反映 a 的新 SONAME: {idx}"
        );
        assert!(
            idx.contains("b|1.0+1"),
            "index 应反映 b 的 release bump: {idx}"
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn rebuild_group_victims_rebuilt_on_abi_change() {
        // data/build/*.yaml 声明式重建组：python 的 libpython SONAME 断裂时，强制重建
        // **不链 libpython** 的 python 生态包（python-*、blueman）——它们没有 needed_so 链接边，
        // 不会进 direct_victims，必须靠声明式组传播（用户规则）。
        let dir = temp_dir("farm-group-abi");
        let out = temp_dir("farm-group-abi-out");
        let gdir = temp_dir("farm-group-abi-data");
        fs::create_dir_all(&gdir).unwrap();
        fs::write(
            gdir.join("python.yaml"),
            "rebuild-on-abichange: python\npackages: python-* blueman\n",
        )
        .unwrap();
        // 旧索引：python-cairo/gobject/blueman **不链** libpython（只有 libc/libcairo/libgobject）
        write_baseline(
            &out,
            "python|3.14:h::libpython3.14.so,libpython3.14.so.1:libc.so.6|\n\
             python-cairo|1.0:h::libpycairo.so:libcairo.so.2,libc.so.6|\n\
             python-gobject|1.0:h::libpygobject.so:libgobject-2.0.so.0,libc.so.6|\n\
             blueman|2.4:h:::libc.so.6|\n",
        );
        write_pkg(
            &dir,
            "python",
            &["libpython3.14.so", "libpython3.14.so.1"],
            &["libc.so.6"],
            &[],
        );
        write_pkg(
            &dir,
            "python-cairo",
            &["libpycairo.so"],
            &["libcairo.so.2", "libc.so.6"],
            &[],
        );
        write_pkg(
            &dir,
            "python-gobject",
            &["libpygobject.so"],
            &["libgobject-2.0.so.0", "libc.so.6"],
            &[],
        );
        write_pkg(&dir, "blueman", &[], &["libc.so.6"], &[]);

        // python 重建 → SONAME .14→.15 断裂（removed libpython3.14.so.1），直连受害者为空
        let python_lpkg = stage_lpkg(
            &out,
            "python",
            "1.0",
            &["libc.so.6"],
            &["libpython3.14.so", "libpython3.15.so", "libpython3.15.so.1"],
        );
        let pc = stage_lpkg(
            &out,
            "python-cairo",
            "1.0",
            &["libcairo.so.2", "libc.so.6"],
            &["libpycairo.so"],
        );
        let pg = stage_lpkg(
            &out,
            "python-gobject",
            "1.0",
            &["libgobject-2.0.so.0", "libc.so.6"],
            &["libpygobject.so"],
        );
        let bl = stage_lpkg(&out, "blueman", "1.0", &["libc.so.6"], &[]);
        let mut outcomes = HashMap::new();
        outcomes.insert(
            "python".into(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libc.so.6".into()],
                provides: vec![
                    "libpython3.14.so".into(),
                    "libpython3.15.so".into(),
                    "libpython3.15.so.1".into(),
                ],
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(python_lpkg),
            },
        );
        outcomes.insert(
            "python-cairo".into(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libcairo.so.2".into(), "libc.so.6".into()],
                provides: vec!["libpycairo.so".into()],
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(pc),
            },
        );
        outcomes.insert(
            "python-gobject".into(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libgobject-2.0.so.0".into(), "libc.so.6".into()],
                provides: vec!["libpygobject.so".into()],
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(pg),
            },
        );
        outcomes.insert(
            "blueman".into(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libc.so.6".into()],
                provides: vec![],
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(bl),
            },
        );
        let mut binding = StubBinding::new(outcomes);
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec!["python".into()],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
            build_data_dir: gdir.clone(),
            validate: false,
            manual_sort: false,
        };
        let report = run_build(&opts, &mut binding, None).unwrap();
        assert!(report.abi_broken.contains(&"python".to_string()));
        assert!(
            report.built.contains(&"python-cairo".to_string()),
            "不链 libpython 的 python 生态包应被声明式组重建: {:?}",
            report.built
        );
        assert!(report.built.contains(&"python-gobject".to_string()));
        assert!(report.built.contains(&"blueman".to_string()));
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
        fs::remove_dir_all(&gdir).ok();
    }

    #[test]
    fn version_change_group_rebuilds_on_minor_bump() {
        // perl 纯解释器（无 libperl.so，只有 libperl.a）→ SONAME 断裂信号不存在，abichange 组永不触发。
        // 改用 version-change 组：on 包（perl）minor 变化（5.44→5.45）→ 脚本判定 exit 0 → 重建 perl-* 组。
        // （曾有的 script_interpreter 回退会"任何重建都触发"，patch 升级也拖垮全组，已删）
        let dir = temp_dir("farm-group-perl-vc");
        let out = temp_dir("farm-group-perl-vc-out");
        let gdir = temp_dir("farm-group-perl-vc-data");
        fs::create_dir_all(&gdir).unwrap();
        fs::write(
            gdir.join("perl.yaml"),
            r#"rebuild-on-version-change: perl
version-change-script: |
  #!/bin/bash
  [ "$(printf '%s' "$OLD_VER" | cut -d. -f1-2)" != "$(printf '%s' "$NEW_VER" | cut -d. -f1-2)" ]
packages: perl-*
"#,
        )
        .unwrap();
        // perl 提供空（无 SONAME）；perl-xml-parser 只链 expat，不链 libperl
        write_baseline(
            &out,
            "perl|5.44:h:::libc.so.6|\n\
             perl-xml-parser|2.47:h:::libc.so.6,libexpat.so.1|\n",
        );
        // perl 配方版本 5.45（minor 5.44→5.45 变化；write_pkg 写死 1.0，用 write_pkg_ver 指定）
        write_pkg_ver(&dir, "perl", "5.45", &[], &["libc.so.6"], &[]);
        write_pkg(
            &dir,
            "perl-xml-parser",
            &[],
            &["libc.so.6", "libexpat.so.1"],
            &[],
        );

        let perl_lpkg = stage_lpkg(&out, "perl", "5.45", &["libc.so.6"], &[]);
        let xp_lpkg = stage_lpkg(
            &out,
            "perl-xml-parser",
            "1.0",
            &["libc.so.6", "libexpat.so.1"],
            &[],
        );
        let mut outcomes = HashMap::new();
        outcomes.insert(
            "perl".into(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libc.so.6".into()],
                provides: vec![],
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(perl_lpkg),
            },
        );
        outcomes.insert(
            "perl-xml-parser".into(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libc.so.6".into(), "libexpat.so.1".into()],
                provides: vec![],
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(xp_lpkg),
            },
        );
        let mut binding = StubBinding::new(outcomes);
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec!["perl".into()],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
            build_data_dir: gdir.clone(),
            validate: false,
            manual_sort: false,
        };
        let report = run_build(&opts, &mut binding, None).unwrap();
        assert!(
            report.abi_broken.is_empty(),
            "perl 无 SONAME，不应报 ABI 断裂: {:?}",
            report.abi_broken
        );
        assert!(
            report.built.contains(&"perl".to_string()),
            "perl 应被构建: {:?}",
            report.built
        );
        assert!(
            report.built.contains(&"perl-xml-parser".to_string()),
            "perl minor 变化 → version-change 组应重建 perl-xml-parser: {:?}",
            report.built
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
        fs::remove_dir_all(&gdir).ok();
    }

    #[test]
    fn version_change_group_skips_on_patch_bump() {
        // patch（5.44.0→5.44.1）minor 未变 → 脚本 exit 1 → perl-* 组**不**重建。
        // 旧 script_interpreter 回退会对 patch 升级也重建全组——正是被删掉的不合理行为。
        let dir = temp_dir("farm-group-perl-vcpatch");
        let out = temp_dir("farm-group-perl-vcpatch-out");
        let gdir = temp_dir("farm-group-perl-vcpatch-data");
        fs::create_dir_all(&gdir).unwrap();
        fs::write(
            gdir.join("perl.yaml"),
            r#"rebuild-on-version-change: perl
version-change-script: |
  #!/bin/bash
  [ "$(printf '%s' "$OLD_VER" | cut -d. -f1-2)" != "$(printf '%s' "$NEW_VER" | cut -d. -f1-2)" ]
packages: perl-*
"#,
        )
        .unwrap();
        write_baseline(
            &out,
            "perl|5.44.0:h:::libc.so.6|\n\
             perl-xml-parser|2.47:h:::libc.so.6,libexpat.so.1|\n",
        );
        write_pkg_ver(&dir, "perl", "5.44.1", &[], &["libc.so.6"], &[]);
        write_pkg(
            &dir,
            "perl-xml-parser",
            &[],
            &["libc.so.6", "libexpat.so.1"],
            &[],
        );

        let perl_lpkg = stage_lpkg(&out, "perl", "5.44.1", &["libc.so.6"], &[]);
        let xp_lpkg = stage_lpkg(
            &out,
            "perl-xml-parser",
            "1.0",
            &["libc.so.6", "libexpat.so.1"],
            &[],
        );
        let mut outcomes = HashMap::new();
        outcomes.insert(
            "perl".into(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libc.so.6".into()],
                provides: vec![],
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(perl_lpkg),
            },
        );
        outcomes.insert(
            "perl-xml-parser".into(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libc.so.6".into(), "libexpat.so.1".into()],
                provides: vec![],
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(xp_lpkg),
            },
        );
        let mut binding = StubBinding::new(outcomes);
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec!["perl".into()],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
            build_data_dir: gdir.clone(),
            validate: false,
            manual_sort: false,
        };
        let report = run_build(&opts, &mut binding, None).unwrap();
        assert!(report.built.contains(&"perl".to_string()));
        assert!(
            !report.built.contains(&"perl-xml-parser".to_string()),
            "perl patch 升级不应触发 perl-* 重建: {:?}",
            report.built
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
        fs::remove_dir_all(&gdir).ok();
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
            build_data_dir: std::path::PathBuf::from("data/build"),
            validate: false,
            manual_sort: false,
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
            &out,
            "a",
            "1.0",
            &["libc.so.6"],
            &["libfoo.so", "libfoo.so.2"],
        );
        let b_lpkg = stage_lpkg(
            &out,
            "b",
            "1.0+1",
            &["libfoo.so.2", "libc.so.6"],
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
            build_data_dir: std::path::PathBuf::from("data/build"),
            validate: false,
            manual_sort: false,
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
    fn abifix_targets_flags_orphan_soname_only() {
        // abifix 检测：c1 的 needed_so 引用 libfoo.so.2（index 无 provider）→ 命中；
        // c2 引用 libfoo.so.1（index 有）→ 不命中；selfy 引用自身 provides → 不命中（scan 语义）。
        let pkgs = temp_dir("farm-abifix-pkgs");
        let out = temp_dir("farm-abifix-out");
        write_baseline(&out, "libfoo|1.0:h::libfoo.so,libfoo.so.1:libc.so.6|\n");
        write_pkg_ver(&pkgs, "c1", "1.0", &["libc1.so"], &["libfoo.so.2"], &[]);
        write_pkg_ver(&pkgs, "c2", "1.0", &["libc2.so"], &["libfoo.so.1"], &[]);
        write_pkg_ver(
            &pkgs,
            "selfy",
            "1.0",
            &["libself.so.1"],
            &["libself.so.1"],
            &[],
        );
        let idx = Index::parse(&fs::read_to_string(out.join("x86_64/index.txt")).unwrap());

        let t = abifix_targets(&pkgs, &idx);
        assert_eq!(
            t,
            vec![("c1".to_string(), vec!["libfoo.so.2".to_string()])],
            "只有引用无 provider SONAME 的包应命中，自提供不算孤儿: {t:?}"
        );
        fs::remove_dir_all(&pkgs).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn abifix_targets_index_is_authoritative() {
        // provider 判定以**仓库 index**为准（非 pkgs/ 配方）：libfoo 配方已提供 libfoo.so.2
        // 但 index 只有 libfoo.so.1 → c1 引用 .2 仍算孤儿（重建容器拉不到 .2）。
        let pkgs = temp_dir("farm-abifix-auth");
        let out = temp_dir("farm-abifix-auth-out");
        write_baseline(&out, "libfoo|1.0:h::libfoo.so,libfoo.so.1:libc.so.6|\n");
        write_pkg_ver(
            &pkgs,
            "libfoo",
            "2.0",
            &["libfoo.so", "libfoo.so.2"],
            &[],
            &[],
        );
        write_pkg_ver(&pkgs, "c1", "1.0", &["libc1.so"], &["libfoo.so.2"], &[]);
        let idx = Index::parse(&fs::read_to_string(out.join("x86_64/index.txt")).unwrap());

        let t = abifix_targets(&pkgs, &idx);
        assert_eq!(t, vec![("c1".to_string(), vec!["libfoo.so.2".to_string()])]);
        fs::remove_dir_all(&pkgs).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn abifix_plan_bumps_only_orphan_packages() {
        // abifix_plan：检测 + bump release（只 bump 命中的孤儿包）+ 返回清单；无孤儿 → 空且不 bump。
        let pkgs = temp_dir("farm-abifix-plan");
        let out = temp_dir("farm-abifix-plan-out");
        write_baseline(&out, "libfoo|1.0:h::libfoo.so,libfoo.so.1:libc.so.6|\n");
        write_pkg_ver(
            &pkgs,
            "libfoo",
            "1.0",
            &["libfoo.so", "libfoo.so.1"],
            &[],
            &[],
        );
        write_pkg_ver(&pkgs, "c1", "1.0", &["libc1.so"], &["libfoo.so.2"], &[]);
        write_pkg_ver(&pkgs, "c2", "1.0", &["libc2.so"], &["libfoo.so.1"], &[]);

        let names = abifix_plan(&pkgs, &out, "x86_64").unwrap();
        assert_eq!(names, vec!["c1"], "只返回孤儿包: {names:?}");
        let c1 = read_lankebuild(&pkgs, "c1").unwrap();
        assert_eq!(c1.release, Some(1), "孤儿包应 bump release");
        let c2 = read_lankebuild(&pkgs, "c2").unwrap();
        assert_eq!(c2.release, None, "无孤儿包不应 bump");

        // 再次 plan（仓库仍一致 / 或无新孤儿——c1 needed_so 还没重扫所以仍命中）→ 幂等性留 run_build
        fs::remove_dir_all(&pkgs).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn backup_cleaned_when_soname_unreferenced() {
        // 整个 build 完成后清理 ABI 过渡备份：备份的旧 SONAME 已不再被任何包 needed_so 引用
        // → 删除（含空根目录）。本用例：无可构建包（队列空），cleanup 仍执行。
        let dir = temp_dir("farm-backup-clean");
        let out = temp_dir("farm-backup-clean-out");
        // libfoo 需 libc.so.6（index.txt 要有 needed_so，cleanup 的"剥离时代遗留"守卫才放行）
        write_baseline(&out, "libfoo|1.0:h::libfoo.so,libfoo.so.1:libc.so.6|\n");
        write_pkg(
            &dir,
            "libfoo",
            &["libfoo.so", "libfoo.so.1"],
            &["libc.so.6"],
            &[],
        );
        // 预置一个"已无引用"的备份（libz.so.1 不在 index.txt 任何 needed_so 里）
        fs::create_dir_all(out.join("backups")).unwrap();
        fs::write(out.join("backups/libz.so.1"), b"").unwrap();
        fs::write(out.join("backups/libz.so.1.2.3"), b"").unwrap();

        let mut binding = StubBinding::new(HashMap::new());
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec![],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
            build_data_dir: std::path::PathBuf::from("data/build"),
            validate: false,
            manual_sort: false,
        };
        let _ = run_build(&opts, &mut binding, None).unwrap();
        assert!(!out.join("backups").exists(), "无引用的备份目录应整体清理");
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn backup_kept_when_soname_still_referenced() {
        // 反向：仍有包的 needed_so 引用旧 SONAME（该包被跳过/BLOCKED 未重建）→ 备份保留，
        // 等下次 build 完成后再清。本用例 gettext 需要 libxml2.so.2 → libxml2 备份保留。
        let dir = temp_dir("farm-backup-keep");
        let out = temp_dir("farm-backup-keep-out");
        write_baseline(
            &out,
            "gettext|1.0:h::libgettext.so:libxml2.so.2,libc.so.6|\n",
        );
        write_pkg(
            &dir,
            "gettext",
            &["libgettext.so"],
            &["libxml2.so.2", "libc.so.6"],
            &[],
        );
        fs::create_dir_all(out.join("backups")).unwrap();
        fs::write(out.join("backups/libxml2.so.2"), b"").unwrap();
        fs::write(out.join("backups/libxml2.so.2.9.14"), b"").unwrap();

        let mut binding = StubBinding::new(HashMap::new());
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec![],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
            build_data_dir: std::path::PathBuf::from("data/build"),
            validate: false,
            manual_sort: false,
        };
        let _ = run_build(&opts, &mut binding, None).unwrap();
        assert!(
            out.join("backups/libxml2.so.2").exists(),
            "仍有引用的备份应保留（过渡未完成）"
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn backup_kept_when_referenced_soname_is_full_versioned() {
        // LLVM 类：SONAME 本身是完整版本化文件名（libLLVM.so.22.1，四段，而非 libLLVM.so.22）。
        // rust 的 needed_so 引用 libLLVM.so.22.1；LLVM 22.1→23.1 升级后旧 SONAME 被备份，但
        // rust 未重建仍引用它 → 备份必须保留到过渡完成。回归：soname_of 把 libLLVM.so.22.1
        // 截断为 libLLVM.so.22，与 referenced 里的完整 needed_so 不匹配 → 备份被误删，
        // 后续 rust 构建时容器里没有旧 libLLVM.so.22.1 可链接，构建必然失败。
        let dir = temp_dir("farm-backup-llvm");
        let out = temp_dir("farm-backup-llvm-out");
        write_pkg(
            &dir,
            "rust",
            &["librustc_driver.so"],
            &["ld-linux-x86-64.so.2", "libLLVM.so.22.1", "libc.so.6"],
            &[],
        );
        write_baseline(
            &out,
            "rust|1.0:h::librustc_driver.so:ld-linux-x86-64.so.2,libLLVM.so.22.1,libc.so.6|\n",
        );
        fs::create_dir_all(out.join("backups")).unwrap();
        fs::write(out.join("backups/libLLVM.so.22.1"), b"").unwrap();

        let mut binding = StubBinding::new(HashMap::new());
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec![],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
            build_data_dir: std::path::PathBuf::from("data/build"),
            validate: false,
            manual_sort: false,
        };
        let _ = run_build(&opts, &mut binding, None).unwrap();
        assert!(
            out.join("backups/libLLVM.so.22.1").exists(),
            "rust 仍引用 libLLVM.so.22.1，备份必须保留（过渡未完成）"
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn llvm_upgrade_keeps_backup_while_victim_unrebuilt() {
        // 综合真实状态端到端（复现用户报告）：LLVM 22.1→23.1 升级 + rust 等受害者未重建。
        //
        // 完整链路：
        //   1. 仓库里有旧 llvm 22.1.0.lpkg（真实内容：libLLVM.so.22.1 实体 + libLLVM.so /
        //      libLLVM-22.so 等 dev 符号链接；SONAME 是完整版本化文件名，四段）
        //   2. 旧 index.txt：llvm 22.1 提供 libLLVM.so.22.1；rust / spirv-llvm-translator
        //      needed_so 链 libLLVM.so.22.1
        //   3. 构建 llvm 23.1 → place_in_repo 备份被移除的旧 SONAME（libLLVM.so.22.1 等）→
        //      index 更新 llvm 行 → ABI 传播把 rust / spirv 入队为直连受害者
        //   4. 受害者构建被 BLOCKED（模拟"LLVM 之后、rust 之前结束构建"：rust 未重建，
        //      index 仍引用 libLLVM.so.22.1）→ cleanup_backups 必须保留 libLLVM.so.22.1
        //      备份（旧 .so 全靠它过渡，删了 rust 构建/运行断链不可恢复）
        //   5. 对照：无任何包引用的 libRemarks.so.22.1 备份仍正常清理（修复只增保守不误留）
        let dir = temp_dir("farm-llvm-full");
        let out = temp_dir("farm-llvm-full-out");

        const OLD_PROV: &[&str] = &[
            "LLVMgold.so",
            "libLLVM-22.so",
            "libLLVM.so",
            "libLLVM.so.22.1",
            "libLTO.so",
            "libLTO.so.22.1",
            "libRemarks.so",
            "libRemarks.so.22.1",
            "libclang-cpp.so",
            "libclang-cpp.so.22.1",
            "libclang.so",
            "libclang.so.22.1",
        ];
        const NEW_PROV: &[&str] = &[
            "LLVMgold.so",
            "libLLVM-23.so",
            "libLLVM.so",
            "libLLVM.so.23.1",
            "libLTO.so",
            "libLTO.so.23.1",
            "libRemarks.so",
            "libRemarks.so.23.1",
            "libclang-cpp.so",
            "libclang-cpp.so.23.1",
            "libclang.so",
            "libclang.so.23.1",
        ];
        const NEW_NEEDED: &[&str] = &[
            "ld-linux-x86-64.so.2",
            "libc.so.6",
            "libcurl.so.4",
            "libedit.so.0",
            "libffi.so.8",
            "libgcc_s.so.1",
            "libm.so.6",
            "libstdc++.so.6",
            "libxml2.so.16",
            "libz.so.1",
        ];
        const RUST_NEEDED: &[&str] = &[
            "ld-linux-x86-64.so.2",
            "libLLVM.so.22.1",
            "libc.so.6",
            "libcrypto.so.3",
            "libcurl.so.4",
            "libgcc_s.so.1",
            "libm.so.6",
            "libssl.so.3",
            "libstdc++.so.6",
            "libz.so.1",
        ];

        // 1. 旧 llvm 22.1 .lpkg 进仓库（真实内容：四段 SONAME 实体 + dev 符号链接）
        let old_src = dir.join("old-llvm-src");
        fs::create_dir_all(old_src.join("content/usr/lib")).unwrap();
        for f in [
            "libLLVM.so.22.1",
            "libLTO.so.22.1",
            "libRemarks.so.22.1",
            "libclang-cpp.so.22.1",
            "libclang.so.22.1",
        ] {
            fs::write(
                old_src.join("content/usr/lib").join(f),
                [0x7f, b'E', b'L', b'F', 2, 1, 1],
            )
            .unwrap();
        }
        for (link, target) in [
            ("libLLVM.so", "libLLVM.so.22.1"),
            ("libLLVM-22.so", "libLLVM.so.22.1"),
            ("libLTO.so", "libLTO.so.22.1"),
            ("libRemarks.so", "libRemarks.so.22.1"),
            ("libclang-cpp.so", "libclang-cpp.so.22.1"),
            ("libclang.so", "libclang.so.22.1"),
        ] {
            std::os::unix::fs::symlink(target, old_src.join("content/usr/lib").join(link)).unwrap();
        }
        let old_meta = serde_json::json!({
            "name": "llvm", "version": "22.1.0", "deps": [],
            "provides": OLD_PROV, "needed_so": [],
        });
        fs::write(
            old_src.join("metadata.json"),
            serde_json::to_string_pretty(&old_meta).unwrap(),
        )
        .unwrap();
        let repo_llvm = out.join("x86_64/llvm");
        fs::create_dir_all(&repo_llvm).unwrap();
        let old_lpkg = repo_llvm.join("22.1.0.lpkg");
        {
            let f = fs::File::create(&old_lpkg).unwrap();
            let enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
            let mut b = tar::Builder::new(enc);
            b.follow_symlinks(false);
            b.append_dir_all(".", &old_src).unwrap();
            let enc = b.into_inner().unwrap();
            enc.finish().unwrap();
        }

        // 2. 旧 index.txt：llvm 22.1 + rust / spirv-llvm-translator 链 libLLVM.so.22.1
        write_baseline(
            &out,
            &format!(
                "llvm|22.1.0:h::{}:{}|\nrust|1.97.1:h::librustc_driver-b18229fd4035e2d5.so:{}|\nspirv-llvm-translator|22.1.5:h::libLLVMSPIRVLib.so,libLLVMSPIRVLib.so.22.1:ld-linux-x86-64.so.2,libLLVM.so.22.1,libc.so.6,libgcc_s.so.1,libm.so.6,libstdc++.so.6|\n",
                OLD_PROV.join(","),
                NEW_NEEDED.join(","),
                RUST_NEEDED.join(","),
            ),
        );

        // 3. 配方：llvm 23.1（新 SONAME）、rust 1.98（旧 SONAME）、spirv（旧 SONAME）
        write_pkg_ver(&dir, "llvm", "23.1.0", NEW_PROV, NEW_NEEDED, &[]);
        write_pkg_ver(
            &dir,
            "rust",
            "1.98.0",
            &["librustc_driver-b18229fd4035e2d5.so"],
            RUST_NEEDED,
            &[],
        );
        write_pkg_ver(
            &dir,
            "spirv-llvm-translator",
            "22.1.5",
            &["libLLVMSPIRVLib.so", "libLLVMSPIRVLib.so.22.1"],
            &["libLLVM.so.22.1"],
            &[],
        );

        // 4. Stub：llvm 构建成功（新 SONAME）；rust / spirv 构建失败 → BLOCKED（未重建）
        let new_lpkg = stage_lpkg(&out, "llvm", "23.1.0", NEW_NEEDED, NEW_PROV);
        let mut outcomes = HashMap::new();
        outcomes.insert(
            "llvm".into(),
            BuildOutcome {
                ok: true,
                needed_so: NEW_NEEDED.iter().map(|s| s.to_string()).collect(),
                provides: NEW_PROV.iter().map(|s| s.to_string()).collect(),
                deps: vec![],
                failure_stage: None,
                lpkg_path: Some(new_lpkg),
            },
        );
        outcomes.insert("rust".into(), BuildOutcome::failure("build"));
        outcomes.insert(
            "spirv-llvm-translator".into(),
            BuildOutcome::failure("build"),
        );
        let mut binding = StubBinding::new(outcomes);
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec!["llvm".into()],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
            build_data_dir: std::path::PathBuf::from("data/build"),
            validate: false,
            manual_sort: false,
        };
        let report = run_build(&opts, &mut binding, None).unwrap();
        assert!(report.abi_broken.contains(&"llvm".to_string()));
        assert!(
            report.blocked.contains(&"rust".to_string()),
            "rust 是 libLLVM.so.22.1 直连受害者且未重建"
        );
        assert!(report
            .blocked
            .contains(&"spirv-llvm-translator".to_string()));
        // llvm 已进仓库并更新 index（新 SONAME）；rust/spirv 行保持旧 needed_so
        assert!(
            out.join("x86_64/llvm/23.1.0.lpkg").exists(),
            "llvm 新版本应进仓库"
        );
        let idx = fs::read_to_string(out.join("x86_64/index.txt")).unwrap();
        assert!(
            idx.contains("libLLVM.so.23.1"),
            "llvm 行应更新为新 SONAME: {idx}"
        );
        assert!(
            idx.contains("libLLVM.so.22.1"),
            "rust/spirv 未重建，index 仍引用旧 SONAME: {idx}"
        );

        // 5. cleanup 后：仍被 rust/spirv 引用的 libLLVM.so.22.1 备份必须保留
        assert!(
            out.join("backups/libLLVM.so.22.1").exists(),
            "rust/spirv 仍引用 libLLVM.so.22.1，旧 .so 备份必须保留（过渡未完成）"
        );
        // 对照：无任何包引用的 libRemarks.so.22.1 备份仍正常清理（修复不误留）
        assert!(
            !out.join("backups/libRemarks.so.22.1").exists(),
            "libRemarks.so.22.1 无引用应被清理"
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn backup_keeps_physical_when_referenced_symlink_name_mismatches() {
        // display-info 类：soversion=3 + version=0.3.0 ⇒ SONAME 符号链接 libdisplay-info.so.3
        // 指向实体 libdisplay-info.so.0.3.0。实体自身文件名派生的 SONAME（libdisplay-info.so.0）
        // 未被引用，但指向它的符号链接 libdisplay-info.so.3 仍被某包 needed_so 引用。
        // 回归：实体不得被删，否则仍被使用的 SONAME 链接 dangling。
        let dir = temp_dir("farm-backup-name-mismatch");
        let out = temp_dir("farm-backup-name-mismatch-out");
        write_pkg(
            &dir,
            "wlroots",
            &["libwlroots.so"],
            &["libdisplay-info.so.3", "libc.so.6"],
            &[],
        );
        write_baseline(
            &out,
            "wlroots|1.0:h::libwlroots.so:libdisplay-info.so.3,libc.so.6|\n",
        );
        fs::create_dir_all(out.join("backups")).unwrap();
        fs::write(out.join("backups/libdisplay-info.so.0.3.0"), b"ELF").unwrap();
        std::os::unix::fs::symlink(
            "libdisplay-info.so.0.3.0",
            out.join("backups/libdisplay-info.so.3"),
        )
        .unwrap();

        let mut binding = StubBinding::new(HashMap::new());
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec![],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
            build_data_dir: std::path::PathBuf::from("data/build"),
            validate: false,
            manual_sort: false,
        };
        let _ = run_build(&opts, &mut binding, None).unwrap();

        assert!(
            out.join("backups/libdisplay-info.so.0.3.0").is_file(),
            "仍被引用的符号链接 libdisplay-info.so.3 指向的实体不得删除"
        );
        assert!(
            fs::symlink_metadata(out.join("backups/libdisplay-info.so.3"))
                .unwrap()
                .file_type()
                .is_symlink(),
            "仍被引用的 SONAME 符号链接应保留"
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn backup_cleans_unreferenced_mismatch_symlink_and_target_together() {
        // 同名不匹配场景的反向：libdisplay-info.so.3 与其实体均未被任何包 needed_so 引用
        // → 符号链接与实体一起清理（保护集仅含「被引用的符号链接」目标）。
        let dir = temp_dir("farm-backup-name-mismatch-clean");
        let out = temp_dir("farm-backup-name-mismatch-clean-out");
        write_pkg(&dir, "wlroots", &["libwlroots.so"], &["libc.so.6"], &[]);
        write_baseline(&out, "wlroots|1.0:h::libwlroots.so:libc.so.6|\n");
        fs::create_dir_all(out.join("backups")).unwrap();
        fs::write(out.join("backups/libdisplay-info.so.0.3.0"), b"ELF").unwrap();
        std::os::unix::fs::symlink(
            "libdisplay-info.so.0.3.0",
            out.join("backups/libdisplay-info.so.3"),
        )
        .unwrap();

        let mut binding = StubBinding::new(HashMap::new());
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec![],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
            build_data_dir: std::path::PathBuf::from("data/build"),
            validate: false,
            manual_sort: false,
        };
        let _ = run_build(&opts, &mut binding, None).unwrap();

        assert!(
            !out.join("backups/libdisplay-info.so.0.3.0").exists(),
            "无引用的实体应被清理"
        );
        assert!(
            !out.join("backups/libdisplay-info.so.3").exists(),
            "无引用的符号链接应一并清理"
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn backup_keeps_subdir_physical_when_referenced_symlink_points_into_it() {
        // 目标实体在备份树子目录内（expect 类布局）：libfoo.so.3 → sub/libfoo.so.0.3.0。
        // 子目录内实体自身 SONAME（libfoo.so.0）未被引用，但顶层符号链接仍被引用 → 实体保留。
        let dir = temp_dir("farm-backup-subdir-target");
        let out = temp_dir("farm-backup-subdir-target-out");
        write_pkg(
            &dir,
            "wlroots",
            &["libwlroots.so"],
            &["libfoo.so.3", "libc.so.6"],
            &[],
        );
        write_baseline(
            &out,
            "wlroots|1.0:h::libwlroots.so:libfoo.so.3,libc.so.6|\n",
        );
        fs::create_dir_all(out.join("backups/sub")).unwrap();
        fs::write(out.join("backups/sub/libfoo.so.0.3.0"), b"ELF").unwrap();
        std::os::unix::fs::symlink("sub/libfoo.so.0.3.0", out.join("backups/libfoo.so.3")).unwrap();

        let mut binding = StubBinding::new(HashMap::new());
        let opts = BuildOptions {
            pkgs_dir: dir.clone(),
            out_dir: out.clone(),
            targets: vec![],
            arch: "x86_64".into(),
            image: String::new(),
            download_retries: 3,
            interactive: false,
            build_data_dir: std::path::PathBuf::from("data/build"),
            validate: false,
            manual_sort: false,
        };
        let _ = run_build(&opts, &mut binding, None).unwrap();

        assert!(
            out.join("backups/sub/libfoo.so.0.3.0").is_file(),
            "子目录内被仍引用符号链接指向的实体不得删除"
        );
        assert!(
            fs::symlink_metadata(out.join("backups/libfoo.so.3"))
                .unwrap()
                .file_type()
                .is_symlink(),
            "顶层仍被引用的符号链接应保留"
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn backup_keeps_runtime_soname_and_patch_but_not_dev_symlink() {
        // 旧 libfoo 1.0：dev symlink libfoo.so + SONAME symlink libfoo.so.1 + 实体 libfoo.so.1.2.3。
        // 新 libfoo 2.0：连 dev symlink 一起移除（provides 只剩 libfoo.so.2）→ removed = {libfoo.so, libfoo.so.1}。
        // 应只备份运行时 SONAME 文件（libfoo.so.1 解引用内容 + 实体 libfoo.so.1.2.3），dev symlink libfoo.so 绝不备份。
        let dir = temp_dir("farm-backup-files");
        let out = temp_dir("farm-backup-files-out");
        fs::create_dir_all(dir.join("content/usr/lib")).unwrap();
        fs::write(
            dir.join("content/usr/lib/libfoo.so.1.2.3"),
            [0x7f, b'E', b'L', b'F', 2, 1, 1],
        )
        .unwrap();
        std::os::unix::fs::symlink("libfoo.so.1.2.3", dir.join("content/usr/lib/libfoo.so.1"))
            .unwrap();
        std::os::unix::fs::symlink("libfoo.so.1", dir.join("content/usr/lib/libfoo.so")).unwrap();
        let meta = serde_json::json!({
            "name": "libfoo",
            "version": "1.0",
            "deps": [],
            "provides": ["libfoo.so", "libfoo.so.1"],
            "needed_so": [],
        });
        fs::write(
            dir.join("metadata.json"),
            serde_json::to_string_pretty(&meta).unwrap(),
        )
        .unwrap();
        let lpkg = out.join("old-libfoo.lpkg");
        {
            let f = fs::File::create(&lpkg).unwrap();
            let enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
            let mut b = tar::Builder::new(enc);
            // 必须 follow_symlinks(false)——默认 follow=true 会把 content 里的符号链接
            // 解引用成普通文件副本，与真实 .lpkg（lpkg packer 用 libarchive 保留 symlink）不符
            b.follow_symlinks(false);
            b.append_dir_all(".", &dir).unwrap();
            let enc = b.into_inner().unwrap();
            enc.finish().unwrap();
        }

        super::repo::backup_removed_sonames(
            &out,
            &lpkg,
            "libfoo",
            &["libfoo.so.2".to_string()], // 新版本：连 dev symlink 都不提供了
        )
        .unwrap();

        assert!(
            !out.join("backups/libfoo.so").exists(),
            "dev symlink (libfoo.so) 即使被移除也不应备份"
        );
        assert!(
            fs::symlink_metadata(out.join("backups/libfoo.so.1"))
                .unwrap()
                .file_type()
                .is_symlink(),
            "SONAME libfoo.so.1 应保留为符号链接（ldconfig 要求版本化 SONAME 是符号链接）"
        );
        assert!(
            fs::exists(out.join("backups/libfoo.so.1")).unwrap_or(false),
            "SONAME 符号链接的目标 libfoo.so.1.2.3 应一并备份（不 dangling）"
        );
        assert!(
            out.join("backups/libfoo.so.1.2.3").is_file(),
            "patch 实体 libfoo.so.1.2.3 应被备份（SONAME 符号链接的落点）"
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn backup_preserves_symlinks_and_replicates_subdir_targets() {
        // 模块库布局（expect 类）：SONAME 是符号链接指向子目录内的实体。
        //   libexpect5.45.4.so → expect5.45.4/libexpect5.45.4.so  （无 SONAME 实体库，未版本化）
        //   libexpect.so.5     → expect5.45.4/libexpect.so.5      （版本化 SONAME 指向子目录）
        // ABI 断裂时：符号链接保留本身（ldconfig 需要版本化 SONAME 是符号链接），并**复刻目录树**
        // 备份目标实体——out/backups/libexpect5.45.4.so (symlink) +
        // out/backups/expect5.45.4/libexpect5.45.4.so (实体)。绝不 dangling。
        let dir = temp_dir("farm-backup-subdir");
        let out = temp_dir("farm-backup-subdir-out");
        fs::create_dir_all(dir.join("content/usr/lib/expect5.45.4")).unwrap();
        let real_v45 = dir.join("content/usr/lib/expect5.45.4/libexpect5.45.4.so");
        let real_v5 = dir.join("content/usr/lib/expect5.45.4/libexpect.so.5");
        fs::write(&real_v45, [0x7f, b'E', b'L', b'F', 2, 1, 1]).unwrap();
        fs::write(&real_v5, [0x7f, b'E', b'L', b'F', 2, 1, 1, 2]).unwrap();
        std::os::unix::fs::symlink(
            "expect5.45.4/libexpect5.45.4.so",
            dir.join("content/usr/lib/libexpect5.45.4.so"),
        )
        .unwrap();
        std::os::unix::fs::symlink(
            "expect5.45.4/libexpect.so.5",
            dir.join("content/usr/lib/libexpect.so.5"),
        )
        .unwrap();
        let meta = serde_json::json!({
            "name": "expect",
            "version": "5.45.4",
            "deps": [],
            "provides": ["libexpect5.45.4.so", "libexpect.so.5"],
            "needed_so": [],
        });
        fs::write(
            dir.join("metadata.json"),
            serde_json::to_string_pretty(&meta).unwrap(),
        )
        .unwrap();
        let lpkg = out.join("old-expect.lpkg");
        {
            let f = fs::File::create(&lpkg).unwrap();
            let enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
            let mut b = tar::Builder::new(enc);
            b.follow_symlinks(false);
            b.append_dir_all(".", &dir).unwrap();
            let enc = b.into_inner().unwrap();
            enc.finish().unwrap();
        }

        super::repo::backup_removed_sonames(
            &out,
            &lpkg,
            "expect",
            &["libexpect6.0.so".to_string()],
        )
        .unwrap();

        // 符号链接保留本身 + 目标实体复刻进子目录 → 不 dangling
        for (name, real) in [
            ("libexpect5.45.4.so", &real_v45),
            ("libexpect.so.5", &real_v5),
        ] {
            let link = out.join("backups").join(name);
            assert!(
                fs::symlink_metadata(&link)
                    .unwrap()
                    .file_type()
                    .is_symlink(),
                "{name} 应保留为符号链接（ldconfig 要求）"
            );
            assert_eq!(
                fs::read_link(&link).unwrap(),
                std::path::PathBuf::from(format!("expect5.45.4/{name}")),
                "{name} 符号链接目标应原样保留"
            );
            assert!(
                fs::exists(&link).unwrap_or(false),
                "{name} 的目标实体应复刻进备份 → 不 dangling"
            );
            let target = out.join("backups/expect5.45.4").join(name);
            assert!(target.is_file(), "子目录实体 {target:?} 应被备份");
            assert_eq!(fs::read(&target).unwrap(), fs::read(real).unwrap());
        }
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn backup_converts_absolute_targets_to_relative() {
        // 绝对目标容错：libfoo.so.1 -> /usr/lib/expect5.45.4/libexpect5.45.4.so。
        // 应在 archive 里定位目标（content/ → /），符号链接转为相对（相对备份树根 /usr/lib），
        // 目标实体复刻到对应位置 → 注入后不 dangling。
        let dir = temp_dir("farm-backup-abs");
        let out = temp_dir("farm-backup-abs-out");
        fs::create_dir_all(dir.join("content/usr/lib/expect5.45.4")).unwrap();
        let real = dir.join("content/usr/lib/expect5.45.4/libexpect5.45.4.so");
        fs::write(&real, [0x7f, b'E', b'L', b'F', 2, 1, 1]).unwrap();
        std::os::unix::fs::symlink(
            "/usr/lib/expect5.45.4/libexpect5.45.4.so",
            dir.join("content/usr/lib/libfoo.so.1"),
        )
        .unwrap();
        let meta = serde_json::json!({
            "name": "foo",
            "version": "1.0",
            "deps": [],
            "provides": ["libfoo.so.1"],
            "needed_so": [],
        });
        fs::write(
            dir.join("metadata.json"),
            serde_json::to_string_pretty(&meta).unwrap(),
        )
        .unwrap();
        let lpkg = out.join("old-foo.lpkg");
        {
            let f = fs::File::create(&lpkg).unwrap();
            let enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
            let mut b = tar::Builder::new(enc);
            b.follow_symlinks(false);
            b.append_dir_all(".", &dir).unwrap();
            let enc = b.into_inner().unwrap();
            enc.finish().unwrap();
        }

        super::repo::backup_removed_sonames(&out, &lpkg, "foo", &["libfoo.so.2".to_string()])
            .unwrap();

        let link = out.join("backups/libfoo.so.1");
        assert!(fs::symlink_metadata(&link)
            .unwrap()
            .file_type()
            .is_symlink());
        assert_eq!(
            fs::read_link(&link).unwrap(),
            std::path::PathBuf::from("expect5.45.4/libexpect5.45.4.so"),
            "绝对目标应转为相对路径"
        );
        assert!(
            fs::exists(&link).unwrap_or(false),
            "目标应复刻 → 不 dangling"
        );
        let target = out.join("backups/expect5.45.4/libexpect5.45.4.so");
        assert!(target.is_file());
        assert_eq!(fs::read(&target).unwrap(), fs::read(&real).unwrap());
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn cleanup_prunes_replicated_subdir_targets() {
        // 复刻树（expect 类）在 SONAME 无引用时：符号链接 + 子目录实体 + 子目录本身都应收割。
        let out = temp_dir("farm-backup-clean-subdir-out");
        // index 只引用 libgettext.so / libc.so.6 → libexpect5.45.4.so 无引用
        write_baseline(&out, "gettext|1.0:h::libgettext.so:libc.so.6|\n");
        fs::create_dir_all(out.join("backups/expect5.45.4")).unwrap();
        std::os::unix::fs::symlink(
            "expect5.45.4/libexpect5.45.4.so",
            out.join("backups/libexpect5.45.4.so"),
        )
        .unwrap();
        fs::write(out.join("backups/expect5.45.4/libexpect5.45.4.so"), b"x").unwrap();

        super::repo::cleanup_backups(&out, "x86_64");

        assert!(
            !out.join("backups").exists(),
            "无引用的复刻树（含子目录）应整体清理"
        );
        fs::remove_dir_all(&out).ok();
    }

    #[test]
    fn backup_includes_unversioned_nosoname_regular_lib() {
        // tcl 类：libtcl8.6.so 是无 SONAME 的实体库（普通文件，文件名即身份，非符号链接）。
        // tcl 8.6 → 9.0 时 removed = {libtcl8.6.so} → 必须直接备份，否则旧二进制过渡期无 .so 可加载。
        let dir = temp_dir("farm-backup-nosoname");
        let out = temp_dir("farm-backup-nosoname-out");
        fs::create_dir_all(dir.join("content/usr/lib")).unwrap();
        fs::write(
            dir.join("content/usr/lib/libtcl8.6.so"),
            [0x7f, b'E', b'L', b'F', 2, 1, 1],
        )
        .unwrap();
        let meta = serde_json::json!({
            "name": "tcl",
            "version": "8.6.16",
            "deps": [],
            "provides": ["libtcl8.6.so"],
            "needed_so": [],
        });
        fs::write(
            dir.join("metadata.json"),
            serde_json::to_string_pretty(&meta).unwrap(),
        )
        .unwrap();
        let lpkg = out.join("old-tcl.lpkg");
        {
            let f = fs::File::create(&lpkg).unwrap();
            let enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
            let mut b = tar::Builder::new(enc);
            b.follow_symlinks(false);
            b.append_dir_all(".", &dir).unwrap();
            let enc = b.into_inner().unwrap();
            enc.finish().unwrap();
        }

        super::repo::backup_removed_sonames(&out, &lpkg, "tcl", &["libtcl9.0.so".to_string()])
            .unwrap();

        let bak = out.join("backups/libtcl8.6.so");
        assert!(
            bak.is_file(),
            "无 SONAME 的实体库 libtcl8.6.so 应被直接备份（文件）"
        );
        fs::remove_dir_all(&dir).ok();
        fs::remove_dir_all(&out).ok();
    }
}
