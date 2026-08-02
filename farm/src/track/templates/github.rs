//! github 模板：releases / tags 探测（一个模板一个文件）。
//!
//! **被动触发**：yaml 里 `tracker-template: github` + `repo` + `mode`(tags|releases) + `tag-prefix` + `template`。
//! 探测用 GitHub API（tags 列表或 releases/latest），稳定版优先。

use crate::net::Fetcher;
use crate::track::templates::{self, matches_major, max_tag_version, strip_version};
use crate::track::{need, ProbeResult, TrackerConfig};

/// 探测最新稳定版本（GitHub API）。`major` 非空时只匹配该主版本的 tag。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &TrackerConfig,
    major: Option<&str>,
) -> Result<ProbeResult, String> {
    let repo = need(&cfg.repo, "repo")?;
    let tag_prefix = cfg.tag_prefix.as_deref().unwrap_or("");
    let template = need(&cfg.template, "template")?;
    let mode = cfg.mode.as_deref().unwrap_or("tags");

    let version = match mode {
        "releases" => {
            let url = format!("https://api.github.com/repos/{repo}/releases/latest");
            let body = fetcher.get(&url)?;
            let tag = templates::extract_latest_release_tag(&body)?;
            strip_version(&tag, tag_prefix)
                .filter(|v| matches_major(v, major))
                .ok_or("release tag 无匹配版本/主版本")?
        }
        _ => {
            let url = format!("https://api.github.com/repos/{repo}/tags");
            let body = fetcher.get(&url)?;
            let names = templates::extract_tag_names(&body)?;
            max_tag_version(&names, tag_prefix, major).ok_or("tags 中无匹配版本/主版本")?
        }
    };
    let tag = format!("{tag_prefix}{version}");
    let name = repo.split('/').next_back().unwrap_or(repo);
    let src = templates::substitute(
        template,
        &[
            ("repo", repo),
            ("tag", &tag),
            ("name", name),
            ("version", &version),
        ],
    );
    Ok(ProbeResult {
        version,
        sources: vec![src],
    })
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
        let cfg = TrackerConfig {
            pkg_name: "systemd".into(),
            tracker_template: "github".into(),
            repo: Some("systemd/systemd".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            template: Some("https://github.com/{repo}/archive/refs/tags/{tag}.tar.gz".into()),
            ..Default::default()
        };
        let r = cfg.probe(&f).unwrap();
        assert_eq!(r.version, "261");
        assert_eq!(
            r.sources,
            vec!["https://github.com/systemd/systemd/archive/refs/tags/v261.tar.gz"]
        );
    }
}
