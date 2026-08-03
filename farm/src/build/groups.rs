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

    /// (victim, on) 依赖边：`on` 的组受害者必须在 `on` 构建**之后**构建。
    /// 这与 needed_so 链接边同等对待——**`--all` 模式下组受害者已在初始队列**，
    /// 若没有这条边，topo 只按链接边排，python-cairo（不链 libpython）会被排到
    /// python 之前，容器里 `lpkg upgrade` 时本地 repo 还是旧 python，构建白跑。
    ///
    /// 只包含**两边都在 `names` 中**的边（`on` 不在本轮 targets 里就没有排序约束）；
    /// 去重且 (victim, on) 字典序排序，确定性。
    pub fn trigger_edges_in(&self, names: &[String]) -> Vec<(String, String)> {
        let present: HashSet<&str> = names.iter().map(String::as_str).collect();
        let mut edges: Vec<(String, String)> = Vec::new();
        for (on, globs) in &self.map {
            if !present.contains(on.as_str()) {
                continue;
            }
            for g in globs {
                for p in names {
                    if p.as_str() != on.as_str() && glob_match(g, p) {
                        edges.push((p.clone(), on.clone()));
                    }
                }
            }
        }
        edges.sort();
        edges.dedup();
        edges
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
    fn load_parses_multiple_space_separated_globs() {
        // 回归：从真实 YAML 内容走 serde_yaml 路径（不是手动 map.insert），
        // 确认 `packages: python-* meson gobject-introspection blueman` 解析出全部 4 个 glob，
        // 而不是只解析出第一个。
        let dir = std::env::temp_dir().join(format!("farm-groups-load-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(
            dir.join("python.yaml"),
            "rebuild-on-abichange: python\npackages: python-* meson gobject-introspection blueman\n",
        )
        .unwrap();
        let g = RebuildGroups::load(&dir);
        let all: Vec<String> = [
            "python",
            "python-cairo",
            "python-gobject",
            "meson",
            "gobject-introspection",
            "blueman",
            "glib",
        ]
        .iter()
        .map(|s| s.to_string())
        .collect();
        let v = g.victims_for("python", &all);
        assert_eq!(
            v,
            vec![
                "blueman",
                "gobject-introspection",
                "meson",
                "python-cairo",
                "python-gobject"
            ],
            "serde_yaml 必须解析出全部 4 个 glob: {v:?}"
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn trigger_edges_only_when_on_in_names() {
        let mut groups = RebuildGroups::default();
        groups.map.insert(
            "python".into(),
            vec!["python-*".into(), "meson".into()],
        );
        // 两边都在 names → 产出边
        let all: Vec<String> = ["python", "python-cairo", "python-gobject", "meson"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let e = groups.trigger_edges_in(&all);
        assert_eq!(
            e,
            vec![
                ("meson".to_string(), "python".to_string()),
                ("python-cairo".to_string(), "python".to_string()),
                ("python-gobject".to_string(), "python".to_string()),
            ]
        );
        // on 不在 names → 无约束（python 不重建，组受害者不强制排序）
        let without_on: Vec<String> = ["python-cairo", "meson"].iter().map(|s| s.to_string()).collect();
        assert!(groups.trigger_edges_in(&without_on).is_empty());
        // 去重：同一 (victim,on) 不会被重复产出
        let dup: Vec<String> = ["python", "python-cairo", "python-cairo", "meson"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let e2 = groups.trigger_edges_in(&dup);
        assert_eq!(e2.len(), 2, "python-cairo 只应出现一次: {e2:?}");
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
