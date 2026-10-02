//! LankeOS build farm — CLI 入口（§12.5）。
//!
//!   farm build <pkg>|--all               构建当前配方（预下载→构建→verify 三分支→无条件 level22 归一化重打→ABI 传播）
//!   farm track <pkg> --run                探测上游 → 新版自动更新 LankeBUILD.json（生成新版）
//!   farm gen-trackers                      batch 调 LLM 生成 tracker yaml（12 个/批）
//!   farm seed / serve                    冷启动播种 / 本地 repo 静态服务器

use lankefarm::error::FarmError;
use std::collections::HashMap;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::Mutex;

/// 运行期诊断日志（`--log-output <file>`）：线程安全追加写，记录所有错误/警告/额外源诊断。
static LOG: Mutex<Option<std::fs::File>> = Mutex::new(None);

fn log_init(path: Option<&str>) -> Result<(), FarmError> {
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

use lankefarm::llm::LlmClient;
use lankefarm::net::{Fetcher, RealFetcher};
use lankefarm::track::vercmp;
use lankefarm::track::{dep_edges, TrackerConfig};

/// 命令实现子模块：每个子命令持自己的 `clap::Args` 结构体（见下方 BuildArgs/TrackArgs/…），
/// `run()` 直接分发，无全局 hub/手工拷贝。
mod build;
mod custom_checks;
mod export;
mod gen_trackers;
mod help_en;
mod seed;
mod serve;
mod track;

/// 构建类共享参数（`Build`/`Validate`/`AbiFix` 三个子命令 `#[command(flatten)]` 复用）。
/// 取代旧的全局 `Args` 神结构体：每个子命令持自己的 clap::Args，run() 直接分发，无手工拷贝。
#[derive(clap::Args, Debug, Clone)]
pub(crate) struct RepoArgs {
    /// pkgs 目录（LankeBUILD 体系）
    #[arg(long, default_value = "pkgs")]
    pub pkgs: PathBuf,
    /// 产物/解包/发布目录
    #[arg(long, default_value = "out")]
    pub out: PathBuf,
    /// SQLite 状态库（job 状态记录，供 operator 排查；farm 是批式 CLI、无后台 requeue
    /// ——「配方 hash 变了就重建」由 validate 在每次运行时评估）
    #[arg(long)]
    pub state: Option<PathBuf>,
    /// 架构（发布到 out/<arch>/<pkg>/）
    #[arg(long, default_value = "x86_64")]
    pub arch: String,
    /// fresh container 基础镜像（wtada233/lankeos:latest）。必填——仅容器构建，
    /// 禁止主机直接 lpkg build（会污染宿主环境）。
    #[arg(long)]
    pub image: Option<String>,
    /// docker 模式内嵌本地 repo 服务器端口（容器 lpkg upgrade 从这拉依赖）
    #[arg(long, default_value_t = 80)]
    pub repo_port: u16,
    /// 源预下载网络重试次数（§8.6）
    #[arg(long, default_value_t = 3)]
    pub download_retries: u32,
}

/// `farm build`：构建目标包；--all 时按版本增量选择并依赖排序。
#[derive(clap::Args, Debug, Clone)]
pub(crate) struct BuildArgs {
    #[command(flatten)]
    pub repo: RepoArgs,
    /// 构建全部需重建的包（版本与本地 repo 不一致者，含 ABI 传播受害者）
    #[arg(long)]
    pub all: bool,
    /// 目标包名，可多个（--all 时省略；指定则强制重建这些包）
    #[arg(required_unless_present = "all", conflicts_with = "all", num_args = 1..)]
    pub pkg: Vec<String>,
    /// 严格按命令行传入的包名顺序构建（引导链/手工编排），不做 topo 重排
    #[arg(long)]
    pub manual_sort: bool,
}

/// `farm validate`：重建所有没有 `.build_ok` 标记的包。
#[derive(clap::Args, Debug, Clone)]
pub(crate) struct ValidateArgs {
    #[command(flatten)]
    pub repo: RepoArgs,
}

/// `farm abifix`：修复「LankeBUILD.json 引用仓库无 provider SONAME」的包。
#[derive(clap::Args, Debug, Clone)]
pub(crate) struct AbiFixArgs {
    #[command(flatten)]
    pub repo: RepoArgs,
}

/// `farm export`：把构建仓库扁平化为发行布局 `<pkg>-<ver>.lpkg`。
#[derive(clap::Args, Debug, Clone)]
pub(crate) struct ExportArgs {
    /// 构建仓库根目录（含 `<arch>/` 子目录）[default: out]
    #[arg(long, default_value = "out")]
    pub input: PathBuf,
    /// 输出目录（扁平 `<pkg>-<ver>.lpkg`）
    #[arg(long)]
    pub output: PathBuf,
    /// 架构（读取 input/<arch>/ 下每个包）
    #[arg(long, default_value = "x86_64")]
    pub arch: String,
}

/// `farm track`：探测上游版本（--run 应用，缺省只读提案）。
#[derive(clap::Args, Debug, Clone)]
pub(crate) struct TrackArgs {
    /// 目标包名，可多个（缺省需 --all）
    #[arg(required_unless_present = "all", conflicts_with = "all", num_args = 1..)]
    pub pkg: Vec<String>,
    /// 遍历 pkgs/ 为所有有 tracker 的包出提案（只读）
    #[arg(long)]
    pub all: bool,
    /// 应用新版到 LankeBUILD.json（默认只出提案，只读；配合 --all 批量应用）
    #[arg(long)]
    pub run: bool,
    /// 源 URL 探测失败时仍继续写入（需同时 --run；否则探测失败跳过写入并告警）
    #[arg(long)]
    pub probe_fail_continue: bool,
    /// pkgs 目录（LankeBUILD 体系）
    #[arg(long, default_value = "pkgs")]
    pub pkgs: PathBuf,
    /// data/trackers 目录
    #[arg(long, default_value = "data/trackers")]
    pub data: PathBuf,
    /// 并行探测数（仅 --all）
    #[arg(short = 'j', long)]
    pub jobs: Option<usize>,
    /// GitHub token（消除 API 限流 403；也可用 GITHUB_TOKEN 环境变量）
    #[arg(long)]
    pub token: Option<String>,
    /// GitLab token（同上，GITLAB_TOKEN 环境变量兜底）
    #[arg(long)]
    pub gitlab_token: Option<String>,
}

/// `farm gen-trackers`：batch 调 LLM 生成 tracker yaml。
#[derive(clap::Args, Debug, Clone)]
pub(crate) struct GenTrackersArgs {
    /// pkgs 目录（LankeBUILD 体系）
    #[arg(long)]
    pub pkgs: PathBuf,
    /// data/trackers 目录
    #[arg(long)]
    pub data: PathBuf,
    /// LLM API 端点
    #[arg(long)]
    pub api_endpoint: String,
    /// LLM API key
    #[arg(long)]
    pub api_key: String,
    /// LLM 模型名
    #[arg(long)]
    pub model: String,
    /// 只处理指定包（逗号分隔）
    #[arg(long)]
    pub packages: Option<String>,
}

/// `farm serve`：本地 repo 静态 HTTP 服务器。
#[derive(clap::Args, Debug, Clone)]
pub(crate) struct ServeArgs {
    /// repo 根目录（含 <arch>/index.txt 与各包 .lpkg）
    #[arg(long, default_value = "out")]
    pub root: PathBuf,
    /// 端口
    #[arg(long, default_value_t = 8000)]
    pub port: u16,
}

/// `farm seed`：冷启动播种远程 repo。
#[derive(clap::Args, Debug, Clone)]
pub(crate) struct SeedArgs {
    /// 远程 repo URL（如 https://lankerepo.wtada233.top）
    #[arg(long)]
    pub remote: String,
    /// 架构
    #[arg(long, default_value = "x86_64")]
    pub arch: String,
    /// 本地 repo 根目录
    #[arg(long, default_value = "out")]
    pub out: PathBuf,
    /// 并行下载/解包线程数
    #[arg(long)]
    pub jobs: Option<usize>,
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

/// `farm chk` 下的检則子命令（qml / pkgconf / pkg-err / hook / abi / full）。这些是**维护期实用工具，
/// 非稳定接口**——可能因技术变迁（qml 归属分析 / pkgconf 解析 / ELF 符号审计 / .lpkg 布局约定变化）
/// 而被移除或改名，别把它们当长期 API。它们共享同一组 `ChkArgs`。
#[derive(clap::Subcommand)]
pub(crate) enum ChkCommand {
    /// QML import 依赖检查：import 模块的归属包须在本包 deps∪needed_so 内（缺则报）。
    Qml {
        #[command(flatten)]
        chk: custom_checks::ChkArgs,
    },
    /// pkg-config Requires 检查：.pc Requires(/private) 模块归属包须在本包 deps∪needed_so 内。
    Pkgconf {
        #[command(flatten)]
        chk: custom_checks::ChkArgs,
    },
    /// 打包错误检查：usr/etc|usr/var 错位、.la、.a（除非 cmake 引用 + IGNORE_CHK_PKGERR 豁免）。
    PkgErr {
        #[command(flatten)]
        chk: custom_checks::ChkArgs,
    },
    /// GObject Introspection 构建依赖检查：装 `.gir` 的包，配方须声明 `gobject-introspection` 构建依赖。
    Introspection {
        #[command(flatten)]
        chk: custom_checks::ChkArgs,
    },
    /// Vala 绑定构建依赖检查：装 `.vapi` 的包，配方须声明 `vala` 构建依赖。
    Vapi {
        #[command(flatten)]
        chk: custom_checks::ChkArgs,
    },
    /// 构建依赖完整性检查：`needed_so` 的每个 SONAME 提供者必须写在本包 `build_deps`（传递满足不算）。
    BuildDeps {
        #[command(flatten)]
        chk: custom_checks::ChkArgs,
    },
    /// Python 字节码缓存检查：包内不得含 `__pycache__` 目录（含空的）。
    Pycache {
        #[command(flatten)]
        chk: custom_checks::ChkArgs,
    },
    /// postinst hook 检查：建了 sysusers.d/tmpfiles.d 的包 postinst 须调 systemd-sysusers /
    /// systemd-tmpfiles --create。
    Hook {
        #[command(flatten)]
        chk: custom_checks::ChkArgs,
    },
    /// 全 ABI 符号/版本审计（原 `manual-abi-fullchk`）：逐包 ELF 扫每个 SONAME 的符号@版本，
    /// 报告 consumer 引用但无任何 provider 提供的符号@版本。非 root 可跑。
    Abi {
        #[command(flatten)]
        chk: custom_checks::ChkArgs,
    },
    /// 用同一组参数依次跑全部检則：qml + pkgconf + pkg-err + introspection + vapi + build-deps + pycache + hook + abi。
    Full {
        #[command(flatten)]
        chk: custom_checks::ChkArgs,
    },
}

#[derive(clap::Subcommand)]
enum Command {
    /// 构建目标包；--all 时按版本增量选择（跳过与本地 repo 一致的包）并依赖排序。
    /// 上游版本更新由 farm track 生成。
    Build(BuildArgs),
    /// 重建所有没有 `.build_ok` 标记的包（成功构建才会写标记；跳过/blocked 不写）。
    /// 排序与增量构建一致（topo_order + ABI 传播）；`--all` 等价物：自动选择缺标记的包。
    Validate(ValidateArgs),
    /// 自动修复「LankeBUILD.json 引用仓库无 provider SONAME」的包：bump release 后重建
    /// （重建重扫 needed_so，孤儿条目不再链接则自动消失；仍真需要则 BLOCKED 提示先更新
    /// provider 配方）。排序与增量构建一致（topo_order + ABI 传播）。
    #[command(name = "abifix")]
    AbiFix(AbiFixArgs),
    /// 把构建仓库扁平化为发行布局 `<pkg>-<ver>.lpkg`（纯复制，不重打包——仓库产物已归一化）。
    /// 遍历 `input/<arch>/<pkg>/*.lpkg`，复制到 output 目录。
    Export(ExportArgs),
    /// 维护期实用检則工具集（qml / pkgconf / pkg-err / introspection / vapi / build-deps / pycache / hook / abi / full，含全 ABI 审计）。
    /// **非稳定接口**——可能因技术变迁移除；具体子命令见 `farm chk --help`。
    Chk {
        #[command(subcommand)]
        sub: ChkCommand,
    },
    /// 探测上游版本
    Track(TrackArgs),
    /// batch 调 LLM 生成 tracker yaml（12 个/批）
    GenTrackers(GenTrackersArgs),
    /// 本地 repo 静态 HTTP 服务器（§12.5）
    Serve(ServeArgs),
    /// 冷启动播种远程 repo（§8）
    Seed(SeedArgs),
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

fn load_build_json(pkg_dir: &std::path::Path) -> Result<BuildJson, FarmError> {
    let path = pkg_dir.join("LankeBUILD.json");
    let content =
        std::fs::read_to_string(&path).map_err(|e| format!("读取 {} 失败: {e}", path.display()))?;
    serde_json::from_str(&content).map_err(|e| format!("解析 {} 失败: {e}", path.display()).into())
}

/// 第一个非 file:// 的 source URL（file:// 是包内自带，无需 track）。
/// 包是否有**远程**源（`file://`/空源是包内自带，无需 track）。
///
/// `sources` 与 `work_sources` 都是下载器字段，**任一**有远程 URL 即可追踪——work_sources-only 的
/// 包（noto 字体、beanshell）也算。历史上 track 侧判两者、gen-trackers 侧只判 `sources`
/// ⇒ 后者会把 work_sources-only 的包当"无远程源"跳过。
fn has_remote_source(b: &BuildJson) -> bool {
    first_remote_source(&b.sources).is_some() || first_remote_source(&b.work_sources).is_some()
}

fn first_remote_source(sources: &[String]) -> Option<&str> {
    sources
        .iter()
        .find(|s| !s.starts_with("file://"))
        .map(String::as_str)
}

/// tracker 探测成功且版本变新 → **原子全量替换**：version + sources + work_sources 全部
/// 用探测出的清单覆盖（旧值丢弃，空列表也写键——lpkg 默认形态，noto 等 work_sources-only
/// 包规范化为 `"sources": []`）。调用方保证只在 vercmp Greater 时应用。
fn apply_version_update(value: &mut serde_json::Value, prop: &lankefarm::track::Proposal) {
    value["version"] = serde_json::Value::String(prop.new_version.clone());
    value["sources"] = serde_json::Value::Array(
        prop.sources
            .iter()
            .map(|s| serde_json::Value::String(s.clone()))
            .collect(),
    );
    value["work_sources"] = serde_json::Value::Array(
        prop.work_sources
            .iter()
            .map(|s| serde_json::Value::String(s.clone()))
            .collect(),
    );
}

/// 应用提案到 LankeBUILD.json：原子全量替换 version + sources + work_sources。返回是否成功写入。
/// `cmd_track_run`（单包 --run）与 `cmd_track_all --run`（批量）共用。
fn apply_proposal(pkg: &str, pkgs_dir: &Path, p: &lankefarm::track::Proposal) -> bool {
    let json_path = lankefarm::build::recipe_json_path(pkgs_dir, pkg);
    let Ok(content) = std::fs::read_to_string(&json_path) else {
        return false;
    };
    let Ok(mut value) = serde_json::from_str::<serde_json::Value>(&content) else {
        return false;
    };
    apply_version_update(&mut value, p);
    match std::fs::write(&json_path, serde_json::to_string_pretty(&value).unwrap()) {
        Err(e) => {
            error_log!("{}", lankefarm::tr!("track.apply_fail", e));
            false
        }
        Ok(()) => {
            println!("{}", lankefarm::tr!("track.applied", pkg, p.new_version));
            true
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
                match TrackerConfig::from_yaml(&content) {
                    Ok(cfg) => {
                        map.insert(cfg.pkg_name.clone(), cfg);
                    }
                    // 解析失败必须**可见**：`if let Ok` 静默跳过时，写错的 tracker 会"看着在、
                    // 实际不生效"（字段拼错/schema 变更后未迁移都会落到这里）。
                    Err(e) => eprintln!(
                        "{}",
                        lankefarm::tr!("track.parse_fail_ignored", path.display(), e)
                    ),
                }
            }
        }
    }
    map
}

/// 构建带 token 的 RealFetcher：CLI `--token`/`--gitlab-token` 优先，环境变量 `GITHUB_TOKEN`/`GITLAB_TOKEN` 兜底。
/// 消除 GitHub/GitLab API 限流 403 噪音（未认证 GitHub API 60 次/小时，认证 5000 次/小时）。
fn build_fetcher(args: &TrackArgs) -> RealFetcher {
    RealFetcher::new(
        args.token
            .clone()
            .or_else(|| std::env::var("GITHUB_TOKEN").ok()),
        args.gitlab_token
            .clone()
            .or_else(|| std::env::var("GITLAB_TOKEN").ok()),
    )
}

/// 单/多包：LankeBUILD.json（包来源）→ 按 name 字段找 tracker → 探测 → 生成新版（全部源升级）。
/// 支持多个包名（按传入顺序逐个探测），tracker 集与 fetcher 复用。
/// 操作命令（会解包/重打包/写 out/ 的命令）必须以 root 运行：`.lpkg` 解包/重打包要读写 root
/// 属主文件与 SUID/SGID（不再走 sudo 降级）。非 root → 打印 i18n 提示并返回错误码（调用方 `return`）。
fn ensure_root() -> Option<ExitCode> {
    if lankefarm::scan::running_as_root() {
        None
    } else {
        eprintln!("{}", lankefarm::tr!("cli.need_root"));
        Some(ExitCode::from(2))
    }
}

pub fn run() -> ExitCode {
    use clap::{CommandFactory, FromArgMatches};
    // 英文环境下覆盖 clap 帮助文本（doc comment 是中文，运行时按 LANG 替换）
    let cli = match Cli::from_arg_matches(&help_en::localize_help(Cli::command()).get_matches()) {
        Ok(c) => c,
        Err(e) => e.exit(),
    };
    if let Err(e) = log_init(cli.log_output.as_deref().and_then(|p| p.to_str())) {
        eprintln!("{e}");
        return ExitCode::from(2);
    }
    match cli.command {
        Command::Build(a) => build::cmd_build(&a),
        Command::Validate(a) => build::cmd_validate(&a),
        Command::AbiFix(a) => build::cmd_abifix(&a),
        Command::Export(a) => export::cmd_export(&a),
        Command::Chk { sub } => match sub {
            ChkCommand::Qml { chk } => custom_checks::cmd_run(&chk, "qml"),
            ChkCommand::Pkgconf { chk } => custom_checks::cmd_run(&chk, "pkgconf"),
            ChkCommand::PkgErr { chk } => custom_checks::cmd_run(&chk, "pkg-err"),
            ChkCommand::Introspection { chk } => custom_checks::cmd_run(&chk, "introspection"),
            ChkCommand::Vapi { chk } => custom_checks::cmd_run(&chk, "vapi"),
            ChkCommand::BuildDeps { chk } => custom_checks::cmd_run(&chk, "build-deps"),
            ChkCommand::Pycache { chk } => custom_checks::cmd_run(&chk, "pycache"),
            ChkCommand::Hook { chk } => custom_checks::cmd_run(&chk, "hook"),
            ChkCommand::Abi { chk } => custom_checks::cmd_run(&chk, "abi"),
            ChkCommand::Full { chk } => custom_checks::cmd_fullchk(&chk),
        },
        Command::Track(a) => {
            if a.all {
                // --all：只出提案；--all --run：批量应用
                track::cmd_track_all(&a, a.run, a.probe_fail_continue, None)
            } else if a.pkg.len() > 1 {
                // 多包名：走并行 worker 池（-j 生效），restrict 到指定包
                track::cmd_track_all(&a, a.run, a.probe_fail_continue, Some(&a.pkg))
            } else {
                // 单包：--run 应用新版；缺省只读出提案（不写 LankeBUILD.json）
                track::cmd_track_run(&a, a.run, a.probe_fail_continue)
            }
        }
        Command::GenTrackers(a) => gen_trackers::cmd_gen_trackers(&a),
        Command::Serve(a) => serve::cmd_serve(&a),
        Command::Seed(a) => seed::cmd_seed(&a),
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
        let resp = "===\npkg-name: acl\nsources:\n  - tracker-template: github\n    repo: a/b\n    mode: tags\n    tag-prefix: v\n    template: https://example.com/acl-{version}.tar.gz\n===\nnone: alacritty\n===\npkg-name: fake\n";
        let r = gen_trackers::parse_batch_blocks(resp, &batch);
        assert_eq!(r.yamls.len(), 1);
        assert_eq!(r.yamls[0].0, "acl");
        assert_eq!(r.skipped, vec!["alacritty"]);
        assert_eq!(r.hallucinations, vec!["fake"]);
        // alsa-lib 既无 yaml 也无 none → 会在批处理里判为"缺"
    }

    #[test]
    fn parses_chk_introspection_vapi_and_build_deps_subcommands() {
        // 钉死 clap 注册面：新增检則必须能作为 `farm chk <kind>` 解析（子命令名 = 检則 label）
        for (arg, want) in [
            ("introspection", "introspection"),
            ("vapi", "vapi"),
            ("build-deps", "build-deps"),
            ("pycache", "pycache"),
        ] {
            let cli = Cli::try_parse_from(["farm", "chk", arg, "--source", "out"]).unwrap();
            let got = match cli.command {
                Command::Chk { sub } => match sub {
                    ChkCommand::Introspection { .. } => "introspection",
                    ChkCommand::Vapi { .. } => "vapi",
                    ChkCommand::BuildDeps { .. } => "build-deps",
                    ChkCommand::Pycache { .. } => "pycache",
                    _ => panic!("{arg} 应解析为 introspection/vapi/build-deps/pycache 子命令"),
                },
                _ => panic!("应解析为 chk 子命令"),
            };
            assert_eq!(got, want, "{arg} 解析成了 {got}");
        }
    }

    #[test]
    fn parses_abifix_subcommand() {
        // abifix 是独立子命令名（非 clap 默认 kebab 的 abi-fix），带 build/validate 同款容器参数
        let cli =
            Cli::try_parse_from(["farm", "abifix", "--image", "img", "--arch", "x86_64"]).unwrap();
        match cli.command {
            Command::AbiFix(a) => {
                assert_eq!(a.repo.image.as_deref(), Some("img"));
                assert_eq!(a.repo.arch, "x86_64");
                assert_eq!(a.repo.pkgs, PathBuf::from("pkgs"));
                assert_eq!(a.repo.out, PathBuf::from("out"));
            }
            _ => panic!("应解析为 abifix 子命令"),
        }
    }

    #[test]
    fn parses_jobs_flag() {
        let cli = Cli::try_parse_from(["farm", "track", "--all", "-j", "8"]).unwrap();
        match cli.command {
            Command::Track(a) => assert_eq!(a.jobs, Some(8)),
            _ => panic!("应解析为 track 子命令"),
        }
        let cli = Cli::try_parse_from(["farm", "track", "--all"]).unwrap();
        match cli.command {
            Command::Track(a) => assert_eq!(a.jobs, None),
            _ => panic!("应解析为 track 子命令"),
        }
    }

    #[test]
    fn apply_version_update_atomic_full_replace() {
        use lankefarm::track::Proposal;
        // 探测成功且版本变新 → **原子全量替换**：sources/work_sources 全部按清单覆盖，
        // 空列表也写键（noto 等 work_sources-only 包规范化为 "sources": []）。
        let mut value = serde_json::json!({
            "name": "noto",
            "version": "2.004",
            "sources": ["https://old.example.com/stale.tar.gz"],
            "work_sources": ["https://github.com/notofonts/noto-cjk/raw/refs/tags/Sans2.004/Sans/Mono/font.otf"]
        });
        // noto 场景：version-source 指向 work_sources[0]，sources 为空清单
        let prop = Proposal {
            pkg_name: "noto".into(),
            current_version: "2.004".into(),
            new_version: "2.005".into(),
            sources: vec![],
            work_sources: vec![
                "https://github.com/notofonts/noto-cjk/raw/refs/tags/Sans2.005/Sans/Mono/font.otf"
                    .into(),
            ],
            kind: "html-index+script".into(),
        };
        apply_version_update(&mut value, &prop);
        assert_eq!(value["version"], "2.005");
        // 空 sources 也写键（lpkg 默认形态），旧值被清空
        assert_eq!(value["sources"], serde_json::json!([]));
        assert_eq!(
            value["work_sources"],
            serde_json::json!([
                "https://github.com/notofonts/noto-cjk/raw/refs/tags/Sans2.005/Sans/Mono/font.otf"
            ])
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
            Command::Track(a) => {
                assert_eq!(a.token.as_deref(), Some("ghp_xxx"));
                assert_eq!(a.gitlab_token.as_deref(), Some("glpat_yyy"));
            }
            _ => panic!("应解析为 track 子命令"),
        }
    }

    #[test]
    fn track_single_without_run_is_readonly_probe() {
        // 只读单包探测：track <pkg>（无 --run）→ pkg 有值、run=false、all=false
        let cli = Cli::try_parse_from(["farm", "track", "gtk3", "--pkgs", "../pkgs"]).unwrap();
        match cli.command {
            Command::Track(a) => {
                assert_eq!(a.pkg, vec!["gtk3".to_string()]);
                assert!(!a.run);
                assert!(!a.all);
            }
            _ => panic!("应解析为 track 子命令"),
        }
        // --run 应用新版
        let cli = Cli::try_parse_from(["farm", "track", "gtk3", "--run"]).unwrap();
        match cli.command {
            Command::Track(a) => {
                assert_eq!(a.pkg, vec!["gtk3".to_string()]);
                assert!(a.run);
                assert!(!a.all);
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
            Command::Track(a) => {
                assert!(a.all);
                assert!(a.run);
            }
            _ => panic!("应解析为 track 子命令"),
        }
    }

    #[test]
    fn parse_yaml_docs_splits_and_strips_fences() {
        let resp = "```\n===\na: 1\n```\n===\nb: 2\n";
        let docs = gen_trackers::parse_yaml_docs(resp);
        assert_eq!(docs.len(), 2);
        assert!(docs[0].contains("a: 1"));
        assert!(docs[1].contains("b: 2"));
    }

    #[test]
    fn ensure_root_matches_euid() {
        // 与环境一致地确定：root → 放行(None)；非 root → 拒绝(Some)。CI（非 root）与本地 root 都能跑。
        let is_root = lankefarm::scan::running_as_root();
        match super::ensure_root() {
            None => assert!(is_root, "非 root 不应放行"),
            Some(_) => assert!(!is_root, "root 不应被拒"),
        }
    }
}
