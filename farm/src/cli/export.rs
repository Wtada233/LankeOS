use super::Args;
use lankefarm::export;
use std::path::PathBuf;
use std::process::ExitCode;

/// export：input（构建仓库，默认 out）→ 扁平化 `<pkg>-<ver>.lpkg`（zstd level 22）到 output。
pub(crate) fn cmd_export(args: &Args) -> ExitCode {
    let input = args.input.clone().unwrap_or_else(|| PathBuf::from("out"));
    let output = args.out.clone().unwrap_or_else(|| PathBuf::from("export"));
    let arch = args.arch.clone().unwrap_or_else(|| "x86_64".to_string());

    match export::export(&input, &output, &arch) {
        Ok(report) => {
            println!(
                "{}",
                lankefarm::tr!(
                    "export.summary",
                    lankefarm::ux::green(&lankefarm::tr!("export.exported", report.exported.len())),
                    lankefarm::ux::red(&lankefarm::tr!("export.failed", report.failed.len()))
                )
            );
            for f in &report.failed {
                eprintln!(
                    "  {}",
                    lankefarm::ux::red(&lankefarm::tr!("export.failed_item", f))
                );
            }
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(2)
        }
    }
}
