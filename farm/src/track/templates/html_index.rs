//! html-index 模板：通用 HTML 目录列表探测（ftpmirror.gnu.org、xorg、kernel 等）。
//!
//! **被动触发**：yaml 里 `tracker-template: html-index` + `url`（目录）+ `pattern`（版本正则）+ `template`。
//! probe 抓目录列表，正则取最大稳定版本。

use regex::Regex;

use crate::net::Fetcher;
use crate::track::templates;
use crate::track::{need, ProbeResult, TrackerConfig};

/// 探测最新稳定版本：抓目录列表，正则提取版本取最大。`major` 非空时只匹配该主版本。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &TrackerConfig,
    major: Option<&str>,
) -> Result<ProbeResult, String> {
    let url = need(&cfg.url, "url")?;
    let pattern = need(&cfg.pattern, "pattern")?;
    let template = need(&cfg.template, "template")?;
    let html = fetcher.get(url)?;
    let re = Regex::new(pattern).map_err(|e| format!("正则无效 {}: {e}", pattern))?;
    let version =
        templates::max_match(&re, &html, major).ok_or_else(|| format!("{url} 中未匹配到版本"))?;
    let name = cfg.source_name().to_string();
    let src = templates::substitute(template, &[("name", &name), ("version", &version)]);
    Ok(ProbeResult {
        version,
        sources: vec![src],
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::net::MockFetcher;
    use std::collections::HashMap;

    #[test]
    fn probe_max_version() {
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://ftp.gnu.org/gnu/autoconf/",
            "autoconf-2.71.tar.xz\nautoconf-2.72.tar.xz\n",
        );
        let cfg = TrackerConfig {
            pkg_name: "autoconf".into(),
            tracker_template: "html-index".into(),
            url: Some("https://ftp.gnu.org/gnu/autoconf/".into()),
            pattern: Some(r"autoconf-(\d[\d.]*)\.tar\.(?:xz|gz|bz2)".into()),
            template: Some("https://ftp.gnu.org/gnu/autoconf/{name}-{version}.tar.gz".into()),
            ..Default::default()
        };
        let r = cfg.probe(&f).unwrap();
        assert_eq!(r.version, "2.72");
        assert_eq!(
            r.sources,
            vec!["https://ftp.gnu.org/gnu/autoconf/autoconf-2.72.tar.gz"]
        );
    }
}
