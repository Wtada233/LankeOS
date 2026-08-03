//! groups.rs — `data/build/*.yaml` 声明式 ABI 重建组。
//!
//! 构建顺序**只根据 needed_so**（链接依赖才必须重建）；但"不链 libpython 但 ABI 敏感"的包
//! （python-cairo / python-gobject / blueman / meson …）不产生链接边 → 不会自动成为 ABI 受害者。
//! 用声明式 YAML 声明：某包 SONAME 断裂时强制重建哪些包（`*` glob）。
//!
//! ```yaml
//! # data/build/python.yaml
//! rebuild-on-abichange: python
//! packages: python-* meson gobject-introspection blueman
//! ```
//!
//! 与 data/trackers 同一套模式：`rebuild-on-abichange` 按包名索引，`packages` 是空格分隔的 glob。

use std::collections::{HashMap, HashSet};
use std::path::Path;

use crate::tr;

#[derive(Debug, Clone, serde::Deserialize)]
struct RebuildGroupRaw {
    #[serde(rename = "rebuild-on-abichange")]
    on: String,
    /// 空格分隔的 glob 列表（如 `python-* meson blueman`）
    #[serde(default)]
    packages: String,
}

/// 全部重建组：`on` 包 → 需要重建的 glob 模式。
#[derive(Debug, Default)]
pub struct RebuildGroups {
    map: HashMap<String, Vec<String>>,
}

impl RebuildGroups {
    /// 扫描 `data/build/*.yaml` 加载全部重建组。目录缺失/空 → 空组（无害）。
    pub fn load(data_dir: &Path) -> RebuildGroups {
        let mut groups = RebuildGroups::default();
        if let Ok(rd) = std::fs::read_dir(data_dir) {
            for entry in rd.flatten() {
                let path = entry.path();
                if path.extension().and_then(|e| e.to_str()) != Some("yaml") {
                    continue;
                }
                let Ok(content) = std::fs::read_to_string(&path) else { continue };
                let Ok(cfg) = serde_yaml::from_str::<RebuildGroupRaw>(&content) else {
                    eprintln!("{}", tr!("build.group_parse_fail", path.display()));
                    continue;
                };
                let globs: Vec<String> = cfg.packages.split_whitespace().map(String::from).collect();
                groups.map.entry(cfg.on).or_default().extend(globs);
            }
        }
        groups
    }

    /// `on` 包 ABI 断裂时，匹配 `packages` glob 的包集合（名字升序，确定性）。
    /// `all_pkgs` = 全部配方包名（`sorted_pkg_names`）；只匹配存在的包。
    pub fn victims_for(&self, on: &str, all_pkgs: &[String]) -> Vec<String> {
        let mut set = HashSet::new();
        if let Some(globs) = self.map.get(on) {
            for g in globs {
                for p in all_pkgs {
                    if glob_match(g, p) {
                        set.insert(p.clone());
                    }
                }
            }
        }
        let mut v: Vec<String> = set.into_iter().collect();
        v.sort();
        v
    }
}

/// 简单 glob：只支持 `*`（任意字符序列）。确定性、无 panic。
/// `python-*` 匹配 `python-cairo`（不匹配 `python` 本身）。
pub fn glob_match(pattern: &str, s: &str) -> bool {
    let p: Vec<char> = pattern.chars().collect();
    let t: Vec<char> = s.chars().collect();
    let (mut pi, mut ti) = (0usize, 0usize);
    let (mut star, mut star_ti) = (None, 0usize);
    while ti < t.len() {
        if pi < p.len() && p[pi] == t[ti] {
            pi += 1;
            ti += 1;
        } else if pi < p.len() && p[pi] == '*' {
            star = Some(pi);
            star_ti = ti;
            pi += 1;
        } else if let Some(sp) = star {
            pi = sp + 1;
            star_ti += 1;
            ti = star_ti;
        } else {
            return false;
        }
    }
    while pi < p.len() && p[pi] == '*' {
        pi += 1;
    }
    pi == p.len()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn glob_match_star_wildcard() {
        assert!(glob_match("python-*", "python-cairo"));
        assert!(glob_match("python-*", "python-gobject"));
        assert!(!glob_match("python-*", "python"), "需要 `python-` 前缀");
        assert!(!glob_match("python-*", "python3"));
        assert!(glob_match("*", "anything"));
        assert!(glob_match("blueman", "blueman"));
        assert!(!glob_match("blueman", "blue"));
        assert!(glob_match("a*c", "abc"));
        assert!(glob_match("a*c", "ac"));
        assert!(!glob_match("a*c", "ab"));
    }

    #[test]
    fn victims_for_matches_globs_and_sorts() {
        let mut groups = RebuildGroups::default();
        groups.map.insert(
            "python".into(),
            vec!["python-*".into(), "meson".into(), "blueman".into()],
        );
        let all: Vec<String> = ["python", "python-cairo", "python-gobject", "meson", "blueman", "glib"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let v = groups.victims_for("python", &all);
        assert_eq!(v, vec!["blueman", "meson", "python-cairo", "python-gobject"]);
        assert!(groups.victims_for("unlisted", &all).is_empty());
    }
}
