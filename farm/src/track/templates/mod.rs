//! track 内置模板：**一个模板一个文件一个探测后端**（§9）。
//!
//! 每个模板文件只含 `probe(fetcher, cfg, major, pkg_name) -> Result<EntryProbe>`：
//! 联网抓最新版本，返回该 source 槽位的版本 + URL。模板**被动触发**——由 source 条目的
//! `tracker_template` 字段指定，模板不主动从 URI 猜格式（yaml 由人工/AI 编写，模板只是探测执行器）。
//!
//! `script` 是**条目级**模板（与其他模板平级，不是包级类型）：一个脚本产一个槽位，
//! stdout 为一行 `<版本>|URL`；声明 `expand: true` 时可产多个槽位（每行一个）。
//! 详见 `script.rs` 的模块文档。

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

use crate::error::FarmError;
use crate::net::Fetcher;
use regex::Regex;

use crate::track::{vercmp, SourceConfig};

// ───────────────────────────────────────────────────────────────────────────
// 版本筛选约束（各模板共享的**单一汇点**）
// ───────────────────────────────────────────────────────────────────────────

/// 候选版本的筛选约束：主版本 / 封顶 / 黑名单 / 偶数 minor。
///
/// 所有探测模板的候选版本都必须从这里过一遍（`max_match` / `max_tag_version` /
/// `max_version_stable_first` 都收 `&VersionFilter`）——**单一汇点**是有意的：
/// 否则会出现"某模板支持 `max-version`、另一个不支持"的漂移（历史上正是如此）。
pub(crate) struct VersionFilter<'a> {
    /// `major-of` / `major-version-lock` 解析出的主版本（首段须相等）。
    pub major: Option<&'a str>,
    /// `max-version`：数值封顶，超过即排除。
    pub cap: Option<&'a str>,
    /// `exclude`：作用于**提取出的版本字符串**的正则，命中即整条候选丢弃。
    /// 用于上游混着历史异常 tag 的场景（uasm 的 `v213`、cython 的 `3.3.0b1`、intel-media 的 `600`）。
    pub exclude: Option<Regex>,
    /// `stable-minor: even`：只保留 minor（第二段）为偶数的候选（GNOME 惯例）。
    pub even_minor: bool,
}

impl VersionFilter<'_> {
    /// 单个候选是否通过**硬约束**（major / `max-version` 封顶 / `exclude` 黑名单）。
    ///
    /// **不含 `stable-minor`**——那是"优先偶 minor、一条都没有才退回全部"的**池级偏好**，
    /// 单条候选上判不了（池里只有一条奇 minor 时它照样合法）。
    /// 供"只拿到一条候选"的路径（如 github `/releases/latest`）复用，避免那条路径完全绕开约束。
    pub(crate) fn allows(&self, v: &str) -> bool {
        matches_major(v, self.major)
            && self
                .cap
                .is_none_or(|c| vercmp::cmp_version(v, c) != std::cmp::Ordering::Greater)
            && self.exclude.as_ref().is_none_or(|re| !re.is_match(v))
    }
}

/// 从条目配置 + 已解析的 `major` 构造约束（`exclude` 正则在此编译）。
/// `major` 的解析（`major-version-lock` 常量 / `major-of` 查表）在 `track/mod.rs`，此处只收结果。
pub(crate) fn version_filter<'a>(
    cfg: &'a SourceConfig,
    major: Option<&'a str>,
) -> Result<VersionFilter<'a>, FarmError> {
    let exclude = match cfg.exclude.as_deref() {
        Some(pat) => Some(Regex::new(pat).map_err(|e| format!("exclude 正则无效 `{pat}`: {e}"))?),
        None => None,
    };
    Ok(VersionFilter {
        major,
        cap: cfg.max_version.as_deref(),
        exclude,
        even_minor: cfg.stable_minor.as_deref() == Some("even"),
    })
}

// ───────────────────────────────────────────────────────────────────────────
// 共享辅助（模板文件内部使用）
// ───────────────────────────────────────────────────────────────────────────

/// 提取 URL/HTML/XML 中符合正则（含一个捕获组）的最大版本。
/// `major` 非空时只匹配该主版本号的 tag（约束 major-of，§9）。
/// 默认稳定版优先，无稳定版才回落（§9：track 追上游最新**稳定**版）。
pub(crate) fn max_match(re: &Regex, text: &str, f: &VersionFilter) -> Option<String> {
    let versions: Vec<String> = re
        .captures_iter(text)
        .filter_map(|c| c.get(1).map(|m| m.as_str().to_string()))
        .filter(|v| v.starts_with(|ch: char| ch.is_ascii_digit()))
        .collect();
    max_version_stable_first(versions, f)
}

/// 候选版本的统一筛选 + 选取：约束过滤 → `stable-minor` → 稳定版优先 → 取最大。
///
/// 约束过滤走 `VersionFilter::allows`（major / max-version 封顶 / exclude 黑名单）。
/// `stable-minor: even` 只保留 minor 为偶数的候选，**全被滤掉时退回全部**（与 GNOME 同款兜底：
/// 上游偶尔没有偶数 minor 的稳定分支时不该直接探测失败）。
/// 稳定版优先：`is_stable` 命中者优先，全都不稳定（全是 rc/beta…）才在所有候选里取最大。
pub(crate) fn max_version_stable_first(versions: Vec<String>, f: &VersionFilter) -> Option<String> {
    let filtered: Vec<String> = versions.into_iter().filter(|v| f.allows(v)).collect();
    let filtered: Vec<String> = if f.even_minor {
        let even: Vec<String> = filtered
            .iter()
            .filter(|v| minor_is_even(v))
            .cloned()
            .collect();
        if even.is_empty() {
            filtered
        } else {
            even
        }
    } else {
        filtered
    };
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
///
/// **这里只放各生态通用的"预发布单词"**。生态专属的版本语义（如 Python 的 PEP 440 短形态
/// `3.3.0b1`）**属于对应模板**，不要塞进来——`is_stable` 被全部 850 个 tracker 共用，
/// 在这里加 Python 惯例会让 Rust/C 包的 `1.2.3a1` 也被当成预发布。PEP 440 见 `pypi.rs`。
fn is_stable(v: &str) -> bool {
    let low = v.to_ascii_lowercase();
    !["rc", "beta", "alpha", "pre", "dev", "snapshot"]
        .iter()
        .any(|m| low.contains(m))
}

/// 解析 JSON 数组响应（列表端点）。
pub(crate) fn parse_json_array(json: &str, ctx: &str) -> Result<Vec<serde_json::Value>, FarmError> {
    let v: serde_json::Value =
        serde_json::from_str(json).map_err(|e| format!("{ctx} API 响应解析失败: {e}"))?;
    match v {
        serde_json::Value::Array(a) => Ok(a),
        _ => Err(format!("{ctx} API 响应非数组").into()),
    }
}

/// 从已解析的 releases 数组取值里提取 `tag_name`（GitHub / GitLab releases 同构）。
pub(crate) fn release_tag_names(items: &[serde_json::Value]) -> Vec<String> {
    items
        .iter()
        .filter_map(|e| e.get("tag_name").and_then(|t| t.as_str()).map(String::from))
        .collect()
}

/// **分页**拉取 JSON 数组列表端点（GitHub / GitLab releases 同构：`?per_page=N&page=M`）。
///
/// 逐页**单独请求**，直到某页条目数 < `per_page`（末页）或到 `max_pages` 兜底。判末页只用 body：
/// `Fetcher` 不暴露响应头（不依赖 `Link` / `X-Next-Page`），而两端点都返回数组且页大小由我们指定
/// ⇒ "本页不足一页"即最后一页。这样 release/tag 数量再多也不会因为"只看第一页"漏掉目标版本。
pub(crate) fn fetch_json_pages(
    fetcher: &dyn Fetcher,
    url_without_page: &str,
    per_page: usize,
    max_pages: usize,
) -> Result<Vec<serde_json::Value>, FarmError> {
    let mut out: Vec<serde_json::Value> = Vec::new();
    for page in 1..=max_pages {
        let url = format!("{url_without_page}&page={page}");
        let body = fetcher.get(&url)?;
        let mut arr = parse_json_array(&body, "列表")?;
        let n = arr.len();
        out.append(&mut arr);
        if n < per_page {
            break;
        }
    }
    Ok(out)
}

/// 从 JSON `{"tag_name": "v1.2"}` 提取最新 release tag。
pub(crate) fn extract_latest_release_tag(json: &str) -> Result<String, FarmError> {
    let v: serde_json::Value =
        serde_json::from_str(json).map_err(|e| format!("release API 响应解析失败: {e}"))?;
    v.get("tag_name")
        .and_then(|t| t.as_str())
        .map(String::from)
        .ok_or_else(|| "release API 响应无 tag_name".to_string().into())
}

/// 取 tag 列表中版本最大的（按 `tag_prefix` 剥离后过 `VersionFilter`，稳定版优先）。
pub(crate) fn max_tag_version(tags: &[String], prefix: &str, f: &VersionFilter) -> Option<String> {
    let versions: Vec<String> = tags
        .iter()
        .filter_map(|t| strip_version(t, prefix))
        .collect();
    max_version_stable_first(versions, f)
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

    /// 造一个约束（不经过 `SourceConfig`，直接构造）。
    fn vf<'a>(major: Option<&'a str>, cap: Option<&'a str>) -> VersionFilter<'a> {
        VersionFilter {
            major,
            cap,
            exclude: None,
            even_minor: false,
        }
    }

    #[test]
    fn max_match_respects_cap() {
        // tcl 锁定场景：max-version 8.6.16 → 9.x 被过滤，取 8.6.16
        let re = Regex::new(r"tcl([\d.]+)-src\.tar\.gz").unwrap();
        let rss =
            "tcl8.6.14-src.tar.gz tcl8.6.16-src.tar.gz tcl9.0.1-src.tar.gz tcl9.0.4-src.tar.gz";
        assert_eq!(
            max_match(&re, rss, &vf(None, None)).as_deref(),
            Some("9.0.4")
        );
        assert_eq!(
            max_match(&re, rss, &vf(None, Some("8.6.16"))).as_deref(),
            Some("8.6.16"),
            "cap 应过滤超过封顶的版本"
        );
        assert_eq!(
            max_match(&re, rss, &vf(Some("8"), Some("8.6.16"))).as_deref(),
            Some("8.6.16"),
            "cap 与 major 约束应叠加"
        );
    }

    #[test]
    fn is_stable_covers_generic_prerelease_wording() {
        // 各生态通用的"预发布单词"。**不要**把生态专属语义（PEP 440 的 `3.3.0b1`）加到这里
        // ——那属于对应模板（Python → `pypi.rs`），加了会污染所有 850 个 tracker。
        for v in [
            "1.0.0-beta1",
            "1.0_rc2",
            "1.0.0alpha",
            "1.0pre1",
            "1.0-dev",
            "1.0.0snapshot",
        ] {
            assert!(!is_stable(v), "{v} 应判为不稳定");
        }
        for v in ["3.3.0", "2.57r", "1.2.3", "5.44.0", "20260917.127dd2a6"] {
            assert!(is_stable(v), "{v} 应判为稳定");
        }
    }

    #[test]
    fn exclude_drops_matching_candidates() {
        // uasm 场景：上游混着旧命名法的 v213，按版本比较 213 > 2.57 会被选中 → 用 exclude 排掉
        let re = Regex::new(r"v([\d.]+[a-z]*)").unwrap();
        let text = "v2.56.2 v2.57 v213";
        assert_eq!(
            max_match(&re, text, &vf(None, None)).as_deref(),
            Some("213"),
            "不设 exclude 时会误选 213（这正是需要 exclude 的原因）"
        );
        let f = VersionFilter {
            exclude: Some(Regex::new(r"^213$").unwrap()),
            ..vf(None, None)
        };
        assert_eq!(max_match(&re, text, &f).as_deref(), Some("2.57"));

        // 注意 exclude 治的是"**不是**预发布标记、但语义上是废弃命名"的 tag（如 uasm 的 213）。
        // 真正的预发布（PEP 440 短形态）由 `is_stable` 负责，不需要 exclude
        // ——见 `pep440_beta_loses_to_stable_without_exclude`。
    }

    #[test]
    fn even_minor_keeps_even_and_falls_back_to_all() {
        let re = Regex::new(r"v([\d.]+)").unwrap();
        let f = VersionFilter {
            even_minor: true,
            ..vf(None, None)
        };
        assert_eq!(
            max_match(&re, "v2.55.0 v2.56.0 v2.57.0", &f).as_deref(),
            Some("2.56.0"),
            "只保留 minor 为偶数的候选"
        );
        // 全是奇 minor → 退回全部（GNOME 同款兜底：不该直接探测失败）
        assert_eq!(
            max_match(&re, "v2.55.0 v2.57.0", &f).as_deref(),
            Some("2.57.0"),
            "偶数候选为空时退回全部"
        );
        // 默认（不设 stable-minor）不受影响
        assert_eq!(
            max_match(&re, "v2.55.0 v2.56.0 v2.57.0", &vf(None, None)).as_deref(),
            Some("2.57.0")
        );
    }
}
