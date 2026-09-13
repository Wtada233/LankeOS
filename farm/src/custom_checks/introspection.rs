//! introspectionchk — 包内 shipped GObject Introspection 元数据（`.gir`）的**构建依赖**检查。
//!
//! `.gir` 由**构建期** `g-ir-scanner`（`gobject-introspection` 包）扫描源码生成 → 配方必须把
//! `gobject-introspection` 写进 `build_deps`；未声明 → **Critical**（构建会失败或产物缺失）。
//! 与 qml/pkgconf 不同：本检則读**配方 `build_deps`**（构建期依赖），不读仓库 index 的运行时 deps。
//!
//! 例外：`pkg == gobject-introspection` 自满足（工具包自带 `.gir`）；其它已知例外用配方
//! `IGNORE_CHK_INTROSPECTION` 整包豁免。

use super::{build_dep_findings, walk_all, ChkOpts, Report};
use crate::error::FarmError;
use std::collections::HashSet;
use std::path::Path;

/// 本检則 analysis 结构版本：只在 **introspection** 分析逻辑变化时递增（与其他检則独立）。
const SCHEMA: u32 = 1;
/// 构建期工具（配方 `build_deps` 里应有的包名）。
const TOOL: &str = "gobject-introspection";

/// 包的内容分析：收集 `.gir` 文件（content 相对路径，排序确定）。
fn analyze(extract: &Path) -> Result<serde_json::Value, FarmError> {
    let content = extract.join("content");
    let mut files: Vec<String> = super::collect_files(&content)
        .iter()
        .map(|f| super::rel_of(f, &content))
        .filter(|rel| rel.ends_with(".gir"))
        .collect();
    files.sort();
    Ok(serde_json::json!({ "files": files }))
}

/// 跑 introspectionchk。
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
        if super::is_ignored(&opts.pkgs_dir, pkg, "INTROSPECTION") {
            continue;
        }
        report.checked += 1;
        let files: Vec<String> = a["files"]
            .as_array()
            .map(|arr| {
                arr.iter()
                    .filter_map(|v| v.as_str().map(String::from))
                    .collect()
            })
            .unwrap_or_default();
        let items = build_dep_findings(
            &opts.pkgs_dir,
            pkg,
            TOOL,
            "包含 g-ir-scanner 生成的 .gir（GObject Introspection 元数据）",
            &files,
        );
        if !items.is_empty() {
            report.findings.insert(pkg.clone(), items);
        }
    }
    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::custom_checks::Severity;

    #[test]
    fn analyze_collects_gir_files_sorted() {
        let tmp = std::env::temp_dir().join(format!("farm-gir-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&tmp);
        std::fs::create_dir_all(tmp.join("content/usr/share/gir-1.0")).unwrap();
        std::fs::create_dir_all(tmp.join("content/usr/lib/nested")).unwrap();
        std::fs::write(
            tmp.join("content/usr/share/gir-1.0/GLib-2.0.gir"),
            "<repository/>",
        )
        .unwrap();
        std::fs::write(
            tmp.join("content/usr/share/gir-1.0/Gio-2.0.gir"),
            "<repository/>",
        )
        .unwrap();
        std::fs::write(tmp.join("content/usr/share/other.txt"), "x").unwrap();
        // 钉死判定口径：**按扩展名匹配、不限目录**（GIR 标准位置是 usr/share/gir-1.0，但
        // 不硬编该前缀——避免上游换目录时漏判；误报由 IGNORE_CHK_INTROSPECTION 兜）
        std::fs::write(
            tmp.join("content/usr/lib/nested/Deep-1.0.gir"),
            "<repository/>",
        )
        .unwrap();
        let a = analyze(&tmp).unwrap();
        let files: Vec<&str> = a["files"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap())
            .collect();
        assert_eq!(
            files,
            vec![
                "usr/lib/nested/Deep-1.0.gir",
                "usr/share/gir-1.0/GLib-2.0.gir",
                "usr/share/gir-1.0/Gio-2.0.gir"
            ]
        );
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn build_dep_findings_requires_declared_tool() {
        let dir = std::env::temp_dir().join(format!("farm-girbd-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let write = |pkg: &str, bd: &[&str]| {
            let p = dir.join(pkg);
            std::fs::create_dir_all(&p).unwrap();
            std::fs::write(
                p.join("LankeBUILD.json"),
                serde_json::to_string(&serde_json::json!({
                    "name": pkg, "version": "1.0", "build_deps": bd
                }))
                .unwrap(),
            )
            .unwrap();
        };
        write("withdep", &["gobject-introspection"]);
        write("nodep", &["base-devel"]);
        let files = vec!["usr/share/gir-1.0/X.gir".to_string()];

        assert!(build_dep_findings(&dir, "withdep", TOOL, "what", &files).is_empty());
        let f = build_dep_findings(&dir, "nodep", TOOL, "what", &files);
        assert_eq!(f.len(), 1);
        assert_eq!(f[0].severity, Severity::Critical);
        assert!(f[0].what.contains("gobject-introspection"));
        // 工具包自身 → 自满足
        assert!(build_dep_findings(&dir, TOOL, TOOL, "what", &files).is_empty());
        // 无 .gir 文件 / 配方不存在 → 不判
        assert!(build_dep_findings(&dir, "nodep", TOOL, "what", &[]).is_empty());
        assert!(build_dep_findings(&dir, "ghost", TOOL, "what", &files).is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
