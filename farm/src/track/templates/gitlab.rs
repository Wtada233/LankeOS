//! gitlab 模板：releases / tags 探测（GitLab API v4）。
//!
//! **被动触发**：source 条目里 `tracker-template: gitlab` + `host` + `project` + `mode`(tags|releases) + `tag-prefix` + `template`。

use crate::net::Fetcher;
use crate::track::templates::{self, max_tag_version, urlencode};
use crate::track::{need, EntryProbe, SourceConfig};

/// 探测最新稳定版本（GitLab API v4），返回该槽位版本 + URL。`major` 非空时只匹配该主版本的 tag。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &SourceConfig,
    major: Option<&str>,
    _pkg_name: &str,
) -> Result<EntryProbe, String> {
    let host = need(&cfg.host, "host")?;
    let project = need(&cfg.project, "project")?;
    let tag_prefix = cfg.tag_prefix.as_deref().unwrap_or("");
    let template = need(&cfg.template, "template")?;
    let mode = cfg.mode.as_deref().unwrap_or("tags");
    let enc = urlencode(project);

    let version = match mode {
        // releases 列表可能含 dev/预发布 tag（如 1.59.1-dev），用 max_tag_version 稳定优先过滤
        "releases" => {
            let url = format!("https://{host}/api/v4/projects/{enc}/releases?per_page=50");
            let body = fetcher.get(&url)?;
            let names = templates::extract_release_tag_names(&body)?;
            max_tag_version(&names, tag_prefix, major).ok_or("releases 中无匹配版本/主版本")?
        }
        _ => {
            let url = format!("https://{host}/api/v4/projects/{enc}/repository/tags?per_page=50");
            let body = fetcher.get(&url)?;
            let names = templates::extract_tag_names(&body)?;
            max_tag_version(&names, tag_prefix, major).ok_or("tags 中无匹配版本/主版本")?
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
    fn releases_filters_dev_and_picks_max_stable() {
        // GitLab releases 列表含 dev tag（如 1.59.1-dev），稳定优先应跳过它
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://gitlab.com/api/v4/projects/NetworkManager%2FNetworkManager/releases?per_page=50",
            r#"[{"tag_name":"1.59.1-dev"},{"tag_name":"1.54.0"},{"tag_name":"1.52.0"}]"#,
        );
        let cfg = SourceConfig {
            tracker_template: "gitlab".into(),
            host: Some("gitlab.com".into()),
            project: Some("NetworkManager/NetworkManager".into()),
            mode: Some("releases".into()),
            tag_prefix: Some(String::new()),
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
}
