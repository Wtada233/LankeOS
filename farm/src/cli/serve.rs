use super::Args;
use std::path::PathBuf;
use std::process::ExitCode;

pub(crate) fn cmd_serve(args: &Args) -> ExitCode {
    let root = args
        .root
        .clone()
        .unwrap_or_else(|| PathBuf::from("out/repo"));
    let port = args.port.unwrap_or(8000);
    match lankefarm::serve::serve("0.0.0.0", &root, port) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(2)
        }
    }
}
