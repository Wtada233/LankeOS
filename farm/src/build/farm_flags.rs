//! farm_flags.rs — LankeBUILD.json 的 `farm_flags` farm metadata。
//!
//! `farm_flags` 是配方（LankeBUILD.json）里的一个字符串数组，**只给 farm 的
//! build/validate 看**（影响构建顺序等 farm 行为）；lpkg 构建不消费它（未知字段
//! serde 忽略，对 lpkg 无害）。
//!
//! 目前支持的 flag：
//!
//! ```json
//! { "farm_flags": ["BUILD_AFTER_BUILD_DEPS"] }
//! ```
//!
//! - `BUILD_AFTER_BUILD_DEPS`：把该包的 `build_deps` 无条件放入依赖边参与 Kahn
//!   拓扑排序（**只要依赖在本轮 targets**）。
//!
//!   farm 的**默认语义**（无 flag）也已让 `build_deps` 进边——但**只限「本轮起点旧索引里没有的
//!   依赖」**（从未进 repo、同轮首建/引导，如 gjs 构建依赖首建的 sysprof）：这类依赖不先建，
//!   依赖方容器 `lpkg upgrade` 装不到它必然 BLOCKED。**已在仓库的依赖默认不建边**——容器里每个
//!   构建 `lpkg upgrade` 从 repo 自取最新版构建工具，无需排队。
//!
//!   本 flag 是**更强的 opt-in**，覆盖默认不建的「依赖已在仓库但本轮也重建」场景：某些包
//!   **构建期就依赖另一个也在重建的包**（如 python-bar 构建时需要 python-foo 刚产出的产物），
//!   两者都在本轮 targets 时须先建被依赖者，否则容器里还是旧版、构建基于旧 ABI 白跑。
//!   该 flag 的效果与链接边/组边一致：**只对 targets 内的包生效**（build_deps
//!   指向本轮不重建的包 → 边被丢弃，包直接构建不等待）。
//!
//! - `IGNORE_CHK_ABI` / `IGNORE_CHK_QML` / `IGNORE_CHK_PKGCONF` / `IGNORE_CHK_PKGERR` /
//!   `IGNORE_CHK_HOOK`：包级**豁免**——`farm chk full` 及各 chk（qml/pkgconf/pkg-err/hook/abi）对带对应 flag 的包跳过该检則（承认已知/有意为之）。这些 flag 不影响构建序，
//!   只被 chk 消费；在此注册以免 build 解析时误报"未知 flag"。

use std::collections::HashSet;

/// 当前支持的 farm flag（字符串形式，即 LankeBUILD.json 里写死的字面量）。
pub const BUILD_AFTER_BUILD_DEPS: &str = "BUILD_AFTER_BUILD_DEPS";
/// `IGNORE_CHK_<KIND>`：fullchk 族对该包跳过检則 KIND（KIND ∈ ABI/QML/PKGCONF/PKGERR/HOOK）。
pub const IGNORE_CHK_PREFIX: &str = "IGNORE_CHK_";
/// **字符串列表 flag**：`NAME=v1,v2,…`（值用 `=` 后逗号分隔）。注册成已知 flag，构建序不消费，
/// 只被 custom-checks 读。例：`QML_CHK_IGN_LST=org.kde.kwin,HelperWidgets`。
pub const QML_CHK_IGN_LST: &str = "QML_CHK_IGN_LST";

/// 解析后的 farm flag（类型化，便于 `contains` 与未来扩展穷举）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum FarmFlag {
    /// 该包的 build_deps 也作为依赖边参与 Kahn 排序。
    BuildAfterBuildDeps,
    /// 忽略 ABI 符号@版本检則（abichk / fullchk）。
    IgnoreChkAbi,
    IgnoreChkQml,
    IgnoreChkPkgconf,
    IgnoreChkPkgErr,
    IgnoreChkHook,
    /// 带字符串列表值的 flag（值 = `=` 后逗号分隔）。类型化只需"已知"，值由 `list_value` 取。
    StringList,
}

/// 取已知列表 flag 的**字符串列表**值：farm_flags 成员形如 `{ "QML_CHK_IGN_LST": ["a","b"] }`，
/// 用 serde_json 直接读 JSON 数组（不用手写转义/切分）。非该形状的成员跳过。
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
    /// 支持 `NAME=v1,v2` 的列表值形式：按第一个 `=` 切出 NAME 再匹配。
    pub fn parse(s: &str) -> Option<FarmFlag> {
        let t = s.trim();
        let name = t.split_once('=').map(|(n, _)| n.trim()).unwrap_or(t);
        if name == BUILD_AFTER_BUILD_DEPS {
            return Some(FarmFlag::BuildAfterBuildDeps);
        }
        if name == QML_CHK_IGN_LST {
            return Some(FarmFlag::StringList);
        }
        match name.strip_prefix(IGNORE_CHK_PREFIX) {
            Some("ABI") => Some(FarmFlag::IgnoreChkAbi),
            Some("QML") => Some(FarmFlag::IgnoreChkQml),
            Some("PKGCONF") => Some(FarmFlag::IgnoreChkPkgconf),
            Some("PKGERR") => Some(FarmFlag::IgnoreChkPkgErr),
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
            None => eprintln!("  未知 farm flag: {raw}"),
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_known_and_unknown() {
        assert_eq!(
            FarmFlag::parse("BUILD_AFTER_BUILD_DEPS"),
            Some(FarmFlag::BuildAfterBuildDeps)
        );
        assert_eq!(FarmFlag::parse("UNKNOWN_FLAG"), None);
        assert_eq!(FarmFlag::parse(""), None);
        // 首尾空白容忍（人为手写 YAML/JSON 常见）
        assert_eq!(
            FarmFlag::parse("  BUILD_AFTER_BUILD_DEPS  "),
            Some(FarmFlag::BuildAfterBuildDeps)
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
            serde_json::json!("BUILD_AFTER_BUILD_DEPS"),
            serde_json::json!("IGNORE_CHK_HOOK"),
            serde_json::json!("NOPE"),
            serde_json::json!("BUILD_AFTER_BUILD_DEPS"),
            serde_json::json!({ "QML_CHK_IGN_LST": ["org.kde.kwin"] }),
        ]);
        assert_eq!(
            set,
            HashSet::from([FarmFlag::BuildAfterBuildDeps, FarmFlag::IgnoreChkHook])
        );
    }
}
