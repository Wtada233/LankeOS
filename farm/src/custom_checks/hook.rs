//! hookchk — postinst hook 检测（CLAUDE.md sysusers/tmpfiles 铁律）：
//! 内容含 `usr/lib/sysusers.d/*.conf` → postinst 必须调 `systemd-sysusers`；
//! 含 `usr/lib/tmpfiles.d/*.conf` → postinst 必须调 `systemd-tmpfiles --create`。
//! postinst 文本在 .lpkg 的 `hooks/postinst.sh`（不是 LankeBUILD.json）。只查"建了档案却没自动跑"，
//! 合并/去重/bump 的写法是 operator 的事。

use super::{walk_all, ChkOpts, Finding, Report, Severity};
use crate::error::FarmError;
use crate::tr;
use std::collections::HashSet;
use std::path::Path;

/// 本检則 analysis 结构版本：只在 **hook** 分析逻辑变化时递增（判定逻辑不入缓存，不在此列）。
const SCHEMA: u32 = 5;

/// postinst 文本里是否存在**非注释行**包含 `needle`（行首可选空白后为 `#` 即整行注释）。
/// 历史事故：旧实现 `postinst.contains(...)` 被注释里的字样骗过（如 `# 用 systemd-sysusers`）→
/// 误判"已调用"→ 漏报（包建了 sysusers.d 却没自动建用户/目录）。
/// 只处理**行首注释**：shell 的行内 `#` 与引号内 `#` 不做解析——配方约定 hook 命令独立成行。
fn has_active_line(postinst: &str, needle: &str) -> bool {
    postinst
        .lines()
        .map(str::trim_start)
        .filter(|l| !l.starts_with('#'))
        .any(|l| l.contains(needle))
}

fn analyze(extract: &Path) -> Result<serde_json::Value, FarmError> {
    let content = extract.join("content");
    let mut sysusers = false;
    let mut tmpfiles = false;
    for f in super::collect_files(&content) {
        let rel = f
            .strip_prefix(&content)
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default();
        if rel.starts_with("usr/lib/sysusers.d/") && rel.ends_with(".conf") {
            sysusers = true;
        }
        if rel.starts_with("usr/lib/tmpfiles.d/") && rel.ends_with(".conf") {
            tmpfiles = true;
        }
    }
    let postinst = std::fs::read_to_string(extract.join("hooks/postinst.sh")).unwrap_or_default();
    Ok(serde_json::json!({
        "sysusers": sysusers,
        "tmpfiles": tmpfiles,
        "postinst": postinst,
    }))
}

/// 跑 hookchk。
pub fn run(opts: &ChkOpts) -> Result<Report, FarmError> {
    let (analyses, hits, misses, failed) = walk_all(opts, SCHEMA, |ext, _pkg| analyze(ext))?;
    let audit_all = opts.subset.is_empty();
    let audit: HashSet<&str> = opts.subset.iter().map(String::as_str).collect();
    let mut report = Report {
        cache_hits: hits,
        cache_misses: misses,
        failed,
        ..Default::default()
    };
    for (pkg, a) in &analyses {
        if !(audit_all || audit.contains(pkg.as_str())) {
            continue;
        }
        if super::is_ignored(&opts.pkgs_dir, pkg, "HOOK") {
            continue;
        }
        report.checked += 1;
        let postinst = a["postinst"].as_str().unwrap_or("");
        let mut items: Vec<Finding> = Vec::new();
        if a["sysusers"].as_bool().unwrap_or(false)
            && !has_active_line(postinst, "systemd-sysusers")
        {
            items.push(Finding {
                file: "usr/lib/sysusers.d/*.conf".into(),
                what: tr!("chk.hook.sysusers").to_string(),
                severity: Severity::Warning,
            });
        }
        if a["tmpfiles"].as_bool().unwrap_or(false)
            && !has_active_line(postinst, "systemd-tmpfiles --create")
        {
            items.push(Finding {
                file: "usr/lib/tmpfiles.d/*.conf".into(),
                what: tr!("chk.hook.tmpfiles").to_string(),
                severity: Severity::Warning,
            });
        }
        if !items.is_empty() {
            report.findings.insert(pkg.clone(), items);
        }
    }
    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn has_active_line_ignores_commented_out_calls() {
        // 注释行里的字样不算"已调用"（漏报根因）
        assert!(!has_active_line("# systemd-sysusers\n", "systemd-sysusers"));
        assert!(!has_active_line(
            "  #\tsystemd-tmpfiles --create\n",
            "systemd-tmpfiles --create"
        ));
        // 真调用命中
        assert!(has_active_line(
            "#!/bin/sh\nsystemd-sysusers\n",
            "systemd-sysusers"
        ));
        assert!(has_active_line(
            "systemd-tmpfiles --create\n",
            "systemd-tmpfiles --create"
        ));
        // 行内注释（命令前的 `#` 只处理行首）——配方约定命令独立成行，此处不解析行内
        assert!(has_active_line(
            "foo # systemd-sysusers\n",
            "systemd-sysusers"
        ));
        // 空文本
        assert!(!has_active_line("", "systemd-sysusers"));
    }
}
