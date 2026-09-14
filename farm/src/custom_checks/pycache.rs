//! pycachechk — 打包是否混入 Python 字节码（缓存目录与散落文件）。
//!
//! 两类都报：
//! 1. **`__pycache__/` 目录**——其中的 `.pyc` 是构建期/运行期生成的缓存：内含源文件绝对路径、
//!    mtime 与编译期解释器版本，既**不可复现**也可能过期；
//! 2. **散落的 `.pyc`/`.pyo`**（不在 `__pycache__` 里，旧式布局）——同类问题。
//!
//! LankeOS 约定包内不得含字节码（安装后由解释器自行生成）。
//!
//! 报告粒度：`__pycache__` **按目录报**（一个目录里动辄上百 `.pyc`，逐文件会刷屏；删除单位就是目录），
//! 散落文件按文件报。目录**遍历目录而非只扫文件**——空的 `__pycache__` 也算（tar 保留空目录）。

use super::{walk_all, ChkOpts, Finding, Report, Severity};
use crate::error::FarmError;
use std::collections::HashSet;
use std::path::Path;

/// 本检則 analysis 结构版本：只在 **pycache** 分析逻辑变化时递增（与其他检則独立）。
/// 已递增至 2：analysis 增加 `loose`（散落 `.pyc`/`.pyo`）桶。
const SCHEMA: u32 = 2;

/// `.pyc` / `.pyo`（Python 字节码）。
fn is_bytecode(rel: &str) -> bool {
    rel.ends_with(".pyc") || rel.ends_with(".pyo")
}

/// DFS 收集 content 下所有**目录**的 content 相对路径（只对目录递归，不 follow 符号链接目录）。
fn walk_dirs(dir: &Path, content: &Path, out: &mut Vec<String>) {
    let Ok(rd) = std::fs::read_dir(dir) else {
        return;
    };
    for e in rd.flatten() {
        let Ok(ft) = e.file_type() else { continue };
        if !ft.is_dir() {
            continue;
        }
        let p = e.path();
        if let Ok(rel) = p.strip_prefix(content) {
            out.push(rel.to_string_lossy().into_owned());
        }
        walk_dirs(&p, content, out);
    }
}

/// 包的内容分析：
/// - `dirs`:  所有名为 `__pycache__` 的目录 → `[(相对路径, 目录内条目数)]`（排序确定）
/// - `loose`: **不在** `__pycache__` 里的散落 `.pyc`/`.pyo`（排序确定；已在缓存目录里的由 `dirs` 覆盖，
///   不重复报）
fn analyze(extract: &Path) -> Result<serde_json::Value, FarmError> {
    let content = extract.join("content");

    // 1) __pycache__ 目录（遍历目录，空的也算）
    let mut all_dirs: Vec<String> = Vec::new();
    walk_dirs(&content, &content, &mut all_dirs);
    let mut dirs: Vec<(String, usize)> = Vec::new();
    let mut cache_prefixes: Vec<String> = Vec::new();
    for rel in all_dirs {
        if !rel.rsplit('/').next().is_some_and(|n| n == "__pycache__") {
            continue;
        }
        let n = std::fs::read_dir(content.join(&rel))
            .map(|rd| rd.count())
            .unwrap_or(0);
        cache_prefixes.push(format!("{rel}/"));
        dirs.push((rel, n));
    }
    dirs.sort();

    // 2) 散落字节码（排除 `__pycache__/` 下的——那些已由目录条目覆盖）
    let mut loose: Vec<String> = super::collect_files(&content)
        .iter()
        .map(|f| super::rel_of(f, &content))
        .filter(|rel| is_bytecode(rel))
        .filter(|rel| !cache_prefixes.iter().any(|p| rel.starts_with(p.as_str())))
        .collect();
    loose.sort();

    Ok(serde_json::json!({ "dirs": dirs, "loose": loose }))
}

/// 跑 pycachechk。
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
        if super::is_ignored(&opts.pkgs_dir, pkg, "PYCACHE") {
            continue;
        }
        report.checked += 1;
        let mut items: Vec<Finding> = Vec::new();
        // 缓存目录：每目录一条
        if let Some(arr) = a["dirs"].as_array() {
            for d in arr {
                let (Some(path), n) = (d[0].as_str(), d[1].as_u64().unwrap_or(0)) else {
                    continue;
                };
                items.push(Finding {
                    file: path.to_string(),
                    what: format!(
                        "包含 __pycache__ 字节码缓存目录（{n} 项；构建期生成、含绝对路径且不可复现，\
                         应在 package 阶段删除）"
                    ),
                    severity: Severity::Warning,
                });
            }
        }
        // 散落字节码：每文件一条
        if let Some(arr) = a["loose"].as_array() {
            for p in arr.iter().filter_map(|v| v.as_str()) {
                items.push(Finding {
                    file: p.to_string(),
                    what: "散落的 Python 字节码（.pyc/.pyo，不在 __pycache__ 里；构建期生成、\
                           不可复现，应在 package 阶段删除）"
                        .to_string(),
                    severity: Severity::Warning,
                });
            }
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

    fn tmp(name: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("farm-{}-{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn dirs_of(a: &serde_json::Value) -> Vec<(String, u64)> {
        a["dirs"]
            .as_array()
            .unwrap()
            .iter()
            .map(|d| (d[0].as_str().unwrap().to_string(), d[1].as_u64().unwrap()))
            .collect()
    }

    fn loose_of(a: &serde_json::Value) -> Vec<String> {
        a["loose"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap().to_string())
            .collect()
    }

    #[test]
    fn analyze_finds_pycache_dirs_including_empty() {
        let tmp = tmp("pyc");
        let c = tmp.join("content");
        std::fs::create_dir_all(c.join("usr/lib/python3.14/site-packages/foo/__pycache__"))
            .unwrap();
        std::fs::write(
            c.join("usr/lib/python3.14/site-packages/foo/__pycache__/a.cpython-314.pyc"),
            b"x",
        )
        .unwrap();
        std::fs::write(
            c.join("usr/lib/python3.14/site-packages/foo/__pycache__/b.cpython-314.pyc"),
            b"x",
        )
        .unwrap();
        // 空 __pycache__（tar 会保留空目录）——也必须报
        std::fs::create_dir_all(c.join("usr/lib/python3.14/empty/__pycache__")).unwrap();
        // 干扰项：名字含 pycache 但不是该目录名
        std::fs::create_dir_all(c.join("usr/share/pycache-notes")).unwrap();

        let a = analyze(&tmp).unwrap();
        assert_eq!(
            dirs_of(&a),
            vec![
                ("usr/lib/python3.14/empty/__pycache__".to_string(), 0),
                (
                    "usr/lib/python3.14/site-packages/foo/__pycache__".to_string(),
                    2
                ),
            ],
            "应报两个 __pycache__（含空的），且不误报 pycache-notes"
        );
        assert!(
            loose_of(&a).is_empty(),
            "缓存目录里的 .pyc 不应再进 loose: {a}"
        );
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn analyze_finds_loose_bytecode_without_double_counting() {
        let tmp = tmp("pyc2");
        let c = tmp.join("content");
        let sp = c.join("usr/lib/python3.14/site-packages");
        // 缓存目录里的（只进 dirs）
        std::fs::create_dir_all(sp.join("foo/__pycache__")).unwrap();
        std::fs::write(sp.join("foo/__pycache__/m.cpython-314.pyc"), b"x").unwrap();
        // 散落字节码（进 loose）：.pyc 与 .pyo 都算
        std::fs::write(sp.join("legacy.pyc"), b"x").unwrap();
        std::fs::write(sp.join("legacy.pyo"), b"x").unwrap();
        // 干扰项：普通源码/数据文件
        std::fs::write(sp.join("mod.py"), b"").unwrap();
        std::fs::write(sp.join("data.pyc.txt"), b"x").unwrap();

        let a = analyze(&tmp).unwrap();
        assert_eq!(dirs_of(&a).len(), 1, "{a}");
        assert_eq!(
            loose_of(&a),
            vec![
                "usr/lib/python3.14/site-packages/legacy.pyc".to_string(),
                "usr/lib/python3.14/site-packages/legacy.pyo".to_string(),
            ],
            "散落 .pyc/.pyo 应报，且缓存目录里的不重复报: {a}"
        );
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn analyze_does_not_match_lookalike_dir_names() {
        let tmp = tmp("pyc3");
        let c = tmp.join("content");
        // 只有精确等于 __pycache__ 的目录名才算（__pycache__x / pycache / .__pycache__ 都不算）
        for d in [
            "usr/lib/x/__pycache__x",
            "usr/lib/x/pycache",
            "usr/lib/x/.__pycache__",
        ] {
            std::fs::create_dir_all(c.join(d)).unwrap();
        }
        let a = analyze(&tmp).unwrap();
        assert!(dirs_of(&a).is_empty(), "不应对近似名误报: {a}");
        let _ = std::fs::remove_dir_all(&tmp);
    }
}
