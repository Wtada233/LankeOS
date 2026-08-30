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
//! - `BUILD_AFTER_BUILD_DEPS`：把该包的 `build_deps` 也放入依赖边，参与 Kahn
//!   拓扑排序。默认 build_deps **不参与排序**（容器里每个构建 `lpkg upgrade`
//!   从 repo 自取最新版构建工具，无需排队）；但某些包**构建期就依赖另一个也
//!   在重建的包**（如 python-bar 构建时需要 python-foo 刚产出的产物），两者都
//!   在本轮 targets 时须先建被依赖者，否则容器里还是旧版、构建基于旧 ABI 白跑。
//!   该 flag 的效果与链接边/组边一致：**只对 targets 内的包生效**（build_deps
//!   指向本轮不重建的包 → 边被丢弃，包直接构建不等待）。

use std::collections::HashSet;
use std::path::Path;

use super::read_lankebuild;

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

/// 读某包配方声明的 farm flags（LankeBUILD.json `farm_flags` 数组）。
/// 配方缺失/解析失败 → 空集（无 flag 声明 = 默认行为）。
pub fn flags_of(pkgs_dir: &Path, pkg: &str) -> HashSet<FarmFlag> {
    match read_lankebuild(pkgs_dir, pkg) {
        Some(lb) => parse_all(&lb.farm_flags),
        None => HashSet::new(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write_pkg_json(dir: &Path, name: &str, farm_flags: &[&str]) {
        let pkg_dir = dir.join(name);
        std::fs::create_dir_all(&pkg_dir).unwrap();
        let json = serde_json::json!({
            "name": name,
            "version": "1.0",
            "build_deps": [],
            "farm_flags": farm_flags,
        });
        std::fs::write(
            pkg_dir.join("LankeBUILD.json"),
            serde_json::to_string(&json).unwrap(),
        )
        .unwrap();
        std::fs::write(pkg_dir.join("LankeBUILD"), "").unwrap();
    }

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

    #[test]
    fn flags_of_reads_recipe() {
        let dir = std::env::temp_dir().join(format!("farm-flags-read-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        write_pkg_json(&dir, "python-bar", &["BUILD_AFTER_BUILD_DEPS"]);
        write_pkg_json(&dir, "python-foo", &[]);
        assert_eq!(
            flags_of(&dir, "python-bar"),
            HashSet::from([FarmFlag::BuildAfterBuildDeps])
        );
        assert!(flags_of(&dir, "python-foo").is_empty(), "无声明 → 空集");
        assert!(flags_of(&dir, "no-such-pkg").is_empty(), "配方缺失 → 空集");
        std::fs::remove_dir_all(&dir).ok();
    }
}
