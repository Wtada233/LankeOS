//! track 内置模板：**一个模板一个文件一个探测后端**（§9）。
//!
//! 每个模板文件只含 `probe(fetcher, cfg, major, pkg_name) -> Result<EntryProbe>`：
//! 联网抓最新版本，返回该 source 槽位的版本 + URL。模板**被动触发**——由 source 条目的
//! `tracker_template` 字段指定，模板不主动从 URI 猜格式（yaml 由人工/AI 编写，模板只是探测执行器）。
//!
//! `script` 是**包级类型**（不是模板）：`type: script` 时整包走脚本，返回完整清单
//! （`ProbeResult`，stdout 第一行=版本，后续行=URL，`# work_sources` 后归 work_sources）。

pub mod gcs;
pub mod github;
pub mod gitlab;
pub mod gnome;
pub mod html_index;
pub mod multi_level_html_index;
pub mod pypi;
pub mod same_version;
pub mod script;
pub mod sourceforge;

use regex::Regex;

use crate::track::vercmp;

// ───────────────────────────────────────────────────────────────────────────
// 共享辅助（模板文件内部使用）
// ───────────────────────────────────────────────────────────────────────────

/// 提取 URL/HTML/XML 中符合正则（含一个捕获组）的最大版本。
/// `major` 非空时只匹配该主版本号的 tag（约束 major-of，§9）。
/// 默认稳定版优先，无稳定版才回落（§9：track 追上游最新**稳定**版）。
pub(crate) fn max_match(
    re: &Regex,
    text: &str,
    major: Option<&str>,
    cap: Option<&str>,
) -> Option<String> {
    let versions: Vec<String> = re
        .captures_iter(text)
        .filter_map(|c| c.get(1).map(|m| m.as_str().to_string()))
        .filter(|v| v.starts_with(|ch: char| ch.is_ascii_digit()))
        .collect();
    max_version_stable_first(versions, major, cap)
}

/// 稳定版优先取最大版本；`major` 非空时先按主版本过滤，`cap` 封顶（超过则排除，
/// 如 tcl 锁 8.6.x：max-version 8.6.16 时 9.0.1 被过滤掉）。
fn max_version_stable_first(
    versions: Vec<String>,
    major: Option<&str>,
    cap: Option<&str>,
) -> Option<String> {
    let filtered: Vec<String> = versions
        .into_iter()
        .filter(|v| matches_major(v, major))
        .filter(|v| cap.is_none_or(|c| vercmp::cmp_version(v, c) != std::cmp::Ordering::Greater))
        .collect();
    let stable: Vec<&String> = filtered.iter().filter(|v| is_stable(v)).collect();
    let pool: Vec<&String> = if stable.is_empty() {
        filtered.iter().collect()
    } else {
        stable
    };
    pool.into_iter()
        .max_by(|a, b| vercmp::cmp_version(a, b))
        .cloned()
}

/// 版本主段是否等于 `major`（`22.1.2` + `22` → true；`220.1` + `22` → false）。
pub(crate) fn matches_major(v: &str, major: Option<&str>) -> bool {
    major.is_none_or(|m| v.split('.').next() == Some(m))
}

/// 是否稳定版：不含预发布标记（rc/beta/alpha/pre/dev/snapshot）。
fn is_stable(v: &str) -> bool {
    let low = v.to_ascii_lowercase();
    !["rc", "beta", "alpha", "pre", "dev", "snapshot"]
        .iter()
        .any(|m| low.contains(m))
}

/// 从 JSON 数组 `[{"name": "v1.2"}]` 提取 tag 名列表。
pub(crate) fn extract_tag_names(json: &str) -> Result<Vec<String>, String> {
    let v: serde_json::Value =
        serde_json::from_str(json).map_err(|e| format!("tags API 响应解析失败: {e}"))?;
    let arr = v.as_array().ok_or("tags API 响应非数组")?;
    Ok(arr
        .iter()
        .filter_map(|e| e.get("name").and_then(|n| n.as_str()))
        .map(String::from)
        .collect())
}

/// 从 GitLab releases 列表 JSON `[{"tag_name": "v1.2"}]` 提取 tag 名列表。
pub(crate) fn extract_release_tag_names(json: &str) -> Result<Vec<String>, String> {
    let v: serde_json::Value =
        serde_json::from_str(json).map_err(|e| format!("releases API 响应解析失败: {e}"))?;
    let arr = v.as_array().ok_or("releases API 响应非数组")?;
    Ok(arr
        .iter()
        .filter_map(|e| e.get("tag_name").and_then(|t| t.as_str()))
        .map(String::from)
        .collect())
}

/// 从 JSON `{"tag_name": "v1.2"}` 提取最新 release tag。
pub(crate) fn extract_latest_release_tag(json: &str) -> Result<String, String> {
    let v: serde_json::Value =
        serde_json::from_str(json).map_err(|e| format!("release API 响应解析失败: {e}"))?;
    v.get("tag_name")
        .and_then(|t| t.as_str())
        .map(String::from)
        .ok_or_else(|| "release API 响应无 tag_name".to_string())
}

/// 取 tag 列表中版本最大的（按 tag_prefix 剥离后 vercmp，稳定版优先）。
/// `major` 非空时只匹配该主版本的 tag（约束 major-of）。
pub(crate) fn max_tag_version(
    tags: &[String],
    prefix: &str,
    major: Option<&str>,
) -> Option<String> {
    let versions: Vec<String> = tags
        .iter()
        .filter_map(|t| strip_version(t, prefix))
        .filter(|v| matches_major(v, major))
        .collect();
    max_version_stable_first(versions, major, None)
}

/// 剥离 tag 前缀并校验版本形态：`v1.2.3` + prefix=`v` → `1.2.3`。
pub(crate) fn strip_version(tag: &str, prefix: &str) -> Option<String> {
    let v = tag.strip_prefix(prefix)?;
    if v.starts_with(|c: char| c.is_ascii_digit()) {
        Some(v.to_string())
    } else {
        None
    }
}

/// `{name}` 占位符替换。
pub(crate) fn substitute(template: &str, vars: &[(&str, &str)]) -> String {
    let mut s = template.to_string();
    for (k, v) in vars {
        s = s.replace(&format!("{{{k}}}"), v);
    }
    s
}

pub(crate) fn urlencode(path: &str) -> String {
    path.replace('/', "%2F")
}

/// 目录段是否为稳定分支候选（GNOME 惯例）。
/// 两段式 `x.y` 看 minor（glib 2.80 稳定 / 2.81 开发）；**单段式 `N`（桌面级版本号）恒为稳定候选**——
/// 桌面每个版本号都是正式版（44/45 都稳定），开发分支（51/90 等）靠"目录里只有 alpha/beta 文件 → 降级"过滤，不按奇偶排除。
pub(crate) fn minor_is_even(dir: &str) -> bool {
    let mut parts = dir.split('.');
    let _major = parts.next();
    match parts.next() {
        Some(minor) => minor.parse::<u64>().map(|m| m % 2 == 0).unwrap_or(false),
        None => true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn max_match_respects_cap() {
        // tcl 锁定场景：max-version 8.6.16 → 9.x 被过滤，取 8.6.16
        let re = Regex::new(r"tcl([\d.]+)-src\.tar\.gz").unwrap();
        let rss =
            "tcl8.6.14-src.tar.gz tcl8.6.16-src.tar.gz tcl9.0.1-src.tar.gz tcl9.0.4-src.tar.gz";
        assert_eq!(max_match(&re, rss, None, None).as_deref(), Some("9.0.4"));
        assert_eq!(
            max_match(&re, rss, None, Some("8.6.16")).as_deref(),
            Some("8.6.16"),
            "cap 应过滤超过封顶的版本"
        );
        assert_eq!(
            max_match(&re, rss, Some("8"), Some("8.6.16")).as_deref(),
            Some("8.6.16"),
            "cap 与 major 约束应叠加"
        );
    }
}
