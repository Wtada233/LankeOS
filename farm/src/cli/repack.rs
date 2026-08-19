use std::path::PathBuf;
use std::process::ExitCode;

use lankefarm::repack;
use lankefarm::ux;

use super::Args;

/// repack：`<input>/<arch>/<pkg>/*.lpkg` 用 zstd -22 --ultra 重打包（原位替换），
/// 并把新 SHA256 写回 index.txt。
pub(crate) fn cmd_repack(args: &Args) -> ExitCode {
    let input = args.input.clone().unwrap_or_else(|| PathBuf::from("out"));
    let arch = args.arch.clone().unwrap_or_else(|| "x86_64".to_string());
    let Some(pkg) = args.pkg.first() else {
        eprintln!("{}", lankefarm::tr!("repack.no_pkg"));
        return ExitCode::from(2);
    };

    match repack::repack_in_repo(&input, &arch, pkg) {
        Ok(items) => {
            for item in &items {
                println!(
                    "{}",
                    ux::green(&lankefarm::tr!(
                        "repack.done",
                        format!("{}-{}", pkg, item.version),
                        item.sha256
                    ))
                );
            }
            println!(
                "{}",
                lankefarm::tr!(
                    "repack.summary",
                    pkg,
                    ux::green(&lankefarm::tr!("repack.ok", items.len())),
                    input.join(&arch).join("index.txt").display()
                )
            );
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(2)
        }
    }
}
