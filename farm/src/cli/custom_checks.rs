use lankefarm::error::FarmError;
use std::path::PathBuf;
use std::process::ExitCode;

use clap::Args as ClapArgs;
use lankefarm::custom_checks::{self, ChkOpts, Report};

/// `farm chk` 各检則（qml/pkgconf/pkg-err/hook/abi/full）共用选项。
#[derive(Debug, Clone, ClapArgs)]
#[command(next_help_heading = "check 选项")]
pub struct ChkArgs {
    /// 构建仓库根（含 `<arch>/`，所有包建 provider/缓存）
    #[arg(long, default_value = "out")]
    pub source: PathBuf,
    /// 架构（读取 source/<arch>/）
    #[arg(long, default_value = "x86_64")]
    pub arch: String,
    /// 配方根（读 LankeBUILD.json 的 deps/farm_flags）
    #[arg(long, default_value = "pkgs")]
    pub pkgs_dir: PathBuf,
    /// 缓存根（每个检則一个子目录；默认 ~/.cache/lankefarm-<chk>）
    #[arg(long)]
    pub cache: Option<PathBuf>,
    /// 只检查/报告这些包（provider 仍来自 --source 全量）
    #[arg(long, num_args = 1..)]
    pub pkg: Vec<String>,
    /// 忽略缓存强制全量重扫
    #[arg(long)]
    pub full_rescan: bool,
}

/// 配方目录：显式给的优先；默认 `pkgs` 在当前目录不存在时回落 monorepo 布局 `../pkgs`
/// （farm/ 常驻仓库根，配方在 ../pkgs——不回落则读不到 deps/farm_flags，豁免和依赖检查全失效）。
fn resolve_pkgs_dir(a: &ChkArgs) -> PathBuf {
    if a.pkgs_dir.is_dir() {
        return a.pkgs_dir.clone();
    }
    if a.pkgs_dir == PathBuf::from("pkgs") {
        let alt = PathBuf::from("../pkgs");
        if alt.is_dir() {
            return alt;
        }
    }
    a.pkgs_dir.clone()
}

fn opts_for(_label: &str, a: &ChkArgs, cache: PathBuf) -> ChkOpts {
    ChkOpts {
        source: a.source.clone(),
        arch: a.arch.clone(),
        cache,
        pkgs_dir: resolve_pkgs_dir(a),
        subset: a.pkg.clone(),
        full_rescan: a.full_rescan,
    }
}

fn print_report(label: &str, r: &Report) {
    println!(
        "{}",
        lankefarm::tr!(
            "chk.summary",
            label,
            r.checked.to_string(),
            r.cache_hits.to_string(),
            r.cache_misses.to_string()
        )
    );
    let total = r.count();
    if total == 0 {
        println!("{}", lankefarm::tr!("chk.none", label));
    } else {
        for (pkg, items) in &r.findings {
            println!(
                "{}",
                lankefarm::tr!("chk.header", pkg, items.len().to_string())
            );
            for it in items {
                println!(
                    "{}",
                    lankefarm::tr!(
                        "chk.item",
                        it.file,
                        format!(
                            "{}{}",
                            lankefarm::custom_checks::sev_marker(it.severity),
                            it.what
                        )
                    )
                );
            }
        }
        println!("{}", lankefarm::tr!("chk.total", label, total.to_string()));
    }
    for f in &r.failed {
        eprintln!("{}", lankefarm::tr!("chk.failed", f));
    }
}

fn finish(r: Result<Report, FarmError>) -> ExitCode {
    match r {
        Ok(_) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(2)
        }
    }
}

/// 检則定义表：**每一项都是同一个契约**（`fn(&ChkOpts) -> Result<Report, FarmError>`）——
/// qml / pkgconf / pkg-err / hook / abi 同级，`farm chk abi` 与 `farm chk full` 都只是查这张表。
struct ChkDef {
    /// 报告 label（= 子命令名）
    label: &'static str,
    /// 默认缓存目录键（`~/.cache/lankefarm-<cache>`；full 时作 base/<cache> 子目录）
    cache: &'static str,
    run: fn(&ChkOpts) -> Result<Report, FarmError>,
}

const CHECKS: [ChkDef; 5] = [
    ChkDef {
        label: "qml",
        cache: "qmlchk",
        run: lankefarm::custom_checks::qml::run,
    },
    ChkDef {
        label: "pkgconf",
        cache: "pkgconfchk",
        run: lankefarm::custom_checks::pkgconf::run,
    },
    ChkDef {
        label: "pkg-err",
        cache: "pkg-errchk",
        run: lankefarm::custom_checks::pkg_err::run,
    },
    ChkDef {
        label: "hook",
        cache: "hookchk",
        run: lankefarm::custom_checks::hook::run,
    },
    ChkDef {
        label: "abi",
        cache: "abi",
        run: lankefarm::custom_checks::abi::run,
    },
];

fn def(label: &str) -> Option<&'static ChkDef> {
    CHECKS.iter().find(|d| d.label == label)
}

/// 单类检則（`farm chk qml|pkgconf|pkg-err|hook|abi`）：从定义表取 runner，跑 + 打印。
pub(crate) fn cmd_run(a: &ChkArgs, label: &str) -> ExitCode {
    let Some(d) = def(label) else {
        eprintln!("未知检則: {label}");
        return ExitCode::from(2);
    };
    let cache = a
        .cache
        .clone()
        .unwrap_or_else(|| custom_checks::default_cache_dir(&a.source, d.cache));
    let r = (d.run)(&opts_for(d.cache, a, cache));
    if let Ok(rr) = &r {
        print_report(d.label, rr);
    }
    finish(r)
}

/// 一键跑全部（`farm chk full`）：同一张定义表依次跑所有检則，每类独立解包/独立缓存。
pub(crate) fn cmd_fullchk(a: &ChkArgs) -> ExitCode {
    let base = a.cache.clone().unwrap_or_else(|| {
        std::env::var("HOME")
            .map(|h| PathBuf::from(h).join(".cache/lankefarm"))
            .unwrap_or_else(|_| a.source.join(".abi-cache"))
    });
    let mut bad = false;
    for d in &CHECKS {
        match (d.run)(&opts_for(d.cache, a, base.join(d.cache))) {
            Ok(r) => print_report(d.label, &r),
            Err(e) => {
                eprintln!("{e}");
                bad = true;
            }
        }
    }
    if bad {
        ExitCode::from(2)
    } else {
        ExitCode::SUCCESS
    }
}
