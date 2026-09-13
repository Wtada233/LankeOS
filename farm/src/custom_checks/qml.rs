//! qmlchk — QML import 的运行时模块/路径依赖检查。
//!
//! LankeOS 约定：QML import 的归属包必须在本包运行时可达（binpkg `deps` ∪ `needed_so` 链接依赖），
//! 否则报缺 deps。needed_so 扫不出 QML 绑定，所以这类必须显式 deps（CLAUDE.md KDE/Qt 规则）。
//!
//! provider 两路：
//! - **URI 模块**：内容 `usr/lib/qt6/qml/<模块路径>/qmldir` → 该目录归属包提供该 import 名（点化）。
//! - **路径 include**：任何包装的 `.qml` 文件都计入全局目录索引（`usr/lib/qt6/qml/...` 之外也计入）；
//!   相对路径 import（`import "../x"` / `import "components"`）解析成 content 根下绝对目录，
//!   到「目录 → 归属包」索引里找 provider（自包命中即无缺）。
//!
//! 三段判定同其它 chk：闭包（deps∪needed_so）内找到即过；否则仓库内有 → Warning；仓库也没有 → Critical。

use super::{walk_all, ChkOpts, Finding, Report, Severity};
use crate::error::FarmError;
use std::collections::{BTreeMap, HashSet};
use std::path::Path;

const QML_ROOT: &str = "usr/lib/qt6/qml";

/// 本检則 analysis 结构版本：只在 **qml** 分析逻辑变化时递增。
/// 已递增至 6：analysis 改为扫描**剥离注释后**的文本（旧缓存里的 requires 含注释掉的 import）。
const SCHEMA: u32 = 6;

/// 引擎/进程内注册型 QML 模块的 URI 列表：由**各包配方 farm_flags** 的字符串列表 flag 声明，
/// 值用 **JSON 数组对象成员**：`{"QML_CHK_IGN_LST": ["org.kde.kwin", "HelperWidgets"]}`
/// （裸串 `NAME=a,b` 不支持——consumer 只读 JSON 数组，见 build/farm_flags.rs）。
/// 这类模块不以 qmldir 目录存在于仓库、靠 QML 引擎/宿主运行时注册，仓库无 provider 属正常——
/// 声明后该包的这些 import 不再判缺失。
fn internal_uris_of(pkgs_dir: &Path, pkg: &str) -> HashSet<String> {
    let Some(b) = crate::build::read_lankebuild(pkgs_dir, pkg) else {
        return HashSet::new();
    };
    // farm_flags 的列表 flag（serde_json 解析，见 build::string_list）
    crate::build::string_list(&b.farm_flags, "QML_CHK_IGN_LST")
        .into_iter()
        .filter(|u| !u.is_empty())
        .collect()
}

/// 把相对路径 import 解析成 content 根下绝对目录（无前导 '/'；越出 content 根 → None）。
/// base_dir 是 importing 文件所在目录的 content 相对路径。
fn norm_join(base_dir: &str, imp: &str) -> Option<String> {
    if imp.starts_with('/') {
        return None;
    }
    let mut parts: Vec<&str> = Vec::new();
    if !base_dir.is_empty() {
        parts.extend(base_dir.split('/'));
    }
    for seg in imp.split('/') {
        match seg {
            "" | "." => {}
            ".." => {
                if parts.pop().is_none() {
                    return None; // 越出 content/（/）根
                }
            }
            s => parts.push(s),
        }
    }
    if parts.is_empty() {
        None
    } else {
        Some(parts.join("/"))
    }
}

/// 剥离 QML/JS 注释（`//` 行注释、`/* */` 块注释），**跳过字符串字面量**（`"…"`/`'…'`，处理 `\`
/// 转义）——否则 `import "https://x/y"` 里的 `//` 会被当成注释起点误截。保留换行以维持 `(?m)^`
/// 锚点行结构。历史事故：import 正则直接扫原文，`// import "components"`（注释掉的 import）
/// 被当成真依赖 → 误报。
fn strip_comments(text: &str) -> String {
    #[derive(PartialEq)]
    enum St {
        Code,
        Line,
        Block,
        Dq,
        Sq,
    }
    let chars: Vec<char> = text.chars().collect();
    let mut out = String::with_capacity(text.len());
    let mut st = St::Code;
    let mut i = 0;
    while i < chars.len() {
        let c = chars[i];
        let next = chars.get(i + 1).copied();
        match st {
            St::Code => {
                if c == '/' && next == Some('/') {
                    st = St::Line;
                    i += 2;
                } else if c == '/' && next == Some('*') {
                    st = St::Block;
                    i += 2;
                } else {
                    if c == '"' {
                        st = St::Dq;
                    } else if c == '\'' {
                        st = St::Sq;
                    }
                    out.push(c);
                    i += 1;
                }
            }
            St::Line => {
                if c == '\n' {
                    out.push('\n');
                    st = St::Code;
                }
                i += 1;
            }
            St::Block => {
                if c == '*' && next == Some('/') {
                    st = St::Code;
                    i += 2;
                } else {
                    if c == '\n' {
                        out.push('\n'); // 保留行结构
                    }
                    i += 1;
                }
            }
            St::Dq | St::Sq => {
                // 字符串内整体保留；`\` 转义吞掉下一字符（不触发注释/闭合判定）
                if c == '\\' {
                    out.push(c);
                    if let Some(n) = next {
                        out.push(n);
                    }
                    i += 2;
                } else {
                    out.push(c);
                    if (st == St::Dq && c == '"') || (st == St::Sq && c == '\'') {
                        st = St::Code;
                    }
                    i += 1;
                }
            }
        }
    }
    out
}

/// import 行捕获：`import <URI>`（点分）或 `import "<相对路径>"`。返回 (kind, value)，
/// kind = "uri" | "path"。
fn imports_in(text: &str) -> Vec<(String, String)> {
    let mut out = Vec::new();
    for m in import_re().captures_iter(text) {
        if let Some(q) = m.get(1) {
            out.push(("path".to_string(), q.as_str().to_string()));
        } else if let Some(u) = m.get(2) {
            out.push(("uri".to_string(), u.as_str().to_string()));
        }
    }
    out
}

/// 包的内容分析：
/// - provides: qmldir 声明的 URI 模块
/// - files:    包内全部 .qml 文件的 content 相对路径（全局目录索引的输入）
/// - requires: (文件, URI import)
/// - rel:      (文件, 归一化后的绝对目录) 相对路径 include
fn analyze(extract: &Path) -> Result<serde_json::Value, FarmError> {
    let content = extract.join("content");
    let mut provides: Vec<String> = Vec::new();
    let mut files: Vec<String> = Vec::new();
    let mut requires: Vec<(String, String)> = Vec::new();
    let mut rel: Vec<(String, String)> = Vec::new();
    for f in super::collect_files(&content) {
        let relp = super::rel_of(&f, &content);
        let name = f.file_name().and_then(|n| n.to_str()).unwrap_or("");
        if name == "qmldir" && relp.starts_with(&format!("{QML_ROOT}/")) {
            if let Some(dir) = f.parent() {
                if let Some(modrel) = dir.strip_prefix(content.join(QML_ROOT)).ok() {
                    provides.push(
                        modrel
                            .to_string_lossy()
                            .replace(std::path::MAIN_SEPARATOR, "."),
                    );
                }
            }
            continue;
        }
        if relp.ends_with(".qml") {
            files.push(relp.clone());
            let Ok(text) = std::fs::read_to_string(&f) else {
                continue;
            };
            // 先剥注释：注释掉的 import 不是真依赖（字符串字面量受保护，`https://` 不被误截）
            let text = strip_comments(&text);
            let base = relp
                .rsplit_once('/')
                .map(|(d, _)| d.to_string())
                .unwrap_or_default();
            for (kind, imp) in imports_in(&text) {
                if kind == "path" {
                    if let Some(abs) = norm_join(&base, &imp) {
                        if !rel.iter().any(|(_, t)| t == &abs) {
                            rel.push((relp.clone(), abs));
                        }
                    }
                } else if !requires.iter().any(|(_, x)| x == &imp) {
                    requires.push((relp.clone(), imp));
                }
            }
        }
    }
    Ok(serde_json::json!({
        "provides": provides,
        "files": files,
        "requires": requires,
        "rel": rel,
    }))
}

/// 跑 qmlchk。
pub fn run(opts: &ChkOpts) -> Result<Report, FarmError> {
    let (analyses, hits, misses, failed) = walk_all(opts, SCHEMA, |ext, _pkg| analyze(ext))?;
    let index = super::load_index(opts);

    // 1) URI 模块 → 归属包
    let mut module_owners: BTreeMap<String, HashSet<String>> = BTreeMap::new();
    // 2) 目录 → 归属包（由各包 .qml 文件目录的祖先建立）
    let mut dir_owners: BTreeMap<String, HashSet<String>> = BTreeMap::new();
    for (pkg, a) in &analyses {
        if let Some(prov) = a["provides"].as_array() {
            for m in prov.iter().filter_map(|v| v.as_str()) {
                module_owners
                    .entry(m.to_string())
                    .or_default()
                    .insert(pkg.clone());
            }
        }
        if let Some(fl) = a["files"].as_array() {
            for f in fl.iter().filter_map(|v| v.as_str()) {
                let dir = f.rsplit_once('/').map(|(d, _)| d).unwrap_or("");
                let mut parts: Vec<&str> = Vec::new();
                for seg in dir.split('/') {
                    if seg.is_empty() {
                        continue;
                    }
                    parts.push(seg);
                    dir_owners
                        .entry(parts.join("/"))
                        .or_default()
                        .insert(pkg.clone());
                }
            }
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
        if super::is_ignored(&opts.pkgs_dir, pkg, "QML") {
            continue;
        }
        report.checked += 1;
        let internal = internal_uris_of(&opts.pkgs_dir, pkg);
        let mut items: Vec<Finding> = Vec::new();
        // URI imports：三段判定（本包 farm_flags 声明的引擎注册型 URI 跳过）
        let mut reqs: Vec<(String, String, HashSet<String>)> = Vec::new();
        if let Some(arr) = a["requires"].as_array() {
            for r in arr {
                let (Some(file), Some(imp)) = (r[0].as_str(), r[1].as_str()) else {
                    continue;
                };
                let owners = longest_owner(&module_owners, imp);
                if owners.is_empty() && internal.contains(imp) {
                    continue; // 引擎注册型模块，仓库无 provider 属正常
                }
                reqs.push((file.to_string(), imp.to_string(), owners));
            }
        }
        items.extend(super::module_dep_findings(&index, pkg, reqs));
        // 相对路径 include：目标目录有归属包；self 覆盖即无缺；别的包未依赖 → Warning
        if let Some(arr) = a["rel"].as_array() {
            for r in arr {
                let (Some(file), Some(target)) = (r[0].as_str(), r[1].as_str()) else {
                    continue;
                };
                let owners = dir_owners.get(target).cloned().unwrap_or_default();
                if owners.is_empty() {
                    continue; // 目录不存在（可能编译期才生成）→ 不判
                }
                let covered = owners.iter().any(|o| super::owner_covered(&index, pkg, o));
                if !covered {
                    let o = owners.iter().cloned().collect::<Vec<_>>().join("|");
                    items.push(Finding {
                        file: file.to_string(),
                        what: format!(
                            "相对路径 include {target} 的提供者在 {o}，不在本包 deps/needed_so"
                        ),
                        severity: Severity::Warning,
                    });
                }
            }
        }
        if !items.is_empty() {
            report.findings.insert(pkg.clone(), items);
        }
    }
    Ok(report)
}

fn longest_owner(map: &BTreeMap<String, HashSet<String>>, imp: &str) -> HashSet<String> {
    let mut best: Option<(usize, HashSet<String>)> = None;
    for (m, o) in map {
        if m == imp || imp.starts_with(&format!("{m}.")) {
            let l = m.len();
            if best.as_ref().map_or(true, |(bl, _)| l > *bl) {
                best = Some((l, o.clone()));
            }
        }
    }
    best.map(|(_, o)| o).unwrap_or_default()
}

static IMPORT_RE: std::sync::OnceLock<regex::Regex> = std::sync::OnceLock::new();
fn import_re() -> &'static regex::Regex {
    IMPORT_RE.get_or_init(|| {
        regex::Regex::new(r#"(?m)^\s*import\s+(?:"([^"]+)"|([A-Za-z0-9_\.]+))"#).unwrap()
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn strip_comments_drops_commented_imports_but_keeps_strings() {
        let src = "\
import QtQuick 2.0
// import \"components\"
/*
import Foo 1.0
*/
import \"https://x/y\"
import A.B
";
        let stripped = strip_comments(src);
        let imports = imports_in(&stripped);
        let uri_names: Vec<&str> = imports.iter().map(|(_, v)| v.as_str()).collect();
        assert!(
            uri_names.contains(&"QtQuick"),
            "真 import 应保留: {imports:?}"
        );
        assert!(uri_names.contains(&"A.B"), "真 import 应保留: {imports:?}");
        assert!(
            uri_names.contains(&"https://x/y"),
            "字符串里的 // 不得被当注释: {imports:?}"
        );
        assert!(
            !uri_names.iter().any(|v| *v == "components"),
            "注释掉的 import 不得命中: {imports:?}"
        );
        assert!(
            !uri_names.iter().any(|v| *v == "Foo"),
            "块注释里的 import 不得命中: {imports:?}"
        );
        // 行结构保留（行数不变）
        assert_eq!(stripped.lines().count(), src.lines().count());
    }

    #[test]
    fn internal_uris_reads_json_array_flag_only() {
        let dir = std::env::temp_dir().join(format!("farm-qml-flags-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let pkg = dir.join("p");
        std::fs::create_dir_all(&pkg).unwrap();
        std::fs::write(
            pkg.join("LankeBUILD.json"),
            serde_json::to_string(&serde_json::json!({
                "name": "p", "version": "1.0",
                "farm_flags": [{"QML_CHK_IGN_LST": ["org.kde.kwin", "HelperWidgets"]}]
            }))
            .unwrap(),
        )
        .unwrap();
        let s = internal_uris_of(&dir, "p");
        assert!(
            s.contains("org.kde.kwin") && s.contains("HelperWidgets"),
            "{s:?}"
        );

        // 逗号串裸形式：不再被接受（值丢失，不再静默）
        std::fs::write(
            pkg.join("LankeBUILD.json"),
            serde_json::to_string(&serde_json::json!({
                "name": "p", "version": "1.0",
                "farm_flags": ["QML_CHK_IGN_LST=org.kde.kwin"]
            }))
            .unwrap(),
        )
        .unwrap();
        assert!(internal_uris_of(&dir, "p").is_empty(), "裸串不应再被读取");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
