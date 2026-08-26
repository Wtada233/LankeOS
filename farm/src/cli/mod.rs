//! LankeOS build farm — CLI 入口（§12.5）。
//!
//!   farm build <pkg>|--all               构建当前配方（预下载→构建→verify 三分支→repack/ABI 传播）
//!   farm track <pkg> --run                探测上游 → 新版自动更新 LankeBUILD.json（生成新版）
//!   farm gen-trackers                      batch 调 LLM 生成 tracker yaml（12 个/批）
//!   farm repack <pkg>                      zstd -22 --ultra 重打包仓库包并更新 index.txt
//!   farm seed / serve                    冷启动播种 / 本地 repo 静态服务器

use std::cmp::Ordering as CmpOrdering;
use std::collections::{HashMap, HashSet, VecDeque};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread;

/// 运行期诊断日志（`--log-output <file>`）：线程安全追加写，记录所有错误/警告/额外源诊断。
static LOG: Mutex<Option<std::fs::File>> = Mutex::new(None);

fn log_init(path: Option<&str>) -> Result<(), String> {
    if let Some(p) = path {
        let f = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(p)
            .map_err(|e| format!("打开日志文件失败 {p}: {e}"))?;
        *LOG.lock().unwrap() = Some(f);
    }
    Ok(())
}

/// 追加一行到日志（若配置了 `--log-output`）；无日志文件时静默。
fn log(line: &str) {
    if let Ok(mut guard) = LOG.lock() {
        if let Some(f) = guard.as_mut() {
            let _ = writeln!(f, "{line}");
        }
    }
}

/// 输出到 stderr 并记日志（错误类诊断）。
macro_rules! error_log {
    ($($arg:tt)*) => {{
        let msg = format!($($arg)*);
        eprintln!("{msg}");
        log(&msg);
    }};
}

/// 输出到 stdout 并记日志（警告类诊断）。
macro_rules! warn_log {
    ($($arg:tt)*) => {{
        let msg = format!($($arg)*);
        println!("{msg}");
        log(&msg);
    }};
}


use lankefarm::llm::LlmClient;
use lankefarm::net::{Fetcher, RealFetcher};
use lankefarm::track::vercmp;
use lankefarm::track::{dep_edges, TrackerConfig};

/// 命令解析结果：clap 子命令 → 扁平结构，供各 cmd_* 使用（保持逻辑层签名不变）。
mod build;
mod export;
mod repack;
mod serve;
mod seed;

#[derive(Default)]
pub(crate) struct Args {
    pkg: Vec<String>,
    pkgs: Option<String>,
    out: Option<PathBuf>,
    input: Option<PathBuf>,
    data: Option<String>,
    packages: Option<String>,
    api_endpoint: Option<String>,
    api_key: Option<String>,
    model: Option<String>,
    token: Option<String>,
    gitlab_token: Option<String>,
    run: bool,
    all: bool,
    manual_sort: bool,
    jobs: Option<usize>,
    root: Option<PathBuf>,
    port: Option<u16>,
    remote: Option<String>,
    arch: Option<String>,
    state: Option<PathBuf>,
    image: Option<String>,
    repo_port: Option<u16>,
    download_retries: Option<u32>,
}

#[derive(clap::Parser)]
#[command(
    name = "farm",
    version,
    about = "LankeOS build farm — ABI-driven incremental package builder",
    arg_required_else_help = true
)]
struct Cli {
    /// 把运行期错误/警告/诊断写入日志文件（可放子命令前或后）
    #[arg(long, global = true)]
    log_output: Option<PathBuf>,
    #[command(subcommand)]
    command: Command,
}

#[derive(clap::Subcommand)]
enum Command {
    /// 构建目标包；--all 时按版本增量选择（跳过与本地 repo 一致的包）并依赖排序。
    /// 上游版本更新由 farm track 生成。
    Build {
        /// 构建全部需重建的包（版本与本地 repo 不一致者，含 ABI 传播受害者）
        #[arg(long)]
        all: bool,
        /// 目标包名，可多个（--all 时省略；指定则强制重建这些包）
        #[arg(required_unless_present = "all", conflicts_with = "all", num_args = 1..)]
        pkg: Vec<String>,
        /// 严格按命令行传入的包名顺序构建（引导链/手工编排），不做 topo 重排
        #[arg(long)]
        manual_sort: bool,
        /// pkgs 目录（LankeBUILD 体系）
        #[arg(long, default_value = "pkgs")]
        pkgs: PathBuf,
        /// 产物/解包/发布目录
        #[arg(long, default_value = "out")]
        out: PathBuf,
        /// SQLite 状态库（job 状态记录，供 operator 排查；自动 requeue 未实现）
        #[arg(long)]
        state: Option<PathBuf>,
        /// 架构（发布到 out/<arch>/<pkg>/）
        #[arg(long, default_value = "x86_64")]
        arch: String,
        /// fresh container 基础镜像（wtada233/lankeos:latest）。必填——仅容器构建，
        /// 禁止主机直接 lpkg build（会污染宿主环境）。
        #[arg(long)]
        image: Option<String>,
        /// docker 模式内嵌本地 repo 服务器端口（容器 lpkg upgrade 从这拉依赖）
        #[arg(long, default_value_t = 80)]
        repo_port: u16,
        /// 源预下载网络重试次数（§8.6）
        #[arg(long, default_value_t = 3)]
        download_retries: u32,
    },
    /// 重建所有没有 `.build_ok` 标记的包（成功构建才会写标记；跳过/blocked 不写）。
    /// 排序与增量构建一致（topo_order + ABI 传播）；`--all` 等价物：自动选择缺标记的包。
    Validate {
        /// pkgs 目录（LankeBUILD 体系）
        #[arg(long, default_value = "pkgs")]
        pkgs: PathBuf,
        /// 产物/解包/发布目录
        #[arg(long, default_value = "out")]
        out: PathBuf,
        /// SQLite 状态库（job 状态记录，供 operator 排查）
        #[arg(long)]
        state: Option<PathBuf>,
        /// 架构（发布到 out/<arch>/<pkg>/）
        #[arg(long, default_value = "x86_64")]
        arch: String,
        /// fresh container 基础镜像（wtada233/lankeos:latest）。必填——仅容器构建。
        #[arg(long)]
        image: Option<String>,
        /// docker 模式内嵌本地 repo 服务器端口（容器 lpkg upgrade 从这拉依赖）
        #[arg(long, default_value_t = 80)]
        repo_port: u16,
        /// 源预下载网络重试次数（§8.6）
        #[arg(long, default_value_t = 3)]
        download_retries: u32,
    },
    /// 把构建仓库扁平化重打包为发行格式 `<pkg>-<ver>.lpkg`（zstd level 22 ultra）。
    /// 遍历 `input/<arch>/<pkg>/*.lpkg`，逐个解包→重打→输出到 output 目录。
    Export {
        /// 构建仓库根目录（含 `<arch>/` 子目录）[default: out]
        #[arg(long, default_value = "out")]
        input: PathBuf,
        /// 输出目录（扁平 `<pkg>-<ver>.lpkg`）
        #[arg(long)]
        output: PathBuf,
        /// 架构（读取 input/<arch>/ 下每个包）
        #[arg(long, default_value = "x86_64")]
        arch: String,
    },
    /// 用 zstd level 22（`-22 --ultra` 最高压缩档）重打包仓库中目标包的 .lpkg（原位替换），
    /// 并把新 SHA256 写回 index.txt。与 build 的快速 repack（level 3）不同：发行前对仓库终极压缩。
    Repack {
        /// 目标包名（对应 input/<arch>/<pkg>/）
        pkg: String,
        /// 构建仓库根目录（含 `<arch>/` 子目录）[default: out]
        #[arg(long, default_value = "out")]
        input: PathBuf,
        /// 架构（读取 input/<arch>/ 下该包）
        #[arg(long, default_value = "x86_64")]
        arch: String,
    },
    /// 探测上游版本
    Track {
        /// 目标包名（缺省需 --all）
        #[arg(required_unless_present = "all", conflicts_with = "all")]
        pkg: Option<String>,
        /// 遍历 pkgs/ 为所有有 tracker 的包出提案（只读）
        #[arg(long)]
        all: bool,
        /// 应用新版到 LankeBUILD.json（默认只出提案，只读；配合 --all 批量应用）
        #[arg(long)]
        run: bool,
        /// pkgs 目录（LankeBUILD 体系）
        #[arg(long, default_value = "pkgs")]
        pkgs: PathBuf,
        /// data/trackers 目录
        #[arg(long, default_value = "data/trackers")]
        data: PathBuf,
        /// 并行探测数（仅 --all）
        #[arg(short = 'j', long)]
        jobs: Option<usize>,
        /// GitHub token（消除 API 限流 403；也可用 GITHUB_TOKEN 环境变量）
        #[arg(long)]
        token: Option<String>,
        /// GitLab token（同上，GITLAB_TOKEN 环境变量兜底）
        #[arg(long)]
        gitlab_token: Option<String>,
    },
    /// batch 调 LLM 生成 tracker yaml（12 个/批）
    GenTrackers {
        /// pkgs 目录（LankeBUILD 体系）
        #[arg(long)]
        pkgs: PathBuf,
        /// data/trackers 目录
        #[arg(long)]
        data: PathBuf,
        /// LLM API 端点
        #[arg(long)]
        api_endpoint: String,
        /// LLM API key
        #[arg(long)]
        api_key: String,
        /// LLM 模型名
        #[arg(long)]
        model: String,
        /// 只处理指定包（逗号分隔）
        #[arg(long)]
        packages: Option<String>,
    },
    /// 本地 repo 静态 HTTP 服务器（§12.5）
    Serve {
        /// repo 根目录（含 <arch>/index.txt 与各包 .lpkg）
        #[arg(long, default_value = "out")]
        root: PathBuf,
        /// 端口
        #[arg(long, default_value_t = 8000)]
        port: u16,
    },
    /// 冷启动播种远程 repo（§8）
    Seed {
        /// 远程 repo URL（如 https://lankerepo.wtada233.top）
        #[arg(long)]
        remote: String,
        /// 架构
        #[arg(long, default_value = "x86_64")]
        arch: String,
        /// 本地 repo 根目录
        #[arg(long, default_value = "out")]
        out: PathBuf,
        /// 并行下载/解包线程数
        #[arg(long)]
        jobs: Option<usize>,
    },
}

/// pkgs/<name>/LankeBUILD.json 的最小字段。
#[derive(serde::Deserialize)]
struct BuildJson {
    name: String,
    version: String,
    #[serde(default)]
    sources: Vec<String>,
    #[serde(default)]
    work_sources: Vec<String>,
}

fn load_build_json(pkg_dir: &std::path::Path) -> Result<BuildJson, String> {
    let path = pkg_dir.join("LankeBUILD.json");
    let content =
        std::fs::read_to_string(&path).map_err(|e| format!("读取 {} 失败: {e}", path.display()))?;
    serde_json::from_str(&content).map_err(|e| format!("解析 {} 失败: {e}", path.display()))
}

/// 第一个非 file:// 的 source URL（file:// 是包内自带，无需 track）。
fn first_remote_source(sources: &[String]) -> Option<&str> {
    sources
        .iter()
        .find(|s| !s.starts_with("file://"))
        .map(String::as_str)
}


/// tracker 生成新版：把 proposal 应用到 LankeBUILD.json（version + sources[0] 主源 + work_sources 版本替换）。
/// 返回是否多源（sources 多于一个）。
/// 把提案应用到 LankeBUILD.json 的 value 上：更新 `version` 和 `sources[0]`。
/// `sources[1..]`：script 多源（prop.sources[1..]）直接落位；否则保留原值（由 cmd_track_run 的
/// `sources:`/`work_sources:` 条目按 url-match 正则匹配后升级）。
/// **work_sources 不做任何自动替换**：LFS patch 等是版本专用文件，盲替换版本只会指向不存在的 URL。
fn apply_version_update(value: &mut serde_json::Value, prop: &lankefarm::track::Proposal) {
    value["version"] = serde_json::Value::String(prop.new_version.clone());
    // 只有包**有 sources 数组**才更新 sources[0]（work_sources-only 包如 noto 字体不得凭空创建 sources 字段，
    // 否则 lpkg 会把 .otf 当 archive 处理；其 URL 由 cmd_track_run 的 work_sources: 配置按正则升级）。
    if let Some(arr) = value.get("sources").and_then(|s| s.as_array()) {
        let mut new_sources = Vec::new();
        if let Some(first) = prop.sources.first() {
            new_sources.push(serde_json::Value::String(first.clone()));
        }
        for (i, s) in arr.iter().skip(1).enumerate() {
            if let Some(ps) = prop.sources.get(i + 1) {
                new_sources.push(serde_json::Value::String(ps.clone()));
            } else {
                new_sources.push(s.clone());
            }
        }
        value["sources"] = serde_json::Value::Array(new_sources);
    }
    // work_sources：script 多源直接落位（覆盖已有槽位，不凭空创建——缺槽位由占位补）。
    // 非 script 多源（prop.work_sources 为空）不动 work_sources，交由 upgrade_list_by_regex
    // 按 url-match 正则升级（glibc/tzdata 模式）。
    if !prop.work_sources.is_empty() {
        if let Some(arr) = value.get("work_sources").and_then(|s| s.as_array()) {
            let mut new_ws = Vec::new();
            for (i, s) in arr.iter().enumerate() {
                if let Some(ps) = prop.work_sources.get(i) {
                    new_ws.push(serde_json::Value::String(ps.clone()));
                } else {
                    new_ws.push(s.clone());
                }
            }
            value["work_sources"] = serde_json::Value::Array(new_ws);
        }
    }
}

/// 应用提案到 LankeBUILD.json：version + sources[0] 主源 + 多源正则升级。返回是否成功写入。
/// `cmd_track_run`（单包 --run）与 `cmd_track_all --run`（批量）共用。
fn apply_proposal(
    pkg: &str,
    pkgs_dir: &Path,
    cfg: &TrackerConfig,
    p: &lankefarm::track::Proposal,
    fetcher: &dyn Fetcher,
    lookup: &dyn Fn(&str) -> Option<String>,
) -> bool {
    let json_path = pkgs_dir.join(pkg).join("LankeBUILD.json");
    let Ok(content) = std::fs::read_to_string(&json_path) else {
        return false;
    };
    let Ok(mut value) = serde_json::from_str::<serde_json::Value>(&content) else {
        return false;
    };
    apply_version_update(&mut value, p);
    // 多源追踪：每个 cfg.sources 条目用 url-match 正则匹配 LankeBUILD.json 里的实际 source URL，
    // 匹配到就探测并替换；work_sources 同理。不依赖索引——删 URL/改顺序不炸。
    let total_sources = value["sources"].as_array().map(|a| a.len()).unwrap_or(0);
    // 已被覆盖的 sources 索引：script 多源（prop.sources[1..]）已直接落位
    let mut upgraded: HashSet<usize> = (1..p.sources.len()).collect();
    // sources[1..] 与 work_sources 是同一机制（正则匹配+探测+替换），仅字段名不同，用同一实现
    upgrade_list_by_regex(
        &mut value,
        "sources",
        &cfg.sources,
        1,
        fetcher,
        lookup,
        pkg,
        Some(&mut upgraded),
    );
    upgrade_list_by_regex(
        &mut value,
        "work_sources",
        &cfg.work_sources,
        0,
        fetcher,
        lookup,
        pkg,
        None,
    );
    match std::fs::write(&json_path, serde_json::to_string_pretty(&value).unwrap()) {
        Err(e) => {
            error_log!("  [!] 写入 LankeBUILD.json 失败 {e}");
            false
        }
        Ok(()) => {
            println!("{}", lankefarm::tr!("track.applied", pkg, p.new_version));
            // 警告未被覆盖的 sources[1..]：无 script 多源、也无 sources 配置匹配到
            let untracked: Vec<usize> =
                (1..total_sources).filter(|i| !upgraded.contains(i)).collect();
            if !untracked.is_empty() {
                warn_log!(
                    "  ⚠ {pkg} 的 sources{untracked:?} 未被追踪配置覆盖（vendored 依赖写死在 LankeBUILD 属预期；否则补 sources: 条目）"
                );
            }
            true
        }
    }
}

/// 统一的多源升级：在 value[field]（"sources"/"work_sources"）里按 url-match 正则匹配实际 URL，
/// 匹配到就探测并替换。`start_idx`：sources 从 1 开始（[0] 是主源，由 apply_version_update 处理），
/// work_sources 从 0 开始。**sources 与 work_sources 对下载器只是字段名不同，用同一实现。**
#[allow(clippy::too_many_arguments)]
fn upgrade_list_by_regex(
    value: &mut serde_json::Value,
    field: &str,
    configs: &[TrackerConfig],
    start_idx: usize,
    fetcher: &dyn Fetcher,
    lookup: &dyn Fn(&str) -> Option<String>,
    pkg: &str,
    mut upgraded: Option<&mut HashSet<usize>>,
) {
    for (i, cfg) in configs.iter().enumerate() {
        let re = match cfg.url_match_regex() {
            Ok(Some(re)) => re,
            Ok(None) => {
                error_log!("  [!] {pkg} 的 {field}[{i}] 缺 url-match，忽略");
                continue;
            }
            Err(e) => {
                error_log!("  [!] {pkg}: {e}，忽略");
                continue;
            }
        };
        let idx = value[field]
            .as_array()
            .and_then(|a| {
                a.iter()
                    .enumerate()
                    .skip(start_idx)
                    .find(|(_, u)| u.as_str().is_some_and(|s| re.is_match(s)))
                    .map(|(idx, _)| idx)
            });
        let Some(idx) = idx else {
            warn_log!(
                "  ⚠ {pkg} 的 {field}[{i}] url-match '{}' 未匹配到任何 {field}（可能已删除/改名）",
                cfg.url_match.as_deref().unwrap_or("")
            );
            continue;
        };
        match cfg.probe_with(fetcher, lookup) {
            Ok(er) => {
                if let Some(new_src) = er.sources.first() {
                    if let Some(arr) = value[field].as_array_mut() {
                        if let Some(slot) = arr.get_mut(idx) {
                            *slot = serde_json::Value::String(new_src.clone());
                            println!(
                                "  [{field}升级] {field}[{idx}] → {new_src}（{}）",
                                cfg.tracker_template
                            );
                            if let Some(set) = upgraded.as_deref_mut() {
                                set.insert(idx);
                            }
                        }
                    }
                }
            }
            Err(e) => error_log!("  [{field}探测失败] {field}[{idx}]（{}）: {e}", cfg.tracker_template),
        }
    }
}

/// 按 `pkg-name` 字段（非文件名）索引 data/trackers/*.yaml。
fn load_trackers(data_dir: &str) -> HashMap<String, TrackerConfig> {
    let mut map = HashMap::new();
    if let Ok(rd) = std::fs::read_dir(data_dir) {
        for entry in rd.flatten() {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("yaml") {
                continue;
            }
            if let Ok(content) = std::fs::read_to_string(&path) {
                if let Ok(cfg) = serde_yaml::from_str::<TrackerConfig>(&content) {
                    map.insert(cfg.pkg_name.clone(), cfg);
                }
            }
        }
    }
    map
}

/// 构建带 token 的 RealFetcher：CLI `--token`/`--gitlab-token` 优先，环境变量 `GITHUB_TOKEN`/`GITLAB_TOKEN` 兜底。
/// 消除 GitHub/GitLab API 限流 403 噪音（未认证 GitHub API 60 次/小时，认证 5000 次/小时）。
fn build_fetcher(args: &Args) -> RealFetcher {
    RealFetcher::new(
        args.token
            .clone()
            .or_else(|| std::env::var("GITHUB_TOKEN").ok()),
        args.gitlab_token
            .clone()
            .or_else(|| std::env::var("GITLAB_TOKEN").ok()),
    )
}

/// 单个包：LankeBUILD.json（包来源）→ 按 name 字段找 tracker → 探测 → 生成新版（全部源升级）。
fn cmd_track_run(args: &Args, apply: bool) -> ExitCode {
    let pkgs_dir = args.pkgs.clone().unwrap_or_else(|| "pkgs".to_string());
    let data_dir = args
        .data
        .clone()
        .unwrap_or_else(|| "data/trackers".to_string());
    let pkg = match args.pkg.first() {
        Some(p) => p.clone(),
        None => {
            eprintln!("farm track <pkg> --run --pkgs <dir> --data <dir>");
            return ExitCode::from(2);
        }
    };
    // 包来源是 LankeBUILD 体系
    let build = match load_build_json(&PathBuf::from(&pkgs_dir).join(&pkg)) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("{e}");
            return ExitCode::from(2);
        }
    };
    // file:// 或无远程源：无需 track（本地产物，无上游可追踪）。
    // sources 和 work_sources 都是下载器字段，任一有远程 URL 就可追踪（work_sources-only 包如 noto 字体）。
    let has_remote = first_remote_source(&build.sources).is_some()
        || first_remote_source(&build.work_sources).is_some();
    if !has_remote {
        println!("[skip] {pkg}: 仅 file:// 或无远程源，无需 track");
        return ExitCode::SUCCESS;
    }
    // 按 name 字段匹配 tracker（文件名无关）
    let trackers = load_trackers(&data_dir);
    let cfg = match trackers.get(&build.name) {
        Some(c) => c,
        None => {
            eprintln!(
                "{} 无对应 tracker（data/trackers 中无 pkg-name 匹配的 yaml，用 farm gen-trackers 生成或手动写）",
                build.name
            );
            return ExitCode::from(2);
        }
    };
    let fetcher = build_fetcher(args);
    let pkg_name = build.name.clone();
    // 本轮解析出的新版本：同包的 same-version 额外源（如 docker 的 moby）必须读到新版本，
    // 否则会锁到 LankeBUILD.json 里的旧版本（应用前的值）。
    let pending_new = std::cell::RefCell::new(None::<String>);
    // 版本约束解析：优先本轮新版本，其次读其他包的 LankeBUILD.json 版本（same-version / major-of）
    let lookup = |pkg: &str| -> Option<String> {
        if pkg == pkg_name {
            if let Some(v) = pending_new.borrow().as_ref() {
                return Some(v.clone());
            }
        }
        load_build_json(&PathBuf::from(&pkgs_dir).join(pkg))
            .ok()
            .map(|b| b.version)
    };
    match cfg.propose_with(&fetcher, &lookup, &build.version) {
        Ok(p) => match vercmp::cmp_version(&p.new_version, &p.current_version) {
            CmpOrdering::Greater => {
                println!(
                    "[track] {}: {} → {}（模板 {}）",
                    p.pkg_name, p.current_version, p.new_version, p.tracker_template
                );
                for s in &p.sources {
                    println!("  {s}");
                }
                // 记录本轮新版本：同包 same-version 额外源（docker→moby）据此生成对应 tag
                *pending_new.borrow_mut() = Some(p.new_version.clone());
                if apply {
                    apply_proposal(&pkg, Path::new(&pkgs_dir), cfg, &p, &fetcher, &lookup);
                }
            }
            CmpOrdering::Equal => {
                println!("{}", lankefarm::tr!("track.latest", p.pkg_name, p.current_version));
            }
            CmpOrdering::Less => {
                error_log!(
                    "[!] {}: 探测到倒退版本 {} → {}（tracker 配置/模板疑似错误，忽略）",
                    p.pkg_name, p.current_version, p.new_version
                );
            }
        },
        Err(e) => {
            error_log!("  [probe 失败] {}: {e}", build.name);
            return ExitCode::from(2);
        }
    }
    ExitCode::SUCCESS
}

/// track --all 并行调度器状态（worker 共享，单 Mutex 防死锁）。
struct TrackSched {
    /// 就绪队列（前置已全部解析的包）
    queue: VecDeque<String>,
    /// 每个包剩余未解析的前置数（=0 才可出队）
    indeg: HashMap<String, usize>,
    /// 环兜底被"强制就绪"的包：不再递减其 indeg（保持真实计数，避免 0 下溢）
    forced: HashSet<String>,
    /// 尚未完成探测的包数（=0 即全部结束）
    remaining: usize,
    /// 本轮已解析出的新版本（供 same-version / major-of 读取）
    resolved: HashMap<String, String>,
}

/// worker：从就绪队列取包探测，完成后释放其依赖者（入度减到 0 才入队）。
/// 入度门控保证 `after(<pkg>)` / `last` / same-version / major-of 的前置先完成；
/// resolved 在释放依赖者前已写入（同一临界区），依赖者读到的是新版本而非 LankeBUILD.json 旧版本。
#[allow(clippy::too_many_arguments)] // worker 的显式参数比包装结构体更易读
fn track_worker(
    sched: &(Mutex<TrackSched>, Condvar),
    configs: &HashMap<String, TrackerConfig>,
    versions: &HashMap<String, String>,
    dependents: &HashMap<String, Vec<String>>,
    pkg_to_dir: &HashMap<String, String>,
    pkgs_dir: &Path,
    fetcher: &RealFetcher,
    proposals: &AtomicUsize,
    errors: &AtomicUsize,
    apply: bool,
) {
    loop {
        // 取一个就绪包；队列空但未完成则等待
        let name = {
            let mut guard = sched.0.lock().unwrap();
            while guard.queue.is_empty() && guard.remaining > 0 {
                guard = sched.1.wait(guard).unwrap();
            }
            if guard.queue.is_empty() {
                return; // 全部完成
            }
            guard.queue.pop_front().unwrap()
        };
        let cfg = &configs[&name];
        let current = &versions[&name];
        // pkg-name → 目录：same-version/major-of/after 引用的都是 pkg-name，目录名可能不同
        let dir_of =
            |pkg: &str| pkg_to_dir.get(pkg).cloned().unwrap_or_else(|| pkg.to_string());
        let lookup = |pkg: &str| {
            if let Some(v) = sched.0.lock().unwrap().resolved.get(pkg) {
                return Some(v.clone());
            }
            load_build_json(&pkgs_dir.join(dir_of(pkg))).ok().map(|b| b.version)
        };
        let result = cfg.propose_with(fetcher, &lookup, current);

        // 完成簿记：先写 resolved，再释放依赖者（同一临界区，顺序保证）
        let mut guard = sched.0.lock().unwrap();
        let mut pending_apply: Option<lankefarm::track::Proposal> = None;
        match result {
            Ok(p) => match vercmp::cmp_version(&p.new_version, &p.current_version) {
                CmpOrdering::Greater => {
                    println!(
                        "[track] {}: {} → {}（{}）",
                        p.pkg_name, p.current_version, p.new_version, p.tracker_template
                    );
                    // 只有更新的版本才参与后续 same-version / major-of 约束
                    guard.resolved.insert(name.clone(), p.new_version.clone());
                    proposals.fetch_add(1, Ordering::Relaxed);
                    pending_apply = Some(p);  // 移到 apply 候选（apply 模式才写）
                }
                CmpOrdering::Equal => {
                    println!("{}", lankefarm::tr!("track.latest", p.pkg_name, p.current_version));
                }
                CmpOrdering::Less => {
                    error_log!(
                        "  [!] {}: 探测到倒退版本 {} → {}（tracker 配置/模板疑似错误，忽略）",
                        p.pkg_name, p.current_version, p.new_version
                    );
                    errors.fetch_add(1, Ordering::Relaxed);
                }
            },
            Err(e) => {
                error_log!("  [!] {}: {e}", cfg.pkg_name);
                errors.fetch_add(1, Ordering::Relaxed);
            }
        }
        if let Some(deps) = dependents.get(&name) {
            for d in deps {
                // 环内被强制的包：保持其 indeg 真实计数不变（否则 0-1 下溢），
                // 且它们已在初始就绪集里，无需再次入队。
                if guard.forced.contains(d.as_str()) {
                    continue;
                }
                let e = guard.indeg.get_mut(d).unwrap();
                *e -= 1;
                if *e == 0 {
                    guard.queue.push_back(d.clone());
                }
            }
        }
        guard.remaining -= 1;
        drop(guard);
        sched.1.notify_all();

        // 批量应用（--all --run）：释放锁后再写 LankeBUILD.json，避免持锁做 I/O/网络探测
        if apply {
            if let Some(p) = pending_apply {
                // 写回目标目录 = pkg-name 对应的目录（目录名可能与 pkg-name 不同）
                apply_proposal(&dir_of(&p.pkg_name), pkgs_dir, cfg, &p, fetcher, &lookup);
            }
        }
    }
}

/// 遍历 pkgs/（LankeBUILD 体系是来源）为每个有 tracker 的包探测 → 提案汇总。
/// `-j N` 并行探测：与串行 `order_entries` 同一套 `dep_edges` 做入度门控——
/// `after(<pkg>)` / `last` / same-version / major-of 的前置先解析，并行不破坏顺序。
fn cmd_track_all(args: &Args, apply: bool) -> ExitCode {
    let pkgs_dir = args.pkgs.clone().unwrap_or_else(|| "pkgs".to_string());
    let data_dir = args
        .data
        .clone()
        .unwrap_or_else(|| "data/trackers".to_string());
    let root = PathBuf::from(&pkgs_dir);
    if !root.is_dir() {
        eprintln!("{}", lankefarm::tr!("pkgs.not_dir", pkgs_dir));
        return ExitCode::from(2);
    }
    let jobs = args.jobs.unwrap_or(1).max(1);
    let trackers = load_trackers(&data_dir);
    let entries: Vec<String> = match std::fs::read_dir(&root) {
        Ok(rd) => rd
            .filter_map(|e| e.ok())
            .filter(|e| e.path().is_dir())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .collect(),
        Err(e) => {
            eprintln!("{}", lankefarm::tr!("pkgs.read_fail", pkgs_dir, e));
            return ExitCode::from(2);
        }
    };

    // 探测集：有 tracker 且有有效 LankeBUILD.json 的包；无 tracker 的走 no_tracker 计数。
    // **统一键为 pkg-name（build.name）**：trackers/dep_edges 都以 tracker 的 pkg-name 为键，
    // 目录名可能与 pkg-name 不一致（LankeBUILD.json 的 name 字段 ≠ 目录名）。
    // 曾用目录名做 configs/versions 的键 → dep_edges 按 pkg-name 查不到 → order/same-version/
    // major-of 边被静默丢弃；apply_proposal 也把 pkg-name 当目录名写，会写到错误路径。
    // pkg_to_dir 维护 pkg-name → 目录 的映射，供文件读写（lookup / apply）。
    let mut configs: HashMap<String, TrackerConfig> = HashMap::new();
    let mut versions: HashMap<String, String> = HashMap::new();
    let mut no_tracker: Vec<String> = Vec::new();
    let mut pkg_to_dir: HashMap<String, String> = HashMap::new();
    let mut sorted = entries.clone();
    sorted.sort();
    for name in &sorted {
        let Ok(build) = load_build_json(&root.join(name)) else {
            continue;
        };
        match trackers.get(&build.name) {
            Some(cfg) => {
                configs.insert(build.name.clone(), cfg.clone());
                versions.insert(build.name.clone(), build.version);
                pkg_to_dir.insert(build.name.clone(), name.clone());
            }
            None => no_tracker.push(name.clone()),
        }
    }

    // 依赖图（与 order_entries 同一套 dep_edges）：入度门控并行
    let probe_names: Vec<String> = configs.keys().cloned().collect();
    let edges = dep_edges(&probe_names, &trackers);
    let mut indeg: HashMap<String, usize> = configs.keys().map(|n| (n.clone(), 0)).collect();
    let mut dependents: HashMap<String, Vec<String>> = HashMap::new();
    for (a, b) in &edges {
        *indeg.get_mut(b).unwrap() += 1;
        dependents.entry(a.clone()).or_default().push(b.clone());
    }
    // 环兜底：Kahn 模拟找出被环阻塞（indeg 无法归零）的包，强制置 0 立即就绪，避免 worker 永久等待
    let mut sim_indeg = indeg.clone();
    let mut sim_q: VecDeque<String> = probe_names
        .iter()
        .filter(|n| sim_indeg[n.as_str()] == 0)
        .cloned()
        .collect();
    let mut sim_done = 0;
    while let Some(n) = sim_q.pop_front() {
        sim_done += 1;
        if let Some(deps) = dependents.get(&n) {
            for d in deps {
                *sim_indeg.get_mut(d).unwrap() -= 1;
                if sim_indeg[d.as_str()] == 0 {
                    sim_q.push_back(d.clone());
                }
            }
        }
    }
    // 环兜底：被环阻塞的包记入 forced 集（强制就绪），但**不修改真实 indeg**——
    // 曾直接置 0，worker 释放依赖者时对已是 0 的 indeg 执行 `*e -= 1` → debug 构建
    // panic / release 构建 usize::MAX 下溢（未定义行为）。
    let mut forced: HashSet<String> = HashSet::new();
    if sim_done < probe_names.len() {
        for n in &probe_names {
            if sim_indeg[n.as_str()] > 0 {
                forced.insert(n.clone());
            }
        }
    }
    let mut ready: Vec<String> = probe_names
        .iter()
        .filter(|n| indeg[n.as_str()] == 0 || forced.contains(n.as_str()))
        .cloned()
        .collect();
    ready.sort();

    let sched = Arc::new((
        Mutex::new(TrackSched {
            queue: ready.into(),
            indeg,
            forced,
            remaining: configs.len(),
            resolved: HashMap::new(),
        }),
        Condvar::new(),
    ));
    let proposals = Arc::new(AtomicUsize::new(0));
    let errors = Arc::new(AtomicUsize::new(0));
    let configs = Arc::new(configs);
    let versions = Arc::new(versions);
    let dependents = Arc::new(dependents);
    let pkg_to_dir = Arc::new(pkg_to_dir);
    let fetcher = Arc::new(build_fetcher(args));

    if jobs > 1 {
        println!("{}", lankefarm::tr!("track.parallel", jobs));
    }
    let mut handles = Vec::new();
    for _ in 0..jobs {
        let sched = Arc::clone(&sched);
        let proposals = Arc::clone(&proposals);
        let errors = Arc::clone(&errors);
        let configs = Arc::clone(&configs);
        let versions = Arc::clone(&versions);
        let dependents = Arc::clone(&dependents);
        let pkg_to_dir = Arc::clone(&pkg_to_dir);
        let fetcher = Arc::clone(&fetcher);
        let root = root.clone();
        handles.push(thread::spawn(move || {
            track_worker(
                &sched,
                &configs,
                &versions,
                &dependents,
                &pkg_to_dir,
                &root,
                &fetcher,
                &proposals,
                &errors,
                apply,
            );
        }));
    }
    for h in handles {
        h.join().expect("track worker panicked");
    }

    let proposals = proposals.load(Ordering::Relaxed);
    let errors = errors.load(Ordering::Relaxed);
    // 孤儿 tracker = 无对应包（pkg-name 不在 pkg_to_dir 里）。曾按目录名比较，
    // 目录名 ≠ pkg-name 时会把有效 tracker 误报为孤儿。
    let orphans: Vec<String> = trackers
        .keys()
        .filter(|n| !pkg_to_dir.contains_key(n.as_str()))
        .cloned()
        .collect();
    println!();
    let summary = format!(
        "[汇总] 包 {} 个：{} {}，探测失败 {}，无 tracker {}，孤儿 yaml {}",
        entries.len(),
        if apply { "已应用" } else { "提案" },
        proposals,
        errors,
        no_tracker.len(),
        orphans.len()
    );
    println!("{summary}");
    log(&summary);
    if !no_tracker.is_empty() {
        println!("{}", lankefarm::tr!("track.no_tracker", no_tracker.join(", ")));
    }
    if !orphans.is_empty() {
        println!(
            "  [忽略] 孤儿 tracker yaml（无对应 LankeBUILD.json）: {}",
            orphans.join(", ")
        );
    }
    ExitCode::SUCCESS
}

/// 抓取源 URL 的父目录（探测输出，供 LLM 判断真实格式）。
fn fetch_listing(source_url: &str, fetcher: &dyn Fetcher) -> (String, String) {
    let parent = match source_url.rfind('/') {
        Some(i) => source_url[..=i].to_string(),
        None => return (source_url.to_string(), "(无法确定目录)".into()),
    };
    match fetcher.get(&parent) {
        Ok(body) => {
            let truncated: String = body.chars().take(2000).collect();
            let shown = if body.len() > 2000 {
                format!("{truncated}\n...(截断)")
            } else {
                truncated
            };
            (parent, shown)
        }
        Err(e) => (parent, format!("(抓取失败: {e})")),
    }
}

/// 目标包：--packages 指定，或所有有远程 source 但无 tracker 的包。file:///空源包跳过。
fn collect_targets(
    root: &std::path::Path,
    trackers: &HashMap<String, TrackerConfig>,
    packages: Option<&str>,
) -> Vec<String> {
    if let Some(list) = packages {
        // 显式指定的也过滤：file:///空源包跳过（无需 track）
        return list
            .split(',')
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .filter(|s| {
                let has_remote = match load_build_json(&root.join(s)) {
                    Ok(b) => first_remote_source(&b.sources).is_some(),
                    Err(_) => false,
                };
                if !has_remote {
                    eprintln!("  [skip] {s}: 仅 file:// 或无远程源");
                }
                has_remote
            })
            .collect();
    }
    let mut result = Vec::new();
    if let Ok(rd) = std::fs::read_dir(root) {
        for e in rd.flatten() {
            let dir = e.path();
            if !dir.is_dir() {
                continue;
            }
            let name = e.file_name().to_string_lossy().into_owned();
            if trackers.contains_key(&name) {
                continue;
            }
            if let Ok(b) = load_build_json(&dir) {
                if first_remote_source(&b.sources).is_some() {
                    result.push(name);
                }
            }
        }
    }
    result.sort();
    result
}

const SYSTEM_PROMPT: &str = r#"你是 LankeOS 发行版的 tracker 配置生成器。根据给定包的源 URL 和探测输出（真实抓取），生成正确的 tracker yaml。

可用 tracker-template 及字段：
- github: repo, mode(tags|releases), tag-prefix, template
- gitlab: host, project, mode(tags|releases), tag-prefix, template
- sourceforge: project, path, pattern, template
- gnome: template
- gcs: url(GCS/S3 桶目录), pattern, template
- html-index: url(HTML 目录列表页), pattern, template
- script: script-content(bash，stdout 第一行=版本，后续行=具体下载 URL)

通用字段：pkg-name（必填，=包名）、tracker-template（必填）。
多源包用 sources:/work_sources: 列表给每个额外源声明独立追踪配置：每条用 url-match 正则匹配 LankeBUILD.json 里实际 URL（非索引），可单独配 template/script/same-version 等。
template 是**完整下载 URL**（含 https:// 和主机名，占位符替换后可直接下载），不要把 URL 拆开只留文件名/相对路径。
template 占位符：{name} {version} {tag} {repo} {project} {path_version}。
pattern 是提取版本的正则，必须含一个捕获组，如 (\d[\d.]*)。

规则：
- 根据探测输出的真实格式选模板，不要猜；探测失败时按源 URL 域名/结构选最合理的。
- github 用 tags/releases API，gitlab 用其 API，GCS/S3 桶用 XML listing（?delimiter=/），纯 HTML 目录列表用 html-index。
- 无法用现成模板覆盖的（独特 API、版本在路径里等）用 script 写 bash 抓版本。
- 稳定版优先（tracker 自动过滤 rc/beta/alpha）。

输出格式：直接输出 N 个 YAML 文档，每个文档前用一行 `===` 分隔。不要 JSON、不要 markdown 代码围栏、不要任何解释。示例：
===
pkg-name: acl
tracker-template: github
repo: ...
===
pkg-name: alacritty
...

容错规则：
- pkg-name 必须是给定批次中的包名，不要发明、不要拼错、不要改名。
- 无法为某个包生成 tracker 时，输出一行 `none: <pkg-name>`（放在 == 分隔的块里），表示跳过该包。
- 每个包要么给有效 yaml，要么给 `none:`，不要省略。"#;

/// 把 LLM 返回的 `===` 分隔 YAML 文档拆成独立文本。
fn parse_yaml_docs(text: &str) -> Vec<String> {
    let mut docs = Vec::new();
    let mut current = String::new();
    for line in text.lines() {
        let t = line.trim();
        if t.starts_with("```") {
            continue;
        }
        if t == "===" {
            if !current.trim().is_empty() {
                docs.push(std::mem::take(&mut current));
            }
        } else {
            current.push_str(line);
            current.push('\n');
        }
    }
    if !current.trim().is_empty() {
        docs.push(current);
    }
    docs
}

/// 批次解析结果：有效 yaml / 显式跳过（none）/ 幻觉（不在批次里的包名）。
struct BatchResult {
    yamls: Vec<(String, String)>, // (pkg-name, yaml 文本)
    skipped: Vec<String>,
    hallucinations: Vec<String>,
}

/// 校验 LLM 输出：pkg-name 必须属于批次；`none: <pkg>` 表示跳过。
fn parse_batch_blocks(text: &str, batch: &[String]) -> BatchResult {
    let mut r = BatchResult {
        yamls: Vec::new(),
        skipped: Vec::new(),
        hallucinations: Vec::new(),
    };
    for doc in parse_yaml_docs(text) {
        let trimmed = doc.trim();
        if let Some(rest) = trimmed.strip_prefix("none:") {
            let name = rest.trim().to_string();
            if batch.contains(&name) {
                r.skipped.push(name);
            } else {
                r.hallucinations.push(name);
            }
            continue;
        }
        match serde_yaml::from_str::<TrackerConfig>(&doc) {
            Ok(cfg) if batch.contains(&cfg.pkg_name) => {
                r.yamls.push((cfg.pkg_name.clone(), doc));
            }
            Ok(cfg) => r.hallucinations.push(cfg.pkg_name),
            Err(_) => {
                // 解析失败：尝试提取 pkg-name 判断是否幻觉；提取不到视为 malformed
                if let Some(name) = extract_pkg_name(&doc) {
                    r.hallucinations.push(name);
                }
            }
        }
    }
    r
}

/// 从（可能残缺的）yaml 文本中提取 `pkg-name: X`。
fn extract_pkg_name(doc: &str) -> Option<String> {
    for line in doc.lines() {
        if let Some(v) = line.trim().strip_prefix("pkg-name:") {
            return Some(v.trim().to_string());
        }
    }
    None
}

/// farm gen-trackers：batch 调 LLM 生成 tracker yaml（12 个一批）。
fn cmd_gen_trackers(args: &Args) -> ExitCode {
    let pkgs_dir = args.pkgs.clone().unwrap_or_else(|| "pkgs".to_string());
    let data_dir = args
        .data
        .clone()
        .unwrap_or_else(|| "data/trackers".to_string());

    // API 配置：CLI 参数优先，env 兜底
    let endpoint = args
        .api_endpoint
        .clone()
        .or_else(|| std::env::var("LANKEFARM_LLM_BASE_URL").ok())
        .unwrap_or_else(|| "http://127.0.0.1:8000/v1".into());
    let key = args
        .api_key
        .clone()
        .or_else(|| std::env::var("LANKEFARM_LLM_API_KEY").ok())
        .unwrap_or_default();
    let model = match args
        .model
        .clone()
        .or_else(|| std::env::var("LANKEFARM_LLM_MODEL").ok())
    {
        Some(m) => m,
        None => {
            eprintln!("{}", lankefarm::tr!("gen.no_model"));
            return ExitCode::from(2);
        }
    };
    let llm = LlmClient::new(endpoint.clone(), key, model.clone());
    let fetcher = build_fetcher(args);

    let trackers = load_trackers(&data_dir);
    let root = PathBuf::from(&pkgs_dir);
    let targets = collect_targets(&root, &trackers, args.packages.as_deref());
    if targets.is_empty() {
        println!("{}", lankefarm::tr!("gen.none"));
        return ExitCode::SUCCESS;
    }
    println!(
        "[gen-trackers] 目标 {} 个包，API {endpoint}，模型 {model}，每批 12 个",
        targets.len()
    );

    std::fs::create_dir_all(&data_dir)
        .map_err(|e| {
            eprintln!("{}", lankefarm::tr!("gen.dir_fail", data_dir, e));
        })
        .ok();

    let mut written = 0;
    for (idx, batch) in targets.chunks(12).enumerate() {
        println!("{}", lankefarm::tr!("gen.batch", idx + 1, batch.len()));
        let mut sections = Vec::new();
        for name in batch {
            let build = match load_build_json(&root.join(name)) {
                Ok(b) => b,
                Err(e) => {
                    eprintln!("  [!] {name}: {e}");
                    continue;
                }
            };
            let src = first_remote_source(&build.sources).unwrap_or("(无远程源)");
            println!("{}", lankefarm::tr!("gen.fetch", name, src));
            let (url, listing) = fetch_listing(src, &fetcher);
            sections.push(format!(
                "[包] name={}, version={}\n  源: {}\n  探测输出（{url}）:\n```\n{listing}\n```",
                build.name, build.version, src
            ));
        }
        if sections.is_empty() {
            continue;
        }
        let base_user = format!("为以下 {} 个包生成 tracker yaml（每个包前用 === 分隔的 YAML 文档）：\n\n{}\n\n直接输出 YAML。", sections.len(), sections.join("\n\n"));
        let mut user = base_user.clone();
        let mut attempts = 0;
        loop {
            attempts += 1;
            println!(
                "  [LLM] 抓取完毕（{} 个包，prompt ~{} 字符），调用 API...",
                sections.len(),
                user.len()
            );
            match llm.chat(SYSTEM_PROMPT, &user) {
                Ok(resp) => {
                    let res = parse_batch_blocks(&resp, batch);
                    // 写有效 yaml（按 pkg-name，校验属于批次）
                    for (pkg, doc) in &res.yamls {
                        let path = PathBuf::from(&data_dir).join(format!("{pkg}.yaml"));
                        match std::fs::write(&path, doc) {
                            Ok(_) => {
                                println!("{}", lankefarm::tr!("gen.write", pkg));
                                written += 1;
                            }
                            Err(e) => eprintln!("{}", lankefarm::tr!("gen.write_fail", pkg, e)),
                        }
                    }
                    // 缺的：批次包既没 yaml 也没显式 none
                    let missing: Vec<String> = batch
                        .iter()
                        .filter(|n| {
                            !res.yamls.iter().any(|(p, _)| p == *n) && !res.skipped.contains(n)
                        })
                        .cloned()
                        .collect();
                    if res.hallucinations.is_empty() && missing.is_empty() {
                        break; // 批次完整（有效 or 显式跳过）
                    }
                    if attempts >= 3 {
                        eprintln!(
                            "  [!] 批次重试 {attempts} 次仍不完整——缺 {}，多 {}",
                            missing.join(", "),
                            res.hallucinations.join(", ")
                        );
                        break;
                    }
                    eprintln!(
                        "  [重试 {attempts}] 缺 {}, 多 {}，带反馈重新调用...",
                        missing.join(","),
                        res.hallucinations.join(",")
                    );
                    user = format!(
                        "{} 上次输出有误：缺 {}，多 {}. 请补全；对无法生成的包输出 `none: <pkg-name>`. 重新输出。",
                        base_user, missing.join(","), res.hallucinations.join(",")
                    );
                }
                Err(e) => {
                    eprintln!("{}", lankefarm::tr!("gen.batch_fail", e));
                    break;
                }
            }
        }
    }
    println!();
    println!("{}", lankefarm::tr!("gen.done", written));
    ExitCode::SUCCESS
}

/// LANG=en 时把 clap 帮助文本（doc comment 是中文）覆盖为英文。builder 风格（消费 self 返回 Self）。
fn localize_help(cmd: clap::Command) -> clap::Command {
    if !lankefarm::i18n::is_en() {
        return cmd;
    }
    cmd.mut_arg("log_output", |a| a.help("Write runtime errors/warnings/diagnostics to a log file"))
        .mut_subcommand("build", |c| c
            .about("Build a target package (--all: version-incremental + dependency order); upstream updates come from farm track")
            .mut_arg("all", |a| a.help("Build all packages needing rebuild (version mismatch or ABI victims)"))
            .mut_arg("pkg", |a| a.help("Target package name (omit with --all; forces a rebuild)"))
            .mut_arg("pkgs", |a| a.help("pkgs directory (LankeBUILD tree)"))
            .mut_arg("out", |a| a.help("Artifacts/extract/publish directory"))
            .mut_arg("state", |a| a.help("SQLite state DB (job status/resume)"))
            .mut_arg("arch", |a| a.help("Architecture (publish to out/<arch>/<pkg>/)"))
            .mut_arg("image", |a| a.help("Fresh container base image. Required - container builds only"))
            .mut_arg("repo_port", |a| a.help("Embedded local repo server port (container lpkg upgrade pulls from it)"))
            .mut_arg("download_retries", |a| a.help("Source pre-download network retries")))
        .mut_subcommand("track", |c| c
            .about("Probe upstream versions")
            .mut_arg("pkg", |a| a.help("Target package name (required without --all)"))
            .mut_arg("all", |a| a.help("Probe all packages with trackers (read-only proposals)"))
            .mut_arg("run", |a| a.help("Apply new versions to LankeBUILD.json (default: read-only proposals)"))
            .mut_arg("data", |a| a.help("data/trackers directory"))
            .mut_arg("jobs", |a| a.help("Parallel probes (--all only)"))
            .mut_arg("token", |a| a.help("GitHub token (avoid API rate-limit 403)"))
            .mut_arg("gitlab_token", |a| a.help("GitLab token (GITLAB_TOKEN env fallback)")))
        .mut_subcommand("gen-trackers", |c| c
            .about("Batch-generate tracker YAML via LLM (12 per batch)")
            .mut_arg("pkgs", |a| a.help("pkgs directory (LankeBUILD tree)"))
            .mut_arg("data", |a| a.help("data/trackers directory"))
            .mut_arg("api_endpoint", |a| a.help("LLM API endpoint"))
            .mut_arg("api_key", |a| a.help("LLM API key"))
            .mut_arg("model", |a| a.help("LLM model name"))
            .mut_arg("packages", |a| a.help("Only process these packages (comma-separated)")))
        .mut_subcommand("repack", |c| c
            .about("Repack a repo package with zstd -22 --ultra (in place) and update index.txt SHA256")
            .mut_arg("pkg", |a| a.help("Target package name (input/<arch>/<pkg>/)"))
            .mut_arg("input", |a| a.help("Build repo root (contains <arch>/ subdirs)"))
            .mut_arg("arch", |a| a.help("Architecture (read the package under input/<arch>/)")))
        .mut_subcommand("serve", |c| c
            .about("Serve the local repo over HTTP")
            .mut_arg("root", |a| a.help("Repo root (contains <arch>/index.txt and package .lpkg)"))
            .mut_arg("port", |a| a.help("Port")))
        .mut_subcommand("seed", |c| c
            .about("Cold-start seed from a remote repo")
            .mut_arg("remote", |a| a.help("Remote repo URL (e.g. https://lankerepo.wtada233.top)"))
            .mut_arg("arch", |a| a.help("Architecture"))
            .mut_arg("out", |a| a.help("Local repo root directory"))
            .mut_arg("jobs", |a| a.help("Parallel download/extract threads")))
}

pub fn run() -> ExitCode {
    use clap::{CommandFactory, FromArgMatches};
    // 英文环境下覆盖 clap 帮助文本（doc comment 是中文，运行时按 LANG 替换）
    let cli = match Cli::from_arg_matches(&localize_help(Cli::command()).get_matches()) {
        Ok(c) => c,
        Err(e) => e.exit(),
    };
    if let Err(e) = log_init(cli.log_output.as_deref().and_then(|p| p.to_str())) {
        eprintln!("{e}");
        return ExitCode::from(2);
    }
    match cli.command {
        Command::Build { all, pkg, manual_sort, pkgs, out, state, arch, image, repo_port, download_retries } => {
            let args = Args {
                all,
                pkg,
                manual_sort,
                pkgs: Some(pkgs.to_string_lossy().into_owned()),
                out: Some(out),
                state: state.map(|p| p.to_path_buf()),
                arch: Some(arch),
                image,
                repo_port: Some(repo_port),
                download_retries: Some(download_retries),
                ..Default::default()
            };
            build::cmd_build(&args)
        }
        Command::Validate { pkgs, out, state, arch, image, repo_port, download_retries } => {
            let args = Args {
                pkgs: Some(pkgs.to_string_lossy().into_owned()),
                out: Some(out),
                state: state.map(|p| p.to_path_buf()),
                arch: Some(arch),
                image,
                repo_port: Some(repo_port),
                download_retries: Some(download_retries),
                ..Default::default()
            };
            build::cmd_validate(&args)
        }
        Command::Export { input, output, arch } => {
            let args = Args {
                input: Some(input),
                out: Some(output),
                arch: Some(arch),
                ..Default::default()
            };
            export::cmd_export(&args)
        }
        Command::Repack { pkg, input, arch } => {
            let args = Args {
                pkg: vec![pkg],
                input: Some(input),
                arch: Some(arch),
                ..Default::default()
            };
            repack::cmd_repack(&args)
        }
        Command::Track { pkg, all, run, pkgs, data, jobs, token, gitlab_token } => {
            let args = Args {
                pkg: pkg.map(|p| vec![p]).unwrap_or_default(),
                all,
                run,
                pkgs: Some(pkgs.to_string_lossy().into_owned()),
                data: Some(data.to_string_lossy().into_owned()),
                jobs,
                token,
                gitlab_token,
                ..Default::default()
            };
            if args.all {
                // --all：只出提案；--all --run：批量应用
                cmd_track_all(&args, args.run)
            } else {
                // 单包：--run 应用新版；缺省只读出提案（不写 LankeBUILD.json）
                cmd_track_run(&args, args.run)
            }
        }
        Command::GenTrackers { pkgs, data, api_endpoint, api_key, model, packages } => {
            let args = Args {
                pkgs: Some(pkgs.to_string_lossy().into_owned()),
                data: Some(data.to_string_lossy().into_owned()),
                api_endpoint: Some(api_endpoint),
                api_key: Some(api_key),
                model: Some(model),
                packages,
                ..Default::default()
            };
            cmd_gen_trackers(&args)
        }
        Command::Serve { root, port } => {
            let args = Args {
                root: Some(root),
                port: Some(port),
                ..Default::default()
            };
            serve::cmd_serve(&args)
        }
        Command::Seed { remote, arch, out, jobs } => {
            let args = Args {
                remote: Some(remote),
                arch: Some(arch),
                out: Some(out),
                jobs,
                ..Default::default()
            };
            seed::cmd_seed(&args)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use clap::Parser; // 测试用 Cli::parse_from/try_parse_from

    #[test]
    fn parse_batch_blocks_validates() {
        let batch = vec![
            "acl".to_string(),
            "alacritty".to_string(),
            "alsa-lib".to_string(),
        ];
        let resp = "===\npkg-name: acl\ntracker-template: github\nrepo: a/b\n===\nnone: alacritty\n===\npkg-name: fake\n";
        let r = parse_batch_blocks(resp, &batch);
        assert_eq!(r.yamls.len(), 1);
        assert_eq!(r.yamls[0].0, "acl");
        assert_eq!(r.skipped, vec!["alacritty"]);
        assert_eq!(r.hallucinations, vec!["fake"]);
        // alsa-lib 既无 yaml 也无 none → 会在批处理里判为"缺"
    }

    #[test]
    fn parses_jobs_flag() {
        let cli = Cli::try_parse_from(["farm", "track", "--all", "-j", "8"]).unwrap();
        match cli.command {
            Command::Track { jobs, .. } => assert_eq!(jobs, Some(8)),
            _ => panic!("应解析为 track 子命令"),
        }
        let cli = Cli::try_parse_from(["farm", "track", "--all"]).unwrap();
        match cli.command {
            Command::Track { jobs, .. } => assert_eq!(jobs, None),
            _ => panic!("应解析为 track 子命令"),
        }
    }

    #[test]
    fn apply_version_update_skips_sources_for_work_only_pkg() {
        use lankefarm::track::Proposal;
        // 无 sources 字段（如 noto 字体，字体在 work_sources）→ 只更新 version，不得凭空创建 sources
        let mut value = serde_json::json!({
            "name": "noto",
            "version": "2.004",
            "work_sources": ["https://github.com/notofonts/noto-cjk/raw/refs/tags/Sans2.004/Sans/Mono/font.otf"]
        });
        let prop = Proposal {
            pkg_name: "noto".into(),
            current_version: "2.004".into(),
            new_version: "2.005".into(),
            sources: vec!["https://github.com/notofonts/noto-cjk/raw/refs/tags/Sans2.005/Sans/Mono/font.otf".into()],
            work_sources: vec![],
            tracker_template: "github".into(),
        };
        apply_version_update(&mut value, &prop);
        assert_eq!(value["version"], "2.005");
        assert!(value.get("sources").is_none(), "不得为 work_sources-only 包凭空创建 sources");
        // work_sources 保持原样（由 cmd_track_run 的 work_sources: 配置按正则升级）
        assert_eq!(
            value["work_sources"][0],
            "https://github.com/notofonts/noto-cjk/raw/refs/tags/Sans2.004/Sans/Mono/font.otf"
        );
    }

    #[test]
    fn parses_log_output_global() {
        let cli = Cli::try_parse_from(["farm", "track", "--all", "--log-output", "/tmp/farm.log"])
            .unwrap();
        assert_eq!(
            cli.log_output.as_deref().map(|p| p.to_str().unwrap()),
            Some("/tmp/farm.log")
        );
        let cli = Cli::try_parse_from(["farm", "track", "--all"]).unwrap();
        assert!(cli.log_output.is_none());
    }

    #[test]
    fn parses_token_flags() {
        let cli = Cli::try_parse_from([
            "farm",
            "track",
            "--all",
            "--token",
            "ghp_xxx",
            "--gitlab-token",
            "glpat_yyy",
        ])
        .unwrap();
        match cli.command {
            Command::Track { token, gitlab_token, .. } => {
                assert_eq!(token.as_deref(), Some("ghp_xxx"));
                assert_eq!(gitlab_token.as_deref(), Some("glpat_yyy"));
            }
            _ => panic!("应解析为 track 子命令"),
        }
    }

    #[test]
    fn track_single_without_run_is_readonly_probe() {
        // 只读单包探测：track <pkg>（无 --run）→ pkg 有值、run=false、all=false
        let cli = Cli::try_parse_from(["farm", "track", "gtk3", "--pkgs", "../pkgs"]).unwrap();
        match cli.command {
            Command::Track { pkg, run, all, .. } => {
                assert_eq!(pkg.as_deref(), Some("gtk3"));
                assert!(!run);
                assert!(!all);
            }
            _ => panic!("应解析为 track 子命令"),
        }
        // --run 应用新版
        let cli = Cli::try_parse_from(["farm", "track", "gtk3", "--run"]).unwrap();
        match cli.command {
            Command::Track { pkg, run, all, .. } => {
                assert_eq!(pkg.as_deref(), Some("gtk3"));
                assert!(run);
                assert!(!all);
            }
            _ => panic!("应解析为 track 子命令"),
        }
    }

    #[test]
    fn track_requires_pkg_or_all() {
        // 既无 pkg 也无 --all → clap 报错
        assert!(Cli::try_parse_from(["farm", "track"]).is_err());
        // --run 单包必须给 <pkg>（无 --all 时）
        assert!(Cli::try_parse_from(["farm", "track", "--run"]).is_err());
        // --all 与 pkg 互斥
        assert!(Cli::try_parse_from(["farm", "track", "--all", "gtk3"]).is_err());
    }

    #[test]
    fn track_all_run_is_bulk_apply() {
        // --all --run = 批量应用（clap 不再互斥）
        let cli = Cli::try_parse_from(["farm", "track", "--all", "--run"]).unwrap();
        match cli.command {
            Command::Track { all, run, .. } => {
                assert!(all);
                assert!(run);
            }
            _ => panic!("应解析为 track 子命令"),
        }
    }

    #[test]
    fn parse_yaml_docs_splits_and_strips_fences() {
        let resp = "```\n===\na: 1\n```\n===\nb: 2\n";
        let docs = parse_yaml_docs(resp);
        assert_eq!(docs.len(), 2);
        assert!(docs[0].contains("a: 1"));
        assert!(docs[1].contains("b: 2"));
    }
}
