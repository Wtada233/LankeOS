//! custom_checks — LankeOS 维护检則工具集（qml / pkgconf / pkg-err / hook / abi）。
//!
//! 这些不是 farm 核心 ABI 功能，而是仓库维护策略的落地检测，参考 ABI 审计（`custom_checks/abi`）的
//! 架构：遍历 `source/<arch>` 下的 .lpkg、每包**一次解包**、按 **.lpkg 文件 sha256** 缓存逐包分析
//! （命中即跳过解包重扫）、报告分包。
//!
//! 通用判定：qml/pkgconf 的「模块 provider 包」满足条件 = 属于本包 binpkg deps（仓库 index 记录的
//! 运行时依赖，`binpkg_deps`；**不读 LankeBUILD.json**）∪ `needed_so` 推导的链接依赖
//! （`graph::link_deps`，即 abichk 已算覆盖的运行时链接）。三段判定：闭包内命中 → 通过；
//! 仓库内其它包提供但不在闭包 → Warning（少依赖）；仓库内无任何 provider → Critical（真缺口；
//! 引擎/进程内注册型 QML 模块用配方 `QML_CHK_IGN_LST` 显式豁免）。`farm_flags` 的
//! `IGNORE_CHK_<KIND>` 可整包豁免某检則。

pub mod abi;
pub mod build_deps;
pub mod hook;
pub mod introspection;
pub mod pkg_err;
pub mod pkgconf;
pub mod pycache;
pub mod qml;
pub mod vapi;

use crate::error::FarmError;
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

/// 文件在 content 下的相对路径（qml/pkg-err/introspection/vapi 共用）。
pub(crate) fn rel_of(path: &Path, root: &Path) -> String {
    path.strip_prefix(root)
        .map(|p| p.to_string_lossy().into_owned())
        .unwrap_or_else(|_| path.display().to_string())
}

/// **构建期依赖判定**（introspectionchk / vapichk 共用）：包内 shipped `files`（如 `.gir`/`.vapi`）由
/// 某构建工具在**构建期**生成，配方 `build_deps` 必须声明该工具包 `dep`；缺失 → **Critical**——这不是
/// 运行期依赖问题，是构建依赖漏写（构建会失败或产物缺失）。
///
/// - `pkg == dep` 自满足：工具包自带自己的产物（如 `vala` 装标准 `.vapi`、
///   `gobject-introspection` 自带 `.gir`），不该要求它把自己写进 build_deps；
/// - 配方不可读（无 `LankeBUILD.json`）→ 不判（无从得知 build_deps）；
/// - 一包只报一条（列出前几个文件 + 计数），避免 `.gir` 多的包刷屏。
pub fn build_dep_findings(
    pkgs_dir: &Path,
    pkg: &str,
    dep: &str,
    what: &str,
    files: &[String],
) -> Vec<Finding> {
    if files.is_empty() || pkg == dep {
        return Vec::new();
    }
    let Some(b) = crate::build::read_lankebuild(pkgs_dir, pkg) else {
        return Vec::new();
    };
    if b.build_deps.iter().any(|d| d == dep) {
        return Vec::new();
    }
    let shown: Vec<&str> = files.iter().take(3).map(String::as_str).collect();
    let more = if files.len() > 3 {
        format!(" 等 {} 个", files.len())
    } else {
        String::new()
    };
    vec![Finding {
        file: format!("{}{more}", shown.join(", ")),
        what: format!("{what}，但配方 build_deps 缺 {dep}（构建期需要该工具，请补进 build_deps）"),
        severity: Severity::Critical,
    }]
}

/// 检則缓存**根**：`$HOME/.cache/lankefarm`（无 HOME 回落 `source/.abi-cache`）。
/// 单跑 `farm chk <kind>` 与 `farm chk full` 共用同一根 → 两个入口共享缓存。
pub fn default_cache_base(source: &Path) -> PathBuf {
    cache_base_from_home(std::env::var("HOME").ok().as_deref(), source)
}

/// 纯函数（可单测，不读环境）：HOME 为 Some 且非空 → `$HOME/.cache/lankefarm`；否则 `source/.abi-cache`。
pub fn cache_base_from_home(home: Option<&str>, source: &Path) -> PathBuf {
    match home {
        Some(h) if !h.is_empty() => PathBuf::from(h).join(".cache/lankefarm"),
        _ => source.join(".abi-cache"),
    }
}

/// 单检則缓存目录 = `default_cache_base(source)/label`（每检則一子目录，互不干扰）。
pub fn default_cache_dir(source: &Path, label: &str) -> PathBuf {
    default_cache_base(source).join(label)
}

pub fn sha256_file(path: &Path) -> Result<String, FarmError> {
    let data = std::fs::read(path).map_err(|e| format!("读 {path:?} 失败: {e}"))?;
    let mut h = Sha256::new();
    h.update(&data);
    Ok(format!("{:x}", h.finalize()))
}

/// 收集叶子成员（常规文件 **和符号链接**；DFS，排序 → 确定序）。qml/pkgconf/pkg-err/hook 用。
/// 符号链接必须算成员：打包常见 `usr/lib/pkgconfig/libpng.pc -> libpng16.pc` 这种软链，provider
/// 模块名取自链接名；只对**目录**递归（不 follow 符号链接目录，避免环）。
pub fn collect_files(root: &Path) -> Vec<PathBuf> {
    collect(root, true)
}

/// 只收集**常规文件**（排除符号链接）的变体，供 abi 审计用：`.so` 真身是常规文件，软链名不产生
/// 符号（且 `fs::read` 软链会读目标，对损坏软链会失败）。行为等价于旧 abi 私有实现。
pub fn collect_regular_files(root: &Path) -> Vec<PathBuf> {
    collect(root, false)
}

/// 两个入口的公共实现；`include_symlinks=false` 时非目录且非常规文件的成员被跳过。
fn collect(root: &Path, include_symlinks: bool) -> Vec<PathBuf> {
    fn walk(dir: &Path, include_symlinks: bool, out: &mut Vec<PathBuf>) {
        let Ok(rd) = std::fs::read_dir(dir) else {
            return;
        };
        let mut subs: Vec<PathBuf> = Vec::new();
        for e in rd.flatten() {
            let p = e.path();
            let Ok(ft) = e.file_type() else { continue };
            if ft.is_dir() {
                subs.push(p);
            } else if include_symlinks || ft.is_file() {
                // include_symlinks：is_file() 或 is_symlink()（含 broken symlink，读取端容错跳过）
                out.push(p);
            }
        }
        subs.sort();
        for s in subs {
            walk(&s, include_symlinks, out);
        }
    }
    let mut v = Vec::new();
    walk(root, include_symlinks, &mut v);
    v.sort();
    v
}

// ── 逐包分析缓存（key = .lpkg 文件 sha + 检查器 schema）───────────────
// schema 常量由**各检則模块自持**（见 `walk_all` 的 `schema` 参数）：改了某检則的分析逻辑只递增
// 该检則的版本、只失效该类缓存，不再波及其余四类。

fn cache_path(cache: &Path, pkg: &str) -> PathBuf {
    cache.join(format!("{pkg}.json"))
}

fn load_analysis(
    cache: &Path,
    pkg: &str,
    lpkg_sha: &str,
    schema: u32,
) -> Option<serde_json::Value> {
    let c: serde_json::Value =
        serde_json::from_str(&std::fs::read_to_string(cache_path(cache, pkg)).ok()?).ok()?;
    if c.get("schema").and_then(|v| v.as_u64()) != Some(schema as u64) {
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
    schema: u32,
) -> Result<(), FarmError> {
    std::fs::create_dir_all(cache).map_err(|e| format!("创建缓存目录 {:?} 失败: {e}", cache))?;
    let c = serde_json::json!({ "schema": schema, "lpkg_sha": lpkg_sha, "analysis": analysis });
    std::fs::write(
        cache_path(cache, pkg),
        serde_json::to_string_pretty(&c).map_err(|e| format!("序列化缓存失败: {e}"))?,
    )
    .map_err(|e| format!("写缓存 {pkg} 失败: {e}").into())
}

/// 一次遍历 source 下**全部**包：每包一个当前 .lpkg，解包一次交给 `analyze`（仅 .lpkg sha 变才解包，
/// 否则用缓存 analysis）。返回每个包的 analysis + 缓存命中/重扫计数。provider 类检則用它对全量建图，
/// 判定类检則只关心（子集）包的 analysis。
///
/// `schema` = **本检則自己的** analysis 结构版本：只在**本检則**分析逻辑变化时递增。历史：5 个检則
/// 共用一个常量，任一检則改逻辑 → 另外四类缓存连带失效（一次 845 包全量重扫，白烧几分钟）。
pub fn walk_all(
    opts: &ChkOpts,
    schema: u32,
    analyze: impl Fn(
        &Path, /*extract_dir*/
        &str,  /*pkg*/
    ) -> Result<serde_json::Value, FarmError>,
) -> Result<(BTreeMap<String, serde_json::Value>, u64, u64, Vec<String>), FarmError> {
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
            if let Some(a) = load_analysis(&opts.cache, &pkg, &sha, schema) {
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
                    if write_analysis(&opts.cache, &pkg, &sha, &a, schema).is_err() {
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cache_base_prefers_home_over_source() {
        let src = Path::new("/srv/out");
        assert_eq!(
            cache_base_from_home(Some("/home/u"), src),
            PathBuf::from("/home/u/.cache/lankefarm")
        );
        // 空 HOME / 无 HOME → 回落 source/.abi-cache
        assert_eq!(cache_base_from_home(Some(""), src), src.join(".abi-cache"));
        assert_eq!(cache_base_from_home(None, src), src.join(".abi-cache"));
        // 单检則目录 = 根/label（单跑与 full 共用根 → 共享缓存）
        assert_eq!(
            cache_base_from_home(Some("/h"), src).join("qmlchk"),
            PathBuf::from("/h/.cache/lankefarm/qmlchk")
        );
    }

    #[test]
    fn collect_variants_include_or_exclude_symlinks() {
        let tmp = std::env::temp_dir().join(format!("farm-chk-collect-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&tmp);
        std::fs::create_dir_all(tmp.join("d")).unwrap();
        std::fs::write(tmp.join("a.txt"), b"x").unwrap();
        std::fs::write(tmp.join("d/b.txt"), b"x").unwrap();
        std::os::unix::fs::symlink("a.txt", tmp.join("link")).unwrap();

        let all = collect_files(&tmp);
        let reg = collect_regular_files(&tmp);
        assert_eq!(all.len(), 3, "collect_files 含符号链接: {all:?}");
        assert_eq!(reg.len(), 2, "collect_regular_files 排除符号链接: {reg:?}");
        assert!(reg
            .iter()
            .all(|p| std::fs::symlink_metadata(p).unwrap().file_type().is_file()));
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    fn schema_or_sha_mismatch_invalidates_cache() {
        let dir = std::env::temp_dir().join(format!("farm-chk-schema-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let a = serde_json::json!({"k": 1});
        write_analysis(&dir, "p", "sha1", &a, 5).unwrap();
        assert!(
            load_analysis(&dir, "p", "sha1", 5).is_some(),
            "同 schema+sha 命中"
        );
        assert!(
            load_analysis(&dir, "p", "sha1", 6).is_none(),
            "schema 变 → 本检則缓存失效"
        );
        assert!(
            load_analysis(&dir, "p", "sha2", 5).is_none(),
            ".lpkg sha 变 → 失效"
        );
        let _ = std::fs::remove_dir_all(&dir);
    }
}
