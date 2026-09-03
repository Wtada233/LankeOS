//! track 系统（§9）：追踪上游最新版本，产出更新提案。
//!
//! 每个包在 `data/trackers/<pkg>.yaml` 维护一个 **tracker 配置**，它是 sources / work_sources 的
//! **完整清单**：`type: template`（默认）时，`sources:` / `work_sources:` 列表逐条声明式探测，
//! 每个条目产出该槽位的一个下载 URL；`version-source` 指定哪条提供包版本。`type: script` 时
//! 整包走内嵌 bash（stdout 第一行=版本，后续行=URL，`# work_sources` 标记行之后归 work_sources），
//! 是模板覆盖不了的逃生舱。
//!
//! ```yaml
//! pkg-name: glibc
//! version-source: sources[0]      # 默认 sources[0]（空则 work_sources[0]）
//! after: foo                      # 包级前置：foo 先探测，本包才能读到其新版本/主版本
//! sources:
//!   - tracker-template: html-index
//!     url: https://ftp.gnu.org/gnu/glibc/
//!     pattern: 'glibc-(\d[\d.]*)\.tar\.xz'
//!     template: https://ftp.gnu.org/gnu/glibc/{name}-{version}.tar.xz
//! work_sources:
//!   - tracker-template: html-index
//!     url: https://www.iana.org/time-zones/repository/releases/
//!     pattern: 'tzdata(\d{4}[a-z])\.tar\.gz'
//!     template: https://www.iana.org/time-zones/repository/releases/tzdata{version}.tar.gz
//! ```
//!
//! **探测成功且版本变新时，LankeBUILD.json 的 sources / work_sources 被原子全量替换**为探测出的
//! 清单（旧值丢弃，空列表也写键，lpkg 默认形态）。任一条目探测失败 → 整包不更新（半截清单比不写更糟）。
//!
//! **主动性**：条目用 `tracker-template` 指定模板，模板不主动从 URI 猜格式（yaml 由人工/AI 编写，
//! 模板只是探测执行器）。

pub mod templates;
pub mod vercmp;

use serde::{Deserialize, Serialize};

use crate::net::Fetcher;

/// 单条 source 的探测结果：检测到的版本 + 该槽位的下载 URL。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EntryProbe {
    pub version: String,
    pub url: String,
}

/// 包级探测结果（= 完整清单）：版本 + 全部 sources / work_sources URL。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProbeResult {
    pub version: String,
    pub sources: Vec<String>,
    pub work_sources: Vec<String>,
}

/// 更新提案：`pkg` 当前版本 → 上游最新版本 + 完整源清单。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Proposal {
    pub pkg_name: String,
    pub current_version: String,
    pub new_version: String,
    pub sources: Vec<String>,
    pub work_sources: Vec<String>,
    pub kind: String, // template | script（显示用）
}

/// tracker yaml 配置（包级）。`deny_unknown_fields`：typo 字段名解析即报错。
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TrackerConfig {
    #[serde(default, rename = "pkg-name")]
    pub pkg_name: String,
    /// 版本来源选择器：`sources[i]` / `work_sources[i]`。缺省 = `sources[0]`（空则 `work_sources[0]`）。
    #[serde(rename = "version-source", skip_serializing_if = "Option::is_none")]
    pub version_source: Option<String>,
    /// 包级前置：指定包先探测（其新版本/主版本是本包探测输入）。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub after: Option<String>,
    /// 最后处理：所有非 last 包都先于它（等价于声明一堆 after 边）。
    #[serde(default, skip_serializing_if = "is_false")]
    pub last: bool,
    /// 探测类型：`template`（默认，逐条目声明式）| `script`（包级逃生舱）。
    #[serde(rename = "type", default, skip_serializing_if = "is_template")]
    pub type_: String,
    /// type: script 时的内嵌 bash（stdout 第一行=版本，后续行=URL，`# work_sources` 后归 work_sources）。
    #[serde(rename = "script-content", skip_serializing_if = "Option::is_none")]
    pub script_content: Option<String>,
    /// sources 各槽位的追踪配置（位置对应 LankeBUILD.json 的 sources 数组）。
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub sources: Vec<SourceConfig>,
    /// work_sources 各槽位的追踪配置（位置对应 LankeBUILD.json 的 work_sources 数组）。
    #[serde(
        rename = "work_sources",
        default,
        skip_serializing_if = "Vec::is_empty"
    )]
    pub work_sources: Vec<SourceConfig>,
}

/// source 条目配置（type: template 的每个槽位）。版本约束只作用于本条目。
/// `deny_unknown_fields`：typo 字段名（如 `tag-prefx`）解析即报错，而非静默忽略。
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SourceConfig {
    #[serde(rename = "tracker-template")]
    pub tracker_template: String,

    // ── github / gitlab ──
    #[serde(skip_serializing_if = "Option::is_none")]
    pub repo: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub host: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mode: Option<String>, // tags | releases（缺省 tags）
    #[serde(rename = "tag-prefix", skip_serializing_if = "Option::is_none")]
    pub tag_prefix: Option<String>,

    // ── html-index / gcs / sourceforge ──
    #[serde(skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub pattern: Option<String>,

    // ── multi-level-html-index（N 级：每级一个 {url, pattern}）──
    /// N 级探测列表：`levels[i]` = 第 i+1 级页面 + 版本正则。
    /// `levels[i].url` 可引用已解出的前级版本 `{v1}..{vi}`；`levels[i].pattern` 提取该级版本。
    /// 最后一个 pattern 的捕获 = 最终版本（`{version}`），`template` 用 `{v1}..{vN}`/`{version}`/`{name}`。
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub levels: Vec<LevelConfig>,

    // ── sourceforge ──
    #[serde(skip_serializing_if = "Option::is_none")]
    pub project: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub path: Option<String>,

    // ── 下载 URL 模板（{name}/{version}/{tag}…占位符）──
    #[serde(skip_serializing_if = "Option::is_none")]
    pub template: Option<String>,

    // ── 版本约束（只作用于本条目）──
    /// same-version 模板专用：锁定为指定包的版本（直接确定版本号，不经上游探测）。
    #[serde(rename = "same-version-of", skip_serializing_if = "Option::is_none")]
    pub same_version_of: Option<String>,
    #[serde(rename = "major-of", skip_serializing_if = "Option::is_none")]
    pub major_of: Option<String>, // 匹配指定包主版本的 tag/目录
    #[serde(rename = "major-version-lock", skip_serializing_if = "Option::is_none")]
    pub major_version_lock: Option<String>, // 锁定探测主版本常量（gtk3 锁 3）
    #[serde(rename = "max-version", skip_serializing_if = "Option::is_none")]
    pub max_version: Option<String>, // 版本封顶（gtk3 稳定系列止于 3.24）
    #[serde(rename = "stable-minor", skip_serializing_if = "Option::is_none")]
    pub stable_minor: Option<String>, // gnome 模板：even(默认) 偶 minor 稳定分支；all 取最大版本
    #[serde(rename = "source-name", skip_serializing_if = "Option::is_none")]
    pub source_name: Option<String>, // 上游源目录/文件名覆盖（gtk3 的上游目录叫 gtk）
}

/// multi-level-html-index 的一级：页面 URL + 版本正则（同一条目配对，无并行数组错位）。
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LevelConfig {
    /// 该级页面 URL。可引用已解出的前级版本 `{v1}..{vN}`。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    /// 该级版本正则（含一个捕获组），提取该级版本。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub pattern: Option<String>,
}

fn is_template(t: &str) -> bool {
    t.is_empty() || t == "template"
}

fn is_false(b: &bool) -> bool {
    !*b
}

impl TrackerConfig {
    /// 序列化为 tracker yaml（提案文件内容）。
    pub fn to_yaml(&self) -> Result<String, String> {
        serde_yaml::to_string(self).map_err(|e| format!("序列化 tracker yaml 失败: {e}"))
    }

    /// 探测类型名（显示用）：script / template。
    pub fn kind(&self) -> &str {
        if is_template(&self.type_) {
            "template"
        } else {
            "script"
        }
    }

    /// 包级探测：type=script 走脚本；否则逐条目声明式探测，按 version-source 取版本。
    /// **任一条目失败 → 整包失败**（原子性：只在全清单可产出时才应用）。
    pub fn probe_with(
        &self,
        fetcher: &dyn Fetcher,
        lookup: &dyn Fn(&str) -> Option<String>,
    ) -> Result<ProbeResult, String> {
        if !is_template(&self.type_) {
            let content = need(&self.script_content, "script-content")?;
            return templates::script::probe(fetcher, content, &self.pkg_name);
        }
        let srcs = probe_entry_list(&self.sources, fetcher, lookup, "sources", &self.pkg_name)?;
        let wss = probe_entry_list(
            &self.work_sources,
            fetcher,
            lookup,
            "work_sources",
            &self.pkg_name,
        )?;
        // version-source 选择器：显式或默认（sources[0] 优先，空则 work_sources[0]）
        let (is_work, idx) = match &self.version_source {
            Some(sel) => parse_version_source(sel)?,
            None => {
                if !self.sources.is_empty() {
                    (false, 0)
                } else if !self.work_sources.is_empty() {
                    (true, 0)
                } else {
                    return Err(format!(
                        "tracker {} 无 sources/work_sources 条目",
                        self.pkg_name
                    ));
                }
            }
        };
        let pool = if is_work { &wss } else { &srcs };
        let version = pool.get(idx).map(|e| e.version.clone()).ok_or_else(|| {
            format!(
                "version-source 越界: {}[{}]（共 {} 条）",
                if is_work { "work_sources" } else { "sources" },
                idx,
                pool.len()
            )
        })?;
        Ok(ProbeResult {
            version,
            sources: srcs.into_iter().map(|e| e.url).collect(),
            work_sources: wss.into_iter().map(|e| e.url).collect(),
        })
    }

    /// 无约束探测（lookup 返回 None；same-version / major-of 会报错）。
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
            kind: self.kind().to_string(),
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

/// 逐条目探测一个列表（sources 或 work_sources），任一条失败即整列表失败（带槽位上下文）。
fn probe_entry_list(
    list: &[SourceConfig],
    fetcher: &dyn Fetcher,
    lookup: &dyn Fn(&str) -> Option<String>,
    field: &str,
    pkg_name: &str,
) -> Result<Vec<EntryProbe>, String> {
    let mut out = Vec::with_capacity(list.len());
    for (i, cfg) in list.iter().enumerate() {
        out.push(
            cfg.probe_with(fetcher, lookup, pkg_name)
                .map_err(|e| format!("{field}[{i}] 探测失败: {e}"))?,
        );
    }
    Ok(out)
}

impl SourceConfig {
    /// 上游名字：`source-name` 覆盖（gtk3 → 上游目录叫 gtk），否则用包名。
    pub fn effective_name<'a>(&'a self, pkg_name: &'a str) -> &'a str {
        self.source_name.as_deref().unwrap_or(pkg_name)
    }

    /// 探测本条目：返回检测版本 + 该槽位下载 URL。
    pub fn probe_with(
        &self,
        fetcher: &dyn Fetcher,
        lookup: &dyn Fn(&str) -> Option<String>,
        pkg_name: &str,
    ) -> Result<EntryProbe, String> {
        // 显式字段校验：声明的 tracker-template 只支持特定字段，设置不支持的 → 报错
        validate_supported_fields(self)?;
        // 主版本约束：major-version-lock（常量）优先，否则 major-of（取指定包主版本）
        let major = if let Some(lock) = &self.major_version_lock {
            Some(lock.clone())
        } else {
            match &self.major_of {
                Some(p) => {
                    let v = lookup(p).ok_or_else(|| {
                        format!("major-of 依赖 {p} 无版本（读 LankeBUILD.json 失败）")
                    })?;
                    Some(v.split('.').next().unwrap_or("").to_string())
                }
                None => None,
            }
        };
        let probe = match self.tracker_template.as_str() {
            // same-version：直接锁定另一包版本（不经网络探测），需要 lookup 解析
            "same-version" => templates::same_version::probe(self, lookup, pkg_name),
            "github" => templates::github::probe(fetcher, self, major.as_deref(), pkg_name),
            "gitlab" => templates::gitlab::probe(fetcher, self, major.as_deref(), pkg_name),
            "sourceforge" => {
                templates::sourceforge::probe(fetcher, self, major.as_deref(), pkg_name)
            }
            "gnome" => templates::gnome::probe(fetcher, self, major.as_deref(), pkg_name),
            "gcs" => templates::gcs::probe(fetcher, self, major.as_deref(), pkg_name),
            "html-index" => templates::html_index::probe(fetcher, self, major.as_deref(), pkg_name),
            "multi-level-html-index" => {
                templates::multi_level_html_index::probe(fetcher, self, major.as_deref(), pkg_name)
            }
            "pypi" => templates::pypi::probe(fetcher, self, major.as_deref(), pkg_name),
            other => Err(format!("未知 tracker_template: {other}")),
        }?;
        validate_url(&probe.url)?;
        Ok(probe)
    }
}

/// 显式字段校验：声明的 `tracker-template` 只支持特定字段，设置了不支持的 → 报错。
/// 把"字段声明集中在 SourceConfig、但模板是否读它全隐式"的静默忽略变成显式错误
/// （如 github 上写 max-version → 报错提示改用支持它的模板或 script 类型）。
/// `major-of` / `major-version-lock` 是探测模板的核心约束（same-version 模板直接锁版本，无过滤）。
fn validate_supported_fields(cfg: &SourceConfig) -> Result<(), String> {
    const CORE: &[&str] = &["major-of", "major-version-lock"];
    let (template, mut supported): (&str, Vec<&str>) = match cfg.tracker_template.as_str() {
        // same-version：直接锁版本，只认 same-version-of + template，占位符仅 {version}/{major_minor}
        // （URL 全写在 template：tag 前缀/仓库路径/上游名都烘进去，不支持 tag-prefix/repo/source-name）
        "same-version" => ("same-version", vec!["same-version-of", "template"]),
        "github" => (
            "github",
            vec!["repo", "mode", "tag-prefix", "template", "max-version"],
        ),
        "gitlab" => (
            "gitlab",
            vec![
                "host",
                "project",
                "mode",
                "tag-prefix",
                "template",
                "max-version",
            ],
        ),
        "html-index" => (
            "html-index",
            vec!["url", "pattern", "template", "max-version", "source-name"],
        ),
        "multi-level-html-index" => (
            "multi-level-html-index",
            vec!["levels", "template", "max-version", "source-name"],
        ),
        "gcs" => (
            "gcs",
            vec!["url", "pattern", "template", "max-version", "source-name"],
        ),
        "gnome" => (
            "gnome",
            vec!["template", "max-version", "stable-minor", "source-name"],
        ),
        "sourceforge" => (
            "sourceforge",
            vec![
                "project",
                "path",
                "pattern",
                "template",
                "max-version",
                "source-name",
            ],
        ),
        "pypi" => ("pypi", vec!["project"]), // URL 来自 PyPI API，不用 template
        other => return Err(format!("未知 tracker_template: {other}")),
    };
    // 探测模板才有版本过滤约束；same-version 直接锁定版本，不参与 major 过滤
    if template != "same-version" {
        supported.extend_from_slice(CORE);
    }
    let set = [
        ("repo", cfg.repo.is_some()),
        ("host", cfg.host.is_some()),
        ("mode", cfg.mode.is_some()),
        ("tag-prefix", cfg.tag_prefix.is_some()),
        ("url", cfg.url.is_some()),
        ("pattern", cfg.pattern.is_some()),
        ("levels", !cfg.levels.is_empty()),
        ("project", cfg.project.is_some()),
        ("path", cfg.path.is_some()),
        ("template", cfg.template.is_some()),
        ("same-version-of", cfg.same_version_of.is_some()),
        ("major-of", cfg.major_of.is_some()),
        ("major-version-lock", cfg.major_version_lock.is_some()),
        ("max-version", cfg.max_version.is_some()),
        ("stable-minor", cfg.stable_minor.is_some()),
        ("source-name", cfg.source_name.is_some()),
    ];
    let unsupported: Vec<&str> = set
        .iter()
        .filter(|(name, is_set)| *is_set && !supported.contains(name))
        .map(|(name, _)| *name)
        .collect();
    if !unsupported.is_empty() {
        return Err(format!(
            "tracker-template {template} 不支持字段: {}（需要版本封顶/稳定分支等约束时改用支持它的模板，或用 script 类型）",
            unsupported.join(", ")
        ));
    }
    Ok(())
}

/// 解析 `version-source` 选择器：`sources[i]` / `work_sources[i]` → (is_work, index)。
pub fn parse_version_source(sel: &str) -> Result<(bool, usize), String> {
    let err = || format!("version-source 无效 '{sel}'（应为 sources[i] 或 work_sources[i]）");
    let (is_work, rest) = if let Some(r) = sel.strip_prefix("sources[") {
        (false, r)
    } else if let Some(r) = sel.strip_prefix("work_sources[") {
        (true, r)
    } else {
        return Err(err());
    };
    let idx = rest
        .strip_suffix(']')
        .and_then(|s| s.parse::<usize>().ok())
        .ok_or_else(err)?;
    Ok((is_work, idx))
}

/// 需要的必填字段缺失时给出清晰错误。
pub(crate) fn need<'a>(opt: &'a Option<String>, field: &str) -> Result<&'a str, String> {
    opt.as_deref()
        .ok_or_else(|| format!("tracker 配置缺 {field}"))
}

/// 校验探测产出的 URL：残留 `{...}` 说明模板引用了未提供的占位符，生成的 URL 必然损坏。
/// 报错而非静默写入坏 URL（杜绝"莫名其妙改 URL"）。
pub(crate) fn validate_url(url: &str) -> Result<(), String> {
    if url.contains('{') {
        return Err(format!("探测生成的 URL 残留未替换占位符: {url}"));
    }
    Ok(())
}

/// 依赖边（prereq -> package，prereq 先处理）。来源：
/// - 条目级 `same-version: X` / `major-of: X`：隐式边 X → 本包（版本/主版本输入必须先就绪）；
/// - `after: X`：显式边 X → 本包；
/// - `last`：所有非 last 包都是它的前置（等价于声明一堆 after 边）。
///
/// 只保留两端都在 `names` 里的边（引用不存在的包 → 落回 LankeBUILD.json 查询，不影响顺序）。
/// 串行 `order_entries` 与并行 `-j` 调度共用这一套边，保证 after/last 顺序不受并行破坏。
pub fn dep_edges(
    names: &[String],
    trackers: &std::collections::HashMap<String, TrackerConfig>,
) -> Vec<(String, String)> {
    let is_last = |n: &str| trackers.get(n).is_some_and(|c| c.last);
    let mut edges: Vec<(String, String)> = Vec::new();
    for name in names {
        let Some(cfg) = trackers.get(name) else {
            continue;
        };
        // 条目级 same-version / major-of：隐式边（前置版本/主版本必须先就绪）
        for e in cfg.sources.iter().chain(&cfg.work_sources) {
            if let Some(p) = &e.same_version_of {
                edges.push((p.clone(), name.clone()));
            }
            if let Some(p) = &e.major_of {
                edges.push((p.clone(), name.clone()));
            }
        }
        if let Some(x) = &cfg.after {
            edges.push((x.clone(), name.clone()));
        }
        if cfg.last {
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
    fn tracker_yaml_roundtrip() {
        let yaml = r#"
pkg-name: glibc
version-source: sources[0]
after: tzdata
sources:
  - tracker-template: html-index
    url: https://ftp.gnu.org/gnu/glibc/
    pattern: 'glibc-(\d[\d.]*)\.tar\.xz'
    template: https://ftp.gnu.org/gnu/glibc/{name}-{version}.tar.xz
work_sources:
  - tracker-template: html-index
    url: https://www.iana.org/time-zones/repository/releases/
    pattern: 'tzdata(\d{4}[a-z])\.tar\.gz'
    template: https://www.iana.org/time-zones/repository/releases/tzdata{version}.tar.gz
"#;
        let cfg: TrackerConfig = serde_yaml::from_str(yaml).unwrap();
        assert_eq!(cfg.pkg_name, "glibc");
        assert_eq!(cfg.version_source.as_deref(), Some("sources[0]"));
        assert_eq!(cfg.after.as_deref(), Some("tzdata"));
        assert_eq!(cfg.sources.len(), 1);
        assert_eq!(cfg.sources[0].tracker_template, "html-index");
        assert_eq!(
            cfg.sources[0].url.as_deref(),
            Some("https://ftp.gnu.org/gnu/glibc/")
        );
        assert_eq!(cfg.work_sources.len(), 1);
        assert_eq!(
            cfg.work_sources[0].template.as_deref(),
            Some("https://www.iana.org/time-zones/repository/releases/tzdata{version}.tar.gz")
        );
    }

    #[test]
    fn script_type_yaml_roundtrip() {
        let yaml = r#"
pkg-name: rhino
type: script
after: base
script-content: |
  #!/bin/bash
  echo "1.7.15"
  echo "https://github.com/mozilla/rhino/releases/download/rhino1.7.15/rhino-1.7.15.zip"
"#;
        let cfg: TrackerConfig = serde_yaml::from_str(yaml).unwrap();
        assert_eq!(cfg.kind(), "script");
        assert_eq!(cfg.after.as_deref(), Some("base"));
        let content = cfg.script_content.unwrap();
        assert!(content.contains("echo \"1.7.15\""));
    }

    #[test]
    fn yaml_serialization_skips_defaults() {
        let cfg = TrackerConfig {
            pkg_name: "bash".into(),
            type_: "template".into(),
            sources: vec![SourceConfig {
                tracker_template: "html-index".into(),
                url: Some("https://ftp.gnu.org/gnu/bash/".into()),
                pattern: Some(r"bash[-_]?(\d[\d.]*)\.tar\.(?:xz|gz|bz2)".into()),
                template: Some("https://ftp.gnu.org/gnu/bash/{name}-{version}.tar.gz".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
        let yaml = cfg.to_yaml().unwrap();
        assert!(yaml.contains("pkg-name: bash"));
        assert!(yaml.contains("tracker-template: html-index"));
        assert!(!yaml.contains("repo:")); // 默认字段不序列化
        assert!(!yaml.contains("type:")); // template 是默认 type，不写
        assert!(!yaml.contains("after:"));
        assert!(!yaml.contains("version-source:"));
    }

    #[test]
    fn parse_version_source_selectors() {
        assert_eq!(parse_version_source("sources[0]").unwrap(), (false, 0));
        assert_eq!(parse_version_source("sources[3]").unwrap(), (false, 3));
        assert_eq!(parse_version_source("work_sources[0]").unwrap(), (true, 0));
        assert!(parse_version_source("sources[]").is_err());
        assert!(parse_version_source("sources[abc]").is_err());
        assert!(parse_version_source("source[0]").is_err());
        assert!(parse_version_source("0").is_err());
    }

    #[test]
    fn package_probe_multi_source_version_source() {
        // 版本由 work_sources[0] 提供，sources 两条各自探测出 URL
        let f = MockFetcher::new(HashMap::new())
            .entry(
                "https://api.github.com/repos/a/main/tags",
                r#"[{"name":"v2.0"},{"name":"v1.0"}]"#,
            )
            .entry(
                "https://api.github.com/repos/b/vendored/tags",
                r#"[{"name":"v9.0"}]"#,
            )
            .entry(
                "https://api.github.com/repos/c/ver/tags",
                r#"[{"name":"v3.1"},{"name":"v3.0"}]"#,
            );
        let cfg = TrackerConfig {
            pkg_name: "pkg".into(),
            version_source: Some("work_sources[0]".into()),
            sources: vec![
                SourceConfig {
                    tracker_template: "github".into(),
                    repo: Some("a/main".into()),
                    mode: Some("tags".into()),
                    tag_prefix: Some("v".into()),
                    template: Some(
                        "https://github.com/a/main/archive/refs/tags/{tag}.tar.gz".into(),
                    ),
                    ..Default::default()
                },
                SourceConfig {
                    tracker_template: "github".into(),
                    repo: Some("b/vendored".into()),
                    mode: Some("tags".into()),
                    tag_prefix: Some("v".into()),
                    template: Some(
                        "https://github.com/b/vendored/archive/refs/tags/{tag}.tar.gz".into(),
                    ),
                    ..Default::default()
                },
            ],
            work_sources: vec![SourceConfig {
                tracker_template: "github".into(),
                repo: Some("c/ver".into()),
                mode: Some("tags".into()),
                tag_prefix: Some("v".into()),
                template: Some("https://github.com/c/ver/archive/refs/tags/{tag}.tar.gz".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
        let r = cfg.probe(&f).unwrap();
        assert_eq!(r.version, "3.1"); // 版本来自 work_sources[0]
        assert_eq!(
            r.sources,
            vec![
                "https://github.com/a/main/archive/refs/tags/v2.0.tar.gz",
                "https://github.com/b/vendored/archive/refs/tags/v9.0.tar.gz"
            ]
        );
        assert_eq!(
            r.work_sources,
            vec!["https://github.com/c/ver/archive/refs/tags/v3.1.tar.gz"]
        );
    }

    #[test]
    fn package_probe_defaults_version_to_sources0() {
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://api.github.com/repos/a/main/tags",
            r#"[{"name":"v2.0"}]"#,
        );
        let cfg = TrackerConfig {
            pkg_name: "pkg".into(),
            sources: vec![SourceConfig {
                tracker_template: "github".into(),
                repo: Some("a/main".into()),
                mode: Some("tags".into()),
                tag_prefix: Some("v".into()),
                template: Some("https://github.com/a/main/archive/refs/tags/{tag}.tar.gz".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
        let r = cfg.probe(&f).unwrap();
        assert_eq!(r.version, "2.0");
        assert_eq!(
            r.sources,
            vec!["https://github.com/a/main/archive/refs/tags/v2.0.tar.gz"]
        );
        assert!(r.work_sources.is_empty());
    }

    #[test]
    fn package_probe_atomic_fails_on_entry_error() {
        // 任一条目探测失败 → 整包失败（原子性，不产出半截清单）
        let f = MockFetcher::new(HashMap::new()); // 无任何响应 → github tags 抓取失败
        let cfg = TrackerConfig {
            pkg_name: "pkg".into(),
            sources: vec![
                SourceConfig {
                    tracker_template: "github".into(),
                    repo: Some("a/main".into()),
                    tag_prefix: Some("v".into()),
                    template: Some("https://x/{tag}".into()),
                    ..Default::default()
                },
                SourceConfig {
                    tracker_template: "github".into(),
                    repo: Some("b/broken".into()),
                    tag_prefix: Some("v".into()),
                    template: Some("https://x/{tag}".into()),
                    ..Default::default()
                },
            ],
            ..Default::default()
        };
        let err = cfg.probe(&f).unwrap_err();
        assert!(err.contains("sources[0] 探测失败"), "err: {err}");
    }

    #[test]
    fn version_source_out_of_range_errors() {
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://api.github.com/repos/a/main/tags",
            r#"[{"name":"v2.0"}]"#,
        );
        let cfg = TrackerConfig {
            pkg_name: "pkg".into(),
            version_source: Some("sources[5]".into()),
            sources: vec![SourceConfig {
                tracker_template: "github".into(),
                repo: Some("a/main".into()),
                mode: Some("tags".into()),
                tag_prefix: Some("v".into()),
                template: Some("https://x/{tag}".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
        let err = cfg.probe(&f).unwrap_err();
        assert!(err.contains("越界"), "err: {err}");
    }

    #[test]
    fn entry_same_version_locks_version_and_builds_url() {
        // 条目级 same-version：锁定另一包版本，URL 全写在 template（tag 前缀/仓库路径烘进），无网络
        let cfg = TrackerConfig {
            pkg_name: "SPIRV-Headers".into(),
            sources: vec![SourceConfig {
                tracker_template: "same-version".into(),
                template: Some(
                    "https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-{version}.tar.gz"
                        .into(),
                ),
                same_version_of: Some("vulkan-headers".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
        let r = cfg
            .probe_with(
                &crate::net::RealFetcher::default(), // same-version 模板不联网
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
    fn entry_same_version_major_minor_for_dir_paths() {
        // qt6 风格：{major_minor}/{version} 拼目录（qt/<6.11>/<6.11.1>/）
        let cfg = TrackerConfig {
            pkg_name: "qt6-declarative".into(),
            sources: vec![SourceConfig {
                tracker_template: "same-version".into(),
                same_version_of: Some("qt6-base".into()),
                template: Some(
                    "https://download.qt.io/official_releases/qt/{major_minor}/{version}/submodules/qtdeclarative-everywhere-src-{version}.tar.xz"
                        .into(),
                ),
                ..Default::default()
            }],
            ..Default::default()
        };
        let r = cfg
            .probe_with(
                &crate::net::RealFetcher::default(), // same-version 模板不联网
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
    fn entry_same_version_missing_lookup_errors() {
        let cfg = TrackerConfig {
            pkg_name: "SPIRV-Headers".into(),
            sources: vec![SourceConfig {
                tracker_template: "same-version".into(),
                same_version_of: Some("nonexistent".into()),
                template: Some("https://x/{tag}".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
        let err = cfg
            .probe_with(&crate::net::RealFetcher::default(), &|_| None)
            .unwrap_err();
        assert!(err.contains("same-version-of"), "err: {err}");
    }

    #[test]
    fn legacy_same_version_key_is_unknown_field() {
        // 旧写法 `same-version:`（无 -of）已是未知字段 → deny_unknown_fields 解析即拒
        let yaml = "tracker-template: github\nrepo: a/b\nsame-version: other\n";
        let err = serde_yaml::from_str::<SourceConfig>(yaml).unwrap_err();
        assert!(err.to_string().contains("same-version"), "err: {err}");
    }

    #[test]
    fn entry_same_version_of_rejected_on_probing_template() {
        // same-version-of 是 same-version 模板专属字段：github 上写它 → 报错
        let cfg = TrackerConfig {
            pkg_name: "x".into(),
            sources: vec![SourceConfig {
                tracker_template: "github".into(),
                repo: Some("a/b".into()),
                mode: Some("tags".into()),
                tag_prefix: Some("v".into()),
                same_version_of: Some("other".into()),
                template: Some("https://x/{tag}".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
        let err = cfg.probe(&MockFetcher::new(HashMap::new())).unwrap_err();
        assert!(err.contains("不支持字段: same-version-of"), "err: {err}");
    }

    #[test]
    fn entry_major_of_filters_by_major() {
        // 条目级 major-of：只匹配指定包主版本的 tag
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://api.github.com/repos/KhronosGroup/SPIRV-LLVM-Translator/tags",
            r#"[{"name":"v21.1.0"},{"name":"v22.1.2"},{"name":"v22.0.0"},{"name":"v23.0.0"}]"#,
        );
        let cfg = TrackerConfig {
            pkg_name: "SPIRV-LLVM-Translator".into(),
            sources: vec![SourceConfig {
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
            }],
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
    fn max_version_cap_honored_by_html_index() {
        // 曾对 html-index/gcs 是死字段：max-version 必须生效（tcl 锁 8.6.x 场景）
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://ftp.gnu.org/gnu/tcl/",
            "tcl8.6.16-src.tar.gz\ntcl9.0.4-src.tar.gz\n",
        );
        let cfg = TrackerConfig {
            pkg_name: "tcl".into(),
            sources: vec![SourceConfig {
                tracker_template: "html-index".into(),
                url: Some("https://ftp.gnu.org/gnu/tcl/".into()),
                pattern: Some(r"tcl([\d.]+)-src\.tar\.gz".into()),
                max_version: Some("8.6.16".into()),
                template: Some("https://ftp.gnu.org/gnu/tcl/tcl{version}-src.tar.gz".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
        let r = cfg.probe(&f).unwrap();
        assert_eq!(r.version, "8.6.16");
    }

    #[test]
    fn template_leftover_placeholder_is_rejected() {
        // 模板引用未提供的占位符 → URL 残留 {unknown} → 探测报错，而非静默生成坏 URL
        let cfg = TrackerConfig {
            pkg_name: "x".into(),
            sources: vec![SourceConfig {
                tracker_template: "github".into(),
                repo: Some("a/b".into()),
                mode: Some("tags".into()),
                tag_prefix: Some("v".into()),
                template: Some("https://example.com/{repo}/{unknown}/{version}.tar.gz".into()),
                ..Default::default()
            }],
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
    fn entry_unsupported_field_is_explicit_error() {
        // github 不支持 host（模板从 repo 拼 api.github.com URL）：设置 → 显式报错而非静默忽略
        let cfg = TrackerConfig {
            pkg_name: "x".into(),
            sources: vec![SourceConfig {
                tracker_template: "github".into(),
                repo: Some("a/b".into()),
                host: Some("github.example".into()),
                mode: Some("tags".into()),
                tag_prefix: Some("v".into()),
                template: Some("https://x/{tag}".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
        let err = cfg.probe(&MockFetcher::new(HashMap::new())).unwrap_err();
        assert!(err.contains("不支持字段: host"), "err: {err}");
        assert!(err.contains("github"), "err: {err}");
    }

    #[test]
    fn github_entry_accepts_max_version_and_caps() {
        // github 模板支持 max-version：tags 列表封顶生效（v261 被过滤取 v256）
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://api.github.com/repos/systemd/systemd/tags",
            r#"[{"name":"v254"},{"name":"v256"},{"name":"v255"},{"name":"v261"}]"#,
        );
        let cfg = TrackerConfig {
            pkg_name: "systemd".into(),
            sources: vec![SourceConfig {
                tracker_template: "github".into(),
                repo: Some("systemd/systemd".into()),
                mode: Some("tags".into()),
                tag_prefix: Some("v".into()),
                max_version: Some("256".into()),
                template: Some(
                    "https://github.com/systemd/systemd/archive/refs/tags/{tag}.tar.gz".into(),
                ),
                ..Default::default()
            }],
            ..Default::default()
        };
        let r = cfg.probe(&f).unwrap();
        assert_eq!(r.version, "256");
        assert_eq!(
            r.sources,
            vec!["https://github.com/systemd/systemd/archive/refs/tags/v256.tar.gz"]
        );
    }

    #[test]
    fn gitlab_entry_accepts_max_version_and_caps() {
        // gitlab 模板支持 max-version：不报"不支持字段"，且封顶生效（v2.0.0 被过滤取 1.5.0）
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://gitlab.com/api/v4/projects/a%2Fb/repository/tags?per_page=50",
            r#"[{"name":"v2.0.0"},{"name":"v1.5.0"},{"name":"v1.2.0"}]"#,
        );
        let cfg = TrackerConfig {
            pkg_name: "x".into(),
            sources: vec![SourceConfig {
                tracker_template: "gitlab".into(),
                host: Some("gitlab.com".into()),
                project: Some("a/b".into()),
                mode: Some("tags".into()),
                tag_prefix: Some("v".into()),
                max_version: Some("1.5.0".into()),
                template: Some("https://gitlab.com/{project}/-/archive/{tag}/x.tar.gz".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
        let r = cfg.probe(&f).unwrap();
        assert_eq!(r.version, "1.5.0");
        assert_eq!(
            r.sources,
            vec!["https://gitlab.com/a/b/-/archive/v1.5.0/x.tar.gz"]
        );
    }

    #[test]
    fn entry_pypi_rejects_template() {
        // pypi 模板不用 template（URL 来自 API），设置 → 报错
        let f = MockFetcher::new(HashMap::new()).entry(
            "https://pypi.org/pypi/setuptools/json",
            r#"{"info":{"version":"1.0"},"urls":[{"packagetype":"sdist","url":"https://x/1.0.tar.gz"}],"releases":{}}"#,
        );
        let cfg = TrackerConfig {
            pkg_name: "x".into(),
            sources: vec![SourceConfig {
                tracker_template: "pypi".into(),
                project: Some("setuptools".into()),
                template: Some("https://x/{version}".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
        let err = cfg.probe(&f).unwrap_err();
        assert!(err.contains("不支持字段: template"), "err: {err}");
    }

    #[test]
    fn entry_unknown_field_in_yaml_is_rejected() {
        // deny_unknown_fields：typo 字段名（tag-prefx）解析即报错，而非静默忽略
        let yaml = "tracker-template: github\nrepo: a/b\ntag-prefx: v\n";
        let err = serde_yaml::from_str::<SourceConfig>(yaml).unwrap_err();
        assert!(err.to_string().contains("tag-prefx"), "err: {err}");
    }

    #[test]
    fn script_type_returns_full_manifest() {
        // type: script：stdout 第一行版本，后续行 sources，`# work_sources` 标记后归 work_sources
        let cfg = TrackerConfig {
            pkg_name: "libreoffice".into(),
            type_: "script".into(),
            script_content: Some(
                "#!/bin/bash\necho \"25.2.0\"\necho \"https://x/lo-25.2.0.tar.xz\"\necho \"# work_sources\"\necho \"https://x/vendor-25.2.0.tar.gz\"\n"
                    .into(),
            ),
            ..Default::default()
        };
        let r = cfg.probe(&crate::net::RealFetcher::default()).unwrap();
        assert_eq!(r.version, "25.2.0");
        assert_eq!(r.sources, vec!["https://x/lo-25.2.0.tar.xz"]);
        assert_eq!(r.work_sources, vec!["https://x/vendor-25.2.0.tar.gz"]);
    }

    #[test]
    fn order_entries_after_and_nested_edges() {
        let mut trackers = HashMap::new();
        for n in ["llvm", "vulkan-headers"] {
            trackers.insert(
                n.to_string(),
                TrackerConfig {
                    pkg_name: n.to_string(),
                    ..Default::default()
                },
            );
        }
        // SPIRV-Headers：after + 条目级 same-version 模板
        trackers.insert(
            "SPIRV-Headers".into(),
            TrackerConfig {
                pkg_name: "SPIRV-Headers".into(),
                after: Some("vulkan-headers".into()),
                sources: vec![SourceConfig {
                    tracker_template: "same-version".into(),
                    same_version_of: Some("vulkan-headers".into()),
                    template: Some("https://x/{tag}".into()),
                    ..Default::default()
                }],
                ..Default::default()
            },
        );
        // SPIRV-LLVM-Translator：after + 条目级 major-of
        trackers.insert(
            "SPIRV-LLVM-Translator".into(),
            TrackerConfig {
                pkg_name: "SPIRV-LLVM-Translator".into(),
                after: Some("llvm".into()),
                sources: vec![SourceConfig {
                    tracker_template: "github".into(),
                    major_of: Some("llvm".into()),
                    template: Some("https://x/{tag}".into()),
                    ..Default::default()
                }],
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
                    ..Default::default()
                },
            );
        }
        trackers.insert(
            "lastpkg".into(),
            TrackerConfig {
                pkg_name: "lastpkg".into(),
                last: true,
                ..Default::default()
            },
        );
        let names = vec!["zz".into(), "aa".into(), "lastpkg".into(), "bb".into()];
        let ordered = order_entries(names, &trackers);
        assert_eq!(&ordered[..3], &["aa", "bb", "zz"]);
        assert_eq!(ordered[3], "lastpkg");
    }
}
