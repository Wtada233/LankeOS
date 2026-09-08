use super::ServeArgs;
use std::process::ExitCode;

pub(crate) fn cmd_serve(args: &ServeArgs) -> ExitCode {
    match lankefarm::serve::serve("0.0.0.0", &args.root, args.port) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::from(2)
        }
    }
}
