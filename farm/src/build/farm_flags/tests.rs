//! `src/build/farm_flags.rs` 的单元测试。
//! 仍是同一父模块的子模块 ⇒ `use super::*` 与 crate 内私有项照旧可用。

use super::*;

#[test]
fn parse_known_and_unknown() {
    // BUILD_AFTER_BUILD_DEPS 必须**未知**——配方里残留会走 warn，不静默
    assert_eq!(FarmFlag::parse("BUILD_AFTER_BUILD_DEPS"), None);
    assert_eq!(FarmFlag::parse("UNKNOWN_FLAG"), None);
    assert_eq!(FarmFlag::parse(""), None);
    // 首尾空白容忍（人为手写 YAML/JSON 常见）
    assert_eq!(
        FarmFlag::parse("  IGNORE_CHK_QML  "),
        Some(FarmFlag::IgnoreChkQml)
    );
    // 豁免 flag：大写 KIND
    assert_eq!(
        FarmFlag::parse("IGNORE_CHK_ABI"),
        Some(FarmFlag::IgnoreChkAbi)
    );
    assert_eq!(
        FarmFlag::parse("IGNORE_CHK_QML"),
        Some(FarmFlag::IgnoreChkQml)
    );
    assert_eq!(
        FarmFlag::parse("IGNORE_CHK_ABI "),
        Some(FarmFlag::IgnoreChkAbi)
    );
    assert_eq!(FarmFlag::parse("IGNORE_CHK_FOO"), None);
}

#[test]
fn parse_all_collects_known_ignores_unknown() {
    let set = parse_all(&[
        serde_json::json!("IGNORE_CHK_HOOK"),
        serde_json::json!("NOPE"),
        serde_json::json!("IGNORE_CHK_HOOK"),
        serde_json::json!({ "QML_CHK_IGN_LST": ["org.kde.kwin"] }),
    ]);
    assert_eq!(set, HashSet::from([FarmFlag::IgnoreChkHook]));
}

#[test]
fn bare_value_form_is_rejected_not_silently_dropped() {
    // `NAME=a,b` 裸串不再被当已知 flag（consumer 只读 JSON 数组）→ 返回 None（调用方告警），
    // 而不是像旧实现那样收下 flag 却静默丢值。
    assert_eq!(FarmFlag::parse("QML_CHK_IGN_LST=a,b"), None);
    // 裸名仍认（不误报"未知 flag"）
    assert_eq!(
        FarmFlag::parse("QML_CHK_IGN_LST"),
        Some(FarmFlag::StringList)
    );
    // 数组对象成员不是字符串 → parse_all 跳过（不进构建序 flag 集）
    assert!(parse_all(&[serde_json::json!({"QML_CHK_IGN_LST": ["a"]})]).is_empty());
    // string_list 只读对象数组，字符串成员一律读不到值
    let good = [serde_json::json!({"QML_CHK_IGN_LST": ["a", "b"]})];
    assert_eq!(string_list(&good, "QML_CHK_IGN_LST"), vec!["a", "b"]);
    let bad = [serde_json::json!("QML_CHK_IGN_LST=a,b")];
    assert!(string_list(&bad, "QML_CHK_IGN_LST").is_empty());
}
