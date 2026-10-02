//! seed.rs — 冷启动播种（§8）。
//!
//! 从远程 lankerepo 播种本地 repo：
//! 1. 下载 `<remote>/<arch>/index.txt`（graph.rs 解析，得每包版本 + SHA256 + **完整 needed_so**）；
//! 2. 逐包下载 `<remote>/<arch>/<pkg>/<ver>.lpkg`（URL 模式对齐 installation_task.cpp:380），
//!    **SHA256 校验**（index 里的 hash，防破损/篡改）；
//! 3. 落本地 repo `out/<arch>/<pkg>/<ver>.lpkg`，index.txt **原样保留**（已含全部字段）。
//!
//! index.txt 是**单一真源**：完整 needed_so 同时供 farm 的 ABI 传播（removed_sonames/revmap）
//! 与容器可见的索引用，不再剥 needed_so、不再维护第二份 .abi.json。
//! 播种得到的远程 index 即"旧索引"，Tier-1 ABI diff 从第一天就可用——无需全量构建（§8 冷启动）。

use crate::error::FarmError;
use std::fs;
use std::path::Path;

use crate::graph::Index;
use crate::tr;

/// 播种结果。
#[derive(Debug, Default, PartialEq, Eq)]
pub struct SeedReport {
    pub total: usize,
    pub ok: usize,
    pub failed: Vec<(String, String)>, // (pkg, 原因)
}

/// 从远程 repo 播种本地 repo。返回报告。`jobs` = 并行下载/解包线程数。
pub fn seed(remote: &str, arch: &str, out: &Path, jobs: usize) -> Result<SeedReport, FarmError> {
    // 1. 下载 + 解析 index.txt（完整 needed_so，单一真源）
    let index_url = format!("{remote}/{arch}/index.txt");
    let index_text = crate::net::fetch_text(&index_url)?;
    let index = Index::parse(&index_text);
    let index = &index; // 借用进各线程闭包（&Index 是 Copy，可被 move 捕获）
    let total = index.packages.len();
    let names = index.sorted_names();

    let dest_arch = out.join(arch);
    fs::create_dir_all(&dest_arch).map_err(|e| format!("创建 {dest_arch:?} 失败: {e}"))?;

    // 空索引防御：names 为空 → div_ceil 得 chunk_size=0 → chunks(0) 直接 panic。
    if names.is_empty() {
        return Ok(SeedReport::default());
    }

    // 2. 并行处理：下载（如缺）→ SHA256 校验 → 清旧版。每包目录独立，线程间无共享写。
    let jobs = jobs.clamp(1, 64);
    let chunk_size = names.len().div_ceil(jobs);
    let mut report = SeedReport {
        total,
        ok: 0,
        failed: Vec::new(),
    };
    let results: Vec<SeedReport> = std::thread::scope(|s| {
        let mut handles = Vec::new();
        for chunk in names.chunks(chunk_size) {
            let names = chunk.to_vec();
            let remote = remote.to_string();
            let dest_arch = dest_arch.clone();
            // 保留该 chunk 的包名：worker panic 时据此归因（见下方 join）
            let owned = names.clone();
            handles.push((
                owned,
                s.spawn(move || seed_chunk(&remote, arch, &dest_arch, index, &names)),
            ));
        }
        handles
            .into_iter()
            .map(|(names, h)| match h.join() {
                Ok(r) => r,
                // worker panic（内部 unwrap/越界等）**不得静默丢包**：旧实现 `unwrap_or_default()`
                // 让该 chunk 的包既不进 ok 也不进 failed，总数与 ok+failed 对不上且无任何告警。
                // 全部记 failed 并在原因里标明，让 operator 看得见。
                Err(_) => SeedReport {
                    total: names.len(),
                    ok: 0,
                    failed: names
                        .into_iter()
                        .map(|n| (n, "worker panicked".to_string()))
                        .collect(),
                },
            })
            .collect()
    });
    for r in results {
        report.ok += r.ok;
        report.failed.extend(r.failed);
    }

    // 3. 本地 index.txt **原样保留**（完整 needed_so）——不再剥、不再重写哈希
    fs::write(dest_arch.join("index.txt"), &index_text)
        .map_err(|e| format!("写本地 index.txt 失败: {e}"))?;
    Ok(report)
}

/// 一个线程的包子集：下载（如缺）→ SHA256 校验 → 清旧版本。
/// **增量**：本地已有该版本 .lpkg → 跳过（不重下，省流量）；.lpkg 保持完整元数据（不剥）。
fn seed_chunk(
    remote: &str,
    arch: &str,
    dest_arch: &Path,
    index: &Index,
    names: &[String],
) -> SeedReport {
    let mut report = SeedReport {
        total: names.len(),
        ok: 0,
        failed: Vec::new(),
    };
    for name in names {
        let info = &index.packages[name];
        let url = format!("{remote}/{arch}/{name}/{}.lpkg", info.version);
        let pkg_dir = dest_arch.join(name);
        if fs::create_dir_all(&pkg_dir).is_err() {
            report.failed.push((name.clone(), "创建目录失败".into()));
            continue;
        }
        let dest = pkg_dir.join(format!("{}.lpkg", info.version));
        match seed_one_pkg(&url, &dest, &pkg_dir, name, info) {
            Ok(()) => report.ok += 1,
            Err(e) => report.failed.push((name.clone(), e.to_string())),
        }
    }
    report
}

/// 播种单个包。
///
/// 完整性保证（曾有的漏洞）：
/// - **已有文件也必须校验 SHA256**：一次中断的下载留下的截断 .lpkg 若被"存在即 OK"
///   跳过，损坏产物会永久入驻本地 repo——哈希校验形同虚设。
/// - **下载失败必须清理残留半文件**：`download` 先 `File::create` 再写，失败会留下
///   截断文件；不清理的话下次 seed 会把半文件当"已下载"。
fn seed_one_pkg(
    url: &str,
    dest: &Path,
    pkg_dir: &Path,
    name: &str,
    info: &crate::graph::PkgInfo,
) -> Result<(), FarmError> {
    // 已有文件：增量跳过，但仍须校验哈希（防半文件/损坏被永久接受）
    if let Ok(meta) = fs::metadata(dest) {
        if meta.is_file() {
            match crate::build::sha256_file(dest) {
                Ok(h) if h == info.sha256 => {
                    keep_only_current_lpkg(pkg_dir, dest);
                    return Ok(());
                }
                _ => {
                    // 哈希校验失败（半文件/损坏）→ 删除后重新下载
                    let _ = fs::remove_file(dest);
                }
            }
        }
    }

    // 下载失败：必须清理半文件，否则下次 seed 把它当"已下载"永久接受
    if let Err(e) = crate::net::download_to_file(url, dest, 3) {
        let _ = fs::remove_file(dest);
        return Err(e);
    }

    match crate::build::sha256_file(dest) {
        Ok(h) if h == info.sha256 => {
            println!("{}", tr!("seed.progress", name, info.version));
            keep_only_current_lpkg(pkg_dir, dest);
            Ok(())
        }
        Ok(_) => {
            let _ = fs::remove_file(dest);
            Err("SHA256 不匹配".to_string().into())
        }
        Err(e) => Err(e),
    }
}

/// 清理 `pkg_dir` 下除 `keep` 外的所有 `*.lpkg`（seed 覆盖 index 后，旧版本 .lpkg 失去作用）。
fn keep_only_current_lpkg(pkg_dir: &Path, keep: &Path) {
    let Ok(rd) = fs::read_dir(pkg_dir) else {
        return;
    };
    for e in rd.flatten() {
        let p = e.path();
        if p != keep && p.extension().and_then(|x| x.to_str()) == Some("lpkg") {
            let _ = fs::remove_file(&p);
        }
    }
}

#[cfg(test)]
mod tests;
