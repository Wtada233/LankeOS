use super::SeedArgs;
use std::process::ExitCode;

pub(crate) fn cmd_seed(args: &SeedArgs) -> ExitCode {
    if let Some(code) = super::ensure_root() {
        return code;
    }
    let jobs = args
        .jobs
        .or_else(|| std::thread::available_parallelism().ok().map(|n| n.get()))
        .unwrap_or(4);
    match lankefarm::seed::seed(&args.remote, &args.arch, &args.out, jobs) {
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
