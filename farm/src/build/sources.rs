//! sources.rs — 源预下载（§8.6）：宿主侧预取网络源，源就绪才入队构建。

use super::read_lankebuild;
use crate::tr;
use std::path::Path;

pub fn pre_download_sources(pkgs_dir: &Path, pkg: &str, retries: u32) -> Result<(), String> {
    let Some(b) = read_lankebuild(pkgs_dir, pkg) else {
        return Err("无 LankeBUILD.json".to_string());
    };
    let pkg_dir = pkgs_dir.join(pkg);
    let mut seen: std::collections::HashSet<String> = std::collections::HashSet::new();
    for url in b.sources.iter().chain(b.work_sources.iter()) {
        if is_skip_source(url) {
            continue;
        }
        let filename = source_filename(url);
        if filename.is_empty() {
            return Err(format!("{url}: 源 URL 无文件名（裸目录 URL 不可作为源）"));
        }
        // 同名落盘冲突 → 显式报错而非静默跳过第二个（曾导致第二个源永不预取、source 门控误判）
        if !seen.insert(filename.to_string()) {
            return Err(format!(
                "{url}: 与另一源解析出相同文件名 {filename:?}（query/fragment 已剥离），无法预下载"
            ));
        }
        let dest = pkg_dir.join(filename);
        if dest.exists() {
            continue; // 已就绪（或 operator 已放置）
        }
        crate::net::download_to_file(url, &dest, retries)
            .map_err(|e| format!("{filename}: {e}"))?;
        println!("{}", tr!("build.source_prefetched", pkg, filename));
    }
    Ok(())
}

/// 从 URL 取落盘文件名：剥掉 query（`?download=1`）与 fragment（`#...`）。
/// 曾 `url.rsplit('/').next()` 直接取——带 query 的 URL 会得到 `foo.tar.gz?download=1`，
/// 落盘的文件名错误，且与真实下载目标不一致。
fn source_filename(url: &str) -> &str {
    let base = url.rsplit('/').next().unwrap_or(url);
    let end = base
        .find('?')
        .or_else(|| base.find('#'))
        .unwrap_or(base.len());
    &base[..end]
}

/// file:// 与 git+ 源跳过（§8.6 / git src 由 lpkg libgit2 处理）。
fn is_skip_source(url: &str) -> bool {
    url.starts_with("file://") || url.starts_with("git+")
}

/// 所有网络源的文件是否已就绪（source-missing 后 operator 放置文件 → 差分应重建）。
#[cfg(test)]
pub fn sources_ready(pkgs_dir: &Path, pkg: &str) -> bool {
    let Some(b) = read_lankebuild(pkgs_dir, pkg) else {
        return false;
    };
    b.sources.iter().chain(b.work_sources.iter()).all(|url| {
        if is_skip_source(url) {
            return true;
        }
        let filename = source_filename(url);
        !filename.is_empty() && pkgs_dir.join(pkg).join(filename).exists()
    })
}
