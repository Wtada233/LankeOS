//! pkg-errchk — 打包错误检测（CLAUDE.md 铁律护栏）：
//! - `usr/etc/` / `usr/var/`：打包时把 PREFIX 拼进了系统根（`/etc`、`/var` 才对，不应有 usr/etc）→ 错位。
//! - `.la`（libtool 存档）：已废弃，禁止进入 .lpkg。
//! - `.a`（静态库）：一般应删，除非包 .cmake 配置引用了它（如 LLVM/aom）；发现即报，operator 用
//!   `IGNORE_CHK_PKGERR` 豁免确需保留静态库的包。

use super::{walk_all, ChkOpts, Finding, Report, Severity};
use crate::error::FarmError;
use std::collections::HashSet;
use std::path::Path;

fn rel_of(path: &Path, root: &Path) -> String {
    path.strip_prefix(root)
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_else(|_| path.display().to_string())
}

fn analyze(extract: &Path) -> Result<serde_json::Value, FarmError> {
    let content = extract.join("content");
    let mut paths: Vec<(String, String)> = Vec::new();
    for f in super::collect_files(&content) {
        let rel = rel_of(&f, &content);
        if rel.starts_with("usr/etc/") || rel.starts_with("usr/var/") {
            paths.push((rel, "misplaced-root".into()));
        } else if rel.ends_with(".la") {
            paths.push((rel, "libtool-archive".into()));
        } else if rel.ends_with(".a") {
            paths.push((rel, "static-lib".into()));
        }
    }
    Ok(serde_json::json!({ "paths": paths }))
}

/// 跑 pkg-errchk。
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
        if super::is_ignored(&opts.pkgs_dir, pkg, "PKGERR") {
            continue;
        }
        report.checked += 1;
        let Some(paths) = a["paths"].as_array() else {
            continue;
        };
        let mut items: Vec<Finding> = Vec::new();
        for p in paths {
            let (Some(file), Some(kind)) = (p[0].as_str(), p[1].as_str()) else {
                continue;
            };
            let what = match kind {
                "misplaced-root" => {
                    "错位：应为 /etc 或 /var（无 usr 前缀），不能有 usr/etc|usr/var".into()
                }
                "libtool-archive" => "包含 .la（libtool 存档，已废弃，应删）".into(),
                "static-lib" => {
                    "包含 .a 静态库（一般应删；若 .cmake 引用则用 IGNORE_CHK_PKGERR 豁免）".into()
                }
                _ => kind.into(),
            };
            items.push(Finding {
                file: file.to_string(),
                what,
                severity: Severity::Warning,
            });
        }
        if !items.is_empty() {
            report.findings.insert(pkg.clone(), items);
        }
    }
    Ok(report)
}
