//! multi-level-html-index 模板：N 级 HTML 目录探测（KDE frameworks、Qt 子模块等）。
//!
//! 解决单级 html-index 拿不到完整版本的结构：逐级进目录，最后一级取完整版本。
//! KDE frameworks 例：`6.29/` 目录 → 目录内 `karchive-6.29.0.tar.xz`。
//!
//! 字段：`levels`（N 级，每级一个 `{url, pattern}` 配对）+ `template`。
//! - `levels[i].url` 可引用已解出的前级版本 `{v1}..{vN}`；
//! - `levels[i].pattern` 提取该级版本（最后一级的捕获 = 最终版本）；
//! - `template` 可用 `{v1}..{vN}`（各级版本）`{version}`（= 最后一级）`{name}`（上游名）；
//! - `max-version` / `major-of` 逐级作用于版本选择（选目录/最终版本都受约束）。

use regex::Regex;

use crate::net::Fetcher;
use crate::track::templates;
use crate::track::{need, EntryProbe, LevelConfig, SourceConfig};

/// 逐级探测：第 i 级页面 → 提取版本 vi → 代入下一级 URL → 直到最后一级 = 最终版本。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &SourceConfig,
    major: Option<&str>,
    pkg_name: &str,
) -> Result<EntryProbe, String> {
    if cfg.levels.is_empty() {
        return Err("multi-level-html-index 需 levels 列表（每级 {url, pattern}）".into());
    }
    let template = need(&cfg.template, "template")?;

    let mut vers: Vec<String> = Vec::new(); // v1, v2, ..., vN
    for (i, lvl) in cfg.levels.iter().enumerate() {
        let v = probe_level(fetcher, lvl, &vers, major, cfg.max_version.as_deref(), i)?;
        vers.push(v);
    }
    let version = vers
        .last()
        .cloned()
        .ok_or_else(|| "levels 为空".to_string())?;

    // 最终 URL：{v1}..{vN} + {version} + {name}
    let mut vars: Vec<(String, String)> = vers
        .iter()
        .enumerate()
        .map(|(j, v)| (format!("v{}", j + 1), v.clone()))
        .collect();
    vars.push(("version".to_string(), version.clone()));
    vars.push(("name".to_string(), cfg.effective_name(pkg_name).to_string()));
    let vars_ref: Vec<(&str, &str)> = vars.iter().map(|(k, v)| (k.as_str(), v.as_str())).collect();
    let url = templates::substitute(template, &vars_ref);
    Ok(EntryProbe { version, url })
}

/// 探测单级：代入前级版本 → 抓页面 → 正则取最大版本。
fn probe_level(
    fetcher: &dyn Fetcher,
    lvl: &LevelConfig,
    prev: &[String],
    major: Option<&str>,
    max_version: Option<&str>,
    idx: usize,
) -> Result<String, String> {
    let lvl_url = need(&lvl.url, &format!("levels[{idx}].url"))?;
    let lvl_pattern = need(&lvl.pattern, &format!("levels[{idx}].pattern"))?;
    // 前级版本代入 {v1}..{vN}
    let prev_vars: Vec<(String, String)> = prev
        .iter()
        .enumerate()
        .map(|(j, v)| (format!("v{}", j + 1), v.clone()))
        .collect();
    let prev_ref: Vec<(&str, &str)> = prev_vars
        .iter()
        .map(|(k, v)| (k.as_str(), v.as_str()))
        .collect();
    let page = templates::substitute(lvl_url, &prev_ref);
    let html = fetcher.get(&page)?;
    let re = Regex::new(lvl_pattern).map_err(|e| format!("正则无效 {lvl_pattern}: {e}"))?;
    templates::max_match(&re, &html, major, max_version)
        .ok_or_else(|| format!("{page} 中未匹配到版本"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::net::MockFetcher;
    use std::collections::HashMap;

    #[test]
    fn probe_two_level_frameworks() {
        // KDE frameworks：一级目录 6.29/ → 二级 karchive-6.29.0.tar.xz
        let f = MockFetcher::new(HashMap::new())
            .entry(
                "https://download.kde.org/stable/frameworks/",
                "href=\"6.28/\" href=\"6.29/\" href=\"6.30/\"",
            )
            .entry(
                "https://download.kde.org/stable/frameworks/6.30/",
                "karchive-6.30.0.tar.xz karchive-6.29.0.tar.xz",
            );
        let cfg = SourceConfig {
            tracker_template: "multi-level-html-index".into(),
            levels: vec![
                LevelConfig {
                    url: Some("https://download.kde.org/stable/frameworks/".into()),
                    pattern: Some(r#"href="([0-9][0-9.]*)/""#.into()),
                },
                LevelConfig {
                    url: Some("https://download.kde.org/stable/frameworks/{v1}/".into()),
                    pattern: Some(r"karchive-([0-9][0-9.]*)\.tar\.xz".into()),
                },
            ],
            template: Some(
                "https://download.kde.org/stable/frameworks/{v1}/karchive-{version}.tar.xz".into(),
            ),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "kf-karchive").unwrap();
        assert_eq!(r.version, "6.30.0", "最后一级取目录内最大完整版本");
        assert_eq!(
            r.url, "https://download.kde.org/stable/frameworks/6.30/karchive-6.30.0.tar.xz",
            "template 用 v1 + version"
        );
    }

    #[test]
    fn probe_three_level_qt_style() {
        // Qt 风格三级：qt/ → 6.11/ → 6.11.2/submodules/qtspeech-6.11.2.tar.xz
        let f = MockFetcher::new(HashMap::new())
            .entry(
                "https://download.qt.io/official_releases/qt/",
                "href=\"6.11/\" href=\"6.12/\"",
            )
            .entry(
                "https://download.qt.io/official_releases/qt/6.12/",
                "href=\"6.12.0/\" href=\"6.12.1/\"",
            )
            .entry(
                "https://download.qt.io/official_releases/qt/6.12/6.12.1/submodules/",
                "qtspeech-6.12.1.tar.xz",
            );
        let cfg = SourceConfig {
            tracker_template: "multi-level-html-index".into(),
            levels: vec![
                LevelConfig {
                    url: Some("https://download.qt.io/official_releases/qt/".into()),
                    pattern: Some(r#"href="([0-9.]+)/""#.into()),
                },
                LevelConfig {
                    url: Some("https://download.qt.io/official_releases/qt/{v1}/".into()),
                    pattern: Some(r#"href="([0-9.]+)/""#.into()),
                },
                LevelConfig {
                    url: Some(
                        "https://download.qt.io/official_releases/qt/{v1}/{v2}/submodules/".into(),
                    ),
                    pattern: Some(r"qtspeech-([0-9.]+)\.tar\.xz".into()),
                },
            ],
            template: Some(
                "https://download.qt.io/official_releases/qt/{v1}/{v2}/submodules/qtspeech-{version}.tar.xz"
                    .into(),
            ),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "qt6-speech").unwrap();
        assert_eq!(r.version, "6.12.1");
        assert_eq!(
            r.url,
            "https://download.qt.io/official_releases/qt/6.12/6.12.1/submodules/qtspeech-6.12.1.tar.xz"
        );
    }

    #[test]
    fn probe_respects_max_version_on_dir() {
        let f = MockFetcher::new(HashMap::new())
            .entry("https://example.com/fw/", "href=\"6.28/\" href=\"9.0/\"")
            .entry("https://example.com/fw/6.28/", "karchive-6.28.0.tar.xz");
        let cfg = SourceConfig {
            tracker_template: "multi-level-html-index".into(),
            levels: vec![
                LevelConfig {
                    url: Some("https://example.com/fw/".into()),
                    pattern: Some(r#"href="([0-9][0-9.]*)/""#.into()),
                },
                LevelConfig {
                    url: Some("https://example.com/fw/{v1}/".into()),
                    pattern: Some(r"karchive-([0-9][0-9.]*)\.tar\.xz".into()),
                },
            ],
            max_version: Some("6.28.0".into()),
            template: Some("https://example.com/fw/{v1}/karchive-{version}.tar.xz".into()),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "kf-karchive").unwrap();
        assert_eq!(r.version, "6.28.0", "9.x 目录被 max-version 过滤");
        assert_eq!(r.url, "https://example.com/fw/6.28/karchive-6.28.0.tar.xz");
    }
}
