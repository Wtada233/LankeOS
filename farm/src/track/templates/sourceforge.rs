//! sourceforge 模板：RSS 探测最新版本。
//!
//! **被动触发**：yaml 里 `tracker-template: sourceforge` + `project` + `path` + `pattern` + `template`。

use regex::Regex;

use crate::net::Fetcher;
use crate::track::templates;
use crate::track::{need, ProbeResult, TrackerConfig};

/// 探测最新稳定版本（SourceForge RSS）。`major` 非空时只匹配该主版本。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &TrackerConfig,
    major: Option<&str>,
) -> Result<ProbeResult, String> {
    let project = need(&cfg.project, "project")?;
    let pattern = need(&cfg.pattern, "pattern")?;
    let template = need(&cfg.template, "template")?;
    let p = cfg
        .path
        .as_deref()
        .map(|p| format!("?path=/{}", p))
        .unwrap_or_default();
    let rss_url = format!("https://sourceforge.net/projects/{project}/rss{p}");
    let rss = fetcher.get(&rss_url)?;
    let re = Regex::new(pattern).map_err(|e| format!("正则无效: {e}"))?;
    let version = templates::max_match(&re, &rss, major).ok_or("RSS 中未匹配到版本")?;
    let name = cfg.source_name().to_string();
    let src = templates::substitute(
        template,
        &[
            ("project", project),
            ("name", &name),
            ("version", &version),
            ("path_version", &version), // SF 目录层版本（如 lame/4.0/），通常等于版本
            ("path", cfg.path.as_deref().unwrap_or("")), // SF 子路径段（如 lame）
        ],
    );
    Ok(ProbeResult {
        version,
        sources: vec![src],
    })
}
