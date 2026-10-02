//! pkg-errchk — 打包错误检测（CLAUDE.md 铁律护栏）：
//! - `usr/etc/` / `usr/var/`：打包时把 PREFIX 拼进了系统根（`/etc`、`/var` 才对，不应有 usr/etc）→ 错位。
//! - `.la`（libtool 存档）：已废弃，禁止进入 .lpkg。
//! - `.a`（静态库）：一般应删，除非包 .cmake 配置引用了它（如 LLVM/aom）；发现即报，operator 用
//!   `IGNORE_CHK_PKGERR` 豁免确需保留静态库的包。

use super::{walk_all, ChkOpts, Finding, Report, Severity};
use crate::error::FarmError;
use crate::tr;
use std::path::Path;

/// 本检則 analysis 结构版本：只在 **pkg-err** 分析逻辑变化时递增（与其他检則独立）。
const SCHEMA: u32 = 5;

fn analyze(extract: &Path) -> Result<serde_json::Value, FarmError> {
    let content = extract.join("content");
    let mut paths: Vec<(String, String)> = Vec::new();
    for f in super::collect_files(&content) {
        let rel = super::rel_of(&f, &content);
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
    let walk = walk_all(opts, SCHEMA, |ext, _pkg| analyze(ext))?;
    // 过滤（subset / IGNORE_CHK_PKGERR）与汇总由公共骨架做，这里只回答"本包有什么问题"
    Ok(super::collect_findings(opts, walk, |_pkg, a| {
        let Some(paths) = a["paths"].as_array() else {
            return Vec::new();
        };
        let mut items: Vec<Finding> = Vec::new();
        for p in paths {
            let (Some(file), Some(kind)) = (p[0].as_str(), p[1].as_str()) else {
                continue;
            };
            let what: String = match kind {
                "misplaced-root" => tr!("chk.pkg-err.misplaced_root").to_string(),
                "libtool-archive" => tr!("chk.pkg-err.libtool_archive").to_string(),
                "static-lib" => tr!("chk.pkg-err.static_lib").to_string(),
                _ => kind.to_string(),
            };
            items.push(Finding {
                file: file.to_string(),
                what,
                severity: Severity::Warning,
            });
        }
        items
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("farm-{}-{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn paths_of(a: &serde_json::Value) -> Vec<(String, String)> {
        let mut v: Vec<(String, String)> = a["paths"]
            .as_array()
            .unwrap()
            .iter()
            .map(|p| {
                (
                    p[0].as_str().unwrap().to_string(),
                    p[1].as_str().unwrap().to_string(),
                )
            })
            .collect();
        v.sort();
        v
    }

    /// 三类打包错误各抓一个；同时验证**干净文件不误报**。
    #[test]
    fn analyze_classifies_misplaced_root_la_and_a() {
        let tmp = tmp("pkgerr");
        let c = tmp.join("content");
        std::fs::create_dir_all(c.join("usr/etc/foo")).unwrap();
        std::fs::create_dir_all(c.join("usr/var/lib/foo")).unwrap();
        std::fs::create_dir_all(c.join("usr/lib")).unwrap();
        std::fs::write(c.join("usr/etc/foo/x.conf"), b"x").unwrap();
        std::fs::write(c.join("usr/var/lib/foo/y"), b"y").unwrap();
        std::fs::write(c.join("usr/lib/libfoo.la"), b"x").unwrap();
        std::fs::write(c.join("usr/lib/libbar.a"), b"x").unwrap();
        std::fs::write(c.join("usr/lib/libok.so.1"), b"x").unwrap();
        assert_eq!(
            paths_of(&analyze(&tmp).unwrap()),
            vec![
                ("usr/etc/foo/x.conf".into(), "misplaced-root".into()),
                ("usr/lib/libbar.a".into(), "static-lib".into()),
                ("usr/lib/libfoo.la".into(), "libtool-archive".into()),
                ("usr/var/lib/foo/y".into(), "misplaced-root".into()),
            ]
        );
    }

    /// **`/etc` 是对的、`usr/etc` 才是错位**——这条最容易被改坏（PREFIX 拼进系统根）。
    #[test]
    fn analyze_accepts_etc_but_not_usr_etc() {
        let tmp = tmp("pkgerr-clean");
        let c = tmp.join("content");
        std::fs::create_dir_all(c.join("etc")).unwrap();
        std::fs::create_dir_all(c.join("usr/lib")).unwrap();
        std::fs::write(c.join("etc/ok.conf"), b"x").unwrap();
        std::fs::write(c.join("usr/lib/libok.so.1"), b"x").unwrap();
        assert!(
            paths_of(&analyze(&tmp).unwrap()).is_empty(),
            "干净的树（/etc + 无 .la/.a）不得报错"
        );
    }
}
