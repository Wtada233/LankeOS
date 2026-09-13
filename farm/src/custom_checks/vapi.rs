//! vapichk — 包内 shipped Vala 绑定（`.vapi`）的**构建依赖**检查。
//!
//! `.vapi` 由**构建期** `valac --vapidir`/`vala-gen-introspect` 之类（`vala` 包）生成 → 配方必须把
//! `vala` 写进 `build_deps`；未声明 → **Critical**（构建会失败或绑定缺失）。
//! 与 qml/pkgconf 不同：本检則读**配方 `build_deps`**（构建期依赖），不读仓库 index 的运行时 deps。
//!
//! 例外：`pkg == vala` 自满足（vala 自带标准 `.vapi`）；其它已知例外用配方 `IGNORE_CHK_VAPI` 整包豁免。

use super::{build_dep_findings, walk_all, ChkOpts, Report};
use crate::error::FarmError;
use std::collections::HashSet;
use std::path::Path;

/// 本检則 analysis 结构版本：只在 **vapi** 分析逻辑变化时递增（与其他检則独立）。
const SCHEMA: u32 = 1;
/// 构建期工具（配方 `build_deps` 里应有的包名）。
const TOOL: &str = "vala";

/// 包的内容分析：收集 `.vapi` 文件（content 相对路径，排序确定）。
fn analyze(extract: &Path) -> Result<serde_json::Value, FarmError> {
    let content = extract.join("content");
    let mut files: Vec<String> = super::collect_files(&content)
        .iter()
        .map(|f| super::rel_of(f, &content))
        .filter(|rel| rel.ends_with(".vapi"))
        .collect();
    files.sort();
    Ok(serde_json::json!({ "files": files }))
}

/// 跑 vapichk。
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
        if super::is_ignored(&opts.pkgs_dir, pkg, "VAPI") {
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
        let items = build_dep_findings(&opts.pkgs_dir, pkg, TOOL, "包含 Vala 绑定 .vapi", &files);
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
    fn analyze_collects_vapi_files() {
        let tmp = std::env::temp_dir().join(format!("farm-vapi-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&tmp);
        std::fs::create_dir_all(tmp.join("content/usr/share/vala/vapi")).unwrap();
        std::fs::write(
            tmp.join("content/usr/share/vala/vapi/libfoo.vapi"),
            "namespace Foo {}",
        )
        .unwrap();
        std::fs::write(
            tmp.join("content/usr/share/vala/vapi/libfoo.deps"),
            "glib-2.0",
        )
        .unwrap();
        let a = analyze(&tmp).unwrap();
        assert_eq!(
            a["files"].as_array().unwrap().len(),
            1,
            "只收 .vapi，不含 .deps: {a}"
        );
        assert_eq!(
            a["files"][0].as_str().unwrap(),
            "usr/share/vala/vapi/libfoo.vapi"
        );
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn build_dep_findings_requires_vala() {
        let dir = std::env::temp_dir().join(format!("farm-vapibd-{}", std::process::id()));
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
        write("withvala", &["base-devel", "vala"]);
        write("novala", &["base-devel"]);
        let files = vec!["usr/share/vala/vapi/libfoo.vapi".to_string()];

        assert!(build_dep_findings(&dir, "withvala", TOOL, "what", &files).is_empty());
        let f = build_dep_findings(&dir, "novala", TOOL, "what", &files);
        assert_eq!(f.len(), 1);
        assert_eq!(f[0].severity, Severity::Critical);
        assert!(f[0].what.contains("vala"));
        assert!(build_dep_findings(&dir, TOOL, TOOL, "what", &files).is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
