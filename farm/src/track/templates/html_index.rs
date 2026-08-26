//! html-index 模板：通用 HTML 目录列表探测（ftpmirror.gnu.org、xorg、kernel 等）。
//!
//! **被动触发**：source 条目里 `tracker-template: html-index` + `url`（目录）+ `pattern`（版本正则）+ `template`。
//! probe 抓目录列表，正则取最大稳定版本。

use regex::Regex;

use crate::net::Fetcher;
use crate::track::templates;
use crate::track::{need, EntryProbe, SourceConfig};

/// 探测最新稳定版本：抓目录列表，正则提取版本取最大。`major` 非空时只匹配该主版本。
/// `max-version` 生效（超过封顶的版本被过滤，如 tcl 锁 8.6.x）。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &SourceConfig,
    major: Option<&str>,
    pkg_name: &str,
) -> Result<EntryProbe, String> {
    let url = need(&cfg.url, "url")?;
    let pattern = need(&cfg.pattern, "pattern")?;
    let template = need(&cfg.template, "template")?;
    let html = fetcher.get(url)?;
    let re = Regex::new(pattern).map_err(|e| format!("正则无效 {}: {e}", pattern))?;
    let version = templates::max_match(&re, &html, major, cfg.max_version.as_deref())
        .ok_or_else(|| format!("{url} 中未匹配到版本"))?;
    let name = cfg.effective_name(pkg_name);
    let url = templates::substitute(template, &[("name", name), ("version", &version)]);
    Ok(EntryProbe { version, url })
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
        let cfg = SourceConfig {
            tracker_template: "html-index".into(),
            url: Some("https://ftp.gnu.org/gnu/autoconf/".into()),
            pattern: Some(r"autoconf-(\d[\d.]*)\.tar\.(?:xz|gz|bz2)".into()),
            template: Some("https://ftp.gnu.org/gnu/autoconf/{name}-{version}.tar.gz".into()),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "autoconf").unwrap();
        assert_eq!(r.version, "2.72");
        assert_eq!(
            r.url,
            "https://ftp.gnu.org/gnu/autoconf/autoconf-2.72.tar.gz"
        );
    }

    #[test]
    fn probe_respects_max_version_cap() {
        // max-version 封顶：9.x 被过滤，取 8.6.16
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://ftp.gnu.org/gnu/tcl/",
            "tcl8.6.14-src.tar.gz tcl8.6.16-src.tar.gz tcl9.0.4-src.tar.gz",
        );
        let cfg = SourceConfig {
            tracker_template: "html-index".into(),
            url: Some("https://ftp.gnu.org/gnu/tcl/".into()),
            pattern: Some(r"tcl([\d.]+)-src\.tar\.gz".into()),
            max_version: Some("8.6.16".into()),
            template: Some("https://ftp.gnu.org/gnu/tcl/tcl{version}-src.tar.gz".into()),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "tcl").unwrap();
        assert_eq!(r.version, "8.6.16");
    }
}
