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
    /// **已废弃**：`type: script` 已降为条目级模板（`sources[i].tracker-template: script`）。
    /// 保留字段只为给出明确的迁移错误——`deny_unknown_fields` 只会吐 serde 的 `unknown field`，
    /// 那对 73 个待迁移的 yaml 毫无帮助。`type: template` 仍静默接受（等价于不写）。
    #[serde(rename = "type", skip_serializing_if = "Option::is_none")]
    pub deprecated_type: Option<String>,
    /// **已废弃**：改用条目级 `sources[i].script`。同上，保留只为报迁移错。
    #[serde(rename = "script-content", skip_serializing_if = "Option::is_none")]
    pub deprecated_script_content: Option<String>,
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
    /// 序列化为 tracker yaml（提案文件内容）。
    pub fn to_yaml(&self) -> Result<String, FarmError> {
        serde_yaml_ng::to_string(self).map_err(|e| format!("序列化 tracker yaml 失败: {e}").into())
    }

    /// 从 tracker yaml 文本解析（与 `to_yaml` 对称）：供 `cli::load_trackers` 使用——
    /// 解析失败必须**可见**（不能像以前那样 `if let Ok` 静默跳过，让写错的 tracker"看着在、实际不生效"）。
    pub fn from_yaml(text: &str) -> Result<TrackerConfig, FarmError> {
        let cfg: TrackerConfig =
            serde_yaml_ng::from_str(text).map_err(|e| format!("解析 tracker yaml 失败: {e}"))?;
        cfg.check_deprecated()?;
        Ok(cfg)
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

    /// 已废弃字段的迁移守卫：给出**可执行**的指引，而不是 serde 的 `unknown field`。
    /// 保留 `type`/`script-content` 两个字段的**唯一**理由就是这个（见字段文档）。
    pub fn check_deprecated(&self) -> Result<(), FarmError> {
        if let Some(t) = &self.deprecated_type {
            if t != "template" {
                return Err(format!(
                    "tracker {} 的 `type: {t}` 已废弃：script 现在是**条目级模板**，\
                     改用 `sources: [{{tracker-template: script, script: |...}}]`\
                     （stdout 每行 `<版本>|URL`；需一个脚本产多个槽位时加 `expand: true`）",
                    self.pkg_name
                )
                .into());
            }
        }
        if self.deprecated_script_content.is_some() {
            return Err(format!(
                "tracker {} 的 `script-content` 已废弃：改用条目级 `sources[i].script`\
                 （stdout 每行 `<版本>|URL`；需多个槽位时加 `expand: true`）",
                self.pkg_name
            )
            .into());
        }
        Ok(())
    }

    /// 包级探测：逐条目探测，按 version-source 取版本。
    /// **任一条目失败 → 整包失败**（原子性：只在全清单可产出时才应用）。
    pub fn probe_with(
        &self,
        fetcher: &dyn Fetcher,
        lookup: &dyn Fn(&str) -> Option<String>,
    ) -> Result<ProbeResult, FarmError> {
        self.check_deprecated()?;
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

    /// 无约束提案。
    pub fn propose(
        &self,
        fetcher: &dyn Fetcher,
        current_version: &str,
    ) -> Result<Proposal, FarmError> {
        self.propose_with(fetcher, &|_| None, current_version)
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
            let (which, idx) = parse_version_source(sel)?;
            let pool = match which {
                SlotList::Sources => &resolved.sources,
                SlotList::WorkSources => &resolved.work_sources,
            };
            let version = pool.get(idx).map(|e| e.version.clone()).ok_or_else(|| {
                format!(
                    "version-var `{name}: {sel}` 尚不可用——只能引用本 tracker 中**位于它之前**的槽位\
                     （sources 先于 work_sources 探测，列表内从左到右；当前 {} 已探测 {} 条）",
                    which.label(),
                    pool.len()
                )
            })?;
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
        // 显式字段校验：声明的 tracker-template 只支持特定字段，设置不支持的 → 报错
        validate_supported_fields(self)?;
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
        let probes: Vec<EntryProbe> = match self.tracker_template.as_str() {
            // script：条目级逃生舱。stdout 每行 `<版本>|URL`，行数受 `expand` 约束。
            "script" => templates::script::probe(fetcher, self, pkg_name, &vars)?,
            // same-version：直接锁定另一包版本（不经网络探测），需要 lookup 解析
            "same-version" => vec![templates::same_version::probe(self, lookup, pkg_name)?],
            "github" => vec![templates::github::probe(
                fetcher,
                self,
                major.as_deref(),
                pkg_name,
            )?],
            "gitlab" => vec![templates::gitlab::probe(
                fetcher,
                self,
                major.as_deref(),
                pkg_name,
            )?],
            "sourceforge" => vec![templates::sourceforge::probe(
                fetcher,
                self,
                major.as_deref(),
                pkg_name,
            )?],
            "gnome" => vec![templates::gnome::probe(
                fetcher,
                self,
                major.as_deref(),
                pkg_name,
            )?],
            "gcs" => vec![templates::gcs::probe(
                fetcher,
                self,
                major.as_deref(),
                pkg_name,
            )?],
            "html-index" => {
                vec![templates::html_index::probe(
                    fetcher,
                    self,
                    major.as_deref(),
                    pkg_name,
                )?]
            }
            "multi-level-html-index" => vec![templates::multi_level_html_index::probe(
                fetcher,
                self,
                major.as_deref(),
                pkg_name,
            )?],
            "pypi" => vec![templates::pypi::probe(
                fetcher,
                self,
                major.as_deref(),
                pkg_name,
            )?],
            other => return Err(format!("未知 tracker_template: {other}").into()),
        };
        for p in &probes {
            validate_url(&p.url)?;
        }
        Ok(probes)
    }
}

/// 显式字段校验：声明的 `tracker-template` 只支持特定字段，设置了不支持的 → 报错。
/// 把"字段声明集中在 SourceConfig、但模板是否读它全隐式"的静默忽略变成显式错误
/// （如 github 上写 max-version → 报错提示改用支持它的模板或 script 类型）。
/// `major-of` / `major-version-lock` 是探测模板的核心约束（same-version 模板直接锁版本，无过滤）。
fn validate_supported_fields(cfg: &SourceConfig) -> Result<(), FarmError> {
    const CORE: &[&str] = &["major-of", "major-version-lock"];
    let (template, mut supported): (&str, Vec<&str>) = match cfg.tracker_template.as_str() {
        // script：条目级逃生舱。脚本自己决定一切（含版本过滤）→ 除 script/expand 外一律不支持，
        // 也**不**参与下面的 CORE 扩展。
        "script" => ("script", vec!["script", "expand", "version-var"]),
        // same-version：直接锁版本，只认 same-version-of + template，占位符仅 {version}/{major_minor}
        // （URL 全写在 template：tag 前缀/仓库路径/上游名都烘进去，不支持 tag-prefix/repo/source-name）
        "same-version" => ("same-version", vec!["same-version-of", "template"]),
        "github" => (
            "github",
            vec![
                "repo",
                "mode",
                "tag-prefix",
                "template",
                "max-version",
                "stable-minor",
                "exclude",
            ],
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
                "stable-minor",
                "exclude",
            ],
        ),
        "html-index" => (
            "html-index",
            vec![
                "url",
                "pattern",
                "template",
                "max-version",
                "stable-minor",
                "exclude",
                "source-name",
            ],
        ),
        "multi-level-html-index" => (
            "multi-level-html-index",
            vec![
                "levels",
                "template",
                "max-version",
                "stable-minor",
                "exclude",
                "source-name",
            ],
        ),
        "gcs" => (
            "gcs",
            vec![
                "url",
                "pattern",
                "template",
                "max-version",
                "stable-minor",
                "exclude",
                "source-name",
            ],
        ),
        "gnome" => (
            "gnome",
            vec![
                "template",
                "max-version",
                "stable-minor",
                "exclude",
                "source-name",
            ],
        ),
        "sourceforge" => (
            "sourceforge",
            vec![
                "project",
                "path",
                "pattern",
                "template",
                "max-version",
                "stable-minor",
                "exclude",
                "source-name",
            ],
        ),
        // URL 来自 PyPI API（不用 template）；版本约束与其它探测模板一致地走 `VersionFilter`
        "pypi" => (
            "pypi",
            vec!["project", "max-version", "stable-minor", "exclude"],
        ),
        other => return Err(format!("未知 tracker_template: {other}").into()),
    };
    // 探测模板才有版本过滤约束；same-version 直接锁定版本、script 自带逻辑，都不参与 major 过滤
    if !matches!(template, "same-version" | "script") {
        supported.extend_from_slice(CORE);
    }
    let set = [
        ("script", cfg.script.is_some()),
        ("expand", cfg.expand),
        ("version-var", !cfg.version_var.is_empty()),
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
        ("exclude", cfg.exclude.is_some()),
        ("source-name", cfg.source_name.is_some()),
    ];
    let unsupported: Vec<&str> = set
        .iter()
        .filter(|(name, is_set)| *is_set && !supported.contains(name))
        .map(|(name, _)| *name)
        .collect();
    if !unsupported.is_empty() {
        return Err(format!(
            "tracker-template {template} 不支持字段: {}（需要版本封顶/稳定分支等约束时改用支持它的模板，或用条目级 script 模板 `tracker-template: script`）",
            unsupported.join(", ")
        )
        .into());
    }
    Ok(())
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
        let cfg: TrackerConfig = serde_yaml_ng::from_str(yaml).unwrap();
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
    fn script_entry_yaml_roundtrip() {
        let yaml = r#"
pkg-name: rhino
after: base
sources:
  - tracker-template: script
    script: |
      #!/bin/bash
      echo "1.7.15|https://github.com/mozilla/rhino/releases/download/rhino1.7.15/rhino-1.7.15.zip"
"#;
        let cfg = TrackerConfig::from_yaml(yaml).unwrap();
        assert_eq!(cfg.kind(), "script");
        assert_eq!(cfg.after.as_deref(), Some("base"));
        assert!(!cfg.sources[0].expand, "expand 缺省 false");
        assert!(cfg.sources[0]
            .script
            .as_ref()
            .unwrap()
            .contains("echo \"1.7.15|"));
        // 序列化往返：提案写回 yaml 时不得丢 script 内容
        let back = TrackerConfig::from_yaml(&cfg.to_yaml().unwrap()).unwrap();
        assert_eq!(back.sources[0].script, cfg.sources[0].script);
    }

    #[test]
    fn deprecated_package_level_script_is_rejected_with_guidance() {
        // 保留 type/script-content 字段的唯一目的是给出**可执行**的迁移指引，
        // 而不是 serde 的 `unknown field`。73 个待迁移 yaml 全靠这条定位。
        let e = TrackerConfig::from_yaml(
            "pkg-name: rhino\ntype: script\nscript-content: |\n  echo x\n",
        )
        .unwrap_err()
        .to_string();
        assert!(e.contains("条目级"), "应给出迁移指引: {e}");
        let e2 = TrackerConfig::from_yaml("pkg-name: rhino\nscript-content: |\n  echo x\n")
            .unwrap_err()
            .to_string();
        assert!(e2.contains("script-content"), "{e2}");
        // `type: template` 等价于不写 → 静默接受
        assert!(TrackerConfig::from_yaml("pkg-name: x\ntype: template\n").is_ok());
    }

    #[test]
    fn kind_lists_distinct_templates() {
        let cfg = TrackerConfig {
            pkg_name: "p".into(),
            sources: vec![SourceConfig {
                tracker_template: "github".into(),
                ..Default::default()
            }],
            work_sources: vec![SourceConfig {
                tracker_template: "script".into(),
                script: Some("x".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
        assert_eq!(cfg.kind(), "github+script");
    }

    #[test]
    fn yaml_serialization_skips_defaults() {
        let cfg = TrackerConfig {
            pkg_name: "bash".into(),
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
        assert!(!yaml.contains("type:"), "已废弃字段不得写出");
        assert!(!yaml.contains("script-content:"));
        assert!(!yaml.contains("script:"));
        assert!(!yaml.contains("expand:"), "expand 缺省 false，不写");
        assert!(!yaml.contains("after:"));
        assert!(!yaml.contains("version-source:"));

        // expand: true 必须写出来（否则提案写回 yaml 会丢掉多槽位语义）
        let mut cfg2 = cfg.clone();
        cfg2.sources[0].tracker_template = "script".into();
        cfg2.sources[0].script = Some("echo x".into());
        cfg2.sources[0].expand = true;
        assert!(cfg2.to_yaml().unwrap().contains("expand: true"));
    }

    #[test]
    fn parse_version_source_selectors() {
        assert_eq!(
            parse_version_source("sources[0]").unwrap(),
            (SlotList::Sources, 0)
        );
        assert_eq!(
            parse_version_source("sources[3]").unwrap(),
            (SlotList::Sources, 3)
        );
        assert_eq!(
            parse_version_source("work_sources[0]").unwrap(),
            (SlotList::WorkSources, 0)
        );
        assert!(parse_version_source("sources[]").is_err());
        assert!(parse_version_source("sources[abc]").is_err());
        assert!(parse_version_source("source[0]").is_err());
        assert!(parse_version_source("0").is_err());
    }

    #[test]
    fn package_probe_multi_source_version_source() {
        // 版本由 work_sources[0] 提供，sources 两条各自探测出 URL
        let f = MockFetcher::new(HashMap::new())
            .tags("https://github.com/a/main.git", &["v2.0", "v1.0"])
            .tags("https://github.com/b/vendored.git", &["v9.0"])
            .tags("https://github.com/c/ver.git", &["v3.1", "v3.0"]);
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
        let f = MockFetcher::new(HashMap::new()).tags("https://github.com/a/main.git", &["v2.0"]);
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
        assert!(
            err.to_string().contains("sources[0] 探测失败"),
            "err: {err}"
        );
    }

    #[test]
    fn version_source_out_of_range_errors() {
        let f = MockFetcher::new(HashMap::new()).tags("https://github.com/a/main.git", &["v2.0"]);
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
        assert!(err.to_string().contains("越界"), "err: {err}");
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
        assert!(err.to_string().contains("same-version-of"), "err: {err}");
    }

    #[test]
    fn legacy_same_version_key_is_unknown_field() {
        // 旧写法 `same-version:`（无 -of）已是未知字段 → deny_unknown_fields 解析即拒
        let yaml = "tracker-template: github\nrepo: a/b\nsame-version: other\n";
        let err = serde_yaml_ng::from_str::<SourceConfig>(yaml).unwrap_err();
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
        assert!(
            err.to_string().contains("不支持字段: same-version-of"),
            "err: {err}"
        );
    }

    #[test]
    fn entry_major_of_filters_by_major() {
        // 条目级 major-of：只匹配指定包主版本的 tag
        let f = MockFetcher::new(HashMap::new()).tags(
            "https://github.com/KhronosGroup/SPIRV-LLVM-Translator.git",
            &["v21.1.0", "v22.1.2", "v22.0.0", "v23.0.0"],
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
        let f = MockFetcher::new(HashMap::new()).tags("https://github.com/a/b.git", &["v1.2"]);
        let err = cfg.probe(&f).unwrap_err();
        assert!(err.to_string().contains("残留未替换占位符"), "err: {err}");
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
        assert!(err.to_string().contains("不支持字段: host"), "err: {err}");
        assert!(err.to_string().contains("github"), "err: {err}");
    }

    #[test]
    fn github_entry_accepts_max_version_and_caps() {
        // github 模板支持 max-version：tags 列表封顶生效（v261 被过滤取 v256）
        let f = MockFetcher::new(HashMap::new()).tags(
            "https://github.com/systemd/systemd.git",
            &["v254", "v256", "v255", "v261"],
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
        let f = MockFetcher::new(HashMap::new()).tags(
            "https://gitlab.com/a/b.git",
            &["v2.0.0", "v1.5.0", "v1.2.0"],
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
        assert!(
            err.to_string().contains("不支持字段: template"),
            "err: {err}"
        );
    }

    #[test]
    fn entry_unknown_field_in_yaml_is_rejected() {
        // deny_unknown_fields：typo 字段名（tag-prefx）解析即报错，而非静默忽略
        let yaml = "tracker-template: github\nrepo: a/b\ntag-prefx: v\n";
        let err = serde_yaml_ng::from_str::<SourceConfig>(yaml).unwrap_err();
        assert!(err.to_string().contains("tag-prefx"), "err: {err}");
    }

    /// 造一个条目级 script 条目（`expand` 缺省 false）。
    fn script_entry(script: &str, expand: bool) -> SourceConfig {
        SourceConfig {
            tracker_template: "script".into(),
            script: Some(script.into()),
            expand,
            ..Default::default()
        }
    }

    #[test]
    fn script_entry_produces_one_slot() {
        let cfg = TrackerConfig {
            pkg_name: "pkg".into(),
            sources: vec![script_entry(
                "#!/bin/bash\necho \"2.0|https://x/a-2.0.tar.gz\"\n",
                false,
            )],
            ..Default::default()
        };
        let r = cfg.probe(&crate::net::RealFetcher::default()).unwrap();
        assert_eq!(r.version, "2.0");
        assert_eq!(r.sources, vec!["https://x/a-2.0.tar.gz"]);
        assert!(r.work_sources.is_empty());
    }

    #[test]
    fn script_entry_works_in_work_sources_and_can_expand() {
        // 同一个模型的三个要点：script 也能放 work_sources；expand 产多槽位；
        // sources/work_sources 各归各的（条目在哪个列表就填哪个列表）。
        let cfg = TrackerConfig {
            pkg_name: "libreoffice".into(),
            sources: vec![script_entry(
                "#!/bin/bash\necho \"26.8.0.3|https://x/main.tar.xz\"\n",
                false,
            )],
            work_sources: vec![script_entry(
                "#!/bin/bash\nprintf '%s\\n' \"26.8.0.3|https://x/v1\" \"26.8.0.3|https://x/v2\" \"26.8.0.3|https://x/v3\"\n",
                true,
            )],
            ..Default::default()
        };
        let r = cfg.probe(&crate::net::RealFetcher::default()).unwrap();
        assert_eq!(r.version, "26.8.0.3");
        assert_eq!(r.sources, vec!["https://x/main.tar.xz"]);
        assert_eq!(
            r.work_sources,
            vec!["https://x/v1", "https://x/v2", "https://x/v3"]
        );
    }

    #[test]
    fn version_source_indexes_into_expanded_slots() {
        // expand 之后 `version-source` 索引的是**扁平槽位表**（= LankeBUILD.json 的 sources 数组）：
        // 第 0 条展开成 3 个槽位，则 sources[2] 是它的第 3 个槽位，sources[3] 才轮到第 1 条。
        let cfg = TrackerConfig {
            pkg_name: "pkg".into(),
            version_source: Some("sources[2]".into()),
            sources: vec![
                script_entry(
                    "#!/bin/bash\nprintf '%s\\n' \"1|https://x/a\" \"2|https://x/b\" \"3|https://x/c\"\n",
                    true,
                ),
                script_entry("#!/bin/bash\necho \"9|https://x/d\"\n", false),
            ],
            ..Default::default()
        };
        let r = cfg.probe(&crate::net::RealFetcher::default()).unwrap();
        assert_eq!(r.version, "3", "sources[2] 应落到展开出的第 3 个槽位");
        assert_eq!(
            r.sources,
            vec!["https://x/a", "https://x/b", "https://x/c", "https://x/d"]
        );
    }

    #[test]
    fn script_template_rejects_other_fields() {
        // script 只认 script/expand（连 CORE 的 major-of/max-version 都不认——脚本自己过滤版本）
        let mut e = script_entry("#!/bin/bash\necho \"1|https://x/a\"\n", false);
        e.max_version = Some("1.0".into());
        let cfg = TrackerConfig {
            pkg_name: "p".into(),
            sources: vec![e],
            ..Default::default()
        };
        let err = cfg.probe(&crate::net::RealFetcher::default()).unwrap_err();
        assert!(err.to_string().contains("不支持字段"), "{err}");

        // expand 只属于 script：放到别的模板上必须报错（否则会静默无效）
        let mut e2 = SourceConfig {
            tracker_template: "html-index".into(),
            url: Some("https://x/".into()),
            pattern: Some(r"v([0-9.]+)".into()),
            template: Some("https://x/{version}".into()),
            ..Default::default()
        };
        e2.expand = true;
        let cfg2 = TrackerConfig {
            pkg_name: "p".into(),
            sources: vec![e2],
            ..Default::default()
        };
        let err2 = cfg2.probe(&crate::net::RealFetcher::default()).unwrap_err();
        assert!(err2.to_string().contains("expand"), "{err2}");
    }

    #[test]
    fn version_var_injects_upstream_resolved_version() {
        // work_sources 的脚本用 $main 取 sources[0] **本轮解析出**的版本——
        // 不再自己重探上游，两个列表因此必然描述同一个版本。
        let cfg = TrackerConfig {
            pkg_name: "p".into(),
            sources: vec![script_entry(
                "#!/bin/bash\necho \"26.8.0.3|https://x/main.tar.xz\"\n",
                false,
            )],
            work_sources: vec![SourceConfig {
                tracker_template: "script".into(),
                script: Some("#!/bin/bash\necho \"$main|https://x/vendor-$main.tar.gz\"\n".into()),
                version_var: BTreeMap::from([("main".to_string(), "sources[0]".to_string())]),
                ..Default::default()
            }],
            ..Default::default()
        };
        let r = cfg.probe(&crate::net::RealFetcher::default()).unwrap();
        assert_eq!(r.version, "26.8.0.3");
        assert_eq!(r.work_sources, vec!["https://x/vendor-26.8.0.3.tar.gz"]);
    }

    #[test]
    fn version_var_only_references_earlier_slots() {
        // sources[0] 引用 sources[1]（前向）→ 取不到 → 报错
        let fwd = TrackerConfig {
            pkg_name: "p".into(),
            sources: vec![
                SourceConfig {
                    tracker_template: "script".into(),
                    script: Some("#!/bin/bash\necho \"1|https://x/a\"\n".into()),
                    version_var: BTreeMap::from([("v".to_string(), "sources[1]".to_string())]),
                    ..Default::default()
                },
                script_entry("#!/bin/bash\necho \"2|https://x/b\"\n", false),
            ],
            ..Default::default()
        };
        let e = fwd
            .probe(&crate::net::RealFetcher::default())
            .unwrap_err()
            .to_string();
        assert!(e.contains("位于它之前"), "{e}");

        // sources 条目不引用 work_sources（后者整列表都在它之后才探测）
        let cross = TrackerConfig {
            pkg_name: "p".into(),
            sources: vec![SourceConfig {
                tracker_template: "script".into(),
                script: Some("#!/bin/bash\necho \"1|https://x/a\"\n".into()),
                version_var: BTreeMap::from([("v".to_string(), "work_sources[0]".to_string())]),
                ..Default::default()
            }],
            work_sources: vec![script_entry("#!/bin/bash\necho \"2|https://x/b\"\n", false)],
            ..Default::default()
        };
        let e2 = cross
            .probe(&crate::net::RealFetcher::default())
            .unwrap_err()
            .to_string();
        assert!(e2.contains("位于它之前"), "{e2}");
    }

    #[test]
    fn version_var_validates_name_and_belongs_to_script_only() {
        let with_name = |name: &str| TrackerConfig {
            pkg_name: "p".into(),
            sources: vec![SourceConfig {
                tracker_template: "script".into(),
                script: Some("#!/bin/bash\necho \"1|https://x/a\"\n".into()),
                version_var: BTreeMap::from([(name.to_string(), "sources[0]".to_string())]),
                ..Default::default()
            }],
            ..Default::default()
        };
        for bad in ["1bad", "has-dash", "PKG_NAME"] {
            assert!(
                with_name(bad)
                    .probe(&crate::net::RealFetcher::default())
                    .is_err(),
                "{bad} 应被拒绝"
            );
        }
        // version-var 只属于 script：放到探测模板上 → 白名单报错
        let cfg = TrackerConfig {
            pkg_name: "p".into(),
            sources: vec![SourceConfig {
                tracker_template: "html-index".into(),
                url: Some("https://x/".into()),
                pattern: Some(r"v([0-9.]+)".into()),
                template: Some("https://x/{version}".into()),
                version_var: BTreeMap::from([("v".to_string(), "sources[0]".to_string())]),
                ..Default::default()
            }],
            ..Default::default()
        };
        let e = cfg
            .probe(&crate::net::RealFetcher::default())
            .unwrap_err()
            .to_string();
        assert!(e.contains("version-var"), "{e}");
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

    /// 钉死：字符串字段（`major-version-lock` 等）写**裸数字**时 serde_yaml_ng **会强转成字符串**。
    /// 仓库里两种写法并存（`'3'` 带引号 vs 裸 `6`），都有效——这条把该依赖行为钉住：一旦将来
    /// serde_yaml_ng 改成报错，`qt6-base`/`tcl` 这类 tracker 会被 `cli::load_trackers` 的
    /// `if let Ok` **静默丢弃**（配置看着在、实际不生效），必须先在此暴露。
    #[test]
    fn string_field_accepts_bare_number() {
        let yaml = "\
pkg-name: p
sources:
- tracker-template: html-index
  url: https://example.com/
  pattern: 'a([0-9]+)'
  template: https://example.com/{version}.tar.gz
  major-version-lock: 6
";
        let cfg = serde_yaml_ng::from_str::<TrackerConfig>(yaml).expect("裸数字应能解析");
        assert_eq!(
            cfg.sources[0].major_version_lock.as_deref(),
            Some("6"),
            "裸数字应被强转为 \"6\""
        );
    }
}
