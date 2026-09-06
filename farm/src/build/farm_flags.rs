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

use std::collections::HashSet;

/// 当前支持的 farm flag（字符串形式，即 LankeBUILD.json 里写死的字面量）。
pub const BUILD_AFTER_BUILD_DEPS: &str = "BUILD_AFTER_BUILD_DEPS";

/// 解析后的 farm flag（类型化，便于 `contains` 与未来扩展穷举）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum FarmFlag {
    /// 该包的 build_deps 也作为依赖边参与 Kahn 排序。
    BuildAfterBuildDeps,
}

impl FarmFlag {
    /// 单个 flag 字符串 → 类型化 flag。未知 → None（调用方告警提示拼写错误）。
    pub fn parse(s: &str) -> Option<FarmFlag> {
        match s.trim() {
            BUILD_AFTER_BUILD_DEPS => Some(FarmFlag::BuildAfterBuildDeps),
            _ => None,
        }
    }
}

/// 解析配方的 `farm_flags` 数组。未知 flag → stderr 告警（不阻断，但拼写错误会
/// 被暴露，避免"写了 flag 却悄悄不生效"的坑）。
pub fn parse_all(flags: &[String]) -> HashSet<FarmFlag> {
    let mut out = HashSet::new();
    for raw in flags {
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
    }

    #[test]
    fn parse_all_collects_known_ignores_unknown() {
        let set = parse_all(&[
            "BUILD_AFTER_BUILD_DEPS".into(),
            "NOPE".into(),
            "BUILD_AFTER_BUILD_DEPS".into(),
        ]);
        assert_eq!(set, HashSet::from([FarmFlag::BuildAfterBuildDeps]));
    }
}
