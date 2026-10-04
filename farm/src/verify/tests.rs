//! `src/verify.rs` 的单元测试（从生产文件搬出：那里测试比生产还长，正文被淹没）。
//! 仍是同一父模块的子模块 ⇒ `use super::*` 与 crate 内私有项照旧可用。

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
        "needed_so": ["libc.so.6"], "provides_soname": ["libp.so.1"], "deps": ["bash"],
    });
    let s = ScanResult::from_metadata_json(&full);
    assert_eq!(s.name, "p");
    assert_eq!(s.version, "1.0");
    assert_eq!(s.needed_so, vec!["libc.so.6"]);
    assert_eq!(s.provides_soname, vec!["libp.so.1"]);
    assert_eq!(s.deps, vec!["bash"]);
    // 缺字段 → 空（不 panic）
    let empty = ScanResult::from_metadata_json(&serde_json::json!({}));
    assert!(
        empty.needed_so.is_empty() && empty.provides_soname.is_empty() && empty.deps.is_empty()
    );
    assert_eq!(empty.name, "");
}

#[test]
fn provides_order_insensitive() {
    let actual = ScanResult::new(&["libc.so.6"], &["libfoo.so.1", "libfoo.so"], &["bash"]);
    let meta = ScanResult::new(&["libc.so.6"], &["libfoo.so", "libfoo.so.1"], &["bash"]);
    assert_eq!(decide(&actual, &meta), VerifyAction::Unchanged);
}
