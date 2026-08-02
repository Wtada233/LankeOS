//! sources.rs — 源预下载（§8.6）：宿主侧预取网络源，源就绪才入队构建。

use std::path::Path;
use crate::tr;
use super::read_lankebuild;

pub fn pre_download_sources(pkgs_dir: &Path, pkg: &str, retries: u32) -> Result<(), String> {
    let Some(b) = read_lankebuild(pkgs_dir, pkg) else {
        return Err("无 LankeBUILD.json".to_string());
    };
    let pkg_dir = pkgs_dir.join(pkg);
    for url in b.sources.iter().chain(b.work_sources.iter()) {
        if is_skip_source(url) {
            continue;
        }
        let filename = url.rsplit('/').next().unwrap_or(url);
        let dest = pkg_dir.join(filename);
        if dest.exists() {
            continue; // 已就绪（或 operator 已放置）
        }
        crate::net::download_to_file(url, &dest, retries).map_err(|e| {
            format!("{filename}: {e}")
        })?;
        println!("{}", tr!("build.source_prefetched", pkg, filename));
    }
    Ok(())
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
    b.sources
        .iter()
        .chain(b.work_sources.iter())
        .all(|url| {
            if is_skip_source(url) {
                return true;
            }
            let filename = url.rsplit('/').next().unwrap_or(url);
            pkgs_dir.join(pkg).join(filename).exists()
        })
}
