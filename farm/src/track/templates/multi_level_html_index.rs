//! multi-level-html-index 模板：N 级 HTML 目录探测（KDE frameworks、Qt 等）。
//!
//! 解决单级 html-index 拿不到完整版本的结构：逐级进目录，取到完整版本。
//! KDE frameworks 例：`6.11/` 目录 → 目录内 `ki18n-6.11.0.tar.xz`。
//!
//! **每级显式命名，名字即占位符**（不再有位置隐式的 `{v1}..{vN}`）：
//!
//! ```yaml
//! levels:
//! - name: series                        # {series} 供后续级与 template 引用
//!   url: https://download.kde.org/stable/frameworks/
//!   pattern: href="([0-9][0-9.]*)/"
//! - name: version                       # 保留名：这一级的捕获 = 包版本
//!   url: https://download.kde.org/stable/frameworks/{series}/
//!   pattern: ki18n-([0-9][0-9.]*)\.tar\.xz
//! template: https://download.kde.org/stable/frameworks/{series}/ki18n-{version}.tar.xz
//! ```
//!
//! 规则（探测前全部校验完，不合法直接报错，不猜）：
//! - 每级 `name` 必填、非空、唯一；不得取名 `name`（保留给上游名占位符 `{name}`）；
//! - **必须且只能有一级叫 `version`** —— 它的捕获即包版本（`{version}`，**按名字定，不按位置**）；
//! - 占位符只能引用**前面已解出**的级名（自己或后面的级 → 报错），外加保留的 `{name}`；
//! - 位置隐式的 `{v1}..{vN}` **已废弃** → 报未知占位符（杜绝"靠级序号猜语义"）；
//! - 每级都取**最大**版本（`max_match`，稳定版优先），`max-version` / `major-of` 逐级生效。

use crate::error::FarmError;
use regex::Regex;

use crate::net::Fetcher;
use crate::track::templates;
use crate::track::{need, EntryProbe, SourceConfig};

/// 保留级名：这一级的捕获 = 包版本。
const VERSION_LEVEL: &str = "version";
/// 保留占位符：上游名（`source-name` 或包名）——级名不得占用。
const UPSTREAM_NAME: &str = "name";

/// 逐级探测：第 i 级页面 → 提取该级版本 → 代入后续级 URL → `version` 级 = 包版本。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &SourceConfig,
    major: Option<&str>,
    pkg_name: &str,
) -> Result<EntryProbe, FarmError> {
    if cfg.levels.is_empty() {
        return Err("multi-level-html-index 需 levels 列表（每级 {name, url, pattern}）".into());
    }
    let template = need(&cfg.template, "template")?;
    let names = validate_levels(cfg)?;
    // validate_levels 已保证存在且唯一
    let version_idx = names
        .iter()
        .position(|n| n == VERSION_LEVEL)
        .ok_or("levels 缺 name: version（该级捕获 = 包版本）")?;
    let upstream = cfg.effective_name(pkg_name);
    // 版本筛选约束（exclude / stable-minor / 封顶）逐级生效，与单级模板同一汇点
    let f = templates::version_filter(cfg, major)?;

    let mut vers: Vec<String> = Vec::with_capacity(cfg.levels.len());
    for (i, lvl) in cfg.levels.iter().enumerate() {
        let lvl_url = need(&lvl.url, &format!("levels[{i}].url"))?;
        // 本级只能引用**前面**的级名（+ 保留的 {name}）
        check_placeholders(
            lvl_url,
            &allowed_before(&names, i),
            &format!("levels[{i}].url"),
        )?;
        let page = templates::substitute(lvl_url, &vars(&names[..i], &vers, upstream));
        let html = fetcher.get(&page)?;
        let pat = need(&lvl.pattern, &format!("levels[{i}].pattern"))?;
        let re = Regex::new(pat).map_err(|e| format!("正则无效 {pat}: {e}"))?;
        let v = templates::max_match(&re, &html, &f)
            .ok_or_else(|| format!("{page} 中未匹配到版本（pattern: {pat}）"))?;
        vers.push(v);
    }

    // template 可引用全部级名 + {name}
    check_placeholders(template, &allowed_before(&names, names.len()), "template")?;
    let version = vers[version_idx].clone();
    let url = templates::substitute(template, &vars(&names, &vers, upstream));
    Ok(EntryProbe { version, url })
}

/// 校验每级名字并返回名字列表：必填、非空、唯一、非保留名，且**有且仅有一级**叫 `version`。
fn validate_levels(cfg: &SourceConfig) -> Result<Vec<String>, FarmError> {
    let mut names: Vec<String> = Vec::with_capacity(cfg.levels.len());
    for (i, lvl) in cfg.levels.iter().enumerate() {
        let n = need(&lvl.name, &format!("levels[{i}].name"))?
            .trim()
            .to_string();
        if n.is_empty() {
            return Err(format!("levels[{i}].name 不得为空").into());
        }
        if n == UPSTREAM_NAME {
            return Err(format!(
                "levels[{i}].name 不得取保留名 `{UPSTREAM_NAME}`（上游名占位符 {{{UPSTREAM_NAME}}} 已被占用）"
            )
            .into());
        }
        if names.contains(&n) {
            return Err(format!("levels[{i}].name `{n}` 与前面的级重名").into());
        }
        names.push(n);
    }
    // 上面已保证名字唯一 → `version` 至多出现一次；只需查"缺"。
    if !names.iter().any(|n| n == VERSION_LEVEL) {
        return Err(format!(
            "levels 必须有一级 name: {VERSION_LEVEL}（该级捕获 = 包版本，不按位置）"
        )
        .into());
    }
    Ok(names)
}

/// 第 `i` 级可用的占位符：前面的级名 + 保留的 `{name}`。
fn allowed_before(names: &[String], i: usize) -> Vec<&str> {
    names[..i]
        .iter()
        .map(String::as_str)
        .chain(std::iter::once(UPSTREAM_NAME))
        .collect()
}

/// 建 `(名字, 值)` 列表（末尾附 `{name}` = 上游名），供 `substitute` 替换。
fn vars<'a>(names: &'a [String], vers: &'a [String], upstream: &'a str) -> Vec<(&'a str, &'a str)> {
    names
        .iter()
        .map(String::as_str)
        .zip(vers.iter().map(String::as_str))
        .chain(std::iter::once((UPSTREAM_NAME, upstream)))
        .collect()
}

/// 校验 `s` 里的 `{x}` 占位符都在 `allowed` 里；未知 → 报错（含可用列表）。
/// 这条把"位置隐式 `{v1}`"变成**显式错误**，也让 `levels[i].url` 同样受校验（原先只查最终 URL）。
fn check_placeholders(s: &str, allowed: &[&str], where_: &str) -> Result<(), FarmError> {
    let re = PLACEHOLDER_RE.get_or_init(|| Regex::new(r"\{([A-Za-z0-9_]+)\}").expect("静态正则"));
    let mut bad: Vec<String> = re
        .captures_iter(s)
        .filter_map(|c| c.get(1).map(|m| m.as_str().to_string()))
        .filter(|n| !allowed.contains(&n.as_str()))
        .collect();
    bad.sort();
    bad.dedup();
    if bad.is_empty() {
        return Ok(());
    }
    let bad: Vec<String> = bad.iter().map(|b| format!("{{{b}}}")).collect();
    Err(format!(
        "{where_} 引用了未知占位符 {}（可用: {}；位置隐式的 {{v1}}..{{vN}} 已废弃——每级须用 name 显式命名）",
        bad.join(", "),
        allowed
            .iter()
            .map(|a| format!("{{{a}}}"))
            .collect::<Vec<_>>()
            .join(", ")
    )
    .into())
}

static PLACEHOLDER_RE: std::sync::OnceLock<Regex> = std::sync::OnceLock::new();

#[cfg(test)]
mod tests {
    use super::*;
    use crate::net::MockFetcher;
    use crate::track::LevelConfig;
    use std::collections::HashMap;

    fn lvl(name: &str, url: &str, pattern: &str) -> LevelConfig {
        LevelConfig {
            name: Some(name.into()),
            url: Some(url.into()),
            pattern: Some(pattern.into()),
        }
    }

    /// KDE frameworks 同款两级。
    fn kf_cfg() -> SourceConfig {
        SourceConfig {
            tracker_template: "multi-level-html-index".into(),
            levels: vec![
                lvl(
                    "series",
                    "https://download.kde.org/stable/frameworks/",
                    r#"href="([0-9][0-9.]*)/""#,
                ),
                lvl(
                    "version",
                    "https://download.kde.org/stable/frameworks/{series}/",
                    r"karchive-([0-9][0-9.]*)\.tar\.xz",
                ),
            ],
            template: Some(
                "https://download.kde.org/stable/frameworks/{series}/karchive-{version}.tar.xz"
                    .into(),
            ),
            ..Default::default()
        }
    }

    fn kf_fetcher() -> MockFetcher {
        MockFetcher::new(HashMap::new())
            .entry(
                "https://download.kde.org/stable/frameworks/",
                r#"href="6.28/" href="6.29/" href="6.30/""#,
            )
            .entry(
                "https://download.kde.org/stable/frameworks/6.30/",
                "karchive-6.30.0.tar.xz karchive-6.29.0.tar.xz",
            )
    }

    #[test]
    fn probe_two_level_named() {
        let r = probe(&kf_fetcher(), &kf_cfg(), None, "kf-karchive").unwrap();
        assert_eq!(r.version, "6.30.0", "version 级取目录内最大完整版本");
        assert_eq!(
            r.url, "https://download.kde.org/stable/frameworks/6.30/karchive-6.30.0.tar.xz",
            "template 用 {{series}} + {{version}}"
        );
    }

    #[test]
    fn version_level_is_chosen_by_name_not_position() {
        // 名字决定包版本：把 version 级放**前面**（取目录），series 级在后
        let cfg = SourceConfig {
            tracker_template: "multi-level-html-index".into(),
            levels: vec![
                lvl("version", "https://example.com/d/", r"v([0-9.]+)"),
                lvl(
                    "sub",
                    "https://example.com/d/{version}/",
                    r#"href="([0-9.]+)/""#,
                ),
            ],
            template: Some("https://example.com/d/{version}/{sub}/pkg-{version}.tar.gz".into()),
            ..Default::default()
        };
        let f = MockFetcher::new(HashMap::new())
            .entry("https://example.com/d/", "v2.3")
            .entry("https://example.com/d/2.3/", r#"href="2.3.1/""#);
        let r = probe(&f, &cfg, None, "p").unwrap();
        assert_eq!(r.version, "2.3", "version 级=第一级（名为 version 的那级）");
        assert_eq!(r.url, "https://example.com/d/2.3/2.3.1/pkg-2.3.tar.gz");
    }

    #[test]
    fn probe_three_level_qt_style() {
        let f = MockFetcher::new(HashMap::new())
            .entry(
                "https://download.qt.io/official_releases/qt/",
                r#"href="6.11/" href="6.12/""#,
            )
            .entry(
                "https://download.qt.io/official_releases/qt/6.12/",
                r#"href="6.12.0/" href="6.12.1/""#,
            )
            .entry(
                "https://download.qt.io/official_releases/qt/6.12/6.12.1/submodules/",
                "qtspeech-6.12.1.tar.xz",
            );
        let cfg = SourceConfig {
            tracker_template: "multi-level-html-index".into(),
            levels: vec![
                lvl("series", "https://download.qt.io/official_releases/qt/", r#"href="([0-9.]+)/""#),
                lvl(
                    "patch",
                    "https://download.qt.io/official_releases/qt/{series}/",
                    r#"href="([0-9.]+)/""#,
                ),
                lvl(
                    "version",
                    "https://download.qt.io/official_releases/qt/{series}/{patch}/submodules/",
                    r"qtspeech-([0-9.]+)\.tar\.xz",
                ),
            ],
            template: Some(
                "https://download.qt.io/official_releases/qt/{series}/{patch}/submodules/qtspeech-{version}.tar.xz"
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
            .entry("https://example.com/fw/", r#"href="6.28/" href="9.0/""#)
            .entry("https://example.com/fw/6.28/", "karchive-6.28.0.tar.xz");
        let cfg = SourceConfig {
            levels: vec![
                lvl(
                    "series",
                    "https://example.com/fw/",
                    r#"href="([0-9][0-9.]*)/""#,
                ),
                lvl(
                    "version",
                    "https://example.com/fw/{series}/",
                    r"karchive-([0-9][0-9.]*)\.tar\.xz",
                ),
            ],
            max_version: Some("6.28.0".into()),
            template: Some("https://example.com/fw/{series}/karchive-{version}.tar.xz".into()),
            ..kf_cfg()
        };
        let r = probe(&f, &cfg, None, "kf-karchive").unwrap();
        assert_eq!(r.version, "6.28.0", "9.x 目录被 max-version 过滤");
        assert_eq!(r.url, "https://example.com/fw/6.28/karchive-6.28.0.tar.xz");
    }

    // ── 校验：全部在探测前报错 ──────────────────────────────

    #[test]
    fn rejects_missing_name_duplicate_name_and_reserved_name() {
        let f = kf_fetcher();
        // 缺 name
        let mut cfg = kf_cfg();
        cfg.levels[0].name = None;
        let e = probe(&f, &cfg, None, "p").unwrap_err().to_string();
        assert!(e.contains("levels[0].name"), "{e}");

        // 重名
        let mut cfg = kf_cfg();
        cfg.levels[1].name = Some("series".into());
        let e = probe(&f, &cfg, None, "p").unwrap_err().to_string();
        assert!(e.contains("重名"), "{e}");

        // 保留名 name
        let mut cfg = kf_cfg();
        cfg.levels[0].name = Some("name".into());
        let e = probe(&f, &cfg, None, "p").unwrap_err().to_string();
        assert!(e.contains("保留名"), "{e}");
    }

    #[test]
    fn requires_exactly_one_version_level() {
        let f = kf_fetcher();
        // 一级都不叫 version → 取不到包版本
        let mut cfg = kf_cfg();
        cfg.levels[1].name = Some("ver".into());
        cfg.template = Some("https://x/{series}/karchive-{ver}.tar.xz".into());
        let e = probe(&f, &cfg, None, "p").unwrap_err().to_string();
        assert!(e.contains("version"), "{e}");

        // 两级都叫 version → 由**唯一性**检查拦下（"重名"，不是单独一条"只能有一级"规则）
        let mut cfg = kf_cfg();
        cfg.levels[0].name = Some("version".into());
        let e = probe(&f, &cfg, None, "p").unwrap_err().to_string();
        assert!(e.contains("重名"), "重复的 version 级应报重名: {e}");
    }

    #[test]
    fn rejects_positional_v1_and_forward_reference() {
        let f = kf_fetcher();
        // {v1} 已废弃 → 明确报未知占位符（不再"靠级序号猜"）
        let mut cfg = kf_cfg();
        cfg.template = Some("https://x/{v1}/karchive-{version}.tar.xz".into());
        let e = probe(&f, &cfg, None, "p").unwrap_err().to_string();
        assert!(e.contains("{v1}") && e.contains("已废弃"), "{e}");

        // 前向引用（第 1 级引用第 2 级的名字）→ 报错
        let mut cfg = kf_cfg();
        cfg.levels[0].url = Some("https://download.kde.org/stable/frameworks/{version}/".into());
        let e = probe(&f, &cfg, None, "p").unwrap_err().to_string();
        assert!(
            e.contains("levels[0].url") && e.contains("{version}"),
            "{e}"
        );
    }
}
