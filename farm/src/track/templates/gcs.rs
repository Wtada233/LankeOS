//! gcs 模板：GCS/S3 存储桶探测（chromium 等）。
//!
//! **被动触发**：yaml 里 `tracker-template: gcs` + `url`（桶目录）+ `pattern`（文件名版本正则）+ `template`。
//! probe 用 GCS **XML listing API**（`{url}?delimiter=/`，分页遍历）拉文件名列表，正则取最大稳定版本。

use regex::Regex;

use crate::net::Fetcher;
use crate::track::templates;
use crate::track::{need, ProbeResult, TrackerConfig};

/// 探测最新稳定版本：GCS XML listing（`?delimiter=/`，**分页遍历**）→ 文件名 → 正则取最大版本。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &TrackerConfig,
    major: Option<&str>,
) -> Result<ProbeResult, String> {
    let url = need(&cfg.url, "url")?;
    let pattern = need(&cfg.pattern, "pattern")?;
    let template = need(&cfg.template, "template")?;

    // GCS XML listing 每页最多 1000 个 key，用 <NextMarker> 分页续取
    let mut all_xml = String::new();
    let mut marker = String::new();
    loop {
        let listing_url = if marker.is_empty() {
            format!("{url}?delimiter=/")
        } else {
            format!("{url}?delimiter=/&marker={}", templates::urlencode(&marker))
        };
        let page = fetcher.get(&listing_url)?;
        all_xml.push_str(&page);
        match extract_next_marker(&page) {
            Some(nm) if !nm.is_empty() && nm != marker => marker = nm,
            _ => break,
        }
        if all_xml.len() > 100_000_000 {
            return Err("GCS 分页超出安全上限".into());
        }
    }

    let re = Regex::new(pattern).map_err(|e| format!("正则无效 {}: {e}", pattern))?;
    let version = templates::max_match(&re, &all_xml, major)
        .ok_or_else(|| format!("{url} GCS 列表未匹配到版本"))?;
    let name = cfg.source_name().to_string();
    let src = templates::substitute(template, &[("name", &name), ("version", &version)]);
    Ok(ProbeResult {
        version,
        sources: vec![src],
    })
}

/// 提取 `<NextMarker>...</NextMarker>` 分页续传标记。
fn extract_next_marker(xml: &str) -> Option<String> {
    let start = xml.find("<NextMarker>")? + "<NextMarker>".len();
    let rest = &xml[start..];
    let end = rest.find("</NextMarker>")?;
    Some(rest[..end].to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::net::MockFetcher;
    use std::collections::HashMap;

    #[test]
    fn gcs_xml_listing_probe() {
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://commondatastorage.googleapis.com/chromium-browser-official/?delimiter=/",
            r#"<ListBucketResult><Contents><Key>chromium-149.0.0.0-lite.tar.xz</Key></Contents><Contents><Key>chromium-150.0.7871.186-lite.tar.xz</Key></Contents></ListBucketResult>"#,
        );
        let cfg = TrackerConfig {
            pkg_name: "chromium".into(),
            tracker_template: "gcs".into(),
            url: Some("https://commondatastorage.googleapis.com/chromium-browser-official/".into()),
            pattern: Some(r"chromium[-_]?(\d[\d.]*)[a-z0-9-]*\.tar\.(?:xz|gz|bz2)".into()),
            template: Some(
                "https://commondatastorage.googleapis.com/chromium-browser-official/chromium-{version}-lite.tar.xz"
                    .into(),
            ),
            ..Default::default()
        };
        let r = cfg.probe(&f).unwrap();
        assert_eq!(r.version, "150.0.7871.186");
        assert_eq!(
            r.sources,
            vec!["https://commondatastorage.googleapis.com/chromium-browser-official/chromium-150.0.7871.186-lite.tar.xz"]
        );
    }
}
