//! LankeOS build farm — 薄入口：参数解析/分发在 `cli` 模块。

mod cli;

fn main() -> std::process::ExitCode {
    cli::run()
}
