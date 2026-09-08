//! pkgconfchk — pkg-config 元数据的模块依赖检查。
//!
//! LankeOS 约定：某包 .pc 的 `Requires` / `Requires.private` 里的模块，其**归属包**（提供该
//! `<mod>.pc` 的包）必须在本包运行时可达（`deps` ∪ `needed_so` 链接依赖），否则报缺。needed_so 已
//! 推导到的链接依赖不再要求手写 deps。仓库内无任何包提供的模块（外部）→ 忽略。
//!
//! provider：内容 `usr/lib/pkgconfig/*.pc` / `usr/share/pkgconfig/*.pc` → 模块名 = 文件名去 `.pc`。
//! consumer：每个 `.pc` 的 `Requires:` / `Requires.private:`（逗号分段，取段首 token = 模块名，
//! 剥 `>=` 等版本约束）。

use super::{walk_all, ChkOpts, Report};
use std::collections::{BTreeMap, HashSet};
use std::path::Path;

fn pc_module(file: &Path, content: &Path) -> Option<String> {
    let rel = file.strip_prefix(content).ok()?;
    let s = rel.to_string_lossy();
    if !s.ends_with(".pc") {
        return None;
    }
    // 仅认 pkgconfig 目录下的
    if !(s.contains("/pkgconfig/") || s.starts_with("pkgconfig/")) {
        return None;
    }
    Some(
        file.file_name()?
            .to_string_lossy()
            .trim_end_matches(".pc")
            .to_string(),
    )
}

/// 解析一行 Requires 值：逗号分段，取每段首 token（剥版本约束 `foo >= 1.2` → `foo`）。
/// 容忍内联注释（`.pc` 常见 `Requires: # nettle`——# 后的整段当注释，不产出模块）。
fn parse_requires_line(value: &str, out: &mut Vec<String>) {
    for part in value.split(',') {
        // 注释从 '#' 起截断
        let t = part.split('#').next().unwrap_or("").trim();
        if t.is_empty() {
            continue;
        }
        let modname = t.split_whitespace().next().unwrap_or("").to_string();
        if !modname.is_empty() {
            out.push(modname);
        }
    }
}

fn analyze(extract: &Path) -> Result<serde_json::Value, String> {
    let content = extract.join("content");
    let mut provides: Vec<String> = Vec::new();
    let mut requires: Vec<(String, String)> = Vec::new();
    for f in super::collect_files(&content) {
        let rel = f
            .strip_prefix(&content)
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default();
        if rel.ends_with(".pc") && (rel.contains("/pkgconfig/") || rel.starts_with("pkgconfig/")) {
            if let Some(m) = pc_module(&f, &content) {
                provides.push(m.clone());
            }
            if let Ok(text) = std::fs::read_to_string(&f) {
                let mut mods: Vec<String> = Vec::new();
                for line in text.lines() {
                    let t = line.trim();
                    for key in ["Requires.private:", "Requires:"] {
                        if let Some(rest) = t.strip_prefix(key) {
                            parse_requires_line(rest, &mut mods);
                        }
                    }
                }
                mods.sort();
                mods.dedup();
                for m in mods {
                    requires.push((rel.clone(), m));
                }
            }
        }
    }
    Ok(serde_json::json!({ "provides": provides, "requires": requires }))
}

/// 跑 pkgconfchk。
pub fn run(opts: &ChkOpts) -> Result<Report, String> {
    let (analyses, hits, misses, failed) = walk_all(opts, |ext, _pkg| analyze(ext))?;
    let index = super::load_index(opts);
    let mut module_owners: BTreeMap<String, HashSet<String>> = BTreeMap::new();
    for (pkg, a) in &analyses {
        let Some(prov) = a["provides"].as_array() else {
            continue;
        };
        for m in prov.iter().filter_map(|v| v.as_str()) {
            module_owners
                .entry(m.to_string())
                .or_default()
                .insert(pkg.clone());
        }
    }

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
        if super::is_ignored(&opts.pkgs_dir, pkg, "PKGCONF") {
            continue;
        }
        report.checked += 1;
        let Some(reqs) = a["requires"].as_array() else {
            continue;
        };
        // 三段判定：闭包命中→无；闭包缺但仓库有→Warning；仓库也无→Critical
        let mut reqs_out: Vec<(String, String, HashSet<String>)> = Vec::new();
        for r in reqs {
            let (Some(file), Some(m)) = (r[0].as_str(), r[1].as_str()) else {
                continue;
            };
            reqs_out.push((
                file.to_string(),
                m.to_string(),
                module_owners.get(m).cloned().unwrap_or_default(),
            ));
        }
        let items = super::module_dep_findings(&index, pkg, reqs_out);
        if !items.is_empty() {
            report.findings.insert(pkg.clone(), items);
        }
    }
    Ok(report)
}
