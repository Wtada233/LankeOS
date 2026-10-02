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

use crate::error::FarmError;
use std::collections::{BTreeMap, HashSet, VecDeque};
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
    read_index, recipe_hash, repack_if_drift, sha256_file, sorted_pkg_names,
    update_lankebuild_metadata, update_repo_index,
};
mod farm_flags;
// `FarmFlag` 也导出：cli 的测试要断言 "由 label 派生的 KIND 是 farm_flags 认识的 flag"
// （测试代码会被编两遍——lib 与 bin——所以只能用 `lankefarm::` 前缀，不能走 `pub(crate)`）
pub use farm_flags::{string_list, FarmFlag};
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
    /// 元数据漂移并已双写 LankeBUILD.json 的包。**重打现在无条件**（每次成功构建都 level22+mtime0
    /// 归一化进 repo），本字段只记录"漂移修正"那一档，不代表"本轮是否重打包"。
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
    /// 格式见 `build/farm_flags.rs`：成员可为字符串 flag，或对象 `{NAME:[…]}` 表达字符串列表 flag。
    #[serde(default)]
    pub farm_flags: Vec<serde_json::Value>,
    #[serde(default)]
    pub sources: Vec<String>,
    #[serde(default)]
    pub work_sources: Vec<String>,
}

pub fn read_lankebuild(pkgs_dir: &Path, pkg: &str) -> Option<LankeBuild> {
    let path = recipe_json_path(pkgs_dir, pkg);
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
fn source_gate(pkg: &str, opts: &BuildOptions, is_victim: bool) -> Result<(), FarmError> {
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
            Err(e) => return Err(tr!("build.source_missing_fatal", pkg, e).into()),
        }
    }
}

/// `pkgs/<pkg>/LankeBUILD.json` —— `(pkgs_dir, pkg)` 这一形态的**唯一拼法**（历史上散在手拼）。
/// 遍历目录/已持有目录路径的场景（`dir.join("LankeBUILD.json")`）不受此约束。
pub fn recipe_json_path(pkgs_dir: &Path, pkg: &str) -> std::path::PathBuf {
    pkgs_dir.join(pkg).join("LankeBUILD.json")
}

/// `pkgs/<pkg>/.build_ok`（validate 标记路径）。
fn build_ok_path(pkgs_dir: &Path, pkg: &str) -> std::path::PathBuf {
    pkgs_dir.join(pkg).join(".build_ok")
}

/// validate 标记：`.build_ok` **存在且内容 == 当前 LankeBUILD+LankeBUILD.json 的 sha256**
/// （recipe_hash）才算"已成功构建且配方未变"。配方变了 → 标记失效 → validate 会重建。
pub(crate) fn has_build_ok(pkgs_dir: &Path, pkg: &str) -> bool {
    let Some(expected) = recipe_hash(pkgs_dir, pkg) else {
        return false;
    };
    let Ok(stored) = std::fs::read_to_string(build_ok_path(pkgs_dir, pkg)) else {
        return false;
    };
    stored.trim() == expected
}

/// validate 标记：成功构建后写 `.build_ok`（内容 = 当前 recipe_hash；跳过/blocked 不写）。
pub(crate) fn mark_build_ok(pkgs_dir: &Path, pkg: &str) -> std::io::Result<()> {
    let h = recipe_hash(pkgs_dir, pkg).unwrap_or_default();
    std::fs::write(build_ok_path(pkgs_dir, pkg), h.as_bytes())
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
pub fn abifix_plan(pkgs_dir: &Path, out_dir: &Path, arch: &str) -> Result<Vec<String>, FarmError> {
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

/// 刷新 binding 的 repo_provides = **当前** index.txt 的全部提供能力。index.txt 是单一真源、
/// 随每包 `update_repo_index` 递增更新——同一次 run 里后构建的包必须看到先构建包刚加入 index 的
/// SONAME，否则真实 DT_NEEDED 被扫描 not-found 过滤丢弃（libvips 丢 libmatio.so.14：两者同轮构建，
/// libmatio 先建好进 index，但 repo_provides 还是 run 起点的旧快照，libvips 扫描时看不到它。
/// 之后单独重建正常 = provider 已进基线）。读失败保守保留旧集。
fn refresh_repo_provides(binding: &mut dyn LpkgBinding, out_dir: &Path, arch: &str) {
    let Some(idx) = repo::read_index(out_dir, arch) else {
        return;
    };
    binding.set_repo_provides(idx.all_provided_capabilities());
}

/// 单包事务失败归类到的构建阶段（state.failure_stage 的稳定 token；operator/读端据此排查）。
/// 单包事务在进 repo 前各阶段失败的稳定 token（state.failure_stage 用；operator/读端据此排查）。
/// build 本身失败 / source 门失败走更细的交互路径（诊断串 = outcome.failure_stage / 直接 Err），
/// 不经此归类——只留真正需要稳定 token 的三个收尾阶段。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum BlockStage {
    Repack,
    Repo,
    Index,
}

impl BlockStage {
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            BlockStage::Repack => "repack",
            BlockStage::Repo => "repo",
            BlockStage::Index => "index",
        }
    }
}

/// 把「仓库缺失」的 build_deps 递归并入本轮构建集（fixpoint，确定性）。
///
/// 判据与 `topo_order` 的 build_deps 边同源：`!old.packages.contains_key(d)` = 该依赖**从未进过仓库**
/// （首建/引导，如 gjs 依赖同轮首建的 sysprof、samba 依赖新加的 talloc/tevent）——依赖方容器
/// `lpkg upgrade` 从本地 repo 装不到它，不并入本轮先建必然 BLOCKED。已在仓库的依赖**不**并入
/// （每个容器各自 `lpkg upgrade` 自取最新版，无需排队）。配方不存在的名字不并入（不是本仓库的包，
/// 留给构建期报错）。`--manual-sort`（严格手工顺序/引导链）不并入，由 operator 点名。
fn expand_missing_build_deps(
    pkgs_dir: &Path,
    initial: Vec<String>,
    old: &Index,
    manual_sort: bool,
) -> Vec<String> {
    if manual_sort {
        return initial;
    }
    let mut out: Vec<String> = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();
    for p in initial {
        if seen.insert(p.clone()) {
            out.push(p);
        }
    }
    let mut i = 0;
    while i < out.len() {
        if let Some(lb) = read_lankebuild(pkgs_dir, &out[i]) {
            let mut added: Vec<String> = Vec::new();
            for d in &lb.build_deps {
                if seen.contains(d) || old.packages.contains_key(d.as_str()) {
                    continue;
                }
                if read_lankebuild(pkgs_dir, d).is_none() {
                    continue;
                }
                added.push(d.clone());
            }
            for d in added {
                if seen.insert(d.clone()) {
                    out.push(d);
                }
            }
        }
        i += 1;
    }
    out.sort();
    out
}

/// 构建计划（增量选择 → 缺失构建依赖并入 → version-change 受害者并入 → 确定性 topo 排序）→ 就绪队列。
///
/// 从 run_build 抽出（纯计算，可单测，不含交互/副作用）：返回 `(就绪队列, version-change
/// 预排受害者名集)`。`version_planned_names` 供弹包时决定是否 release bump；`all_pkgs`
/// 由调用方保留（loop 里 groups.victims_for 需要）。
fn build_plan(
    opts: &BuildOptions,
    old: &Index,
    groups: &RebuildGroups,
    all_pkgs: &[String],
) -> (VecDeque<(String, bool)>, HashSet<String>) {
    // 2. 增量选择（用户规则）：effective_version 与本地 repo 旧索引一致的包跳过构建。
    //    LankeBUILD.json 的 version 是 raw；有 release 字段拼 version+release（如 1.1+2）。
    //    validate 模式：选择改为"所有没有 `.build_ok` 标记的包"（成功构建才会写标记，
    //    跳过/blocked 不写 → 下次 validate 重试）。排序仍走同一 topo_order。
    let initial: Vec<String> = if opts.targets.is_empty() {
        let v: Vec<String> = all_pkgs
            .iter()
            .filter(|p| {
                if opts.validate {
                    !has_build_ok(&opts.pkgs_dir, p)
                } else {
                    needs_build(&opts.pkgs_dir, p, old)
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
    // ---- 仓库缺失的构建依赖：**并入本轮先建** ----
    // 点名 `build samba` 时，samba 的 build_deps 里从未进过仓库的包（talloc/tevent 这类首建依赖）
    // 若不同轮先建，依赖方容器 `lpkg upgrade` 装不到它 → 必然 BLOCKED。故递归并入初始集，
    // 再由 topo_order 的 build_deps 边排到依赖者之前。已在仓库的依赖不并入。
    let initial = expand_missing_build_deps(&opts.pkgs_dir, initial, old, opts.manual_sort);
    // 组边参与初始排序：声明式组受害者（python-* 等）排在触发包之后——
    // `--all` 时它们已在初始队列，没有 needed_so 链接边，须靠组边强制 python 先建（见下方 `edges`）。

    // ---- version-change 组受害者：开工前即定，提前算好并入初始队列 ----
    // 与 ABI breaking 不同：ABI 断裂不可预知（要等某包重建、SONAME 变了才知道受害者），只能运行时
    // 动态入队；version-change 在编排开始就确定——on 包旧索引版本 vs 当前配方版本一比较就知道会触发，
    // 脚本（含动态扫 Qt private importers）立刻能算出受害者。所以把它们**加进确认集**：进概览、release
    // bump、bulk 预下载，不再等 on 包建完才冒出（`--manual-sort` 严格手工顺序时不并入）。
    let mut planned_version: BTreeMap<String, Vec<String>> = BTreeMap::new();
    let mut version_planned_names: HashSet<String> = HashSet::new();
    if !(opts.manual_sort && !opts.targets.is_empty()) {
        for on in &initial {
            if !groups.is_version_change_on(on) {
                continue;
            }
            let Some(ov) = old.packages.get(on) else {
                continue;
            };
            let newv = effective_version(&opts.pkgs_dir, on).unwrap_or_default();
            if newv.is_empty() || newv == ov.version {
                continue; // 本轮 on 版本没变 → 组不触发
            }
            match groups.version_victims_ctx(
                on,
                &ov.version,
                &newv,
                all_pkgs,
                Some(groups::ScriptEnv {
                    pkgs_dir: opts.pkgs_dir.as_path(),
                    out_dir: opts.out_dir.as_path(),
                    arch: opts.arch.as_str(),
                }),
            ) {
                Ok(v) if !v.is_empty() => {
                    for x in &v {
                        version_planned_names.insert(x.clone());
                    }
                    planned_version.insert(on.clone(), v);
                }
                Ok(_) => {}
                Err(e) => eprintln!("{}", tr!("build.version_change_fail", on, e)),
            }
        }
    }
    let mut selection: Vec<String> = initial.clone();
    selection.extend(version_planned_names.iter().cloned());
    selection.sort();
    selection.dedup();
    // 组边（静态 glob；动态受害者并入选集后同样参与）+ version-change 动态边 (victim → on)，
    // 保证新并入受害者排在 on 包之后（否则按名字字母序可能在 on 前建，容器里还是旧 Qt → 白跑）。
    let mut edges = groups.trigger_edges_in(&selection);
    for (on, vics) in &planned_version {
        for v in vics {
            if v != on {
                edges.push((v.clone(), on.clone()));
            }
        }
    }
    edges.sort();
    edges.dedup();
    // --manual-sort：严格按命令行传入的包名顺序构建（引导链/手工编排用），不做 topo 重排。
    // 否则纯 python 包（无 needed_so 边）会按字母序建，bootstrap（setuptools→flit-core→build）会断。
    let queue: VecDeque<(String, bool)> = if opts.manual_sort && !opts.targets.is_empty() {
        opts.targets.iter().cloned().map(|p| (p, false)).collect()
    } else {
        topo_order(&opts.pkgs_dir, &selection, old, &edges)
            .into_iter()
            .map(|p| (p, false))
            .collect()
    };
    (queue, version_planned_names)
}

/// 进程内交互接管（§8.5）：BLOCKED 时提示 operator 选择，不退出进程。
/// 主调度：返回构建报告（built/repacked/abi_broken/blocked）。
/// `state` 非空时记录 job 状态 + 配方 hash（§11 持久化；读端/差分 requeue 尚未实现，
/// 仅作 operator 排查用）。失败路径（source 缺失 / repack / repo / index）也落 Blocked 库。
/// 单包阻断收尾：记入 `report.blocked` + 把 job 标成 `Blocked(stage)`。
/// 调用方打印自己的错误文案，之后 `continue`（本包不再往下走）。
///
/// repack 失败 / 进仓库失败 / 更新索引失败三处共用——历史上是三段各写一遍的同形代码。
fn block_package(
    report: &mut BuildReport,
    state: Option<&State>,
    pkg: &str,
    rhash: Option<&str>,
    stage: BlockStage,
) {
    report.blocked.push(pkg.to_string());
    if let Some(st) = state {
        let _ = st.set_job(pkg, JobStatus::Blocked, Some(stage.as_str()), rhash);
    }
}

pub fn run_build(
    opts: &BuildOptions,
    binding: &mut dyn LpkgBinding,
    state: Option<&State>,
) -> Result<BuildReport, FarmError> {
    // 1. 旧索引（§7.2 传播反图的锚）——必须由 seed 落地的本地 repo index.txt，缺失/为空直接报错
    //    （禁止无基线构建：needed_so provider 校验、ABI diff 都需要它）。
    let old = load_old_index(&opts.out_dir, &opts.arch)?;
    // 仓库全部提供能力 → binding 扫描 not-found 判定（needed_so 无 provider → 不进 needed_so）。
    // **这是活跃集，随每包 index 更新而刷新**（见 refresh_repo_provides）——否则同一次 run 里
    // 先构建包新加入的 SONAME 对后构建包不可见。
    binding.set_repo_provides(old.all_provided_capabilities());
    let revmap = RevMap::build(&old);
    // 声明式重建组（data/build/*.yaml）：不链但 ABI 敏感的包（python 生态等）。
    let groups = RebuildGroups::load(&opts.build_data_dir);

    // 2. 增量选择（用户规则）：effective_version 与本地 repo 旧索引一致的包跳过构建。
    //    LankeBUILD.json 的 version 是 raw；有 release 字段拼 version+release（如 1.1+2）。
    //    validate 模式：选择改为"所有没有 `.build_ok` 标记的包"（成功构建才会写标记，
    //    跳过/blocked 不写 → 下次 validate 重试）。排序仍走同一 topo_order。
    let all_pkgs = sorted_pkg_names(&opts.pkgs_dir);
    let (mut queue, version_planned_names) = build_plan(opts, &old, &groups, &all_pkgs);

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
        // 传播重建（被 ABI 断裂波及，is_victim）或 version-change 组预排受害者（开工前已定）
        // → 先 bump release（用户规则 1），再构建
        if is_victim || version_planned_names.contains(&pkg) {
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

        // 元数据漂移检测 + **无条件归一化重打**（打包完成 → SONAME 检测 → 与 .lpkg 内 metadata.json
        // 比对 → 漂移与否都重打 level22+mtime0；漂移才改 metadata + 双写 LankeBUILD）。
        // 只比 needed_so/provides；deps 由 gen_deps/deprules 生成，不读不改。
        // **repack 失败 → BLOCK**（曾静默降级为"无漂移"照发陈旧 metadata，.lpkg 与 index 永久失配）。
        let drifted = match repack_if_drift(&outcome, opts, &pkg) {
            Ok(d) => d,
            Err(e) => {
                eprintln!("{}", tr!("build.repack_fail", pkg, e));
                block_package(
                    &mut report,
                    state,
                    &pkg,
                    rhash.as_deref(),
                    BlockStage::Repack,
                );
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
                block_package(&mut report, state, &pkg, rhash.as_deref(), BlockStage::Repo);
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
            block_package(
                &mut report,
                state,
                &pkg,
                rhash.as_deref(),
                BlockStage::Index,
            );
            continue;
        }
        // 本包已进 index → 刷新 binding 的 repo_provides 为**当前**能力集，供下一包扫描。
        refresh_repo_provides(binding, &opts.out_dir, &opts.arch);
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
        // version-change 受害者不在此动态入队——开工前已在初始选择里算好并入队列（见上 2.5 前），
        // release bump 在弹包时处理；这里只处理 ABI/abichange 的动态受害者。
        if !victims.is_empty() {
            victims.sort();
            victims.dedup();
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
mod tests;
