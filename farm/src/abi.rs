//! ABI 断裂检测 + direct-only 传播（§7）。
//!
//! 核心原则（§7.2）：`rust > llvm > libxml2`——libxml2 断裂只重建 llvm；
//! llvm 重建后 ABI 未变则 rust 不动。传播锚定**被移除的旧 SONAME**，
//! 反图必须从【旧索引】构建；重建一个包后 re-diff 其 provides，变了才级联（固定点）。

use std::collections::{HashSet, VecDeque};

use crate::graph::{soname_provides_of, Index, RevMap};
use crate::lpkg_binding::LpkgBinding;

/// pkg 相对 `new_provides` 被移除的版本化 SONAME（ABI 断裂信号）。
pub fn removed_sonames(old: &Index, pkg: &str, new_provides: &[String]) -> Vec<String> {
    let old_s = old.soname_provides(pkg);
    let new_s = soname_provides_of(new_provides);
    let mut v: Vec<String> = old_s.difference(&new_s).cloned().collect();
    v.sort();
    v
}

/// Tier-1 检测：old vs new 索引的版本化 SONAME provides 差（排序稳定）。
/// 返回有 ABI 断裂的包。
pub fn detect_abi_breaks(old: &Index, new: &Index) -> Vec<String> {
    let mut breaks = Vec::new();
    for name in old.sorted_names() {
        let old_s = old.soname_provides(&name);
        let new_s = match new.packages.get(&name) {
            Some(info) => soname_provides_of(&info.provides),
            None => HashSet::new(),
        };
        if old_s != new_s {
            breaks.push(name);
        }
    }
    breaks
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

/// 从 root 断裂包出发传播：
/// removed SONAME → 直连受害者 → 重建（binding）→ re-diff 决定是否级联。
pub fn propagate(
    old: &Index,
    revmap: &RevMap,
    root: &str,
    root_new_provides: &[String],
    binding: &mut dyn LpkgBinding,
) -> PropagationResult {
    let mut rebuilt: Vec<String> = Vec::new();
    let mut blocked: Vec<String> = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();
    seen.insert(root.to_string());
    rebuilt.push(root.to_string());

    let mut queue: VecDeque<(String, Vec<String>)> = VecDeque::new();
    queue.push_back((root.to_string(), root_new_provides.to_vec()));

    while let Some((p, p_new_provides)) = queue.pop_front() {
        let removed = removed_sonames(old, &p, &p_new_provides);
        for victim in direct_victims(revmap, &removed) {
            if !seen.insert(victim.clone()) {
                continue;
            }
            let outcome = binding.build(&victim);
            if !outcome.ok {
                blocked.push(victim.clone());
                continue;
            }
            rebuilt.push(victim.clone());
            // re-diff：victim 重建后其 ABI 是否也变了 → 级联
            if !removed_sonames(old, &victim, &outcome.provides).is_empty() {
                queue.push_back((victim, outcome.provides));
            }
        }
    }

    rebuilt.sort();
    blocked.sort();
    PropagationResult { rebuilt, blocked }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::graph::RevMap;
    use crate::lpkg_binding::{BuildOutcome, StubBinding};
    use std::collections::HashMap;

    const CHAIN: &str = "\
libxml2|2.9.0:hash::libxml2.so,libxml2.so.2:ld-linux.so.2,libc.so.6|
llvm|18.1.0:hash::libLLVM.so,libLLVM.so.18:libxml2.so.2,libc.so.6|
rust|1.80.0:hash::rustc:libLLVM.so.18,libc.so.6|
glibc|2.39:hash::libc.so,libc.so.6,ld-linux.so.2:|
";

    fn abi_preserving_stub(index: &Index, victims: &[String]) -> StubBinding {
        let mut outcomes = HashMap::new();
        for v in victims {
            let info = &index.packages[v];
            outcomes.insert(
                v.clone(),
                BuildOutcome::success(&to_refs(&info.needed_so), &to_refs(&info.provides), &[]),
            );
        }
        StubBinding::new(outcomes)
    }

    fn to_refs(v: &[String]) -> Vec<&str> {
        v.iter().map(String::as_str).collect()
    }

    #[test]
    fn removed_sonames_detects_bump() {
        let old = Index::parse(CHAIN);
        let new_provides = vec!["libxml2.so".to_string(), "libxml2.so.3".to_string()];
        assert_eq!(
            removed_sonames(&old, "libxml2", &new_provides),
            vec!["libxml2.so.2"]
        );
    }

    #[test]
    fn libxml2_break_only_rebuilds_llvm_rust_untouched() {
        let old = Index::parse(CHAIN);
        let rev = RevMap::build(&old);
        let new_provides = vec!["libxml2.so".to_string(), "libxml2.so.3".to_string()];
        let victims = vec!["llvm".to_string()];
        let mut binding = abi_preserving_stub(&old, &victims);

        let res = propagate(&old, &rev, "libxml2", &new_provides, &mut binding);
        assert_eq!(res.rebuilt, vec!["libxml2", "llvm"]);
        assert!(res.blocked.is_empty());
        assert!(!res.rebuilt.contains(&"rust".to_string()));
    }

    #[test]
    fn cascade_when_intermediate_abi_changes() {
        let old = Index::parse(CHAIN);
        let rev = RevMap::build(&old);
        let new_provides = vec!["libxml2.so".to_string(), "libxml2.so.3".to_string()];
        let mut outcomes = HashMap::new();
        outcomes.insert(
            "llvm".to_string(),
            BuildOutcome {
                ok: true,
                needed_so: vec!["libxml2.so.3".into(), "libc.so.6".into()],
                provides: vec!["libLLVM.so".into(), "libLLVM.so.19".into()],
                deps: vec![],
                failure_stage: None,
                lpkg_path: None,
            },
        );
        let mut binding = StubBinding::new(outcomes);

        let res = propagate(&old, &rev, "libxml2", &new_provides, &mut binding);
        assert_eq!(res.rebuilt, vec!["libxml2", "llvm", "rust"]);
        assert!(res.blocked.is_empty());
    }

    #[test]
    fn build_failure_goes_to_blocked() {
        let old = Index::parse(CHAIN);
        let rev = RevMap::build(&old);
        let new_provides = vec!["libxml2.so".to_string(), "libxml2.so.3".to_string()];
        let mut outcomes = HashMap::new();
        outcomes.insert(
            "llvm".to_string(),
            BuildOutcome::failure("lankebuild_build"),
        );
        let mut binding = StubBinding::new(outcomes);

        let res = propagate(&old, &rev, "libxml2", &new_provides, &mut binding);
        assert_eq!(res.rebuilt, vec!["libxml2"]);
        assert_eq!(res.blocked, vec!["llvm"]);
    }
}
