//! track 系统（§9）：追踪上游最新版本，产出更新提案。
//!
//! 每个包在 `data/trackers/<pkg>.yaml` 维护一个 **tracker 配置**，它是 sources / work_sources 的
//! **完整清单**：`sources:` / `work_sources:` 列表逐条探测，每个条目产出**该槽位**的一个下载 URL；
//! `version-source` 指定哪条提供包版本。**没有包级类型**——所有条目一视同仁。
//!
//! 条目用 `tracker-template` 选探测后端（github / gitlab / html-index / … / script）。
//! `script` 是**条目级**逃生舱：内嵌 bash，stdout 每行 `<版本>|URL`，默认**恰好一行**
//! （声明 `expand: true` 时可多行，每行 = 同一列表里的一个连续槽位）。模板覆盖不了时才用它。
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
//! 条目级 script（模板覆盖不了时用；`expand: true` 才允许一个脚本产多个槽位）：
//!
//! ```yaml
//! sources:
//!   - tracker-template: script
//!     script: |
//!       page=$(curl -fsSL "https://example.com/src/")
//!       ver=$(printf '%s' "$page" | grep -oE 'pkg-[0-9.]+\.tar\.xz' | sed 's/pkg-//; s/\.tar\.xz//' | sort -V | tail -1)
//!       test -n "$ver" || exit 1
//!       echo "$ver|https://example.com/src/pkg-$ver.tar.xz"
//! ```
//!
//! **探测成功且版本变新时，LankeBUILD.json 的 sources / work_sources 被原子全量替换**为探测出的
//! 清单（旧值丢弃，空列表也写键，lpkg 默认形态）。任一条目探测失败 → 整包不更新（半截清单比不写更糟）。
//!
//! **主动性**：条目用 `tracker-template` 指定模板，模板不主动从 URI 猜格式（yaml 由人工/AI 编写，
//! 模板只是探测执行器）。

pub mod templates;
pub mod vercmp;

use crate::error::FarmError;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

use crate::net::Fetcher;

/// 单条 source 的探测结果：检测到的版本 + 该槽位的下载 URL。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EntryProbe {
    pub version: String,
    pub url: String,
}

/// **已探测出的槽位表**（`version-var` 解析用）。
///
/// 探测顺序固定为「`sources` 全部 → `work_sources` 全部」，且列表内**从左到右**逐条探测；
/// 因此 `version-var` 只能引用**位于它之前**的槽位——`work_sources` 条目可引用全部 sources
/// 槽位，`sources` 条目只能引用更靠前的 sources 槽位。前向/自引用取不到 → 报错。
/// 这与 multi-level 的「占位符只能引用前面已解出的级」是同一条规则。
#[derive(Debug, Default, Clone)]
pub(crate) struct ResolvedSlots {
    pub sources: Vec<EntryProbe>,
    pub work_sources: Vec<EntryProbe>,
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
    /// 用到的模板名（去重、升序、`+` 连接；显示用，如 `script` / `html-index+script`）。
    pub kind: String,
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

/// source 条目配置（每个槽位一条）。版本约束只作用于本条目。
/// `deny_unknown_fields`：typo 字段名（如 `tag-prefx`）解析即报错，而非静默忽略。
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SourceConfig {
    #[serde(rename = "tracker-template")]
    pub tracker_template: String,

    // ── script（**条目级**逃生舱；模板覆盖不了时才用）──
    /// 内嵌 bash：stdout 每行 `<版本>|URL`。默认**恰好一行**（多行报错）。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub script: Option<String>,
    /// 允许本脚本周产出**多个**槽位（每行一个，顺序即槽位顺序）。缺省 false = 必须恰好一行。
    #[serde(default, skip_serializing_if = "is_false")]
    pub expand: bool,
    /// **其他槽位的版本注入为脚本环境变量**：`{变量名: 选择器}`，如
    /// `version-var: {main: sources[0]}` → 脚本里 `$main` 即 `sources[0]` 探测出的版本。
    ///
    /// 存在的理由：一个条目的**内容派生自**另一条目时（libreoffice 的 vendor 文件名来自主源里的
    /// `download.lst`），若各自重探上游，两次探测之间上游发新版就会产出**版本不一致的清单**
    /// （主源 26.8.0.3 + vendor 26.8.0.4 → 构建必坏）。用本字段把派生关系显式表达出来，
    /// 下游条目直接拿上游**本轮已解析**的值，天然同版本、还省掉重复探测。
    ///
    /// **只能引用位于它之前的槽位**（与 multi-level "只能引用前面的级" 同规则）：
    /// `sources` 先于 `work_sources` 探测，所以 `work_sources` 条目可引用全部 sources 槽位，
    /// 而 `sources` 条目只能引用更靠前的 sources 槽位。前向/自引用 → 探测时报错。
    #[serde(
        rename = "version-var",
        default,
        skip_serializing_if = "BTreeMap::is_empty"
    )]
    pub version_var: BTreeMap<String, String>,

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

    // ── multi-level-html-index（N 级：每级 {name, url, pattern}）──
    /// N 级探测列表：每级一个 `{name, url, pattern}`。**名字即占位符**（`{名字}`），
    /// 只能在**后续级**的 url 与 template 里引用；名为 `version` 的那一级 = 包版本。
    /// 详见 `templates/multi_level_html_index.rs` 的模块文档。
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
    /// same-version 模板专用：锁定为指定**包**的版本（直接确定版本号，不经上游探测）。
    #[serde(rename = "same-version-of", skip_serializing_if = "Option::is_none")]
    pub same_version_of: Option<String>,
    /// same-version-of-source 模板专用：锁定为**本 tracker 中位于它之前**的槽位、**本轮**解析出的
    /// 版本（`sources[i]` / `work_sources[i]`）。与 `same-version-of` 的差别是"版本从哪来"：
    /// 那个读**另一个包**的已解析版本（受跨包顺序影响），这个读同一次探测里已经解出的槽位。
    #[serde(
        rename = "same-version-of-source",
        skip_serializing_if = "Option::is_none"
    )]
    pub same_version_of_source: Option<String>,
    #[serde(rename = "major-of", skip_serializing_if = "Option::is_none")]
    pub major_of: Option<String>, // 匹配指定包主版本的 tag/目录
    #[serde(rename = "major-version-lock", skip_serializing_if = "Option::is_none")]
    pub major_version_lock: Option<String>, // 锁定探测主版本常量（gtk3 锁 3）
    #[serde(rename = "max-version", skip_serializing_if = "Option::is_none")]
    pub max_version: Option<String>, // 版本封顶（gtk3 稳定系列止于 3.24）
    #[serde(rename = "stable-minor", skip_serializing_if = "Option::is_none")]
    pub stable_minor: Option<String>, // even：偶 minor 稳定分支；all：不按奇偶过滤（缺省不过滤）
    /// **版本黑名单**（正则，作用于**提取出的版本字符串**）：命中即整条候选丢弃。
    /// 上游混着历史异常 tag 时用——uasm 的 `v213`（旧命名法，按版本比较 213 > 2.57 会被误选）、
    /// cython 的 `3.3.0b1`（PEP 440 预发布，`is_stable` 只认 rc/beta 这类**单词**，认不出裸 `bN`）、
    /// intel-media-driver 的 `600`。**所有探测模板都支持**（script / same-version 除外）。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub exclude: Option<String>,
    #[serde(rename = "source-name", skip_serializing_if = "Option::is_none")]
    pub source_name: Option<String>, // 上游源目录/文件名覆盖（gtk3 的上游目录叫 gtk）
}

/// multi-level-html-index 的一级：**显式名字**（= 占位符名）+ 页面 URL + 版本正则。
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LevelConfig {
    /// 该级名字 —— **名字即占位符**：`{名字}` 可在**后续级**的 `url` 与 `template` 里引用。
    ///
    /// 保留名 **`version`**：这一级的捕获就是**包版本**（每份配置必须且只能有一级叫 `version`）。
    /// 不得取名 `name`（与上游名占位符 `{name}` 冲突）。名字必须非空且唯一。
    /// 位置隐式的 `{v1}..{vN}` 已废弃（引用它 → 未知占位符报错）。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    /// 该级页面 URL，可引用**前面已解出**的级名 `{x}`（引用后面/不存在的级 → 报错）。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub url: Option<String>,
    /// 该级版本正则（含一个捕获组），提取该级版本。
    #[serde(skip_serializing_if = "Option::is_none")]
    pub pattern: Option<String>,
}

fn is_false(b: &bool) -> bool {
    !*b
}

/// 是否合法 shell 变量名（`version-var` 的键会作为环境变量名注入脚本）。
fn is_shell_ident(s: &str) -> bool {
    let mut it = s.chars();
    matches!(it.next(), Some(c) if c.is_ascii_alphabetic() || c == '_')
        && it.all(|c| c.is_ascii_alphanumeric() || c == '_')
}

impl TrackerConfig {
    /// 从 tracker yaml 文本解析：供 `cli::load_trackers` 使用——
    /// 解析失败必须**可见**（不能像以前那样 `if let Ok` 静默跳过，让写错的 tracker"看着在、实际不生效"）。
    pub fn from_yaml(text: &str) -> Result<TrackerConfig, FarmError> {
        serde_yaml_ng::from_str(text).map_err(|e| format!("解析 tracker yaml 失败: {e}").into())
    }

    /// 用到的模板名（去重、升序、`+` 连接；显示用）。
    pub fn kind(&self) -> String {
        let mut names: Vec<&str> = self
            .sources
            .iter()
            .chain(&self.work_sources)
            .map(|e| e.tracker_template.as_str())
            .collect();
        names.sort_unstable();
        names.dedup();
        names.join("+")
    }

    /// 包级探测：逐条目探测，按 version-source 取版本。
    /// **任一条目失败 → 整包失败**（原子性：只在全清单可产出时才应用）。
    pub fn probe_with(
        &self,
        fetcher: &dyn Fetcher,
        lookup: &dyn Fn(&str) -> Option<String>,
    ) -> Result<ProbeResult, FarmError> {
        // 已探测槽位表随探测推进（供 version-var 引用）；顺序见 ResolvedSlots 文档。
        let mut resolved = ResolvedSlots::default();
        let srcs = probe_entry_list(
            &self.sources,
            fetcher,
            lookup,
            SlotList::Sources,
            &self.pkg_name,
            &mut resolved,
        )?;
        let wss = probe_entry_list(
            &self.work_sources,
            fetcher,
            lookup,
            SlotList::WorkSources,
            &self.pkg_name,
            &mut resolved,
        )?;
        // version-source 选择器：显式或默认（sources[0] 优先，空则 work_sources[0]）
        let (which, idx) = match &self.version_source {
            Some(sel) => parse_version_source(sel)?,
            None => {
                if !self.sources.is_empty() {
                    (SlotList::Sources, 0)
                } else if !self.work_sources.is_empty() {
                    (SlotList::WorkSources, 0)
                } else {
                    return Err(
                        format!("tracker {} 无 sources/work_sources 条目", self.pkg_name).into(),
                    );
                }
            }
        };
        let pool = match which {
            SlotList::Sources => &srcs,
            SlotList::WorkSources => &wss,
        };
        let version = pool.get(idx).map(|e| e.version.clone()).ok_or_else(|| {
            format!(
                "version-source 越界: {}[{}]（共 {} 条）",
                which.label(),
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
    pub fn probe(&self, fetcher: &dyn Fetcher) -> Result<ProbeResult, FarmError> {
        self.probe_with(fetcher, &|_| None)
    }

    /// 探测 + 生成更新提案（对照当前版本）。
    pub fn propose_with(
        &self,
        fetcher: &dyn Fetcher,
        lookup: &dyn Fn(&str) -> Option<String>,
        current_version: &str,
    ) -> Result<Proposal, FarmError> {
        let result = self.probe_with(fetcher, lookup)?;
        Ok(Proposal {
            pkg_name: self.pkg_name.clone(),
            current_version: current_version.to_string(),
            new_version: result.version,
            sources: result.sources,
            work_sources: result.work_sources,
            kind: self.kind(),
        })
    }
}

/// 槽位所在的列表。**显式枚举**——不要拿"报错用的字段名"当行为开关：
/// 那样改个文案就会悄悄改掉往哪张表里并槽位。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum SlotList {
    Sources,
    WorkSources,
}

impl SlotList {
    /// 报错与选择器用的字段名（与 yaml 键一致）。
    fn label(self) -> &'static str {
        match self {
            SlotList::Sources => "sources",
            SlotList::WorkSources => "work_sources",
        }
    }
}

/// 逐条目探测一个列表（sources 或 work_sources），任一条失败即整列表失败（带槽位上下文）。
fn probe_entry_list(
    list: &[SourceConfig],
    fetcher: &dyn Fetcher,
    lookup: &dyn Fn(&str) -> Option<String>,
    which: SlotList,
    pkg_name: &str,
    resolved: &mut ResolvedSlots,
) -> Result<Vec<EntryProbe>, FarmError> {
    let field = which.label();
    let mut out = Vec::with_capacity(list.len());
    for (i, cfg) in list.iter().enumerate() {
        // 一条目可产多个槽位（`expand` 的 script）→ 展平进同一张扁平槽位表。
        // 槽位顺序 = 条目顺序 × 条目内展开顺序，即 LankeBUILD.json 数组的位置语义。
        let probes = cfg
            .probe_with(fetcher, lookup, pkg_name, resolved)
            .map_err(|e| format!("{field}[{i}] 探测失败: {e}"))?;
        // 立即并入已探测表：只有**后面的**条目能用 version-var 引用它（前向引用取不到 → 报错）
        match which {
            SlotList::Sources => resolved.sources.extend(probes.iter().cloned()),
            SlotList::WorkSources => resolved.work_sources.extend(probes.iter().cloned()),
        }
        out.extend(probes);
    }
    Ok(out)
}

impl SourceConfig {
    /// 上游名字：`source-name` 覆盖（gtk3 → 上游目录叫 gtk），否则用包名。
    pub fn effective_name<'a>(&'a self, pkg_name: &'a str) -> &'a str {
        self.source_name.as_deref().unwrap_or(pkg_name)
    }

    /// 解析 `version-var`：把**已探测**槽位的版本取出来，供脚本作环境变量使用。
    /// 引用尚不可用（前向/自引用/越界）→ 报错并说明可用范围（不猜、不静默给空值）。
    fn version_vars(&self, resolved: &ResolvedSlots) -> Result<Vec<(String, String)>, FarmError> {
        let mut out = Vec::with_capacity(self.version_var.len());
        // BTreeMap → 迭代顺序确定（序列化与报错信息都可复现）
        for (name, sel) in &self.version_var {
            if !is_shell_ident(name) {
                return Err(format!(
                    "version-var 名 `{name}` 不是合法 shell 变量名（应为 [A-Za-z_][A-Za-z0-9_]*）"
                )
                .into());
            }
            if name == "PKG_NAME" {
                return Err("version-var 名不得占用保留变量 `PKG_NAME`"
                    .to_string()
                    .into());
            }
            let version =
                resolved_slot_version(sel, resolved, &format!("version-var `{name}: {sel}`"))?;
            out.push((name.clone(), version));
        }
        Ok(out)
    }

    /// 探测本条目：返回它占据的**全部槽位**（检测版本 + 下载 URL）。
    /// 绝大多数模板是 1 条；`tracker-template: script` + `expand: true` 可产多条。
    pub(crate) fn probe_with(
        &self,
        fetcher: &dyn Fetcher,
        lookup: &dyn Fn(&str) -> Option<String>,
        pkg_name: &str,
        resolved: &ResolvedSlots,
    ) -> Result<Vec<EntryProbe>, FarmError> {
        // 显式字段校验：声明的 tracker-template 只支持特定字段，设置不支持的 → 报错。
        // 顺带拿到注册项，下面的分发与它共用（查一次表）。
        let spec = validate_supported_fields(self)?;
        // version-var：把**已探测**槽位的版本解析成脚本环境变量（见字段文档）
        let vars = self.version_vars(resolved)?;
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
        // 一条目 → N 槽位：`script` 可直接产多条（`expand`），其余模板恒为 1 条。
        // 分发**查注册表**（`templates::TEMPLATES`）——这里不再有模板清单。
        let probes: Vec<EntryProbe> = match &spec.probe {
            templates::ProbeFn::Web(f) => vec![f(fetcher, self, major.as_deref(), pkg_name)?],
            templates::ProbeFn::LockPackage(f) => vec![f(self, lookup, pkg_name)?],
            templates::ProbeFn::LockSlot(f) => vec![f(self, resolved)?],
            templates::ProbeFn::Script(f) => f(fetcher, self, pkg_name, &vars)?,
        };
        // **残留占位符的唯一检查点**：模板不自己验（历史上 same-version / script 等各调一遍，纯冗余）。
        // 与 `multi_level_html_index::check_placeholders` 分工不同：那个在**替换前**校验"允许哪些
        // 占位符"（含各级动态级名），这里在替换后只兜"URL 里还残留 { 未替换"。
        for p in &probes {
            validate_url(&p.url)?;
        }
        Ok(probes)
    }
}

/// 显式字段校验：声明的 `tracker-template` 只支持特定字段，设置了不支持的 → 报错。
/// 把"字段声明集中在 SourceConfig、但模板是否读它全隐式"的静默忽略变成显式错误
/// （如 github 上写 max-version → 报错提示改用支持它的模板或 script 类型）。
///
/// **本函数不再含任何字段清单**：模板支持集来自注册表（`templates::spec`），"用户实际设了哪些字段"
/// 来自 `serde` 投影（序列化 `SourceConfig` 取键集——各字段的 `skip_serializing_if` 保证未设的字段
/// 不出现）。所以新增字段不必动这里，只有"某模板要开始支持某字段"才改注册表那一行。
/// 返回注册项，供调用方直接分发（省一次查表）。
fn validate_supported_fields(
    cfg: &SourceConfig,
) -> Result<&'static templates::TemplateSpec, FarmError> {
    let name = cfg.tracker_template.as_str();
    let spec = templates::spec(name).ok_or_else(|| format!("未知 tracker_template: {name}"))?;
    let value =
        serde_json::to_value(cfg).map_err(|e| format!("tracker 字段校验失败（{name}）: {e}"))?;
    let mut unsupported: Vec<&str> = value
        .as_object()
        .map(|o| o.keys().map(String::as_str).collect::<Vec<_>>())
        .unwrap_or_default()
        .into_iter()
        .filter(|k| {
            *k != "tracker-template"
                && !spec.supported.contains(k)
                && !(spec.version_constraints && templates::VERSION_CONSTRAINT_FIELDS.contains(k))
        })
        .collect();
    unsupported.sort_unstable();
    if !unsupported.is_empty() {
        return Err(format!(
            "tracker-template {name} 不支持字段: {}（需要版本封顶/稳定分支等约束时改用支持它的模板，或用条目级 script 模板 `tracker-template: script`）",
            unsupported.join(", ")
        )
        .into());
    }
    Ok(spec)
}

/// 解析 `version-source` / `version-var` 选择器：`sources[i]` / `work_sources[i]`
/// → `(列表, 下标)`。
pub(crate) fn parse_version_source(sel: &str) -> Result<(SlotList, usize), FarmError> {
    let err = || format!("version-source 无效 '{sel}'（应为 sources[i] 或 work_sources[i]）");
    let (which, rest) = if let Some(r) = sel.strip_prefix("sources[") {
        (SlotList::Sources, r)
    } else if let Some(r) = sel.strip_prefix("work_sources[") {
        (SlotList::WorkSources, r)
    } else {
        return Err(err().into());
    };
    let idx = rest
        .strip_suffix(']')
        .and_then(|s| s.parse::<usize>().ok())
        .ok_or_else(err)?;
    Ok((which, idx))
}

/// 取**已探测**槽位的版本（选择器 `sources[i]` / `work_sources[i]`）。
///
/// `version-var`（脚本环境变量）与 `same-version-of-source`（锁同 tracker 内更早槽位的版本）
/// 共用这一处解析：引用尚不可用（**前向 / 自引用 / 越界**）→ 报错并说明可用范围
/// （不猜、不静默给空值）。`what` 是报错前缀，形如 ``version-var `main: sources[0]` ``。
pub(crate) fn resolved_slot_version(
    sel: &str,
    resolved: &ResolvedSlots,
    what: &str,
) -> Result<String, FarmError> {
    let (which, idx) = parse_version_source(sel)?;
    let pool = match which {
        SlotList::Sources => &resolved.sources,
        SlotList::WorkSources => &resolved.work_sources,
    };
    pool.get(idx).map(|e| e.version.clone()).ok_or_else(|| {
        format!(
            "{what} 尚不可用——只能引用本 tracker 中**位于它之前**的槽位\
             （sources 先于 work_sources 探测，列表内从左到右；当前 {} 已探测 {} 条）",
            which.label(),
            pool.len()
        )
        .into()
    })
}

/// 需要的必填字段缺失时给出清晰错误。
pub(crate) fn need<'a>(opt: &'a Option<String>, field: &str) -> Result<&'a str, FarmError> {
    opt.as_deref()
        .ok_or_else(|| format!("tracker 配置缺 {field}").into())
}

/// 校验探测产出的 URL：残留 `{...}` 说明模板引用了未提供的占位符，生成的 URL 必然损坏。
/// 报错而非静默写入坏 URL（杜绝"莫名其妙改 URL"）。
pub(crate) fn validate_url(url: &str) -> Result<(), FarmError> {
    if url.contains('{') {
        return Err(format!("探测生成的 URL 残留未替换占位符: {url}").into());
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
mod tests;
