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
use crate::error::FarmError;
use std::collections::{BTreeMap, HashSet};
use std::path::Path;

/// 本检則 analysis 结构版本：只在 **pkgconf** 分析逻辑变化时递增（与其他检則独立）。
const SCHEMA: u32 = 5;

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

fn analyze(extract: &Path) -> Result<serde_json::Value, FarmError> {
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

pub fn run(opts: &ChkOpts) -> Result<Report, FarmError> {
    let walk = walk_all(opts, SCHEMA, |ext, _pkg| analyze(ext))?;

    let index = super::load_index(opts);
    let mut module_owners: BTreeMap<String, HashSet<String>> = BTreeMap::new();
    // 预聚合：模块 → 提供它的包（全仓一遍，供逐包判定归属）
    for (pkg, a) in &walk.0 {
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

    // 过滤（subset / IGNORE flag）与汇总由公共骨架做，这里只回答"本包有什么问题"
    Ok(super::collect_findings(opts, walk, |pkg, a| {
        let Some(reqs) = a["requires"].as_array() else {
            return Vec::new();
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
        super::module_dep_findings(&index, pkg, reqs_out)
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 逗号分段 + 剥版本约束 + 内联注释截断（`.pc` 里 `Requires: # nettle` 很常见）。
    #[test]
    fn parse_requires_line_splits_strips_versions_and_comments() {
        let mut out = Vec::new();
        parse_requires_line("foo >= 1.2, bar, baz # nettle", &mut out);
        assert_eq!(out, vec!["foo", "bar", "baz"]);
        let mut out = Vec::new();
        parse_requires_line(" # 整行都是注释", &mut out);
        assert!(out.is_empty());
        let mut out = Vec::new();
        parse_requires_line("", &mut out);
        assert!(out.is_empty());
    }

    /// 只有 `*/pkgconfig/*.pc` 才算模块——`usr/lib/cmake/foo.pc` 之类的名字空间不同，不能混进来。
    #[test]
    fn pc_module_only_accepts_pkgconfig_dirs() {
        let content = Path::new("/x/content");
        assert_eq!(
            pc_module(Path::new("/x/content/usr/lib/pkgconfig/foo.pc"), content).as_deref(),
            Some("foo")
        );
        assert_eq!(
            pc_module(Path::new("/x/content/usr/share/pkgconfig/bar.pc"), content).as_deref(),
            Some("bar")
        );
        assert!(pc_module(Path::new("/x/content/usr/lib/cmake/foo.pc"), content).is_none());
        assert!(pc_module(Path::new("/x/content/usr/lib/pkgconfig/foo.h"), content).is_none());
    }
}
