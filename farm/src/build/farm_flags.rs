//! farm_flags.rs — LankeBUILD.json 的 `farm_flags` farm metadata。
//!
//! `farm_flags` 是配方（LankeBUILD.json）里的一个字符串数组，**只给 farm 的
//! build/validate 看**（影响构建顺序等 farm 行为）；lpkg 构建不消费它（未知字段
//! serde 忽略，对 lpkg 无害）。
//!
//! 目前支持的 flag：
//!
//! ```json
//! { "farm_flags": ["IGNORE_CHK_QML"] }
//! ```
//!
//! > **已删除：`BUILD_AFTER_BUILD_DEPS`**。它曾把该包的 `build_deps` **无条件**放进拓扑依赖边；
//! > 该行为现在就是**默认语义**（见 `build/sched.rs::topo_order`：`build_deps` 无条件进边，
//! > 仅限本轮 targets 内）。配方里若还写着它 → 走"未知 farm flag"告警（`parse_all`），
//! > **不是**静默忽略——留着它没有意义，应删掉。
//!
//! - `IGNORE_CHK_ABI` / `IGNORE_CHK_QML` / `IGNORE_CHK_PKGCONF` / `IGNORE_CHK_PKGERR` /
//!   `IGNORE_CHK_INTROSPECTION` / `IGNORE_CHK_VAPI` / `IGNORE_CHK_BUILDDEPS` / `IGNORE_CHK_PYCACHE` /
//!   `IGNORE_CHK_HOOK`：包级**豁免**——`farm chk full` 及各 chk
//!   （qml/pkgconf/pkg-err/introspection/vapi/build-deps/pycache/hook/abi）对带对应 flag 的包跳过该检則
//!   （承认已知/有意为之）。这些 flag 不影响构建序，只被 chk 消费；
//!   在此注册以免 build 解析时误报"未知 flag"。

use std::collections::HashSet;

/// `IGNORE_CHK_<KIND>`：fullchk 族对该包跳过检則 KIND
/// （KIND ∈ ABI/QML/PKGCONF/PKGERR/INTROSPECTION/VAPI/BUILDDEPS/PYCACHE/HOOK）。
pub const IGNORE_CHK_PREFIX: &str = "IGNORE_CHK_";
/// **字符串列表 flag** 的**裸名**：值必须写成 JSON 数组对象成员
/// `{"QML_CHK_IGN_LST": ["org.kde.kwin", "HelperWidgets"]}`（consumer = `string_list`）。
/// 注册裸名只是让它在构建序解析里被认作已知 flag（不告警）；`NAME=a,b` 裸串形式已废弃，见 `parse`。
pub const QML_CHK_IGN_LST: &str = "QML_CHK_IGN_LST";

/// 解析后的 farm flag（类型化，便于 `contains` 与未来扩展穷举）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum FarmFlag {
    /// 忽略 ABI 符号@版本检則（abichk / fullchk）。
    IgnoreChkAbi,
    IgnoreChkQml,
    IgnoreChkPkgconf,
    IgnoreChkPkgErr,
    /// 忽略 introspectionchk（装了 .gir 却未声明 gobject-introspection 构建依赖）。
    IgnoreChkIntrospection,
    /// 忽略 vapichk（装了 .vapi 却未声明 vala 构建依赖）。
    IgnoreChkVapi,
    /// 忽略 build-deps chk（needed_so 的提供者未写进 build_deps）。
    IgnoreChkBuildDeps,
    /// 忽略 pycachechk（包内含 `__pycache__` 字节码缓存目录）。
    IgnoreChkPycache,
    IgnoreChkHook,
    /// 列表值 flag：类型化只需"已知"（构建序不消费，值由 `string_list` 从 JSON 数组读）。
    StringList,
}

/// 取已知列表 flag 的**字符串列表**值：farm_flags 成员形如 `{ "QML_CHK_IGN_LST": ["a","b"] }`，
/// 用 serde_json 直接读 JSON 数组（不用手写转义/切分）。**非该形状的成员一律跳过**——字符串成员
/// （如 `"QML_CHK_IGN_LST=a,b"`）不被读取，值会丢失（裸串形式已废弃，见 `parse`）。
pub fn string_list(flags: &[serde_json::Value], name: &str) -> Vec<String> {
    let mut out = Vec::new();
    for v in flags {
        let Some(obj) = v.as_object() else {
            continue;
        };
        let Some(arr) = obj.get(name).and_then(|a| a.as_array()) else {
            continue;
        };
        out.extend(arr.iter().filter_map(|x| x.as_str()).map(String::from));
    }
    out
}

impl FarmFlag {
    /// 单个 flag 字符串 → 类型化 flag。未知 → None（调用方告警提示拼写错误）。
    /// **只认裸 flag 名**：`NAME=...` 值形式已废弃——列表值只能写成 JSON 数组对象成员
    /// （`{"QML_CHK_IGN_LST": ["a","b"]}`，见 `string_list`）。裸串永远不被 consumer 读取，
    /// 曾静默丢值；现在会走"未知 flag"告警，响亮暴露拼写错误。
    pub fn parse(s: &str) -> Option<FarmFlag> {
        let name = s.trim();
        if name == QML_CHK_IGN_LST {
            return Some(FarmFlag::StringList);
        }
        match name.strip_prefix(IGNORE_CHK_PREFIX) {
            Some("ABI") => Some(FarmFlag::IgnoreChkAbi),
            Some("QML") => Some(FarmFlag::IgnoreChkQml),
            Some("PKGCONF") => Some(FarmFlag::IgnoreChkPkgconf),
            Some("PKGERR") => Some(FarmFlag::IgnoreChkPkgErr),
            Some("INTROSPECTION") => Some(FarmFlag::IgnoreChkIntrospection),
            Some("VAPI") => Some(FarmFlag::IgnoreChkVapi),
            Some("BUILDDEPS") => Some(FarmFlag::IgnoreChkBuildDeps),
            Some("PYCACHE") => Some(FarmFlag::IgnoreChkPycache),
            Some("HOOK") => Some(FarmFlag::IgnoreChkHook),
            _ => None,
        }
    }
}

/// 解析配方的 `farm_flags`（成员可为字符串 flag 或对象 `{NAME:[…]}`）。**只有字符串成员当 flag**：
/// 对象/数组成员不参与构建序。未知字符串 flag → stderr 告警（暴露拼写错误）。
pub fn parse_all(flags: &[serde_json::Value]) -> HashSet<FarmFlag> {
    let mut out = HashSet::new();
    for v in flags {
        let Some(raw) = v.as_str() else {
            continue; // 对象/数组（列表值 flag）不是构建序 flag
        };
        match FarmFlag::parse(raw) {
            Some(f) => {
                out.insert(f);
            }
            None => eprintln!("{}", crate::tr!("farm_flags.unknown", raw)),
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_known_and_unknown() {
        // 已删除的 BUILD_AFTER_BUILD_DEPS 现在必须**未知**——配方里残留会走 warn，不静默
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
}
