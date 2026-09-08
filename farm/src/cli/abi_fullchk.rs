use std::path::PathBuf;
use std::process::ExitCode;

use lankefarm::abi_fullchk::{self, FullchkOpts};

use super::Args;

/// 全 ABI 审计（manual）。非 root 可跑（只读仓库 .lpkg + 写用户缓存目录）。
pub(crate) fn cmd_manual_abi_fullchk(args: &Args) -> ExitCode {
    let source = args.input.clone().unwrap_or_else(|| PathBuf::from("out"));
    let arch = args.arch.clone().unwrap_or_else(|| "x86_64".to_string());
    let cache = args
        .cache
        .clone()
        .unwrap_or_else(|| abi_fullchk::default_cache_dir(&source));
    let pkgs = args.pkg.clone();
    // 配方根：豁免(IGNORE_CHK_ABI)读 LankeBUILD.json farm_flags；默认 pkgs 不存在回落 ../pkgs
    let recipes = args
        .pkgs
        .as_deref()
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("pkgs"));
    let pkgs_dir = if recipes.is_dir() {
        recipes.clone()
    } else if recipes == PathBuf::from("pkgs") {
        let alt = PathBuf::from("../pkgs");
        if alt.is_dir() {
            alt
        } else {
            recipes
        }
    } else {
        recipes
    };
    println!(
        "{}",
        lankefarm::tr!(
            "abi_fullchk.start",
            source.display().to_string(),
            arch,
            cache.display().to_string(),
            if pkgs.is_empty() {
                lankefarm::tr!("abi_fullchk.all").to_string()
            } else {
                pkgs.join(",")
            }
        )
    );
    match abi_fullchk::run_fullchk(&FullchkOpts {
        source,
        arch,
        cache,
        pkgs,
        pkgs_dir,
        full_rescan: args.full_rescan,
    }) {
        Ok(r) => {
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
                    for it in items.iter().take(12) {
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
                    if items.len() > 12 {
                        println!(
                            "{}",
                            lankefarm::tr!("abi_fullchk.more", (items.len() - 12).to_string())
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
            for f in &r.failed {
                eprintln!("{}", lankefarm::tr!("abi_fullchk.failed", f));
            }
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(2)
        }
    }
}
