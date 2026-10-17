//! repack.rs — 解包 .lpkg → 改 metadata.json → 重打（§6：repack 不 rebuild）。
//!
//! 语义对标 Gentoo stage3 解压：保留 mode（含 SUID/SGID）+ numeric-owner（不查用户名表）；解包不用
//! /tmp（NOSUID，suid 程序受影响，§6）。解包/重打包都走 **libarchive 绑定**（`libarchive2` crate，
//! 自带静态 libarchive）—— 与 `.lpkg` 写方 lpkg 的打包器同一份实现；不再 spawn `sudo`/`tar`/`zstd`
//! CLI，也不自己解释 tar 语义（GNU sparse 惯例名、pax 的 xattr/ACL、同名重复成员都由它处理）。
//!
//! **xattr / ACL 保留：做**。两侧都交给 libarchive：打包时磁盘读取器把条目的 xattr/ACL 一并带进条目
//! （pax 键 `SCHILY.xattr.*` / `SCHILY.acl.*` 由它写出），解包时用 `ExtractFlags::{XATTR,ACL}` 还原
//! —— farm 不再自己拼 pax 键。丢 xattr 的代价是**功能性的**：`security.capability` 一丢，systemd
//! native 二进制、ping 这类靠文件能力提权的程序就废了；SUID/SGID 由 mode 保留覆盖不到它。
//!
//! repack 有效性由构建不变量保证（§6：构建成功 ⇒ needed_so 的 provider 当时都在 local repo），
//! 无需额外 guard。
//!
//! repack 只改 `needed_so`/`provides_soname`（deps 不动，由 gen_deps/deprules 规则生成；
//! **`provides`（虚拟 provider）是手写值，farm 原样保留、绝不覆盖**）。
//! **同时返回 LankeBUILD.json 需要的字段**（`update_lankebuild` 在调用方），确保仓库源定义
//! 与包内 metadata 一致（§6：把漂移 diff 落回仓库，源定义是真相）。

use crate::error::FarmError;
use std::fs;
use std::path::{Path, PathBuf};

use crate::scan;

/// tmp 残骸清理守卫：`repack_lpkg_at` 的任何提前返回/panic 都删掉半成品 `*.lpkg.tmp`，
/// 成功 `disarm()` 后不再动它。曾失败路径把 `<ver>.lpkg.tmp` 残骸永久留在 `out/<arch>/<pkg>/`。
struct TmpGuard(Option<PathBuf>);

impl TmpGuard {
    fn disarm(&mut self) {
        self.0 = None;
    }
}

impl Drop for TmpGuard {
    fn drop(&mut self) {
        if let Some(p) = self.0.take() {
            let _ = fs::remove_file(p);
        }
    }
}

/// 解包（若未解包）→ 改 metadata.json 的 needed_so/provides_soname → 重打覆盖 .lpkg。
/// **`provides`（虚拟 provider）原样保留、绝不触碰**——它是手写值，扫描不产出。
/// `extract_dir` 复用 scan 的解包目录（单包单趟）。
pub fn repack_with_metadata(
    lpkg_path: &Path,
    extract_dir: &Path,
    new_needed_so: &[String],
    new_provides_soname: &[String],
) -> Result<(), FarmError> {
    if !extract_dir.join("metadata.json").exists() {
        scan::extract_lpkg(lpkg_path, extract_dir)?;
    }
    // 1. 改 metadata.json
    let meta_path = extract_dir.join("metadata.json");
    let mut meta = scan::read_metadata_json(&meta_path)?;
    meta["needed_so"] = serde_json::Value::Array(
        new_needed_so
            .iter()
            .map(|s| serde_json::Value::String(s.clone()))
            .collect(),
    );
    meta["provides_soname"] = serde_json::Value::Array(
        new_provides_soname
            .iter()
            .map(|s| serde_json::Value::String(s.clone()))
            .collect(),
    );
    fs::write(&meta_path, serde_json::to_string_pretty(&meta).unwrap())
        .map_err(|e| format!("写 {meta_path:?} 失败: {e}"))?;
    // 2. 重打覆盖 .lpkg
    repack_lpkg(extract_dir, lpkg_path)
}

/// 把 `extract_dir` 重打成 .lpkg（zstd PAX tar，保留 mode/owner），原子替换原文件。
///
/// 遍历用 `symlink_metadata`（不 follow）——content 里的损坏 symlink（如 dbus 的
/// `var/lib/dbus/machine-id` → 包内不存在的目标）按 symlink 存，不炸 `No such file`。
/// build 每次成功都经此把容器产物归一化为发行档（level 22 + mtime 1970，字节可复现）。
fn repack_lpkg(extract_dir: &Path, out_path: &Path) -> Result<(), FarmError> {
    repack_lpkg_at(extract_dir, out_path, 22)
}

fn repack_lpkg_at(extract_dir: &Path, out_path: &Path, level: u8) -> Result<(), FarmError> {
    let tmp = out_path.with_extension("lpkg.tmp");
    // 先登记守卫（建文件/打包失败也清掉可能存在的旧残骸）
    let mut guard = TmpGuard(Some(tmp.clone()));
    // 打包必须 root（操作命令已强制）：普通用户读不到完整 mode（SUID/SGID 被内核剥掉），也读不了
    // root-only 文件（如 /etc/shadow 0600）与 `trusted.*` / `security.*` xattr。
    pack_dir_tar(extract_dir, &tmp, level)?;
    fs::rename(&tmp, out_path).map_err(|e| format!("替换 {out_path:?} 失败: {e}"))?;
    guard.disarm(); // 成功：tmp 已 rename 到目标，无需再删
    Ok(())
}

/// 把 `root` 树打成 `.lpkg`（zstd 帧 + pax tar）写进 `out_path`：先按字节序取全量路径（顺序确定是
/// "字节可复现"的前提），再交给 `crate::libarchive2_compat` 落盘。`level` = zstd 压缩级别（发行档 22）。
pub fn pack_dir_tar(root: &Path, out_path: &Path, level: u8) -> Result<(), FarmError> {
    let rel_paths = sorted_tree_paths(root)?;
    crate::libarchive_ffi::write_tree(root, out_path, level, &rel_paths)
}

/// `root` 下的全部路径（相对 `root`），**按字节序全局排序** —— 目录必然排在自己的子项之前
/// （`content/d` < `content/d/x`），tar 消费端依赖这个顺序。符号链接指向目录也不下探。
fn sorted_tree_paths(root: &Path) -> Result<Vec<PathBuf>, FarmError> {
    let mut out: Vec<PathBuf> = Vec::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let children = fs::read_dir(&dir).map_err(|e| format!("读取 {dir:?} 失败: {e}"))?;
        for child in children {
            let abs = child
                .map_err(|e| format!("读取 {dir:?} 的目录项失败: {e}"))?
                .path();
            let rel = abs.strip_prefix(root).unwrap_or(&abs).to_path_buf();
            let is_dir = fs::symlink_metadata(&abs)
                .map_err(|e| format!("stat {abs:?} 失败: {e}"))?
                .is_dir();
            if is_dir {
                stack.push(abs);
            }
            out.push(rel);
        }
    }
    out.sort();
    Ok(out)
}

#[cfg(test)]
mod tests;
