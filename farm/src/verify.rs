//! 元数据一致性校验与 repack/传播决策（§6）。
//!
//! 实际扫描结果 vs metadata.json（配方/上一版）比较**两**个 farm 所有权的字段：
//! - `provides` 漂移 → ABI 面变化 → 最高信号：repack 修正 + 传播重建依赖者
//! - `needed_so` 漂移 → 元数据陈旧（二进制没变）→ repack（不 rebuild）
//!
//! **`deps` 不参与比较**：deps 由 gen_deps/deprules 规则生成，farm 不扫不比（repack.rs 同
//! 契约"deps 不读不改"）。`ScanResult.deps` 保留以表达扫描输出形状，但 decide 不读它。

use std::collections::HashSet;

/// 构建后扫描结果 / 期望元数据 —— **全库唯一来源**。
///
/// 历史：曾有两份同名异构 `ScanResult`（`scan.rs` 版含 name/version，本模块版只有
/// needed_so/provides/deps），调用方 `build/repo.rs` 手工把两侧 `deps` 填空 → 字段增减时极易只改
/// 一边。现由 verify 拥有该类型：`decide()` 是唯一消费者、放它旁边最不易漂移；verify 不依赖
/// scan/lpkg，分层不变（是上层 scan 依赖本模块）。
/// `name`/`version` 是扫描侧的溯源信息，**`decide()` 不读**（只读 needed_so/provides；deps 亦不读）。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ScanResult {
    pub name: String,
    pub version: String,
    pub needed_so: Vec<String>,
    pub provides: Vec<String>,
    pub deps: Vec<String>,
}

impl ScanResult {
    /// 三名参构造（name/version 留空）：比较/测试侧用。
    pub fn new(needed_so: &[&str], provides: &[&str], deps: &[&str]) -> Self {
        ScanResult {
            needed_so: needed_so.iter().map(|s| s.to_string()).collect(),
            provides: provides.iter().map(|s| s.to_string()).collect(),
            deps: deps.iter().map(|s| s.to_string()).collect(),
            ..Default::default()
        }
    }

    /// 由已知的三字段（已拥有所有权）构造（比较侧：`BuildOutcome` → actual）。
    pub fn from_parts(needed_so: Vec<String>, provides: Vec<String>, deps: Vec<String>) -> Self {
        ScanResult {
            needed_so,
            provides,
            deps,
            ..Default::default()
        }
    }

    /// 由 `.lpkg` 内 metadata.json 的 `Value` 构造（期望值；缺字段 → 空）。name/version 仅作溯源。
    pub fn from_metadata_json(meta: &serde_json::Value) -> Self {
        let arr = |k: &str| -> Vec<String> {
            meta[k]
                .as_array()
                .map(|a| {
                    a.iter()
                        .filter_map(|v| v.as_str().map(String::from))
                        .collect()
                })
                .unwrap_or_default()
        };
        ScanResult {
            name: meta["name"].as_str().unwrap_or("").to_string(),
            version: meta["version"].as_str().unwrap_or("").to_string(),
            needed_so: arr("needed_so"),
            provides: arr("provides"),
            deps: arr("deps"),
        }
    }

    /// 补溯源信息（扫描侧：`scan_lpkg` 用）。
    pub fn with_name(mut self, name: impl Into<String>, version: impl Into<String>) -> Self {
        self.name = name.into();
        self.version = version.into();
        self
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
    fn name_version_do_not_affect_decide() {
        // 合并类型后 name/version 只是溯源信息：两侧仅这两项不同 → 仍 Unchanged
        let mut actual = ScanResult::new(&["libc.so.6"], &["libz.so"], &[]);
        actual.name = "pkg-a".into();
        actual.version = "1.0".into();
        let mut meta = ScanResult::new(&["libc.so.6"], &["libz.so"], &[]);
        meta.name = "pkg-a".into();
        meta.version = "2.0".into();
        assert_eq!(decide(&actual, &meta), VerifyAction::Unchanged);
    }

    #[test]
    fn from_metadata_json_handles_missing_fields() {
        let full = serde_json::json!({
            "name": "p", "version": "1.0",
            "needed_so": ["libc.so.6"], "provides": ["libp.so.1"], "deps": ["bash"],
        });
        let s = ScanResult::from_metadata_json(&full);
        assert_eq!(s.name, "p");
        assert_eq!(s.version, "1.0");
        assert_eq!(s.needed_so, vec!["libc.so.6"]);
        assert_eq!(s.provides, vec!["libp.so.1"]);
        assert_eq!(s.deps, vec!["bash"]);
        // 缺字段 → 空（不 panic）
        let empty = ScanResult::from_metadata_json(&serde_json::json!({}));
        assert!(empty.needed_so.is_empty() && empty.provides.is_empty() && empty.deps.is_empty());
        assert_eq!(empty.name, "");
    }

    #[test]
    fn provides_order_insensitive() {
        let actual = ScanResult::new(&["libc.so.6"], &["libfoo.so.1", "libfoo.so"], &["bash"]);
        let meta = ScanResult::new(&["libc.so.6"], &["libfoo.so", "libfoo.so.1"], &["bash"]);
        assert_eq!(decide(&actual, &meta), VerifyAction::Unchanged);
    }
}
