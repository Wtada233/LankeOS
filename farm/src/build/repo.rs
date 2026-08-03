//! repo.rs — 仓库侧操作：版本判定 / 漂移 repack / 上传 / index 更新 / 配方读写。

use std::fs;
use std::path::{Path, PathBuf};
use crate::graph::Index;
use crate::lpkg_binding::BuildOutcome;
use crate::repack;
use crate::tr;
use super::{read_lankebuild, BuildOptions};
use sha2::{Digest, Sha256};

pub(crate) fn effective_version(pkgs_dir: &Path, pkg: &str) -> Option<String> {
    let b = read_lankebuild(pkgs_dir, pkg)?;
    Some(if let Some(r) = b.release {
        format!("{}+{r}", b.version)
    } else {
        b.version
    })
}

/// 增量判断（用户规则）：effective_version 与本地 repo 旧索引一致 → 跳过构建；
/// 除非后续有包 ABI breaking 导致该包依赖其需要重建（那会经传播入队，不受此限制）。
pub(crate) fn needs_build(pkgs_dir: &Path, pkg: &str, old: &Index) -> bool {
    let Some(ver) = effective_version(pkgs_dir, pkg) else {
        return false;
    };
    old.packages.get(pkg).map(|i| i.version.as_str()) != Some(ver.as_str())
}

/// 元数据漂移检测 + repack（gen_deps 语义，§6）：解包 .lpkg，扫描实际 vs 包内 metadata.json 的
/// needed_so/provides，**不一致才**改 metadata.json 并重打包（不 rebuild）。deps 不读不改。
/// 返回是否发生了 repack。
pub(crate) fn repack_if_drift(outcome: &BuildOutcome, opts: &BuildOptions, pkg: &str) -> bool {
    let Some(lpkg) = &outcome.lpkg_path else { return false };
    let extract = opts.out_dir.join("extract").join(pkg);
    // 期望值 = .lpkg 内 metadata.json（由 lpkg build 从 LankeBUILD.json 写入）
    let Ok(meta) = crate::scan::read_metadata_json(&extract.join("metadata.json")) else {
        return false;
    };
    let meta_needed: Vec<String> = meta["needed_so"]
        .as_array()
        .map(|a| a.iter().filter_map(|v| v.as_str().map(String::from)).collect())
        .unwrap_or_default();
    let meta_provides: Vec<String> = meta["provides"]
        .as_array()
        .map(|a| a.iter().filter_map(|v| v.as_str().map(String::from)).collect())
        .unwrap_or_default();
    if sorted_str(&meta_needed) == sorted_str(&outcome.needed_so)
        && sorted_str(&meta_provides) == sorted_str(&outcome.provides)
    {
        return false; // 无漂移
    }
    // 漂移 → repack（改 metadata.json + 重打包，复用 scan 的解包目录）
    match repack::repack_with_metadata(lpkg, &extract, &outcome.needed_so, &outcome.provides) {
        Ok(()) => true,
        Err(e) => {
            eprintln!("{}", tr!("build.repack_fail", pkg, e));
            false
        }
    }
}

/// 上传本地仓库 `out/<arch>/<pkg>/`：把 staging 的 .lpkg 移入并**取代旧版本**。返回最终路径。
///
/// 文件名必须精确匹配 lpkg 的下载 URL `<mirror>/<arch>/<pkg>/<version>.lpkg`
/// （lpkg installation_task.cpp:380 硬编码拼 `<version>.lpkg`）——**不能用构建产物名
/// `<pkg>-<version>.lpkg`**，否则 lpkg 拉依赖时 404（"文件名不对"）。
pub(crate) fn place_in_repo(outcome: &BuildOutcome, opts: &BuildOptions, pkg: &str) -> Result<PathBuf, String> {
    let Some(lpkg) = &outcome.lpkg_path else {
        return Err("无构建产物".to_string());
    };
    let version = effective_version(&opts.pkgs_dir, pkg).ok_or("配方无有效版本")?;
    let repo_pkg_dir = opts.out_dir.join(&opts.arch).join(pkg);
    fs::create_dir_all(&repo_pkg_dir).map_err(|e| format!("创建 {repo_pkg_dir:?} 失败: {e}"))?;
    let dest = repo_pkg_dir.join(format!("{version}.lpkg"));
    fs::rename(lpkg, &dest).map_err(|e| format!("移动 {lpkg:?} → {dest:?} 失败: {e}"))?;
    // 取代旧版本：先备份旧包中「新版不再提供」的 SONAME .so，再清旧版本
    if let Ok(rd) = fs::read_dir(&repo_pkg_dir) {
        for e in rd.flatten() {
            let p = e.path();
            if p.extension().and_then(|x| x.to_str()) == Some("lpkg") && p != dest {
                let _ = backup_removed_sonames(&opts.out_dir, &p, pkg, &outcome.provides);
                let _ = fs::remove_file(&p);
            }
        }
    }
    Ok(dest)
}

/// ABI 过渡：把旧包中「新版不再提供」的 SONAME 对应 .so 备份到 `out/backups/<pkg>/`。
/// 容器构建时 cp 进 /usr/lib（见 lpkg_binding），旧二进制（链旧 SONAME，如 gettext 链
/// libxml2.so.2）在过渡期能加载旧 .so；新构建用新 .so。
///
/// **只备份旧 provides 有、新打包消失的 SONAME**（`removed = old_provides − new_provides`），
/// 且只备份版本化 `.so.*` 文件（SONAME 本体 + 其实体），排除裸 `.so` dev 符号链接（那归新包）。
/// 扫全部系统库目录（usr/lib、lib、usr/lib64、lib64）——lib64 是 lib 的合并符号链接时
/// 内容重复，但备份目录扁平去重（同名覆盖，无害）。
fn backup_removed_sonames(
    out_dir: &Path,
    old_lpkg: &Path,
    pkg: &str,
    new_provides: &[String],
) -> Result<(), String> {
    let Ok(meta) = crate::scan::read_lpkg_metadata(old_lpkg) else {
        return Ok(());
    };
    let old_provides: Vec<String> = meta["provides"]
        .as_array()
        .map(|a| a.iter().filter_map(|v| v.as_str().map(String::from)).collect())
        .unwrap_or_default();
    let removed: Vec<&str> = old_provides
        .iter()
        .filter(|p| !new_provides.contains(p))
        .map(|s| s.as_str())
        .collect();
    if removed.is_empty() {
        return Ok(());
    }
    // 提取旧包，把各系统库目录下匹配被移除 SONAME 的版本化 .so 复制到备份目录
    let tmp = out_dir.join("backup_tmp").join(pkg);
    crate::scan::extract_lpkg(old_lpkg, &tmp)?;
    let backup_dir = out_dir.join("backups").join(pkg);
    fs::create_dir_all(&backup_dir).map_err(|e| format!("创建备份目录失败: {e}"))?;
    for sub in ["usr/lib", "lib", "usr/lib64", "lib64"] {
        let lib_dir = tmp.join("content").join(sub);
        if !lib_dir.is_dir() {
            continue;
        }
        if let Ok(rd) = fs::read_dir(&lib_dir) {
            for e in rd.flatten() {
                let p = e.path();
                let fname = p.file_name().and_then(|n| n.to_str()).unwrap_or("").to_string();
                // 只备份版本化 .so.*（排除裸 .so dev 符号链接——那归新包），且文件名属于某被移除 SONAME
                if !fname.contains(".so.") || !is_removed_soname_file(&fname, &removed) {
                    continue;
                }
                let dest = backup_dir.join(&fname);
                let Ok(ft) = fs::symlink_metadata(&p) else { continue };
                if ft.file_type().is_symlink() {
                    if let Ok(target) = fs::read_link(&p) {
                        let _ = fs::remove_file(&dest);
                        let _ = std::os::unix::fs::symlink(target, &dest);
                    }
                } else {
                    let _ = fs::copy(&p, &dest);
                }
            }
        }
    }
    let _ = fs::remove_dir_all(&tmp);
    Ok(())
}

/// 文件名是否属于某被移除的 SONAME：`r` 本身或其版本化派生名 `r.x.y`。
/// 用精确前缀（`r.` 而非 `starts_with(r)`）避免 `libfoo.so.2` 误匹配 `libfoo.so.20`。
fn is_removed_soname_file(fname: &str, removed: &[&str]) -> bool {
    removed.iter().any(|r| {
        fname == *r || fname.strip_prefix(r).is_some_and(|rest| rest.starts_with('.'))
    })
}

/// ABI 过渡备份的清理：**整个 build 完成**（而非单包）后调用。
///
/// 语义：备份存在的意义是让仍在引用旧 SONAME 的旧二进制在过渡期能加载；全部相关包重建完毕后，
/// 当前 index.txt（完整 needed_so，单一真源）不再有任何包的 `needed_so` 引用旧 SONAME → 备份作废。
/// 仍有引用（有包被跳过 / BLOCKED 未重建）则保留，留待下次 build 完成后再次清理。
/// 只在 index.txt 可读且含 needed_so 时清理；读不到/为空/全无 needed_so（剥离时代遗留）则保守保留，
/// 绝不误删。
pub(crate) fn cleanup_backups(out_dir: &Path, arch: &str) {
    let backups = out_dir.join("backups");
    if !backups.is_dir() {
        return;
    }
    let Ok(text) = fs::read_to_string(out_dir.join(arch).join("index.txt")) else {
        return;
    };
    let idx = Index::parse(&text);
    if idx.packages.is_empty() || idx.packages.values().all(|p| p.needed_so.is_empty()) {
        return; // 剥离时代遗留的旧索引 → 引用无从判断，保守保留
    }
    let referenced: std::collections::HashSet<String> = idx
        .packages
        .values()
        .flat_map(|p| p.needed_so.iter().cloned())
        .collect();
    let mut any_removed = false;
    if let Ok(rd) = fs::read_dir(&backups) {
        for e in rd.flatten() {
            let dir = e.path();
            if !dir.is_dir() {
                continue;
            }
            let sonames: Vec<String> = fs::read_dir(&dir)
                .map(|rd| {
                    rd.flatten()
                        .filter_map(|f| {
                            let name = f.file_name().to_string_lossy().into_owned();
                            soname_of(&name).map(str::to_string)
                        })
                        .collect()
                })
                .unwrap_or_default();
            if sonames.iter().any(|s| referenced.contains(s)) {
                continue; // 仍有包需要旧 SONAME → 保留
            }
            if fs::remove_dir_all(&dir).is_ok() {
                println!("{}", tr!("build.backup_clean", dir.display()));
                any_removed = true;
            }
        }
    }
    // 根目录变空 → 一并清掉（下次 build 有新备份时重建）
    if any_removed || fs::read_dir(&backups).map(|mut r| r.next().is_none()).unwrap_or(false) {
        let _ = fs::remove_dir(&backups);
    }
}

/// 备份文件名 → SONAME：`libfoo.so.1.2.3` → `libfoo.so.1`（前 3 段，第二段须是 `so`）。
/// 与 scan 的 SONAME 约定一致（`lib<name>.so.<major>`），非版本化/非库文件返回 None。
fn soname_of(filename: &str) -> Option<&str> {
    let mut parts = filename.splitn(4, '.');
    let a = parts.next()?;
    if parts.next()? != "so" {
        return None;
    }
    let c = parts.next()?;
    if c.is_empty() || !c.as_bytes()[0].is_ascii_digit() {
        return None;
    }
    let len = a.len() + 1 + 2 + 1 + c.len();
    filename.get(..len)
}

/// 更新本地 repo index.txt：替换该包的版本块（保留旧 deps；新 version/hash/provides）。
/// **写回完整 needed_so**——index.txt 是唯一真源（容器可见索引与 farm 的 ABI 传播共用，
/// 不再剥 needed_so、不再有第二份 .abi.json）。
pub(crate) fn update_repo_index(out_dir: &Path, arch: &str, pkg: &str, version: &str, hash: &str,
                     provides: &[String], needed_so: &[String]) -> Result<(), String> {
    let path = out_dir.join(arch).join("index.txt");
    let content = fs::read_to_string(&path).map_err(|e| format!("读 {path:?} 失败: {e}"))?;
    let mut found = false;
    let mut lines: Vec<String> = Vec::new();
    for line in content.lines() {
        if line.trim().is_empty() || line.starts_with('#') {
            lines.push(line.to_string());
            continue;
        }
        let name = line.split('|').next().unwrap_or("");
        if name != pkg {
            lines.push(line.to_string());
            continue;
        }
        // 替换该包的行：保留旧 deps（farm 不扫 deps）
        let mut parts = line.splitn(3, '|');
        let _ = parts.next();
        let rest = parts.next().unwrap_or("");
        let pkg_level = parts.next().unwrap_or("").to_string();
        let last_block = rest.split(';').next_back().unwrap_or("");
        let vparts: Vec<&str> = last_block.splitn(6, ':').collect();
        let old_deps = vparts.get(2).copied().unwrap_or("");
        let new_line = format!(
            "{pkg}|{version}:{hash}:{old_deps}:{}:{}|{pkg_level}",
            provides.join(","),
            needed_so.join(",")
        );
        lines.push(new_line);
        found = true;
    }
    if !found {
        // 新包：追加一行（deps 空，写完整 provides + needed_so）
        lines.push(format!(
            "{pkg}|{version}:{hash}::{provides}:{needed_so}|",
            provides = provides.join(","),
            needed_so = needed_so.join(",")
        ));
    }
    fs::write(&path, lines.join("\n") + "\n").map_err(|e| format!("写 {path:?} 失败: {e}"))
}

fn sorted_str(v: &[String]) -> Vec<&str> {
    let mut s: Vec<&str> = v.iter().map(String::as_str).collect();
    s.sort_unstable();
    s
}

pub(crate) fn sha256_file(path: &Path) -> Result<String, String> {
    let data = fs::read(path).map_err(|e| format!("读 {path:?} 失败: {e}"))?;
    let mut hasher = Sha256::new();
    hasher.update(&data);
    Ok(format!("{:x}", hasher.finalize()))
}

/// 传播重建前 bump release（规则 1）。
pub(crate) fn bump_release(pkgs_dir: &Path, pkg: &str) {
    let path = pkgs_dir.join(pkg).join("LankeBUILD.json");
    let Ok(content) = fs::read_to_string(&path) else { return };
    let Ok(mut v) = serde_json::from_str::<serde_json::Value>(&content) else { return };
    let rel = v["release"].as_u64().unwrap_or(0) + 1;
    v["release"] = serde_json::json!(rel);
    if fs::write(&path, serde_json::to_string_pretty(&v).unwrap()).is_ok() {
        println!("{}", tr!("build.release_bump", pkg, rel));
    }
}

/// 元数据漂移双写：LankeBUILD.json 的 needed_so/provides 同步为扫描实际值（规则 2）。
pub(crate) fn update_lankebuild_metadata(pkgs_dir: &Path, pkg: &str, outcome: &BuildOutcome) {
    let path = pkgs_dir.join(pkg).join("LankeBUILD.json");
    let Ok(content) = fs::read_to_string(&path) else { return };
    let Ok(mut v) = serde_json::from_str::<serde_json::Value>(&content) else { return };
    v["needed_so"] = serde_json::Value::Array(
        outcome.needed_so.iter().map(|s| serde_json::Value::String(s.clone())).collect(),
    );
    v["provides"] = serde_json::Value::Array(
        outcome.provides.iter().map(|s| serde_json::Value::String(s.clone())).collect(),
    );
    if fs::write(&path, serde_json::to_string_pretty(&v).unwrap()).is_ok() {
        println!("{}", tr!("build.meta_sync", pkg));
    }
}

/// 旧索引（ABI diff 基准，§7.2）：读本地 repo 的 `<out>/<arch>/index.txt`（seed 播种/发布产物）。
/// index.txt 含**完整 needed_so**（单一真源），传播（removed_sonames/revmap）、构建序（link_deps）
/// 都从这里读。**必须有**——无基线构建是盲人摸象（needed_so 的 provider 无从校验、ABI diff 无从对比）。
/// 缺失/为空 → 报错，要求先 `farm seed` 引入 repo 数据；不做网络 fallback，在线状态由 seed 显式落地。
pub(crate) fn load_old_index(out_dir: &Path, arch: &str) -> Result<Index, String> {
    let path = out_dir.join(arch).join("index.txt");
    let text = fs::read_to_string(&path).map_err(|e| {
        format!("缺少本地 repo 索引 {path:?}（{e}）——请先 `farm seed` 播种，禁止无基线构建")
    })?;
    let idx = Index::parse(&text);
    if idx.packages.is_empty() {
        return Err(format!(
            "本地 repo 索引 {path:?} 为空——请先 `farm seed` 播种，禁止无基线构建"
        ));
    }
    // 全零 needed_so = 剥离时代遗留的旧索引（曾剥 needed_so）→ 传播会失明，提示重新 seed
    if idx.packages.values().all(|p| p.needed_so.is_empty()) {
        println!("{}", tr!("build.index_no_soname", path.display()));
    }
    Ok(idx)
}

pub(crate) fn sorted_pkg_names(pkgs_dir: &Path) -> Vec<String> {
    let mut names: Vec<String> = fs::read_dir(pkgs_dir)
        .map(|rd| {
            rd.flatten()
                .filter(|e| e.path().join("LankeBUILD.json").exists())
                .map(|e| e.file_name().to_string_lossy().into_owned())
                .collect()
        })
        .unwrap_or_default();
    names.sort();
    names
}

/// build_deps 拓扑分批（Kahn；环兜底按字典序）。
/// Kahn 拓扑排序（精确链接依赖图）+ 循环检测切断。
///
/// 依赖边 = **旧索引 needed_so → provider**（`graph::link_deps`，需重建的链接库）∪
/// **配方 build_deps**（构建工具），仅限 targets（本轮需要重建的包）内。
/// 语义：让每个包在其所有"需要重建"的 needed_so provider 重建完毕之后再构建——
/// 先建链接库（libc/glib/zlib…），再建依赖者（chromium 等叶子），避免
/// "先建叶子、其依赖随后重建（ABI 变）导致叶子白跑一遍"。
///
/// 参考 lpkg/main/scripts/lankeos-world-rebuild-helper.py（确定性 Kahn + 三色 DFS 切环），
/// 区别：farm 增量构建，已就绪（不在 targets）的包不进图，无需全量重建。
/// 循环依赖：打印警告并切断构成环的后向边（每轮一条，确定性），保证总能给出完整顺序。
pub(crate) fn recipe_hash(pkgs_dir: &Path, pkg: &str) -> Option<String> {
    let mut hasher = Sha256::new();
    for f in ["LankeBUILD", "LankeBUILD.json"] {
        let content = fs::read_to_string(pkgs_dir.join(pkg).join(f)).ok()?;
        hasher.update(f.as_bytes());
        hasher.update(content.as_bytes());
    }
    Some(format!("{:x}", hasher.finalize()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn removed_soname_file_matches_exact_soname() {
        let removed = ["libfoo.so.2"];
        // SONAME 本体 + 其实体版本文件 → 匹配
        assert!(is_removed_soname_file("libfoo.so.2", &removed));
        assert!(is_removed_soname_file("libfoo.so.2.1.3", &removed));
        // 精确前缀（`r.`），绝不误匹配别的 major：libfoo.so.2 不该吞掉 libfoo.so.20
        assert!(!is_removed_soname_file("libfoo.so.20", &removed), "不应误匹配 libfoo.so.20");
        assert!(!is_removed_soname_file("libfoo.so.1", &removed));
        // 裸 .so dev 符号链接（归新包）→ 不匹配
        assert!(!is_removed_soname_file("libfoo.so", &removed));
    }

    #[test]
    fn soname_of_derives_versioned_soname() {
        // 备份文件名 → SONAME（lib<name>.so.<major>，取前 3 段）
        assert_eq!(soname_of("libfoo.so.1"), Some("libfoo.so.1"));
        assert_eq!(soname_of("libfoo.so.1.2.3"), Some("libfoo.so.1"));
        assert_eq!(soname_of("libxml2.so.2"), Some("libxml2.so.2"));
        assert_eq!(soname_of("ld-linux.so.2"), Some("ld-linux.so.2"));
        // 非库 / 无版本 / 第二段不是 so → None
        assert_eq!(soname_of("libfoo.so"), None, "裸 .so 无 SONAME");
        assert_eq!(soname_of("libfoo.1"), None, "第二段须是 so");
        assert_eq!(soname_of("README.txt"), None);
        assert_eq!(soname_of(""), None);
    }
}

