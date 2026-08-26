//! pypi 模板：PyPI JSON API 探测最新稳定版 + sdist 源 URL。
//!
//! yaml 里 `tracker-template: pypi` + `project: <PyPI 项目名>`（可与 pkg-name 不同，
//! 如 `pkg-name: python-setuptools` / `project: setuptools`）。
//! probe 抓 `https://pypi.org/pypi/{project}/json`：
//! - 默认取 `info.version`（PyPI 最新版）与最新版 `urls` 里的 sdist URL；
//! - `major-version-lock` 时在 `releases` 里按主版本过滤取最大稳定版（多数 python 包用不到）。

use crate::net::Fetcher;
use crate::track::templates;
use crate::track::{need, ProbeResult, TrackerConfig};

pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &TrackerConfig,
    major: Option<&str>,
) -> Result<ProbeResult, String> {
    let project = need(&cfg.project, "project")?;
    let url = format!("https://pypi.org/pypi/{project}/json");
    let body = fetcher.get(&url)?;
    let json: serde_json::Value = serde_json::from_str(&body)
        .map_err(|e| format!("PyPI JSON 解析失败（{url}）: {e}"))?;

    let (version, sdist_url) = match major {
        Some(m) => {
            // 按主版本过滤 releases，取最大稳定版，再找对应 sdist
            let releases = json["releases"]
                .as_object()
                .ok_or_else(|| format!("PyPI {url} 无 releases"))?;
            let versions: Vec<String> = releases.keys().cloned().collect();
            let v = templates::max_version_stable_first(versions, Some(m), None)
                .ok_or_else(|| format!("PyPI {project} 主版本 {m} 无稳定版本"))?;
            let sdist = json["releases"][&v]
                .as_array()
                .and_then(|arr| {
                    arr.iter()
                        .find(|f| f["packagetype"].as_str() == Some("sdist"))
                })
                .and_then(|f| f["url"].as_str())
                .ok_or_else(|| format!("PyPI {project} {v} 无 sdist URL"))?
                .to_string();
            (v, sdist)
        }
        None => {
            let v = json["info"]["version"]
                .as_str()
                .ok_or_else(|| format!("PyPI {url} 无 info.version"))?
                .to_string();
            let sdist = json["urls"]
                .as_array()
                .and_then(|arr| {
                    arr.iter()
                        .find(|f| f["packagetype"].as_str() == Some("sdist"))
                })
                .and_then(|f| f["url"].as_str())
                .ok_or_else(|| format!("PyPI {project} 最新版无 sdist URL"))?
                .to_string();
            (v, sdist)
        }
    };
    Ok(ProbeResult {
        version,
        sources: vec![sdist_url],
        work_sources: vec![],
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::net::MockFetcher;
    use std::collections::HashMap;

    #[test]
    fn pypi_latest_version_and_sdist() {
        let json = r#"{
            "info": {"version": "83.0.0"},
            "urls": [
                {"packagetype": "bdist_wheel", "url": "https://files/setuptools-83.0.0-py3-none-any.whl"},
                {"packagetype": "sdist", "url": "https://files/setuptools-83.0.0.tar.gz"}
            ],
            "releases": {}
        }"#;
        let f = MockFetcher::new(HashMap::new())
            .entry("https://pypi.org/pypi/setuptools/json", json);
        let cfg = TrackerConfig {
            pkg_name: "python-setuptools".into(),
            tracker_template: "pypi".into(),
            project: Some("setuptools".into()),
            ..Default::default()
        };
        let r = cfg.probe(&f).unwrap();
        assert_eq!(r.version, "83.0.0");
        assert_eq!(r.sources, vec!["https://files/setuptools-83.0.0.tar.gz"]);
    }
}
