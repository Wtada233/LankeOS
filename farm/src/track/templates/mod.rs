//! track 内置模板：**一个模板一个文件一个探测后端**（§9）。
//!
//! 每个模板文件只含一个 `probe(...) -> Result<EntryProbe>`（返回该槽位的版本 + URL），
//! 签名按"版本从哪来"分两类：
//! - **探测模板**（github/gitlab/…）：`probe(fetcher, cfg, major, pkg_name)`——联网抓上游最新版本；
//! - **锁版本模板**（`same-version` / `same-version-of-source`）：不联网，分别取
//!   `lookup(包名)` / `resolved`（本 tracker 已探测槽位）。
//!
//! 模板**被动触发**——由 source 条目的 `tracker_template` 字段指定，模板不主动从 URI 猜格式
//! （yaml 由人工/AI 编写，模板只是探测执行器）。
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
pub mod same_version_of_source;
pub mod script;
pub mod sourceforge;

use crate::error::FarmError;
use crate::net::Fetcher;
use regex::Regex;

use crate::track::{vercmp, EntryProbe, ResolvedSlots, SourceConfig};

// ───────────────────────────────────────────────────────────────────────────
// 模板注册表：**唯一的模板清单** —— 新增模板 = 新文件 + 这里一行
// ───────────────────────────────────────────────────────────────────────────

/// 探测函数的签名族。模板"版本从哪来"的差异是**本质的**（联网探测 / 读另一个包 / 读本 tracker
/// 更早槽位 / 脚本自述），所以不强行统一成一个 ctx 结构体——用枚举把四种形态显式写出来。
/// 联网探测上游：`(fetcher, cfg, major, pkg_name)`（8 个探测模板都是这一形态）
pub(crate) type WebProbe =
    fn(&dyn Fetcher, &SourceConfig, Option<&str>, &str) -> Result<EntryProbe, FarmError>;
/// 锁**另一个包**的版本：`lookup(包名) -> 版本`（`same-version`）
pub(crate) type LookupProbe =
    fn(&SourceConfig, &dyn Fn(&str) -> Option<String>, &str) -> Result<EntryProbe, FarmError>;
/// 锁**本 tracker 更早槽位**的版本（`same-version-of-source`）
pub(crate) type SlotProbe = fn(&SourceConfig, &ResolvedSlots) -> Result<EntryProbe, FarmError>;
/// 脚本自述：`(fetcher, cfg, pkg_name, version-var 变量表)`（`script`；`expand: true` 时可产多个槽位）
pub(crate) type ScriptProbe = fn(
    &dyn Fetcher,
    &SourceConfig,
    &str,
    &[(String, String)],
) -> Result<Vec<EntryProbe>, FarmError>;

#[derive(Debug)]
pub(crate) enum ProbeFn {
    Web(WebProbe),
    LockPackage(LookupProbe),
    LockSlot(SlotProbe),
    Script(ScriptProbe),
}

/// 一个模板的注册项。
#[derive(Debug)]
pub(crate) struct TemplateSpec {
    pub name: &'static str,
    /// 本模板接受的**条目字段**（yaml 键）。`tracker-template` 由注册表统一放行，不必写在这里。
    ///
    /// 注意"用户实际设了哪些字段"**不在这里**——那由 `serde` 投影得到（`track::validate_supported_fields`
    /// 序列化 `SourceConfig` 取键集，靠各字段的 `skip_serializing_if`）。所以**新增字段不必逐个模板登记**，
    /// 只有"某模板要开始支持某字段"才改这里。
    pub supported: &'static [&'static str],
    /// 是否吃版本约束（`major-of` / `major-version-lock`）：探测模板吃；锁版本 / 脚本不吃。
    pub version_constraints: bool,
    pub probe: ProbeFn,
}

/// 版本约束字段。用 `VersionFilter` 的模板才有意义（`TemplateSpec::version_constraints`）。
pub(crate) const VERSION_CONSTRAINT_FIELDS: &[&str] = &["major-of", "major-version-lock"];

/// 模板注册表。字段白名单（`validate_supported_fields`）与探测分发（`probe_with`）都从这里取，
/// 别再各写一份 `match`——历史上加一个模板要同步改 5 处（struct / set 表 / supported / 分发 / 文档）。
pub(crate) const TEMPLATES: &[TemplateSpec] = &[
    TemplateSpec {
        name: "github",
        supported: &[
            "repo",
            "mode",
            "tag-prefix",
            "template",
            "max-version",
            "stable-minor",
            "exclude",
        ],
        version_constraints: true,
        probe: ProbeFn::Web(github::probe),
    },
    TemplateSpec {
        name: "gitlab",
        supported: &[
            "host",
            "project",
            "mode",
            "tag-prefix",
            "template",
            "max-version",
            "stable-minor",
            "exclude",
        ],
        version_constraints: true,
        probe: ProbeFn::Web(gitlab::probe),
    },
    TemplateSpec {
        name: "html-index",
        supported: &[
            "url",
            "pattern",
            "template",
            "max-version",
            "stable-minor",
            "exclude",
            "source-name",
        ],
        version_constraints: true,
        probe: ProbeFn::Web(html_index::probe),
    },
    TemplateSpec {
        name: "multi-level-html-index",
        supported: &[
            "levels",
            "template",
            "max-version",
            "stable-minor",
            "exclude",
            "source-name",
        ],
        version_constraints: true,
        probe: ProbeFn::Web(multi_level_html_index::probe),
    },
    TemplateSpec {
        name: "gcs",
        supported: &[
            "url",
            "pattern",
            "template",
            "max-version",
            "stable-minor",
            "exclude",
            "source-name",
        ],
        version_constraints: true,
        probe: ProbeFn::Web(gcs::probe),
    },
    TemplateSpec {
        name: "gnome",
        supported: &[
            "template",
            "max-version",
            "stable-minor",
            "exclude",
            "source-name",
        ],
        version_constraints: true,
        probe: ProbeFn::Web(gnome::probe),
    },
    TemplateSpec {
        name: "sourceforge",
        supported: &[
            "project",
            "path",
            "pattern",
            "template",
            "max-version",
            "stable-minor",
            "exclude",
            "source-name",
        ],
        version_constraints: true,
        probe: ProbeFn::Web(sourceforge::probe),
    },
    TemplateSpec {
        name: "pypi",
        supported: &["project", "max-version", "stable-minor", "exclude"],
        version_constraints: true,
        probe: ProbeFn::Web(pypi::probe),
    },
    TemplateSpec {
        name: "same-version",
        supported: &["same-version-of", "template"],
        version_constraints: false,
        probe: ProbeFn::LockPackage(same_version::probe),
    },
    TemplateSpec {
        name: "same-version-of-source",
        supported: &["same-version-of-source", "template"],
        version_constraints: false,
        probe: ProbeFn::LockSlot(same_version_of_source::probe),
    },
    TemplateSpec {
        name: "script",
        supported: &["script", "expand", "version-var"],
        version_constraints: false,
        probe: ProbeFn::Script(script::probe),
    },
];

/// 按名字取注册项（`None` = 未知模板，由调用方报错）。
pub(crate) fn spec(name: &str) -> Option<&'static TemplateSpec> {
    TEMPLATES.iter().find(|s| s.name == name)
}

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

impl<'a> VersionFilter<'a> {
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

    /// 去掉 `max-version` 封顶的副本（其余约束不变）。
    ///
    /// 给"封顶已经在**上一层**判过、本层不该再判"的两段式探测用——gnome 按**目录**判封顶，
    /// 而目录里的**文件**版本串更长（`3.24.1` vs 封顶 `3.24`），再按封顶过滤会让整个目录落空。
    pub(crate) fn without_cap(&self) -> VersionFilter<'a> {
        VersionFilter {
            major: self.major,
            cap: None,
            exclude: self.exclude.clone(),
            even_minor: self.even_minor,
        }
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

/// **池级过滤**：`allows`（major / `max-version` 封顶 / `exclude`）→ `stable-minor` 的奇偶偏好。
///
/// 这是"约束筛选"在**池**这个粒度上的唯一实现。`max_version_stable_first`（一维候选列表）与
/// `gnome`（两段式：先挑目录、再挑目录里的文件，候选不是一维的）都从这里走，
/// 免得同一套约束在模板里出现第二份实现。
///
/// 奇偶偏好（`stable-minor`）：只保留 minor 为偶数的候选，**全被滤掉时退回全部**——上游偶尔
/// 没有偶数 minor 的稳定分支（GNOME 里非核心库就是），那不该直接探测失败。
/// 顺序是"先硬约束、后软偏好"：先 `allows` 再 even（历史 gnome 实现是反的，见 CHANGELOG）。
pub(crate) fn pool_filter(versions: Vec<String>, f: &VersionFilter) -> Vec<String> {
    let filtered: Vec<String> = versions.into_iter().filter(|v| f.allows(v)).collect();
    if !f.even_minor {
        return filtered;
    }
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
}

/// 候选版本的统一筛选 + 选取：池级过滤 → 稳定版优先 → 取最大。
///
/// 稳定版优先：`is_stable` 命中者优先，全都不稳定（全是 rc/beta…）才在所有候选里取最大。
pub(crate) fn max_version_stable_first(versions: Vec<String>, f: &VersionFilter) -> Option<String> {
    let filtered = pool_filter(versions, f);
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

/// 「锁定一个已知版本」类模板（`same-version` / `same-version-of-source`）共用的占位符替换。
///
/// 提供的占位符（两个模板完全一致）：
/// - `{version}`：锁定的版本号；
/// - `{major_minor}` = `{version:2}`；`{major_minor_patch}` = `{version:3}`
///   （上游把版本拆进目录层级时用：qt6 的 `qt/<6.11>/<6.11.1>/`、libreoffice 的
///   `src/<26.8.0>/libreoffice-<26.8.0.3>.tar.xz`）；
/// - `{version:N}`：版本**前 N 段**（点分）——上两个具名占位符的通用形式。
///
/// 段数不足（如 2 段版本配 `{major_minor_patch}`、N 大于实际段数）时占位符**不会被替换** →
/// `validate_url` 报"残留占位符"（显式失败，不静默产坏 URL）。
/// tag 前缀 / 仓库路径 / 上游名一律**烘进 template**（这两个模板不认 tag-prefix/repo/source-name）。
pub(crate) fn substitute_locked_version(template: &str, version: &str) -> String {
    let segs: Vec<&str> = version.split('.').collect();
    let prefix = |n: usize| segs.iter().take(n).copied().collect::<Vec<_>>().join(".");
    let mut owned: Vec<(String, String)> = vec![("version".to_string(), version.to_string())];
    owned.push(("major_minor".to_string(), prefix(2)));
    if segs.len() >= 3 {
        owned.push(("major_minor_patch".to_string(), prefix(3)));
    }
    for n in 1..=segs.len() {
        owned.push((format!("version:{n}"), prefix(n)));
    }
    let vars: Vec<(&str, &str)> = owned
        .iter()
        .map(|(k, v)| (k.as_str(), v.as_str()))
        .collect();
    substitute(template, &vars)
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
    fn pool_filter_applies_hard_constraints_before_even_preference() {
        // 唯一偶 minor 的候选被 major 排除时，**不能**整体落空：先 allows、再 even（空则退回全部）
        let f = VersionFilter {
            major: Some("3"),
            cap: None,
            exclude: None,
            even_minor: true,
        };
        assert_eq!(
            pool_filter(vec!["2.10".into(), "3.1".into()], &f),
            vec!["3.1".to_string()],
            "偶数候选被 major 排除后应退回全部（而不是空池）"
        );
        // 无硬约束时 even 偏好照常生效
        let f = VersionFilter {
            major: None,
            cap: None,
            exclude: None,
            even_minor: true,
        };
        assert_eq!(
            pool_filter(vec!["2.10".into(), "3.1".into()], &f),
            vec!["2.10".to_string()]
        );
    }

    #[test]
    fn locked_version_placeholders_cover_segment_prefixes() {
        // {version} / {major_minor}(={version:2}) / {major_minor_patch}(={version:3}) / {version:N}
        assert_eq!(
            substitute_locked_version(
                "a/{version}/b/{major_minor}/c/{major_minor_patch}/d/{version:3}",
                "26.8.0.3"
            ),
            "a/26.8.0.3/b/26.8/c/26.8.0/d/26.8.0"
        );
        // 段数不足 → 占位符残留（调用方的 validate_url 会拒，不静默产坏 URL）
        assert!(substitute_locked_version("{version:5}", "1.2").contains('{'));
        assert!(substitute_locked_version("{major_minor_patch}", "1.2").contains('{'));
    }

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
