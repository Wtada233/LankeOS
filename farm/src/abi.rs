//! ABI 断裂检测 + direct-only 传播（§7）。
//!
//! 核心原则（§7.2）：`rust > llvm > libxml2`——libxml2 断裂只重建 llvm；
//! llvm 重建后 ABI 未变则 rust 不动。传播锚定**被移除的旧 SONAME**，
//! 反图必须从【旧索引】构建；重建一个包后 re-diff 其 provides，变了才级联（固定点）。

use std::collections::HashSet;

use crate::graph::{soname_provides_of, Index, RevMap};

/// pkg 相对 `new_provides` 被移除的版本化 SONAME（ABI 断裂信号）。
pub fn removed_sonames(old: &Index, pkg: &str, new_provides: &[String]) -> Vec<String> {
    let old_s = old.soname_provides(pkg);
    let new_s = soname_provides_of(new_provides);
    let mut v: Vec<String> = old_s.difference(&new_s).cloned().collect();
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
