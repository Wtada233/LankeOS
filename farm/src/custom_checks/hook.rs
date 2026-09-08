//! hookchk — postinst hook 检测（CLAUDE.md sysusers/tmpfiles 铁律）：
//! 内容含 `usr/lib/sysusers.d/*.conf` → postinst 必须调 `systemd-sysusers`；
//! 含 `usr/lib/tmpfiles.d/*.conf` → postinst 必须调 `systemd-tmpfiles --create`。
//! postinst 文本在 .lpkg 的 `hooks/postinst.sh`（不是 LankeBUILD.json）。只查"建了档案却没自动跑"，
//! 合并/去重/bump 的写法是 operator 的事。

use super::{walk_all, ChkOpts, Finding, Report, Severity};
use crate::error::FarmError;
use std::collections::HashSet;
use std::path::Path;

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
    let (analyses, hits, misses, failed) = walk_all(opts, |ext, _pkg| analyze(ext))?;
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
        if a["sysusers"].as_bool().unwrap_or(false) && !postinst.contains("systemd-sysusers") {
            items.push(Finding {
                file: "usr/lib/sysusers.d/*.conf".into(),
                what: "建了 sysusers 档案但 postinst 未调 systemd-sysusers".into(),
                severity: Severity::Warning,
            });
        }
        if a["tmpfiles"].as_bool().unwrap_or(false)
            && !postinst.contains("systemd-tmpfiles --create")
        {
            items.push(Finding {
                file: "usr/lib/tmpfiles.d/*.conf".into(),
                what: "建了 tmpfiles 档案但 postinst 未调 systemd-tmpfiles --create".into(),
                severity: Severity::Warning,
            });
        }
        if !items.is_empty() {
            report.findings.insert(pkg.clone(), items);
        }
    }
    Ok(report)
}
