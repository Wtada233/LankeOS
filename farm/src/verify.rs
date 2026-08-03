//! 元数据一致性校验与 repack/传播决策（§6）。
//!
//! 实际扫描结果 vs metadata.json（配方/上一版）比较**两**个 farm 所有权的字段：
//! - `provides` 漂移 → ABI 面变化 → 最高信号：repack 修正 + 传播重建依赖者
//! - `needed_so` 漂移 → 元数据陈旧（二进制没变）→ repack（不 rebuild）
//!
//! **`deps` 不参与比较**：deps 由 gen_deps/deprules 规则生成，farm 不扫不比（repack.rs 同
//! 契约"deps 不读不改"）。`ScanResult.deps` 保留以表达扫描输出形状，但 decide 不读它。

use std::collections::HashSet;

/// 构建后实际扫描结果（scan.rs 对 .lpkg 解包的产物；demo 中来自 stub）。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ScanResult {
    pub needed_so: Vec<String>,
    pub provides: Vec<String>,
    pub deps: Vec<String>,
}

impl ScanResult {
    pub fn new(needed_so: &[&str], provides: &[&str], deps: &[&str]) -> Self {
        ScanResult {
            needed_so: needed_so.iter().map(|s| s.to_string()).collect(),
            provides: provides.iter().map(|s| s.to_string()).collect(),
            deps: deps.iter().map(|s| s.to_string()).collect(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VerifyAction {
    /// 两字段全部一致 → 直接进 local repo
    Unchanged,
    /// needed_so 漂移（二进制未变，只元数据错）→ repack（不 rebuild）
    Repack { needed_drift: bool },
    /// provides 漂移 → ABI 面变化 → repack 修正 + 传播重建依赖者
    AbiBreak,
}

/// 决策：实际扫描 vs 期望 metadata。
/// provides 漂移优先（ABI 面变化是最高信号）。deps 不比较（见模块头注释）。
pub fn decide(actual: &ScanResult, meta: &ScanResult) -> VerifyAction {
    let needed_drift = sorted(&actual.needed_so) != sorted(&meta.needed_so);
    let provides_drift = set(&actual.provides) != set(&meta.provides);
    if provides_drift {
        VerifyAction::AbiBreak
    } else if needed_drift {
        VerifyAction::Repack { needed_drift }
    } else {
        VerifyAction::Unchanged
    }
}

fn sorted(v: &[String]) -> Vec<&str> {
    let mut s: Vec<&str> = v.iter().map(String::as_str).collect();
    s.sort_unstable();
    s
}

fn set(v: &[String]) -> HashSet<&str> {
    v.iter().map(String::as_str).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn all_consistent_is_unchanged() {
        let actual = ScanResult::new(
            &["libc.so.6", "libm.so.6"],
            &["libacl.so", "libacl.so.1"],
            &["bash"],
        );
        let meta = actual.clone();
        assert_eq!(decide(&actual, &meta), VerifyAction::Unchanged);
    }

    #[test]
    fn needed_drift_is_repack_not_rebuild() {
        let meta = ScanResult::new(&["libc.so.6", "libz.so.1"], &["libz.so", "libz.so.1"], &[]);
        let actual = ScanResult {
            needed_so: vec!["libc.so.6".into(), "libz.so.1".into(), "libm.so.6".into()],
            ..meta.clone()
        };
        assert_eq!(
            decide(&actual, &meta),
            VerifyAction::Repack { needed_drift: true }
        );
    }

    #[test]
    fn deps_drift_is_ignored() {
        // deps 由 gen_deps 生成，farm 不扫不比——只 deps 漂移必须判定为 Unchanged（不 repack）
        let actual = ScanResult::new(
            &["libc.so.6"],
            &["libz.so", "libz.so.1"],
            &["bash", "python"],
        );
        let meta = ScanResult::new(&["libc.so.6"], &["libz.so", "libz.so.1"], &["bash"]);
        assert_eq!(decide(&actual, &meta), VerifyAction::Unchanged);
    }

    #[test]
    fn provides_drift_is_abibreak() {
        let actual = ScanResult::new(
            &["libc.so.6", "libfoo.so.2"],
            &["libfoo.so", "libfoo.so.2"],
            &[],
        );
        let meta = ScanResult::new(
            &["libc.so.6", "libfoo.so.1"],
            &["libfoo.so", "libfoo.so.1"],
            &[],
        );
        assert_eq!(decide(&actual, &meta), VerifyAction::AbiBreak);
    }

    #[test]
    fn provides_drift_takes_precedence_over_needed() {
        let actual = ScanResult::new(
            &["libc.so.6", "libfoo.so.2"],
            &["libfoo.so", "libfoo.so.2"],
            &[],
        );
        let meta = ScanResult::new(
            &["libc.so.6", "libfoo.so.1", "libold.so.1"],
            &["libfoo.so", "libfoo.so.1"],
            &[],
        );
        assert_eq!(decide(&actual, &meta), VerifyAction::AbiBreak);
    }

    #[test]
    fn provides_order_insensitive() {
        let actual = ScanResult::new(&["libc.so.6"], &["libfoo.so.1", "libfoo.so"], &["bash"]);
        let meta = ScanResult::new(&["libc.so.6"], &["libfoo.so", "libfoo.so.1"], &["bash"]);
        assert_eq!(decide(&actual, &meta), VerifyAction::Unchanged);
    }
}
