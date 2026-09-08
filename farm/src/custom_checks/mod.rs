//! custom_checks — LankeOS 策略/打包检测（qmlchk / pkgconfchk / pkg-errchk / hookchk）。
//!
//! 这些不是 farm 核心 ABI 功能，而是仓库维护策略的落地检测，参考 `manual-abi-fullchk`（abichk）的
//! 架构：遍历 `source/<arch>` 下的 .lpkg、每包**一次解包**、按 **.lpkg 文件 sha256** 缓存逐包分析
//! （命中即跳过解包重扫）、报告分包。
//!
//! 通用判定：qml/pkgconf 的「模块 provider 包」满足条件 = 属于本包 `deps`（读 pkgs 配方）∪
//! `needed_so` 推导的链接依赖（`graph::link_deps`，即 abichk 已算覆盖的运行时链接）；不在仓库内任何
//! 包提供的模块（外部模块）→ 忽略。`farm_flags` 的 `IGNORE_CHK_<KIND>` 可整包豁免某检則。

pub mod hook;
pub mod pkg_err;
pub mod pkgconf;
pub mod qml;

use std::collections::{BTreeMap, HashSet};
use std::path::{Path, PathBuf};

use crate::graph::Index;
use sha2::{Digest, Sha256};

/// 检則共同选项（与 abichk 同构）。
#[derive(Debug, Clone)]
pub struct ChkOpts {
    /// provider/cache 来源：构建仓库根（含 `<arch>/`），默认 `out`。
    pub source: PathBuf,
    pub arch: String,
    /// 本检則缓存目录（每包一个 json，key = .lpkg sha）。
    pub cache: PathBuf,
    /// 配方根（读 LankeBUILD.json 的 deps / farm_flags），默认 `pkgs`。
    pub pkgs_dir: PathBuf,
    /// 空 = 检查全部；否则只审计/报告这些包（provider 仍来自全量 source）。
    pub subset: Vec<String>,
    /// 忽略缓存强制全量重扫。
    pub full_rescan: bool,
}

/// 检則报告。
#[derive(Debug, Default, Clone)]
pub struct Report {
    pub checked: usize,
    pub cache_hits: u64,
    pub cache_misses: u64,
    /// pkg -> 发现项（排序）
    pub findings: BTreeMap<String, Vec<Finding>>,
    pub failed: Vec<String>,
}

/// 严重度。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Severity {
    /// 依赖树不正确：对象（module/符号）存在但不在本包 deps∪needed_so 内（缺依赖）。
    Warning,
    /// 全仓库都没有该对象（无任何 provider）——真断裂/缺口。
    Critical,
}

pub fn sev_marker(s: Severity) -> &'static str {
    match s {
        Severity::Warning => "[W] ",
        Severity::Critical => "[C] ",
    }
}

/// 单条发现。
#[derive(Debug, Clone)]
pub struct Finding {
    /// 来源文件（相对 content，如 `usr/share/.../a.qml` / `usr/lib/pkgconfig/glib-2.0.pc`）
    pub file: String,
    /// 问题描述 / 缺失对象（模块名、路径、hook 类型…）
    pub what: String,
    pub severity: Severity,
}

impl Report {
    pub fn count(&self) -> usize {
        self.findings.values().map(|v| v.len()).sum()
    }
}

/// 三段判定（qml/pkgconf 共用）：给定目标包的依赖闭包判定 + 全仓库 provider。
/// 输入 reqs = (file, module/符号, 全仓库该对象的 provider 包集合)。
/// - 闭包内有 provider（deps∪needed_so，含自身）→ 无 finding（已覆盖）；
/// - 闭包内无、但仓库有该对象 → **Warning**（依赖树不正确，少依赖）；
/// - 仓库也没有 → **Critical**（无任何包提供）。
pub fn module_dep_findings(
    index: &Index,
    pkg: &str,
    reqs: Vec<(String, String, HashSet<String>)>,
) -> Vec<Finding> {
    let mut out = Vec::new();
    for (file, m, owners) in reqs {
        if owners.is_empty() {
            out.push(Finding {
                file,
                what: format!("{m}：仓库内无任何包提供（{m} 缺失）"),
                severity: Severity::Critical,
            });
            continue;
        }
        let covered = owners.iter().any(|o| owner_covered(index, pkg, o));
        if !covered {
            // 主语 = 缺依赖的包（本 .pc/.qml 的归属包），不是模块名——模块名常等于提供者包名，会自指
            let o = owners.iter().cloned().collect::<Vec<_>>().join("|");
            out.push(Finding {
                file,
                what: format!(
                    "依赖树不正确：{pkg} 少依赖（提供者 {o}，请加入 deps 或由 needed_so 覆盖）"
                ),
                severity: Severity::Warning,
            });
        }
    }
    out
}

/// 默认缓存目录：`$HOME/.cache/lankefarm-<label>`（无 HOME 回落 source/.abi-cache）。
pub fn default_cache_dir(source: &Path, label: &str) -> PathBuf {
    match std::env::var("HOME") {
        Ok(h) if !h.is_empty() => PathBuf::from(h).join(format!(".cache/lankefarm-{label}")),
        _ => source.join(".abi-cache"),
    }
}

pub fn sha256_file(path: &Path) -> Result<String, String> {
    let data = std::fs::read(path).map_err(|e| format!("读 {path:?} 失败: {e}"))?;
    let mut h = Sha256::new();
    h.update(&data);
    Ok(format!("{:x}", h.finalize()))
}

/// 收集目录下所有叶子成员（常规文件 **和符号链接**；DFS，排序 → 确定序）。
/// 符号链接必须算成员：打包常见 `usr/lib/pkgconfig/libpng.pc -> libpng16.pc` 这种软链，provider
/// 模块名取自链接名；只对**目录**递归（不 follow 符号链接目录，避免环）。
pub fn collect_files(root: &Path) -> Vec<PathBuf> {
    fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
        let Ok(rd) = std::fs::read_dir(dir) else {
            return;
        };
        let mut subs: Vec<PathBuf> = Vec::new();
        for e in rd.flatten() {
            let p = e.path();
            let Ok(ft) = e.file_type() else { continue };
            if ft.is_dir() {
                subs.push(p);
            } else {
                out.push(p); // is_file() 或 is_symlink()（含 broken symlink，读取端容错跳过）
            }
        }
        subs.sort();
        for s in subs {
            walk(&s, out);
        }
    }
    let mut v = Vec::new();
    walk(root, &mut v);
    v.sort();
    v
}

// ── 逐包分析缓存（key = .lpkg 文件 sha + 检查器 schema）───────────────
/// 检查器/提取逻辑版本：改了分析逻辑（如收集符号链接成员）→ 递增使旧缓存整体失效重扫。
const SCHEMA: u32 = 5;

fn cache_path(cache: &Path, pkg: &str) -> PathBuf {
    cache.join(format!("{pkg}.json"))
}

fn load_analysis(cache: &Path, pkg: &str, lpkg_sha: &str) -> Option<serde_json::Value> {
    let c: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(cache_path(cache, pkg)).ok()?).ok()?;
    if c.get("schema").and_then(|v| v.as_u64()) != Some(SCHEMA as u64) {
        return None;
    }
    if c.get("lpkg_sha").and_then(|v| v.as_str()) != Some(lpkg_sha) {
        return None;
    }
    c.get("analysis").cloned()
}

fn write_analysis(
    cache: &Path,
    pkg: &str,
    lpkg_sha: &str,
    analysis: &serde_json::Value,
) -> Result<(), String> {
    std::fs::create_dir_all(cache).map_err(|e| format!("创建缓存目录 {:?} 失败: {e}", cache))?;
    let c = serde_json::json!({ "schema": SCHEMA, "lpkg_sha": lpkg_sha, "analysis": analysis });
    std::fs::write(
        cache_path(cache, pkg),
        serde_json::to_string_pretty(&c).map_err(|e| format!("序列化缓存失败: {e}"))?,
    )
    .map_err(|e| format!("写缓存 {pkg} 失败: {e}"))
}

/// 一次遍历 source 下**全部**包：每包一个当前 .lpkg，解包一次交给 `analyze`（仅 .lpkg sha 变才解包，
/// 否则用缓存 analysis）。返回每个包的 analysis + 缓存命中/重扫计数。provider 类检則用它对全量建图，
/// 判定类检則只关心（子集）包的 analysis。
pub fn walk_all(
    opts: &ChkOpts,
    analyze: impl Fn(&Path /*extract_dir*/, &str /*pkg*/) -> Result<serde_json::Value, String>,
) -> Result<(BTreeMap<String, serde_json::Value>, u64, u64, Vec<String>), String> {
    std::fs::create_dir_all(&opts.cache).map_err(|e| format!("创建缓存目录失败: {e}"))?;
    let repo_root = opts.source.join(&opts.arch);
    let mut pkgdirs: Vec<PathBuf> = std::fs::read_dir(&repo_root)
        .map_err(|e| format!("读取 {repo_root:?} 失败: {e}"))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.is_dir())
        .collect();
    pkgdirs.sort();

    let scratch = std::env::temp_dir().join(format!(
        "lankefarm-chk-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.subsec_nanos())
            .unwrap_or(0)
    ));
    std::fs::create_dir_all(&scratch).map_err(|e| format!("创建临时工作目录失败: {e}"))?;
    let mut analyses = BTreeMap::new();
    let mut hits = 0u64;
    let mut misses = 0u64;
    let mut failed: Vec<String> = Vec::new();

    for pkgdir in &pkgdirs {
        let pkg = pkgdir
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("?")
            .to_string();
        // 当前 .lpkg（通常一个；取最新的）
        let mut lpkg: Vec<PathBuf> = std::fs::read_dir(pkgdir)
            .map(|rd| {
                rd.filter_map(|e| e.ok().map(|e| e.path()))
                    .filter(|p| p.extension().is_some_and(|x| x == "lpkg"))
                    .collect()
            })
            .unwrap_or_default();
        lpkg.sort_by_key(|p| std::fs::metadata(p).and_then(|m| m.modified()).ok());
        let Some(l) = lpkg.last() else { continue };
        let sha = match sha256_file(l) {
            Ok(s) => s,
            Err(_) => continue,
        };
        if !opts.full_rescan {
            if let Some(a) = load_analysis(&opts.cache, &pkg, &sha) {
                analyses.insert(pkg.clone(), a);
                hits += 1;
                continue;
            }
        }
        let extract_dir = scratch.join(&pkg);
        match crate::scan::extract_lpkg(l, &extract_dir) {
            Ok(()) => match analyze(&extract_dir, &pkg) {
                Ok(a) => {
                    misses += 1;
                    if write_analysis(&opts.cache, &pkg, &sha, &a).is_err() {
                        // 缓存写失败不致命：本轮照用内存分析
                    }
                    analyses.insert(pkg.clone(), a);
                }
                Err(e) => failed.push(format!("{pkg}: {e}")),
            },
            Err(e) => failed.push(format!("{pkg}: 解包失败 {e}")),
        }
        let _ = crate::scan::remove_dir_tree(&extract_dir);
    }
    let _ = std::fs::remove_dir_all(&scratch);
    Ok((analyses, hits, misses, failed))
}

/// 本包是否带 `IGNORE_CHK_<KIND>` farm flag（豁免该检則；farm_flags 在配方里）。
pub fn is_ignored(pkgs_dir: &Path, pkg: &str, kind: &str) -> bool {
    let Some(b) = crate::build::read_lankebuild(pkgs_dir, pkg) else {
        return false;
    };
    let want = format!("IGNORE_CHK_{kind}");
    b.farm_flags.iter().any(|v| {
        v.as_str()
            .is_some_and(|f| f.trim().eq_ignore_ascii_case(&want))
    })
}

/// 载入仓库旧/当前索引（needed_so 链接判定 + binpkg deps 用）。缺失 → 空 Index（无覆盖信息）。
pub fn load_index(opts: &ChkOpts) -> Index {
    std::fs::read_to_string(opts.source.join(&opts.arch).join("index.txt"))
        .map(|t| Index::parse(&t))
        .unwrap_or_default()
}

/// binpkg deps（repo index 记录的运行时手写依赖——**以装好的包为准**，不读 LankeBUILD.json）。
pub fn binpkg_deps(index: &Index, pkg: &str) -> Vec<String> {
    index
        .packages
        .get(pkg)
        .map(|i| i.deps.clone())
        .unwrap_or_default()
}

/// owner 是否已被本包 binpkg 运行时覆盖：owner == 自身，或在 index deps（binpkg），
/// 或已是 needed_so 链接依赖。
pub fn owner_covered(index: &Index, pkg: &str, owner: &str) -> bool {
    if owner == pkg {
        return true;
    }
    if binpkg_deps(index, pkg).iter().any(|d| d == owner) {
        return true;
    }
    crate::graph::link_deps(index, pkg)
        .iter()
        .any(|d| d == owner)
}
