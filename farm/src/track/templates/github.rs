//! github 模板：releases / tags 探测（一个模板一个文件）。
//!
//! **被动触发**：source 条目里 `tracker-template: github` + `repo` + `mode`(tags|releases) + `tag-prefix` + `template`。
//! 探测用 GitHub API（tags 列表或 releases/latest），稳定版优先。

use crate::error::FarmError;
use crate::net::Fetcher;
use crate::track::templates::{self, max_tag_version, strip_version};
use crate::track::{need, EntryProbe, SourceConfig};

/// 探测最新稳定版本（GitHub API），返回该槽位版本 + URL。`major` 非空时只匹配该主版本的 tag。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &SourceConfig,
    major: Option<&str>,
    _pkg_name: &str,
) -> Result<EntryProbe, FarmError> {
    let repo = need(&cfg.repo, "repo")?;
    let tag_prefix = cfg.tag_prefix.as_deref().unwrap_or("");
    let template = need(&cfg.template, "template")?;
    let mode = cfg.mode.as_deref().unwrap_or("tags");
    let f = templates::version_filter(cfg, major)?;

    let version = match mode {
        "releases" => {
            if f.cap.is_some() {
                // max-version 需在版本列表上过滤（单条 /latest 无法封顶）→ 拉 releases 列表，
                // 每页 100 条（GitHub 上限）**逐页单独请求**直到末页，稳定优先取不超过封顶的最大版。
                let base = format!("https://api.github.com/repos/{repo}/releases?per_page=100");
                let items = templates::fetch_json_pages(fetcher, &base, 100, 10)?;
                let names = templates::release_tag_names(&items);
                max_tag_version(&names, tag_prefix, &f).ok_or("releases 中无匹配版本/主版本")?
            } else {
                // 无封顶：单条 `/releases/latest`（无分页窗口）
                let url = format!("https://api.github.com/repos/{repo}/releases/latest");
                let body = fetcher.get(&url)?;
                let tag = templates::extract_latest_release_tag(&body)?;
                // 单条候选也要过约束（否则 /releases/latest 会绕开 max-version/exclude/稳定版过滤）
                strip_version(&tag, tag_prefix)
                    .filter(|v| f.allows(v))
                    .ok_or("release tag 无匹配版本/主版本")?
            }
        }
        _ => {
            // tags 模式：走 **git 协议列全量 tag**（libgit2 ref advertisement），**不是** REST `/tags`
            // —— 后者分页（默认 30 / 最大 100，靠 Link 头翻页）且**顺序与版本无关**，只看首页会取到
            // 老 tag（hdf5 事故：首页 30 个全是老 tag，`max` 够不到 2.2.0）。
            let names = fetcher.list_tags(&format!("https://github.com/{repo}.git"))?;
            max_tag_version(&names, tag_prefix, &f).ok_or("tags 中无匹配版本/主版本")?
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
        let f = MockFetcher::new(std::collections::HashMap::new()).tags(
            "https://github.com/systemd/systemd.git",
            &["v254", "v256", "v255", "v261"],
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
        let f = MockFetcher::new(std::collections::HashMap::new()).tags(
            "https://github.com/systemd/systemd.git",
            &["v254", "v256", "v255", "v261"],
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
        // max-version（releases 模式）：单条 /releases/latest 无法封顶 → 拉 releases 列表
        // （每页 100 条，逐页请求），稳定优先取不超过封顶的最大版（v2.0.0 被过滤取 v1.9.0）
        let list_url = "https://api.github.com/repos/a/b/releases?per_page=100&page=1";
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
    fn releases_cap_paginates_when_target_is_not_on_first_page() {
        // 第 1 页满 100 条（= per_page）→ 必须继续请求第 2 页；目标 v1.9.0 在第 2 页
        let page1 = format!(
            "[{}]",
            (0..100)
                .map(|i| format!(r#"{{"tag_name":"v2.{i}.0"}}"#))
                .collect::<Vec<_>>()
                .join(",")
        );
        let f = MockFetcher::new(std::collections::HashMap::new())
            .entry(
                "https://api.github.com/repos/a/b/releases?per_page=100&page=1",
                &page1,
            )
            .entry(
                "https://api.github.com/repos/a/b/releases?per_page=100&page=2",
                r#"[{"tag_name":"v1.9.0"},{"tag_name":"v1.8.0"}]"#,
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
        assert_eq!(r.version, "1.9.0", "第 2 页的目标版本必须被翻页找到");
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
