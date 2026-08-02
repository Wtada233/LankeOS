//! ux.rs — 终端输出格式化（ANSI 颜色）。
//!
//! 非 TTY（管道/日志/CI）或设了 `NO_COLOR` 时自动降级为纯文本，不污染日志。
//! 视觉层级：成功 → 绿、信息 → 灰、警告 → 黄、错误 → 红。所有颜色都带 `\x1b[0m` reset。
//!
//! 文案在 i18n.rs，本模块只管颜色；`paint`/`enabled_on` 是纯函数，供测试。

use std::io::IsTerminal;

/// 纯函数：颜色是否启用 = 是 TTY 且未设 `NO_COLOR`。
pub fn enabled_on(is_tty: bool, has_no_color: bool) -> bool {
    is_tty && !has_no_color
}

/// 纯函数：给文本上 ANSI 色（含 reset）。`on=false` 时原样返回。
pub fn paint(code: &str, s: &str, on: bool) -> String {
    if on {
        format!("\x1b[{code}m{s}\x1b[0m")
    } else {
        s.to_string()
    }
}

fn enabled() -> bool {
    enabled_on(std::io::stdout().is_terminal(), std::env::var("NO_COLOR").is_ok())
}

macro_rules! paint {
    ($name:ident, $code:literal) => {
        pub fn $name(s: &str) -> String {
            paint($code, s, enabled())
        }
    };
}

paint!(green, "32");
paint!(yellow, "33");
paint!(red, "31");
paint!(cyan, "36");
paint!(bold, "1");
paint!(dim, "2");

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paint_emits_ansi_with_reset_when_enabled() {
        assert_eq!(paint("32", "ok", true), "\x1b[32mok\x1b[0m");
        assert_eq!(paint("1", "bold", true), "\x1b[1mbold\x1b[0m");
    }

    #[test]
    fn paint_plain_when_disabled() {
        assert_eq!(paint("32", "ok", false), "ok");
        assert_eq!(paint("1", "bold", false), "bold");
    }

    #[test]
    fn enabled_requires_tty_and_no_no_color() {
        assert!(enabled_on(true, false), "TTY + 无 NO_COLOR → 上色");
        assert!(!enabled_on(false, false), "非 TTY → 不上色");
        assert!(!enabled_on(true, true), "NO_COLOR → 不上色");
        assert!(!enabled_on(false, true));
    }

    #[test]
    fn helpers_never_leak_unescaped_color() {
        // 无论 TTY 与否：要么纯文本（非 TTY 降级），要么 ANSI 以 \x1b[0m 收尾——
        // 绝不以未闭合的色码结尾（防配色泄漏到后续输出）。
        for out in [green("x"), yellow("x"), red("x"), cyan("x"), bold("x"), dim("x")] {
            if out.starts_with("\x1b[") {
                assert!(out.ends_with("\x1b[0m"), "缺 reset: {out:?}");
            }
        }
    }
}
