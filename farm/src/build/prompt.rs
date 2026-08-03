//! prompt.rs — BLOCKED/源缺失的交互接管（开 shell / 跳过 / 结束）+ 构建计划预览/确认。

use std::collections::VecDeque;
use std::io::Write;

use crate::tr;
use crate::ux;
use super::{effective_version, BuildOptions};

pub(crate) enum PromptChoice {
    Retry,
    Skip,
    End,
}

/// 构建计划预览：按 topo 顺序列出待构建包（+ 版本），供 operator 在开始前确认。
/// 只含"最开始能确认需要 build"的初始集；ABI 受害者是动态入队的，不在预览里。
pub(crate) fn print_build_plan(queue: &VecDeque<(String, bool)>, opts: &BuildOptions) {
    println!();
    println!("{}", tr!("build.plan", queue.len()));
    for (pkg, _is_victim) in queue {
        let ver = effective_version(&opts.pkgs_dir, pkg).unwrap_or_else(|| "?".to_string());
        println!("  {pkg}  {ver}");
    }
    println!();
}

/// operator 确认（仅交互模式调用）：回车继续，n 取消；读取失败一律视为继续（避免误取消）。
pub(crate) fn confirm_plan() -> bool {
    eprint!("{}", tr!("build.plan_confirm"));
    let _ = std::io::stdout().flush();
    let mut input = String::new();
    match std::io::stdin().read_line(&mut input) {
        Ok(_) => !input.trim().eq_ignore_ascii_case("n"),
        Err(_) => true,
    }
}


pub(crate) fn prompt_blocked(pkg: &str, opts: &BuildOptions, stage: &str) -> PromptChoice {
    loop {
        eprintln!();
        eprintln!("  {}", ux::red(&tr!("build.blocked", pkg, stage)));
        eprintln!("{}", tr!("build.prompt"));
        let mut input = String::new();
        match std::io::stdin().read_line(&mut input) {
            Ok(_) => match input.trim() {
                "1" => {
                    open_shell(pkg, opts);
                    return PromptChoice::Retry;
                }
                "2" => return PromptChoice::Skip,
                "3" => return PromptChoice::End,
                _ => eprintln!("{}", tr!("build.prompt_invalid")),
            },
            Err(_) => return PromptChoice::End,
        }
    }
}

/// 打开**宿主 shell**（pkgs/<pkg>/）让 operator 修复配方/源——修改持久化，
/// 退出后"继续构建"（docker cp 重新拷入修复后的配方）才真正生效。
/// 绝不开新容器（容器易失，改了等于没改）。
fn open_shell(pkg: &str, opts: &BuildOptions) {
    let workdir = opts.pkgs_dir.join(pkg);
    eprintln!("{}", tr!("build.fix_shell", workdir.display()));
    let _ = std::process::Command::new("bash").current_dir(&workdir).status();
    println!("{}", tr!("build.fix_shell_exit", pkg));
}

