use super::Args;
use std::path::PathBuf;
use std::process::ExitCode;

pub(crate) fn cmd_seed(args: &Args) -> ExitCode {
    let remote = match &args.remote {
        Some(r) => r.clone(),
        None => {
            eprintln!("{}", lankefarm::tr!("seed.usage"));
            return ExitCode::from(2);
        }
    };
    let arch = args.arch.clone().unwrap_or_else(|| "x86_64".to_string());
    let out = args.out.clone().unwrap_or_else(|| PathBuf::from("out"));
    let jobs = args
        .jobs
        .or_else(|| std::thread::available_parallelism().ok().map(|n| n.get()))
        .unwrap_or(4);
    match lankefarm::seed::seed(&remote, &arch, &out, jobs) {
        Ok(report) => {
            println!();
            println!(
                "{}",
                lankefarm::tr!("seed.summary", report.total, report.ok, report.failed.len())
            );
            for (p, why) in &report.failed {
                eprintln!("{}", lankefarm::tr!("seed.failed_item", p, why));
            }
            if report.failed.is_empty() {
                ExitCode::SUCCESS
            } else {
                ExitCode::from(1)
            }
        }
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(2)
        }
    }
}
