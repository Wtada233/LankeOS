//! ABI 断裂检测 + direct-only 传播（§7）。
//!
//! 核心原则（§7.2）：`rust > llvm > libxml2`——libxml2 断裂只重建 llvm；
//! llvm 重建后 ABI 未变则 rust 不动。传播锚定**被移除的旧 SONAME**，
//! 反图必须从【旧索引】构建；重建一个包后 re-diff 其 provides，变了才级联（固定点）。

use std::collections::HashSet;

use crate::graph::{soname_provides_of, Index, RevMap};

/// pkg 相对 `new_provides_soname` 被移除的版本化 SONAME（ABI 断裂信号）。
pub fn removed_sonames(old: &Index, pkg: &str, new_provides_soname: &[String]) -> Vec<String> {
    let old_s = old.soname_provides(pkg);
    let new_s = soname_provides_of(new_provides_soname);
    let mut v: Vec<String> = old_s.difference(&new_s).cloned().collect();
    v.sort();
    v
}

/// pkg 相对新扫描**不再提供的 SONAME 版本**（`X@V` 串，排序）—— 库还在、**那个版本没了**。
///
/// 与 `removed_sonames()`（整个 SONAME 消失）是**两条互补的边**：那条走裸名、这条走版本，
/// 合起来才是"ABI 面到底少了什么"。版本串的规范化（去重 + 字典序）在 `graph::parse_so_spec()`。
pub fn removed_soname_versions(
    old: &Index,
    pkg: &str,
    new_provides_soname: &[String],
) -> Vec<String> {
    let old_specs = old
        .packages
        .get(pkg)
        .map(|i| i.provides_soname.as_slice())
        .unwrap_or(&[]);
    crate::graph::removed_provided_versions(old_specs, new_provides_soname)
}

/// 需要任一 `removed` **版本**的包（版本级直连受害者；反图来自旧索引）。
pub(crate) fn version_victims(revmap: &RevMap, removed_versions: &[String]) -> Vec<String> {
    let mut set = HashSet::new();
    for key in removed_versions {
        for needer in revmap.version_needers(key) {
            set.insert(needer.clone());
        }
    }
    let mut v: Vec<String> = set.into_iter().collect();
    v.sort();
    v
}

/// 需要任一 `removed` SONAME 的包（直连受害者；反图来自旧索引）。
pub(crate) fn direct_victims(revmap: &RevMap, removed: &[String]) -> Vec<String> {
    let mut set = HashSet::new();
    for soname in removed {
        for needer in revmap.needers(soname) {
            set.insert(needer.clone());
        }
    }
    let mut v: Vec<String> = set.into_iter().collect();
    v.sort();
    v
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct PropagationResult {
    /// 需要重建的包（含 root 断裂包；排序稳定）
    pub rebuilt: Vec<String>,
    /// 构建失败 → BLOCKED，等待 operator 接管（§8.5）
    pub blocked: Vec<String>,
}

#[cfg(test)]
mod tests;
