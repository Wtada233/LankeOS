//! prompt.rs — BLOCKED/源缺失的交互接管（开 shell / 跳过 / 结束）。

use crate::tr;
use crate::ux;
use super::BuildOptions;

pub(crate) enum PromptChoice {
    Retry,
    Skip,
    End,
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

