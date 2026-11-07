//! /proc/cmdline 解析。

pub fn parse(cmdline: &str) -> Vec<&str> {
    cmdline.split_whitespace().collect()
}

pub fn has_arg(cmdline: &str, arg: &str) -> bool {
    parse(cmdline).iter().any(|a| *a == arg)
}

pub fn read() -> String {
    std::fs::read_to_string("/proc/cmdline").unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::{has_arg, parse};

    #[test]
    fn splits_and_matches() {
        let cmd = "BOOT_IMAGE=/boot/vmlinuz-lanke loglevel=3 live toram";
        assert_eq!(parse(cmd).len(), 4);
        assert!(has_arg(cmd, "live"));
        assert!(has_arg(cmd, "toram"));
        assert!(!has_arg(cmd, "erase"));
        assert!(!has_arg(cmd, "liv"));
    }
}
