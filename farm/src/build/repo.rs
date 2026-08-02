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
    // 取代旧版本：清掉该目录下其他 .lpkg
    if let Ok(rd) = fs::read_dir(&repo_pkg_dir) {
        for e in rd.flatten() {
            let p = e.path();
            if p.extension().and_then(|x| x.to_str()) == Some("lpkg") && p != dest {
                let _ = fs::remove_file(&p);
            }
        }
    }
    Ok(dest)
}

/// 更新本地 repo index.txt：替换该包的版本块（保留 deps；新 version/hash/provides/needed_so）。
/// 更新本地 repo index.txt：替换该包的版本块。**剥掉 needed_so**（容器可见的索引不带 SONAME，
/// lpkg 一致性检查失效；完整 needed_so 在 abidb 里供 farm 传播用）。
pub(crate) fn update_repo_index(out_dir: &Path, arch: &str, pkg: &str, version: &str, hash: &str,
                     provides: &[String]) -> Result<(), String> {
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
            "{pkg}|{version}:{hash}:{old_deps}:{}:|{pkg_level}",
            provides.join(",")
        );
        lines.push(new_line);
        found = true;
    }
    if !found {
        // 新包：追加一行（deps 空，needed_so 空）
        lines.push(format!("{pkg}|{version}:{hash}::{}:|", provides.join(",")));
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
/// **必须有**——无基线构建是盲人摸象（needed_so 的 provider 无从校验、ABI diff 无从对比）。
/// 缺失/为空 → 报错，要求先 `farm seed` 引入 repo 数据；不做网络 fallback，在线状态由 seed 显式落地。
/// 旧索引基线 = farm 自己的 ABI 数据库（out/<arch>/.abi.json），含完整 needed_so/provides。
/// 容器可见的 index.txt 已剥掉 needed_so，传播必须从这里读（禁止无基线构建）。
pub(crate) fn load_old_index(out_dir: &Path, arch: &str) -> Result<Index, String> {
    crate::abidb::load_index(out_dir, arch)
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

