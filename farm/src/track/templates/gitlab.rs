//! gitlab 模板：releases / tags 探测（GitLab API v4）。
//!
//! **被动触发**：source 条目里 `tracker-template: gitlab` + `host` + `project` + `mode`(tags|releases) + `tag-prefix` + `template`。

use crate::error::FarmError;
use crate::net::Fetcher;
use crate::track::templates::{self, max_tag_version, urlencode};
use crate::track::{need, EntryProbe, SourceConfig};

/// 探测最新稳定版本（GitLab API v4），返回该槽位版本 + URL。`major` 非空时只匹配该主版本的 tag。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &SourceConfig,
    major: Option<&str>,
    _pkg_name: &str,
) -> Result<EntryProbe, FarmError> {
    let host = need(&cfg.host, "host")?;
    let project = need(&cfg.project, "project")?;
    let tag_prefix = cfg.tag_prefix.as_deref().unwrap_or("");
    let template = need(&cfg.template, "template")?;
    let mode = cfg.mode.as_deref().unwrap_or("tags");
    let enc = urlencode(project);

    let f = templates::version_filter(cfg, major)?;
    let version = match mode {
        // releases：**先打 latest 快捷方式**（`/releases/permalink/latest`，单条响应、无分页窗口）；
        // 它不满足 major/max-version 约束（或端点不可用）时，回退到 releases 列表——每页 100 条
        // （GitLab 的 per_page 上限）逐页单独请求，直到末页，避免"只看第一页"漏掉目标版本。
        "releases" => {
            let latest_url =
                format!("https://{host}/api/v4/projects/{enc}/releases/permalink/latest");
            let from_latest = fetcher
                .get(&latest_url)
                .ok()
                .and_then(|b| templates::extract_latest_release_tag(&b).ok())
                .and_then(|tag| max_tag_version(&[tag], tag_prefix, &f));
            match from_latest {
                Some(v) => v,
                None => {
                    let base =
                        format!("https://{host}/api/v4/projects/{enc}/releases?per_page=100");
                    let items = templates::fetch_json_pages(fetcher, &base, 100, 10)?;
                    let names = templates::release_tag_names(&items);
                    max_tag_version(&names, tag_prefix, &f).ok_or("releases 中无匹配版本/主版本")?
                }
            }
        }
        _ => {
            // tags 模式：同 github——走 **git 协议列全量 tag**（libgit2）；REST tags 端点 per_page
            // 上限 100，靠翻页且顺序与版本无关。
            let names = fetcher.list_tags(&format!("https://{host}/{project}.git"))?;
            max_tag_version(&names, tag_prefix, &f).ok_or("tags 中无匹配版本/主版本")?
        }
    };
    let tag = format!("{tag_prefix}{version}");
    let name = project.split('/').next_back().unwrap_or(project);
    let url = templates::substitute(
        template,
        &[
            ("tag", &tag),
            ("name", name),
            ("version", &version),
            ("project", project), // gitlab 模板常引用 {project}（如 {project}/-/archive/...）
        ],
    );
    Ok(EntryProbe { version, url })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::net::MockFetcher;
    use std::collections::HashMap;

    #[test]
    fn releases_prefers_latest_permalink() {
        // 无 max-version/major 约束 → 优先 **latest 快捷方式**（单条响应，无分页窗口）
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://gitlab.freedesktop.org/api/v4/projects/libdecor%2Flibdecor/releases/permalink/latest",
            r#"{"tag_name":"0.2.5","name":"libdecor 0.2.5"}"#,
        );
        let cfg = SourceConfig {
            tracker_template: "gitlab".into(),
            host: Some("gitlab.freedesktop.org".into()),
            project: Some("libdecor/libdecor".into()),
            mode: Some("releases".into()),
            tag_prefix: Some(String::new()),
            template: Some(
                "https://gitlab.freedesktop.org/{project}/-/releases/{tag}/downloads/libdecor-{tag}.tar.xz"
                    .into(),
            ),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "libdecor").unwrap();
        assert_eq!(r.version, "0.2.5");
        assert_eq!(
            r.url,
            "https://gitlab.freedesktop.org/libdecor/libdecor/-/releases/0.2.5/downloads/libdecor-0.2.5.tar.xz"
        );
    }

    #[test]
    fn releases_paginates_list_when_latest_is_over_cap() {
        // 带 max-version 封顶：latest（2.0.0）超封顶 → 回退列表；第 1 页 100 条（= per_page）
        // 触发第 2 页，目标 1.9.0 在第 2 页 → 必须翻页才找得到（旧实现只看第一页会报"无匹配版本"）
        let page1 = format!(
            "[{}]",
            (0..100)
                .map(|i| format!(r#"{{"tag_name":"2.{i}.0"}}"#))
                .collect::<Vec<_>>()
                .join(",")
        );
        let f = MockFetcher::new(HashMap::new())
            .entry(
                "https://gitlab.com/api/v4/projects/foo%2Fbar/releases/permalink/latest",
                r#"{"tag_name":"2.0.0"}"#,
            )
            .entry(
                "https://gitlab.com/api/v4/projects/foo%2Fbar/releases?per_page=100&page=1",
                &page1,
            )
            .entry(
                "https://gitlab.com/api/v4/projects/foo%2Fbar/releases?per_page=100&page=2",
                r#"[{"tag_name":"1.9.0"}]"#,
            );
        let cfg = SourceConfig {
            tracker_template: "gitlab".into(),
            host: Some("gitlab.com".into()),
            project: Some("foo/bar".into()),
            mode: Some("releases".into()),
            tag_prefix: Some(String::new()),
            max_version: Some("1.9.0".into()),
            template: Some("https://gitlab.com/{name}/-/archive/{tag}/x.tar.gz".into()),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "bar").unwrap();
        assert_eq!(r.version, "1.9.0", "第 2 页的 1.9.0 必须被翻页找到");
    }

    #[test]
    fn releases_list_stable_preferred_under_cap() {
        // 列表路径（带封顶）：dev tag（1.59.1-dev）被稳定优先过滤，取 1.54.0
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://gitlab.com/api/v4/projects/NetworkManager%2FNetworkManager/releases?per_page=100&page=1",
            r#"[{"tag_name":"1.59.1-dev"},{"tag_name":"1.58.0"},{"tag_name":"1.54.0"}]"#,
        );
        let cfg = SourceConfig {
            tracker_template: "gitlab".into(),
            host: Some("gitlab.com".into()),
            project: Some("NetworkManager/NetworkManager".into()),
            mode: Some("releases".into()),
            tag_prefix: Some(String::new()),
            max_version: Some("1.54.0".into()),
            template: Some("https://gitlab.com/{name}/-/archive/{tag}/x.tar.gz".into()),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "NetworkManager").unwrap();
        assert_eq!(r.version, "1.54.0");
        assert_eq!(
            r.url,
            "https://gitlab.com/NetworkManager/-/archive/1.54.0/x.tar.gz"
        );
    }

    #[test]
    fn tags_respects_max_version_cap() {
        // max-version 封顶（tags 模式）：超过封顶的 v1.8.0 被过滤，取封顶内最大稳定 v1.6.0
        let f = MockFetcher::new(HashMap::new()).tags(
            "https://gitlab.com/foo/bar.git",
            &["v1.8.0", "v1.6.0", "v1.4.0"],
        );
        let cfg = SourceConfig {
            tracker_template: "gitlab".into(),
            host: Some("gitlab.com".into()),
            project: Some("foo/bar".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            max_version: Some("1.6.0".into()),
            template: Some("https://gitlab.com/{name}/-/archive/{tag}/x.tar.gz".into()),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "bar").unwrap();
        assert_eq!(r.version, "1.6.0");
        assert_eq!(r.url, "https://gitlab.com/bar/-/archive/v1.6.0/x.tar.gz");
    }

    #[test]
    fn releases_respects_max_version_cap() {
        // max-version 封顶（列表路径）：更高 release（含 dev）被过滤，取 1.54.0
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://gitlab.com/api/v4/projects/foo%2Fbar/releases?per_page=100&page=1",
            r#"[{"tag_name":"1.59.1-dev"},{"tag_name":"1.58.0"},{"tag_name":"1.54.0"}]"#,
        );
        let cfg = SourceConfig {
            tracker_template: "gitlab".into(),
            host: Some("gitlab.com".into()),
            project: Some("foo/bar".into()),
            mode: Some("releases".into()),
            tag_prefix: Some(String::new()),
            max_version: Some("1.54.0".into()),
            template: Some("https://gitlab.com/{name}/-/archive/{tag}/x.tar.gz".into()),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "bar").unwrap();
        assert_eq!(r.version, "1.54.0");
        assert_eq!(r.url, "https://gitlab.com/bar/-/archive/1.54.0/x.tar.gz");
    }
}
