//! track 系统（§9）：追踪上游最新版本，产出更新提案。
//!
//! 每个包在 `data/trackers/<pkg>.yaml` 维护一个 **tracker 配置**，扁平结构：
//!
//! ```yaml
//! pkg-name: systemd
//! tracker-template: github          # github|gitlab|sourceforge|gnome|gcs|html-index|script
//! repo: systemd/systemd
//! mode: tags
//! tag-prefix: v
//! template: "https://github.com/{repo}/archive/refs/tags/{tag}.tar.gz"
//! ```
//!
//! 多源包用 `sources:` / `work_sources:` 给每个额外源声明独立追踪配置：每个条目用 `url-match` 正则
//! 匹配 LankeBUILD.json 里的实际 URL（非索引），各自带完整追踪配置（template/script/same-version…）。
//!
//! **主动性**：yaml 指定模板 → 调用该模板探测；模板不主动从 URI 猜格式（yaml 由人工/AI 编写）。

pub mod templates;
pub mod vercmp;

use serde::{Deserialize, Serialize};

use crate::net::Fetcher;

/// 探测结果：最新版本 + 具体下载 URL（提案的基础）。
/// `sources`：主源（lpkg 解压）；`work_sources`：只下载不解压（Arch noextract 对应，
/// 如 LibreOffice 的 vendor tarball 走 --with-external-tar）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProbeResult {
    pub version: String,
    pub sources: Vec<String>,
    pub work_sources: Vec<String>,
}

/// 更新提案：`pkg` 当前版本 → 上游最新版本 + 具体源 URL。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Proposal {
    pub pkg_name: String,
    pub current_version: String,
    pub new_version: String,
    pub sources: Vec<String>,
    pub work_sources: Vec<String>,
    pub tracker_template: String,
}

/// tracker yaml 配置（扁平结构，模板参数按 `tracker_template` 不同取用）。
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct TrackerConfig {
    #[serde(default, rename = "pkg-name")]
    pub pkg_name: String,
    #[serde(rename = "tracker-template")]
    pub tracker_template: String,

    // ── github / gitlab ──
    #[serde(skip_serializing_if = "Option::is_none")]
    pub repo: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub host: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mode: Option<String>, // tags | releases
    #[serde(rename = "tag-prefix", skip_serializing_if = "Option::is_none")]
    pub tag_prefix: Option<String>,

    // ── html-index / gcs / sourceforge ──
    #[serde(skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub pattern: Option<String>,

    // ── sourceforge ──
    #[serde(skip_serializing_if = "Option::is_none")]
    pub project: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub path: Option<String>,

    // ── 通用：下载 URL 模板（{name}/{version}/{tag}…占位符）──
    #[serde(skip_serializing_if = "Option::is_none")]
    pub template: Option<String>,

    // ── script 模板 ──
    #[serde(rename = "script-content", skip_serializing_if = "Option::is_none")]
    pub script_content: Option<String>,

    // ── 多源包：sources[1..] / work_sources 各自的追踪配置（正则匹配实际 URL，非索引）──
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub sources: Vec<TrackerConfig>, // sources[1..] 的追踪配置：每个条目用 url-match 正则匹配实际 URL
    #[serde(rename = "work_sources", default, skip_serializing_if = "Vec::is_empty")]
    pub work_sources: Vec<TrackerConfig>, // work_sources 的追踪配置：同上
    #[serde(rename = "url-match", skip_serializing_if = "Option::is_none")]
    pub url_match: Option<String>, // sources/work_sources 条目专用：正则匹配对应 source URL（区别于 html-index 的 version pattern）

    // ── 版本约束 / 顺序 ──
    #[serde(skip_serializing_if = "Option::is_none")]
    pub order: Option<String>, // "last"：最后处理；"after(<pkg>)"：在指定包之后处理（版本/主版本前置）
    #[serde(rename = "stable-minor", skip_serializing_if = "Option::is_none")]
    pub stable_minor: Option<String>, // gnome 模板："even"(默认)偶 minor 稳定分支；"all"取最大版本（非核心 GNOME 库如 libepoxy）
    #[serde(rename = "same-version", skip_serializing_if = "Option::is_none")]
    pub same_version: Option<String>, // 锁定为指定包的 LankeBUILD.json 版本（忽略匹配规则）
    #[serde(rename = "major-of", skip_serializing_if = "Option::is_none")]
    pub major_of: Option<String>, // 匹配指定包主版本的 tag
    #[serde(rename = "source-name", skip_serializing_if = "Option::is_none")]
    pub source_name: Option<String>, // 上游源目录/文件名覆盖（gtk3 的上游目录叫 gtk，python-gobject 叫 pygobject）
    #[serde(rename = "major-version-lock", skip_serializing_if = "Option::is_none")]
    pub major_version_lock: Option<String>, // 锁定探测主版本常量（gtk3 锁 3，gtk4 锁 4）
    #[serde(rename = "max-version", skip_serializing_if = "Option::is_none")]
    pub max_version: Option<String>, // 探测目录/版本封顶（gtk3 稳定系列止于 3.24，3.98 是历史 dev 分支）
}

impl TrackerConfig {
    /// 序列化为 tracker yaml（提案文件内容）。
    pub fn to_yaml(&self) -> Result<String, String> {
        serde_yaml::to_string(self).map_err(|e| format!("序列化 tracker yaml 失败: {e}"))
    }

    /// 上游名字：`source-name` 覆盖（gtk3 → 上游目录叫 gtk），否则用包名。
    pub fn source_name(&self) -> &str {
        self.source_name.as_deref().unwrap_or(&self.pkg_name)
    }

    /// 编译 `url-match` 正则（sources/work_sources 条目用于匹配 LankeBUILD.json 里的实际 source URL）。
    pub fn url_match_regex(&self) -> Result<Option<regex::Regex>, String> {
        match &self.url_match {
            None => Ok(None),
            Some(p) => regex::Regex::new(p)
                .map(Some)
                .map_err(|e| format!("url-match 正则无效 '{p}': {e}")),
        }
    }

    /// 按 `tracker_template` 分发到对应模板探测最新版本。
    /// `lookup` 供版本约束解析（读其他包的 LankeBUILD.json 版本）。
    pub fn probe_with(
        &self,
        fetcher: &dyn Fetcher,
        lookup: &dyn Fn(&str) -> Option<String>,
    ) -> Result<ProbeResult, String> {
        // 约束 1：same-version —— 锁定为指定包的版本，忽略匹配规则
        if let Some(pkg) = &self.same_version {
            let v = lookup(pkg).ok_or_else(|| {
                format!("same-version 依赖 {pkg} 无版本（读 LankeBUILD.json/已解析版本失败）")
            })?;
            let template = need(&self.template, "template")?;
            let name = self.pkg_name.clone();
            // 下载 URL：tag = tag_prefix + 版本（如 vulkan-sdk-1.4.350.1）。模板可能是 {tag} 或 {version} 形态，都替换。
            let tag = format!("{}{v}", self.tag_prefix.as_deref().unwrap_or(""));
            // {major_minor} = 版本前两段（qt6 等：目录结构 qt/<6.11>/<6.11.1>/，不能锁死 minor）。
            let major_minor: String = v.split('.').take(2).collect::<Vec<_>>().join(".");
            let mut vars = vec![
                ("name", name.as_str()),
                ("version", v.as_str()),
                ("tag", tag.as_str()),
                ("major_minor", major_minor.as_str()),
            ];
            if let Some(repo) = &self.repo {
                vars.push(("repo", repo.as_str()));
            }
            let src = templates::substitute(template, &vars);
            return validate_probe_result(ProbeResult {
                version: v,
                sources: vec![src],
                work_sources: vec![],
            });
        }
        // 约束 2：主版本约束 —— major-version-lock（常量）优先，否则 major-of（取指定包主版本）
        let major = if let Some(lock) = &self.major_version_lock {
            Some(lock.clone())
        } else {
            match &self.major_of {
                Some(pkg) => {
                    let v = lookup(pkg).ok_or_else(|| {
                        format!("major-of 依赖 {pkg} 无版本（读 LankeBUILD.json 失败）")
                    })?;
                    Some(v.split('.').next().unwrap_or("").to_string())
                }
                None => None,
            }
        };
        let result = match self.tracker_template.as_str() {
            "github" => templates::github::probe(fetcher, self, major.as_deref()),
            "gitlab" => templates::gitlab::probe(fetcher, self, major.as_deref()),
            "sourceforge" => templates::sourceforge::probe(fetcher, self, major.as_deref()),
            "gnome" => templates::gnome::probe(fetcher, self, major.as_deref()),
            "gcs" => templates::gcs::probe(fetcher, self, major.as_deref()),
            "html-index" => templates::html_index::probe(fetcher, self, major.as_deref()),
            "pypi" => templates::pypi::probe(fetcher, self, major.as_deref()),
            "script" => templates::script::probe(fetcher, self, major.as_deref()),
            other => Err(format!("未知 tracker_template: {other}")),
        }?;
        validate_probe_result(result)
    }

    /// 无约束探测（lookup 返回 None）。
    pub fn probe(&self, fetcher: &dyn Fetcher) -> Result<ProbeResult, String> {
        self.probe_with(fetcher, &|_| None)
    }

    /// 探测 + 生成更新提案（对照当前版本）。
    pub fn propose_with(
        &self,
        fetcher: &dyn Fetcher,
        lookup: &dyn Fn(&str) -> Option<String>,
        current_version: &str,
    ) -> Result<Proposal, String> {
        let result = self.probe_with(fetcher, lookup)?;
        Ok(Proposal {
            pkg_name: self.pkg_name.clone(),
            current_version: current_version.to_string(),
            new_version: result.version,
            sources: result.sources,
            work_sources: result.work_sources,
            tracker_template: self.tracker_template.clone(),
        })
    }

    /// 无约束提案。
    pub fn propose(
        &self,
        fetcher: &dyn Fetcher,
        current_version: &str,
    ) -> Result<Proposal, String> {
        self.propose_with(fetcher, &|_| None, current_version)
    }
}

/// 需要的必填字段缺失时给出清晰错误。
pub(crate) fn need<'a>(opt: &'a Option<String>, field: &str) -> Result<&'a str, String> {
    opt.as_deref()
        .ok_or_else(|| format!("tracker 配置缺 {field}"))
}

/// 校验探测结果：URL 残留 `{...}` 说明模板引用了未提供的占位符，生成的 URL 必然损坏。
/// 报错而非静默写入坏 URL（杜绝"莫名其妙改 URL"）。
fn validate_probe_result(r: ProbeResult) -> Result<ProbeResult, String> {
    for url in &r.sources {
        if url.contains('{') {
            return Err(format!("探测生成的 URL 残留未替换占位符: {url}"));
        }
    }
    Ok(r)
}

/// 解析 `order` 值：None | `last` | `after(<pkg>)`。
/// `after(<pkg>)`：在指定包**之后**处理（该包的新版本/主版本是当前包的前置输入）。
fn order_target(cfg: &TrackerConfig) -> Option<String> {
    let o = cfg.order.as_deref()?;
    o.strip_prefix("after(")?
        .strip_suffix(')')
        .map(|s| s.trim().to_string())
}

/// 依赖边（prereq -> package，prereq 先处理）。来源：
/// - `same-version: X` / `major-of: X`：隐式边 X → 本包（版本/主版本输入必须先就绪）；
/// - `order: after(<pkg>)`：显式边 <pkg> → 本包（不必最后，只要在其前置之后）；
/// - `order: last`：所有非 last 包都是它的前置（等价于声明一堆 after 边）。
///
/// 只保留两端都在 `names` 里的边（引用不存在的包 → 落回 LankeBUILD.json 查询，不影响顺序）。
/// 串行 `order_entries` 与并行 `-j` 调度共用这一套边，保证 after/last 顺序不受并行破坏。
pub fn dep_edges(
    names: &[String],
    trackers: &std::collections::HashMap<String, TrackerConfig>,
) -> Vec<(String, String)> {
    let is_last = |n: &str| {
        trackers
            .get(n)
            .is_some_and(|c| c.order.as_deref() == Some("last"))
    };
    let mut edges: Vec<(String, String)> = Vec::new();
    for name in names {
        let Some(cfg) = trackers.get(name) else {
            continue;
        };
        if let Some(p) = &cfg.same_version {
            edges.push((p.clone(), name.clone()));
        }
        if let Some(p) = &cfg.major_of {
            edges.push((p.clone(), name.clone()));
        }
        if let Some(x) = order_target(cfg) {
            edges.push((x, name.clone()));
        }
        if is_last(name) {
            for other in names {
                if !is_last(other) {
                    edges.push((other.clone(), name.clone()));
                }
            }
        }
    }
    // 去重：same-version/major-of 隐式边 与 after(X) 显式边可能指向同一前置
    let mut seen = std::collections::HashSet::new();
    edges.retain(|(a, b)| seen.insert((a.clone(), b.clone())));
    edges
        .into_iter()
        .filter(|(a, b)| names.iter().any(|n| n == a) && names.iter().any(|n| n == b))
        .collect()
}

/// 包处理顺序（`farm track --all` 的串行顺序；`-j` 并行时由同一套 `dep_edges` 做入度门控）。
///
/// 例：`SPIRV-LLVM-Translator`(`after(llvm)` + major-of llvm) 在 llvm 之后处理；
/// `SPIRV-Headers`(`after(vulkan-headers)` + same-version vulkan-headers) 在 vulkan-headers 之后处理，
/// 从而读到其本轮解析出的新版本，而不是落回 LankeBUILD.json 的旧版本。
/// 环/引用不存在的包：对应边被忽略，未排出的包按名补在后，不阻塞整个 --all。
pub fn order_entries(
    names: Vec<String>,
    trackers: &std::collections::HashMap<String, TrackerConfig>,
) -> Vec<String> {
    let edges = dep_edges(&names, trackers);

    // Kahn 拓扑排序：入度 = 前置数；入度 0 的包先处理，按名稳定
    let mut indeg: std::collections::HashMap<String, usize> =
        names.iter().map(|n| (n.clone(), 0)).collect();
    for (_, b) in &edges {
        *indeg.entry(b.clone()).or_default() += 1;
    }
    let mut ready: Vec<String> = names
        .iter()
        .filter(|n| indeg.get(n.as_str()) == Some(&0))
        .cloned()
        .collect();
    ready.sort();
    let mut out: Vec<String> = Vec::new();
    while !ready.is_empty() {
        let n = ready.remove(0);
        out.push(n.clone());
        for (a, b) in &edges {
            if *a == n {
                *indeg.get_mut(b.as_str()).unwrap() -= 1;
                if indeg[b.as_str()] == 0 {
                    ready.push(b.clone());
                }
            }
        }
        ready.sort();
    }
    // 环/异常兜底：未排出的按名补上
    if out.len() < names.len() {
        let mut rest: Vec<String> = names.into_iter().filter(|n| !out.contains(n)).collect();
        rest.sort();
        out.extend(rest);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::net::MockFetcher;
    use std::collections::HashMap;

    #[test]
    fn sources_work_sources_yaml_roundtrip() {
        let yaml = r#"
pkg-name: libplacebo
tracker-template: github
repo: haasn/libplacebo
mode: tags
tag-prefix: v
template: "https://github.com/haasn/libplacebo/archive/refs/tags/{tag}.tar.gz"
sources:
  - pkg-name: Vulkan-Headers
    url-match: "Vulkan-Headers/archive/refs/tags/vulkan-sdk-"
    tracker-template: github
    repo: KhronosGroup/Vulkan-Headers
    mode: tags
    tag-prefix: vulkan-sdk-
    template: "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/{tag}.tar.gz"
    same-version: vulkan-headers
  - pkg-name: fast_float
    url-match: "fastfloat/fast_float"
    tracker-template: github
    repo: fastfloat/fast_float
    mode: tags
    tag-prefix: v
    template: "https://github.com/fastfloat/fast_float/archive/refs/tags/{tag}.tar.gz"
work_sources:
  - pkg-name: some-patch
    url-match: "patches/lfs/"
    tracker-template: script
    script-content: |
      #!/bin/bash
      echo "1.0"
      echo "https://patches/lfs/foo-1.0.patch"
"#;
        let cfg: TrackerConfig = serde_yaml::from_str(yaml).unwrap();
        assert_eq!(cfg.sources.len(), 2);
        assert_eq!(cfg.sources[0].url_match.as_deref(), Some("Vulkan-Headers/archive/refs/tags/vulkan-sdk-"));
        assert_eq!(cfg.sources[0].same_version.as_deref(), Some("vulkan-headers"));
        assert_eq!(cfg.sources[0].repo.as_deref(), Some("KhronosGroup/Vulkan-Headers"));
        assert_eq!(cfg.sources[1].repo.as_deref(), Some("fastfloat/fast_float"));
        assert_eq!(cfg.work_sources.len(), 1);
        assert_eq!(cfg.work_sources[0].tracker_template, "script");
        // url-match 正则可编译
        assert!(cfg.sources[0].url_match_regex().unwrap().is_some());
        assert!(cfg.work_sources[0].url_match_regex().unwrap().is_some());
    }

    #[test]
    fn template_leftover_placeholder_is_rejected() {
        // 模板引用未提供的占位符 → URL 残留 {unknown} → 探测报错，而非静默生成坏 URL
        let cfg = TrackerConfig {
            pkg_name: "x".into(),
            tracker_template: "github".into(),
            repo: Some("a/b".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            template: Some("https://example.com/{repo}/{unknown}/{version}.tar.gz".into()),
            ..Default::default()
        };
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://api.github.com/repos/a/b/tags",
            r#"[{"name":"v1.2"}]"#,
        );
        let err = cfg.probe(&f).unwrap_err();
        assert!(err.contains("残留未替换占位符"), "err: {err}");
    }

    #[test]
    fn url_match_regex_invalid_falls_back_to_literal() {
        // 无效正则返回 Err（调用方警告后跳过），有效正则正常编译
        let cfg = TrackerConfig {
            pkg_name: "x".into(),
            tracker_template: "github".into(),
            url_match: Some("([".into()), // 无效正则
            ..Default::default()
        };
        assert!(cfg.url_match_regex().is_err());
        let ok = TrackerConfig {
            pkg_name: "x".into(),
            tracker_template: "github".into(),
            url_match: Some("vulkan-sdk-".into()),
            ..Default::default()
        };
        assert!(ok.url_match_regex().unwrap().is_some());
    }

    #[test]
    fn tracker_yaml_roundtrip() {
        let yaml = r#"
pkg-name: systemd
tracker-template: github
repo: systemd/systemd
mode: tags
tag-prefix: v
template: "https://github.com/{repo}/archive/refs/tags/{tag}.tar.gz"
"#;
        let cfg: TrackerConfig = serde_yaml::from_str(yaml).unwrap();
        assert_eq!(cfg.pkg_name, "systemd");
        assert_eq!(cfg.tracker_template, "github");
        assert_eq!(cfg.repo.as_deref(), Some("systemd/systemd"));
        assert_eq!(cfg.tag_prefix.as_deref(), Some("v"));
    }

    #[test]
    fn script_yaml_with_inline_bash() {
        let yaml = r#"
pkg-name: tmux
tracker-template: script
script-content: |
  #!/bin/bash
  echo "3.7b"
  echo "https://github.com/tmux/tmux/releases/download/3.7b/tmux-3.7b.tar.gz"
"#;
        let cfg: TrackerConfig = serde_yaml::from_str(yaml).unwrap();
        assert_eq!(cfg.tracker_template, "script");
        let content = cfg.script_content.unwrap();
        assert!(content.contains("echo \"3.7b\""));
    }

    #[test]
    fn yaml_serialization_skips_none() {
        let cfg = TrackerConfig {
            pkg_name: "bash".into(),
            tracker_template: "html-index".into(),
            url: Some("https://ftp.gnu.org/gnu/bash/".into()),
            pattern: Some(r"bash[-_]?(\d[\d.]*)\.tar\.(?:xz|gz|bz2)".into()),
            template: Some("https://ftp.gnu.org/gnu/bash/{name}-{version}.tar.gz".into()),
            ..Default::default()
        };
        let yaml = cfg.to_yaml().unwrap();
        assert!(yaml.contains("pkg-name: bash"));
        assert!(yaml.contains("tracker-template: html-index"));
        assert!(!yaml.contains("repo:"));
    }

    #[test]
    fn same_version_locks_version_and_substitutes_tag() {
        // same-version 直接锁定 vulkan-headers 的版本，忽略 github tags 匹配；
        // 下载 URL 用 {tag}（tag_prefix + version）与 {repo} 生成，不能残留占位符。
        let cfg = TrackerConfig {
            pkg_name: "SPIRV-Headers".into(),
            tracker_template: "github".into(),
            repo: Some("KhronosGroup/SPIRV-Headers".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("vulkan-sdk-".into()),
            template: Some("https://github.com/{repo}/archive/refs/tags/{tag}.tar.gz".into()),
            same_version: Some("vulkan-headers".into()),
            ..Default::default()
        };
        let r = cfg
            .probe_with(
                &crate::net::RealFetcher::default(), // same-version 分支不联网
                &|pkg| (pkg == "vulkan-headers").then(|| "1.4.350.1".to_string()),
            )
            .unwrap();
        assert_eq!(r.version, "1.4.350.1");
        assert_eq!(
            r.sources,
            vec!["https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz"]
        );
    }

    #[test]
    fn same_version_substitutes_major_minor_for_dir_paths() {
        // qt6 风格：same-version 锁定 qt6-base 的版本后，模板用 {major_minor}/{version} 拼目录
        //（qt/<6.11>/<6.11.1>/，不能锁死 minor——6.12 发布自动跟进）。
        let cfg = TrackerConfig {
            pkg_name: "qt6-declarative".into(),
            tracker_template: "html-index".into(),
            same_version: Some("qt6-base".into()),
            template: Some(
                "https://download.qt.io/official_releases/qt/{major_minor}/{version}/submodules/qtdeclarative-everywhere-src-{version}.tar.xz"
                    .into(),
            ),
            ..Default::default()
        };
        let r = cfg
            .probe_with(
                &crate::net::RealFetcher::default(), // same-version 分支不联网
                &|pkg| (pkg == "qt6-base").then(|| "6.12.1".to_string()),
            )
            .unwrap();
        assert_eq!(r.version, "6.12.1");
        assert_eq!(
            r.sources,
            vec!["https://download.qt.io/official_releases/qt/6.12/6.12.1/submodules/qtdeclarative-everywhere-src-6.12.1.tar.xz"]
        );
    }

    #[test]
    fn same_version_missing_lookup_errors() {
        let cfg = TrackerConfig {
            pkg_name: "SPIRV-Headers".into(),
            tracker_template: "github".into(),
            same_version: Some("nonexistent".into()),
            template: Some("https://x/{tag}".into()),
            ..Default::default()
        };
        assert!(cfg
            .probe_with(&crate::net::RealFetcher::default(), &|_| None)
            .is_err());
    }

    #[test]
    fn major_of_filters_template_by_major() {
        // SPIRV-LLVM-Translator major-of llvm → 只匹配主版本 22 的 tag（21/23 排除）
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://api.github.com/repos/KhronosGroup/SPIRV-LLVM-Translator/tags",
            r#"[{"name":"v21.1.0"},{"name":"v22.1.2"},{"name":"v22.0.0"},{"name":"v23.0.0"}]"#,
        );
        let cfg = TrackerConfig {
            pkg_name: "SPIRV-LLVM-Translator".into(),
            tracker_template: "github".into(),
            repo: Some("KhronosGroup/SPIRV-LLVM-Translator".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            template: Some(
                "https://github.com/KhronosGroup/SPIRV-LLVM-Translator/archive/refs/tags/{tag}.tar.gz"
                    .into(),
            ),
            major_of: Some("llvm".into()),
            ..Default::default()
        };
        let r = cfg
            .probe_with(&f, &|pkg| (pkg == "llvm").then(|| "22.1.7".to_string()))
            .unwrap();
        assert_eq!(r.version, "22.1.2");
        assert_eq!(
            r.sources,
            vec!["https://github.com/KhronosGroup/SPIRV-LLVM-Translator/archive/refs/tags/v22.1.2.tar.gz"]
        );
    }

    #[test]
    fn order_entries_after_sorts_after_prereq() {
        let mut trackers = HashMap::new();
        // llvm / vulkan-headers：普通包（无 order；后者是 vulkan-sdk 版本源头）
        for n in ["llvm", "vulkan-headers"] {
            trackers.insert(
                n.to_string(),
                TrackerConfig {
                    pkg_name: n.to_string(),
                    tracker_template: "github".into(),
                    ..Default::default()
                },
            );
        }
        // SPIRV-Headers：after(vulkan-headers) + same-version
        trackers.insert(
            "SPIRV-Headers".into(),
            TrackerConfig {
                pkg_name: "SPIRV-Headers".into(),
                tracker_template: "github".into(),
                order: Some("after(vulkan-headers)".into()),
                same_version: Some("vulkan-headers".into()),
                ..Default::default()
            },
        );
        // SPIRV-LLVM-Translator：after(llvm) + major-of llvm（不必最后，只在其前置之后）
        trackers.insert(
            "SPIRV-LLVM-Translator".into(),
            TrackerConfig {
                pkg_name: "SPIRV-LLVM-Translator".into(),
                tracker_template: "github".into(),
                order: Some("after(llvm)".into()),
                major_of: Some("llvm".into()),
                ..Default::default()
            },
        );

        let names = vec![
            "SPIRV-LLVM-Translator".to_string(),
            "llvm".to_string(),
            "vulkan-headers".to_string(),
            "SPIRV-Headers".to_string(),
        ];
        let ordered = order_entries(names, &trackers);
        let pos = |p: &str| ordered.iter().position(|n| n == p).unwrap();
        assert!(
            pos("llvm") < pos("SPIRV-LLVM-Translator"),
            "ordered: {ordered:?}"
        );
        assert!(
            pos("vulkan-headers") < pos("SPIRV-Headers"),
            "ordered: {ordered:?}"
        );
    }

    #[test]
    fn order_entries_last_goes_after_all_normal() {
        let mut trackers = HashMap::new();
        for n in ["aa", "bb", "zz"] {
            trackers.insert(
                n.to_string(),
                TrackerConfig {
                    pkg_name: n.to_string(),
                    tracker_template: "github".into(),
                    ..Default::default()
                },
            );
        }
        trackers.insert(
            "lastpkg".into(),
            TrackerConfig {
                pkg_name: "lastpkg".into(),
                tracker_template: "github".into(),
                order: Some("last".into()),
                ..Default::default()
            },
        );
        let names = vec!["zz".into(), "aa".into(), "lastpkg".into(), "bb".into()];
        let ordered = order_entries(names, &trackers);
        assert_eq!(&ordered[..3], &["aa", "bb", "zz"]);
        assert_eq!(ordered[3], "lastpkg");
    }

    #[test]
    fn tracker_yaml_fields_and_ordering() {
        // 用内嵌 fixture 校验字段解析 + order_entries 排序。
        // 不读 data/trackers 真实文件——那是项目数据，改名/增删会导致测试假失败。
        let read = |yaml: &str| serde_yaml::from_str::<TrackerConfig>(yaml).unwrap();

        // 字段解析：order / same-version / major-of / 模板字段
        let vh = read("pkg-name: vulkan-headers\ntracker-template: github\n");
        assert_eq!(vh.order, None); // 版本源头，无需前置
        assert_eq!(vh.same_version, None);
        let sh = read(
            "pkg-name: spirv-headers\ntracker-template: github\n\
             order: after(vulkan-headers)\nsame-version: vulkan-headers\n",
        );
        assert_eq!(sh.order.as_deref(), Some("after(vulkan-headers)"));
        assert_eq!(sh.same_version.as_deref(), Some("vulkan-headers"));
        let sllvm = read(
            "pkg-name: spirv-llvm-translator\ntracker-template: github\n\
             order: after(llvm)\nmajor-of: llvm\n",
        );
        assert_eq!(sllvm.order.as_deref(), Some("after(llvm)"));
        assert_eq!(sllvm.major_of.as_deref(), Some("llvm"));
        let libpl = read(
            "pkg-name: libplacebo\ntracker-template: github\nrepo: haasn/libplacebo\n\
             mode: tags\ntag-prefix: v\ntemplate: git+https://github.com/{repo}@{tag}\n",
        );
        assert_eq!(libpl.repo.as_deref(), Some("haasn/libplacebo"));
        assert_eq!(libpl.mode.as_deref(), Some("tags"));
        assert_eq!(libpl.tag_prefix.as_deref(), Some("v"));
        assert!(libpl.sources.is_empty());
        let qtbase = read("pkg-name: qt6-base\ntracker-template: github\n");
        assert_eq!(qtbase.order, None);
        assert_eq!(qtbase.same_version, None);

        // order_entries 端到端：fixture 集上的相对顺序必须正确
        let mut trackers = HashMap::new();
        for (name, y) in [
            ("vulkan-headers", "pkg-name: vulkan-headers\ntracker-template: github\n"),
            ("spirv-headers",
                "pkg-name: spirv-headers\ntracker-template: github\norder: after(vulkan-headers)\n"),
            ("spirv-llvm-translator",
                "pkg-name: spirv-llvm-translator\ntracker-template: github\norder: after(llvm)\n"),
            ("llvm", "pkg-name: llvm\ntracker-template: github\n"),
            ("vulkan-loader", "pkg-name: vulkan-loader\ntracker-template: github\n"),
            ("qt6-base", "pkg-name: qt6-base\ntracker-template: github\n"),
            ("qt6-declarative",
                "pkg-name: qt6-declarative\ntracker-template: github\norder: after(qt6-base)\n"),
            ("qt6-imageformats",
                "pkg-name: qt6-imageformats\ntracker-template: github\norder: after(qt6-base)\n"),
            ("qt6-svg",
                "pkg-name: qt6-svg\ntracker-template: github\norder: after(qt6-base)\n"),
            ("qt5compat",
                "pkg-name: qt5compat\ntracker-template: github\norder: after(qt6-base)\n"),
            ("lastpkg", "pkg-name: lastpkg\ntracker-template: github\norder: last\n"),
        ] {
            let c = read(y);
            trackers.insert(name.to_string(), c);
        }
        let names: Vec<String> = trackers.keys().cloned().collect();
        let ordered = order_entries(names, &trackers);
        let pos = |p: &str| ordered.iter().position(|n| n == p).unwrap();
        assert!(pos("llvm") < pos("spirv-llvm-translator"), "ordered: {ordered:?}");
        assert!(pos("vulkan-headers") < pos("spirv-headers"), "ordered: {ordered:?}");
        assert!(pos("vulkan-headers") < pos("vulkan-loader"), "ordered: {ordered:?}");
        for m in ["qt6-declarative", "qt6-imageformats", "qt6-svg", "qt5compat"] {
            assert!(pos("qt6-base") < pos(m), "qt6-base 应排在 {m} 之前，ordered: {ordered:?}");
        }
        assert_eq!(ordered.last().map(String::as_str), Some("lastpkg"));
    }
}
