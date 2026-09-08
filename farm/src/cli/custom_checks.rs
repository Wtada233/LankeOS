use std::path::PathBuf;
use std::process::ExitCode;

use clap::Args as ClapArgs;
use lankefarm::custom_checks::{self, ChkOpts, Report};

/// fullchk/qmlchk/pkgconfchk/pkg-errchk/hookchk 共用选项（与 manual-abi-fullchk 同构）。
#[derive(Debug, Clone, ClapArgs)]
#[command(next_help_heading = "check 选项")]
pub struct ChkArgs {
    /// 构建仓库根（含 `<arch>/`，所有包建 provider/缓存）[default: out]
    #[arg(long, default_value = "out")]
    pub source: PathBuf,
    /// 架构（读取 source/<arch>/）
    #[arg(long, default_value = "x86_64")]
    pub arch: String,
    /// 配方根（读 LankeBUILD.json 的 deps/farm_flags）[default: pkgs]
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

fn finish(r: Result<Report, String>) -> ExitCode {
    match r {
        Ok(_) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(2)
        }
    }
}

pub(crate) fn cmd_qmlchk(a: &ChkArgs) -> ExitCode {
    let cache = a
        .cache
        .clone()
        .unwrap_or_else(|| custom_checks::default_cache_dir(&a.source, "qmlchk"));
    let r = lankefarm::custom_checks::qml::run(&opts_for("qmlchk", a, cache));
    if let Ok(rr) = &r {
        print_report("qmlchk", rr);
    }
    finish(r)
}

pub(crate) fn cmd_pkgconfchk(a: &ChkArgs) -> ExitCode {
    let cache = a
        .cache
        .clone()
        .unwrap_or_else(|| custom_checks::default_cache_dir(&a.source, "pkgconfchk"));
    let r = lankefarm::custom_checks::pkgconf::run(&opts_for("pkgconfchk", a, cache));
    if let Ok(rr) = &r {
        print_report("pkgconfchk", rr);
    }
    finish(r)
}

pub(crate) fn cmd_pkg_errchk(a: &ChkArgs) -> ExitCode {
    let cache = a
        .cache
        .clone()
        .unwrap_or_else(|| custom_checks::default_cache_dir(&a.source, "pkg-errchk"));
    let r = lankefarm::custom_checks::pkg_err::run(&opts_for("pkg-errchk", a, cache));
    if let Ok(rr) = &r {
        print_report("pkg-errchk", rr);
    }
    finish(r)
}

pub(crate) fn cmd_hookchk(a: &ChkArgs) -> ExitCode {
    let cache = a
        .cache
        .clone()
        .unwrap_or_else(|| custom_checks::default_cache_dir(&a.source, "hookchk"));
    let r = lankefarm::custom_checks::hook::run(&opts_for("hookchk", a, cache));
    if let Ok(rr) = &r {
        print_report("hookchk", rr);
    }
    finish(r)
}

/// 用同一组参数跑全部 chk：abichk(manual-abi-fullchk) + qmlchk + pkgconfchk + pkg-errchk + hookchk。
pub(crate) fn cmd_fullchk(a: &ChkArgs) -> ExitCode {
    let base = a.cache.clone().unwrap_or_else(|| {
        std::env::var("HOME")
            .map(|h| PathBuf::from(h).join(".cache/lankefarm"))
            .unwrap_or_else(|_| a.source.join(".abi-cache"))
    });
    let mut bad = false;
    // 每个检則独立 cache 子目录
    let run_one = |opts_cache: PathBuf| opts_for("", a, opts_cache);
    let qml = lankefarm::custom_checks::qml::run(&run_one(base.join("qmlchk")));
    if let Ok(r) = &qml {
        print_report("qmlchk", r);
    } else {
        bad = true;
    }
    let pc = lankefarm::custom_checks::pkgconf::run(&run_one(base.join("pkgconfchk")));
    if let Ok(r) = &pc {
        print_report("pkgconfchk", r);
    } else {
        bad = true;
    }
    let pe = lankefarm::custom_checks::pkg_err::run(&run_one(base.join("pkg-errchk")));
    if let Ok(r) = &pe {
        print_report("pkg-errchk", r);
    } else {
        bad = true;
    }
    let hk = lankefarm::custom_checks::hook::run(&run_one(base.join("hookchk")));
    if let Ok(r) = &hk {
        print_report("hookchk", r);
    } else {
        bad = true;
    }
    let abi = lankefarm::abi_fullchk::run_fullchk(&lankefarm::abi_fullchk::FullchkOpts {
        source: a.source.clone(),
        arch: a.arch.clone(),
        cache: base.join("abi"),
        pkgs: a.pkg.clone(),
        pkgs_dir: resolve_pkgs_dir(a),
        full_rescan: a.full_rescan,
    });
    match abi {
        Ok(r) => {
            // abi 报告用它的字段打印
            println!(
                "{}",
                lankefarm::tr!(
                    "abi_fullchk.summary",
                    r.lpkg_files.to_string(),
                    r.elf_files.to_string(),
                    r.cache_hits.to_string(),
                    r.cache_misses.to_string(),
                    r.provider_sonames.to_string()
                )
            );
            if r.missing.is_empty() {
                println!("{}", lankefarm::tr!("abi_fullchk.none"));
            } else {
                let mut total = 0usize;
                for (pkg, items) in &r.missing {
                    println!(
                        "{}",
                        lankefarm::tr!("abi_fullchk.missing_pkg", pkg, items.len().to_string())
                    );
                    for it in items {
                        println!(
                            "{}",
                            lankefarm::tr!(
                                "abi_fullchk.missing_item",
                                it.elf,
                                format!(
                                    "{}{}",
                                    lankefarm::custom_checks::sev_marker(it.severity),
                                    it.name
                                ),
                                it.ver,
                                it.candidates.join(",")
                            )
                        );
                    }
                    total += items.len();
                }
                println!(
                    "{}",
                    lankefarm::tr!(
                        "abi_fullchk.total",
                        total.to_string(),
                        r.missing.len().to_string()
                    )
                );
            }
        }
        Err(e) => {
            eprintln!("{e}");
            bad = true;
        }
    }
    if bad {
        ExitCode::from(2)
    } else {
        ExitCode::SUCCESS
    }
}
