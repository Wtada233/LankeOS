//! github 模板：releases / tags 探测（一个模板一个文件）。
//!
//! **被动触发**：source 条目里 `tracker-template: github` + `repo` + `mode`(tags|releases) + `tag-prefix` + `template`。
//! 探测用 GitHub API（tags 列表或 releases/latest），稳定版优先。

use crate::net::Fetcher;
use crate::track::templates::{self, matches_major, max_tag_version, strip_version};
use crate::track::{need, EntryProbe, SourceConfig};

/// 探测最新稳定版本（GitHub API），返回该槽位版本 + URL。`major` 非空时只匹配该主版本的 tag。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &SourceConfig,
    major: Option<&str>,
    _pkg_name: &str,
) -> Result<EntryProbe, String> {
    let repo = need(&cfg.repo, "repo")?;
    let tag_prefix = cfg.tag_prefix.as_deref().unwrap_or("");
    let template = need(&cfg.template, "template")?;
    let mode = cfg.mode.as_deref().unwrap_or("tags");
    let cap = cfg.max_version.as_deref();

    let version = match mode {
        "releases" => {
            if let Some(cap) = cap {
                // max-version 需在版本列表上过滤（单条 /latest 无法封顶）→ 拉全部 releases，
                // 稳定优先取不超过封顶的最大版。无 max-version 时保持 /releases/latest 语义。
                let url = format!("https://api.github.com/repos/{repo}/releases?per_page=100");
                let body = fetcher.get(&url)?;
                let names = templates::extract_release_tag_names(&body)?;
                max_tag_version(&names, tag_prefix, major, Some(cap))
                    .ok_or("releases 中无匹配版本/主版本")?
            } else {
                let url = format!("https://api.github.com/repos/{repo}/releases/latest");
                let body = fetcher.get(&url)?;
                let tag = templates::extract_latest_release_tag(&body)?;
                strip_version(&tag, tag_prefix)
                    .filter(|v| matches_major(v, major))
                    .ok_or("release tag 无匹配版本/主版本")?
            }
        }
        _ => {
            let url = format!("https://api.github.com/repos/{repo}/tags");
            let body = fetcher.get(&url)?;
            let names = templates::extract_tag_names(&body)?;
            max_tag_version(&names, tag_prefix, major, cap).ok_or("tags 中无匹配版本/主版本")?
        }
    };
    let tag = format!("{tag_prefix}{version}");
    let name = repo.split('/').next_back().unwrap_or(repo);
    let url = templates::substitute(
        template,
        &[
            ("repo", repo),
            ("tag", &tag),
            ("name", name),
            ("version", &version),
        ],
    );
    Ok(EntryProbe { version, url })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::net::MockFetcher;

    #[test]
    fn probe_tags_max_version() {
        let f = MockFetcher::new(std::collections::HashMap::new()).entry(
            "https://api.github.com/repos/systemd/systemd/tags",
            r#"[{"name":"v254"},{"name":"v256"},{"name":"v255"},{"name":"v261"}]"#,
        );
        let cfg = SourceConfig {
            tracker_template: "github".into(),
            repo: Some("systemd/systemd".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            template: Some("https://github.com/{repo}/archive/refs/tags/{tag}.tar.gz".into()),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "systemd").unwrap();
        assert_eq!(r.version, "261");
        assert_eq!(
            r.url,
            "https://github.com/systemd/systemd/archive/refs/tags/v261.tar.gz"
        );
    }

    #[test]
    fn tags_respects_max_version_cap() {
        // max-version 封顶（tags 模式）：超过封顶的 v261 被过滤，取 v256
        let f = MockFetcher::new(std::collections::HashMap::new()).entry(
            "https://api.github.com/repos/systemd/systemd/tags",
            r#"[{"name":"v254"},{"name":"v256"},{"name":"v255"},{"name":"v261"}]"#,
        );
        let cfg = SourceConfig {
            tracker_template: "github".into(),
            repo: Some("systemd/systemd".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            max_version: Some("256".into()),
            template: Some("https://github.com/{repo}/archive/refs/tags/{tag}.tar.gz".into()),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "systemd").unwrap();
        assert_eq!(r.version, "256");
        assert_eq!(
            r.url,
            "https://github.com/systemd/systemd/archive/refs/tags/v256.tar.gz"
        );
    }

    #[test]
    fn releases_cap_fetches_list_and_filters() {
        // max-version（releases 模式）：单条 /releases/latest 无法封顶 → 改拉 releases 列表
        // （?per_page=100），稳定优先取不超过封顶的最大版（v2.0.0 被过滤取 v1.9.0）
        let list_url = "https://api.github.com/repos/a/b/releases?per_page=100";
        let f = MockFetcher::new(std::collections::HashMap::new()).entry(
            list_url,
            r#"[{"tag_name":"v2.0.0"},{"tag_name":"v1.9.0"},{"tag_name":"v1.8.0"}]"#,
        );
        let cfg = SourceConfig {
            tracker_template: "github".into(),
            repo: Some("a/b".into()),
            mode: Some("releases".into()),
            tag_prefix: Some("v".into()),
            max_version: Some("1.9.0".into()),
            template: Some("https://github.com/{repo}/archive/refs/tags/{tag}.tar.gz".into()),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "b").unwrap();
        assert_eq!(r.version, "1.9.0");
        assert_eq!(
            r.url,
            "https://github.com/a/b/archive/refs/tags/v1.9.0.tar.gz"
        );
    }

    #[test]
    fn releases_without_cap_keeps_latest_endpoint() {
        // 无 max-version 时保持原语义：走 /releases/latest 单条
        let f = MockFetcher::new(std::collections::HashMap::new()).entry(
            "https://api.github.com/repos/a/b/releases/latest",
            r#"{"tag_name":"v2.0.0"}"#,
        );
        let cfg = SourceConfig {
            tracker_template: "github".into(),
            repo: Some("a/b".into()),
            mode: Some("releases".into()),
            tag_prefix: Some("v".into()),
            template: Some("https://github.com/{repo}/archive/refs/tags/{tag}.tar.gz".into()),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "b").unwrap();
        assert_eq!(r.version, "2.0.0");
        assert_eq!(
            r.url,
            "https://github.com/a/b/archive/refs/tags/v2.0.0.tar.gz"
        );
    }
}
