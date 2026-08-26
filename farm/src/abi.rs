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
    fn removed_sonames_detects_unversioned_soname_change() {
        // tcl 8.6 → 9.0：无 SONAME 实体库 libtcl8.6.so → libtcl9.0.so 必须被识别为 ABI 断裂
        //（tcl 的 provides 只有裸 .so，没有版本化兄弟项 → 是实体库不是 dev symlink）。
        let old = Index::parse("tcl|8.6.16:hash::libtcl8.6.so:libc.so.6,libtcl8.6.so|\n");
        assert_eq!(
            removed_sonames(&old, "tcl", &["libtcl9.0.so".to_string()]),
            vec!["libtcl8.6.so"]
        );
        // dev symlink（libfoo.so 有版本化兄弟 libfoo.so.1）不算 ABI 信号，不能被误报
        assert_eq!(
            removed_sonames(&old, "tcl", &["libtcl8.6.so".to_string()]),
            Vec::<String>::new()
        );
    }

    #[test]
    fn unversioned_break_rebuilds_expect() {
        // tcl 8.6 → 9.0 移除 libtcl8.6.so → 链接它的 expect（needed_so 含 libtcl8.6.so）
        // 是直连受害者；expect 自己的 libexpect5.45.4.so（无 SONAME 实体库）是 ABI 信号，
        // 重建后 provides 未变 → 不级联。
        let old = Index::parse(
            "tcl|8.6.16:hash::libtcl8.6.so:libc.so.6,libtcl8.6.so|\n\
             expect|5.45.4:hash::libexpect5.45.4.so:libtcl8.6.so,libc.so.6|\n",
        );
        let rev = RevMap::build(&old);
        let new_provides = vec!["libtcl9.0.so".to_string()];
        let victims = vec!["expect".to_string()];
        let mut binding = abi_preserving_stub(&old, &victims);

        let res = propagate(&old, &rev, "tcl", &new_provides, &mut binding);
        assert_eq!(res.rebuilt, vec!["expect", "tcl"]);
        assert!(res.blocked.is_empty());
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
    fn detection_and_backup_share_abi_soname_set() {
        // 对称性锁死：备份端（repo::backup_removed_sonames 内部）与检测端（removed_sonames）
        // 必须用同一个 ABI 面集合（soname_provides_of）。dev symlink（libfoo.so 有版本化兄弟
        // libfoo.so.1）两端都排除；无 SONAME 实体库（libtcl8.6.so）两端都纳入。
        let old = Index::parse(
            "libfoo|1.0:hash::libfoo.so,libfoo.so.1:libc.so.6|\n\
             tcl|8.6.16:hash::libtcl8.6.so:libc.so.6,libtcl8.6.so|\n",
        );
        // 检测端
        let det = removed_sonames(
            &old,
            "libfoo",
            &["libfoo.so".to_string(), "libfoo.so.2".to_string()],
        );
        // 备份端（同一计算：ABI(old) − ABI(new)）
        let old_s = soname_provides_of(&old.packages["libfoo"].provides);
        let new_s = soname_provides_of(&["libfoo.so".to_string(), "libfoo.so.2".to_string()]);
        let bak: Vec<String> = old_s.difference(&new_s).cloned().collect();
        assert_eq!(det, bak, "dev symlink 场景：检测与备份必须一致");
        assert_eq!(
            det,
            vec!["libfoo.so.1"],
            "libfoo.so（dev symlink）不应是 ABI 面"
        );

        // tcl 无 SONAME 实体库：两端都识别 libtcl8.6.so 为 ABI 面
        let det2 = removed_sonames(&old, "tcl", &["libtcl9.0.so".to_string()]);
        let old_s2 = soname_provides_of(&old.packages["tcl"].provides);
        let new_s2 = soname_provides_of(&["libtcl9.0.so".to_string()]);
        let bak2: Vec<String> = old_s2.difference(&new_s2).cloned().collect();
        assert_eq!(det2, bak2, "无 SONAME 实体库场景：检测与备份必须一致");
        assert_eq!(det2, vec!["libtcl8.6.so"]);
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
