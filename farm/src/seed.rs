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

use std::fs;
use std::io::Write;
use std::path::Path;

use crate::graph::Index;
use crate::tr;
use sha2::{Digest, Sha256};

/// 播种结果。
#[derive(Debug, Default, PartialEq, Eq)]
pub struct SeedReport {
    pub total: usize,
    pub ok: usize,
    pub failed: Vec<(String, String)>, // (pkg, 原因)
}

/// 从远程 repo 播种本地 repo。返回报告。`jobs` = 并行下载/解包线程数。
pub fn seed(remote: &str, arch: &str, out: &Path, jobs: usize) -> Result<SeedReport, String> {
    // 1. 下载 + 解析 index.txt（完整 needed_so，单一真源）
    let index_url = format!("{remote}/{arch}/index.txt");
    let index_text = fetch(&index_url)?;
    let index = Index::parse(&index_text);
    let index = &index; // 借用进各线程闭包（&Index 是 Copy，可被 move 捕获）
    let total = index.packages.len();
    let names = index.sorted_names();

    let dest_arch = out.join(arch);
    fs::create_dir_all(&dest_arch).map_err(|e| format!("创建 {dest_arch:?} 失败: {e}"))?;

    // 2. 并行处理：下载（如缺）→ SHA256 校验 → 清旧版。每包目录独立，线程间无共享写。
    let jobs = jobs.clamp(1, 64);
    let chunk_size = names.len().div_ceil(jobs);
    let mut report = SeedReport { total, ok: 0, failed: Vec::new() };
    let results: Vec<SeedReport> = std::thread::scope(|s| {
        let mut handles = Vec::new();
        for chunk in names.chunks(chunk_size) {
            let names = chunk.to_vec();
            let remote = remote.to_string();
            let dest_arch = dest_arch.clone();
            handles.push(s.spawn(move || seed_chunk(&remote, arch, &dest_arch, index, &names)));
        }
        handles.into_iter().map(|h| h.join().unwrap_or_default()).collect()
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
    let mut report = SeedReport { total: names.len(), ok: 0, failed: Vec::new() };
    for name in names {
        let info = &index.packages[name];
        let url = format!("{remote}/{arch}/{name}/{}.lpkg", info.version);
        let pkg_dir = dest_arch.join(name);
        if fs::create_dir_all(&pkg_dir).is_err() {
            report.failed.push((name.clone(), "创建目录失败".into()));
            continue;
        }
        let dest = pkg_dir.join(format!("{}.lpkg", info.version));
        if dest.exists() {
            report.ok += 1; // 已下载（增量跳过）
            keep_only_current_lpkg(&pkg_dir, &dest);
            continue;
        }
        match download(&url, &dest) {
            Ok(()) => match sha256_file(&dest) {
                Ok(h) if h == info.sha256 => {
                    println!("{}", tr!("seed.progress", name, info.version));
                    report.ok += 1;
                    keep_only_current_lpkg(&pkg_dir, &dest);
                }
                Ok(_) => {
                    report.failed.push((name.clone(), "SHA256 不匹配".into()));
                    fs::remove_file(&dest).ok();
                }
                Err(e) => report.failed.push((name.clone(), e)),
            },
            Err(e) => report.failed.push((name.clone(), e)),
        }
    }
    report
}

/// 下载文本（index.txt）。
fn fetch(url: &str) -> Result<String, String> {
    let body = ureq::get(url)
        .set("User-Agent", crate::net::BROWSER_UA)
        .call()
        .map_err(|e| format!("GET {url}: {e}"))?;
    body.into_string().map_err(|e| format!("读 {url}: {e}"))
}

/// 流式下载到文件。
fn download(url: &str, dest: &Path) -> Result<(), String> {
    let resp = ureq::get(url)
        .set("User-Agent", crate::net::BROWSER_UA)
        .call()
        .map_err(|e| format!("GET {url}: {e}"))?;
    let mut f = fs::File::create(dest).map_err(|e| format!("创建 {dest:?} 失败: {e}"))?;
    std::io::copy(&mut resp.into_reader(), &mut f)
        .map_err(|e| format!("下载 {url} 写盘失败: {e}"))?;
    f.flush().map_err(|e| format!("flush 失败: {e}"))?;
    Ok(())
}

fn sha256_file(path: &Path) -> Result<String, String> {
    let data = fs::read(path).map_err(|e| format!("读 {path:?} 失败: {e}"))?;
    let mut hasher = Sha256::new();
    hasher.update(&data);
    Ok(format!("{:x}", hasher.finalize()))
}

/// 清理 `pkg_dir` 下除 `keep` 外的所有 `*.lpkg`（seed 覆盖 index 后，旧版本 .lpkg 失去作用）。
fn keep_only_current_lpkg(pkg_dir: &Path, keep: &Path) {
    let Ok(rd) = fs::read_dir(pkg_dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if p != keep && p.extension().and_then(|x| x.to_str()) == Some("lpkg") {
            let _ = fs::remove_file(&p);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sha256_matches_known() {
        let f = std::env::temp_dir().join("farm-sha-test");
        fs::write(&f, b"hello").unwrap();
        // sha256("hello") = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
        assert_eq!(
            sha256_file(&f).unwrap(),
            "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
        );
        fs::remove_file(&f).ok();
    }

    #[test]
    fn keep_only_current_lpkg_removes_stale_versions() {
        let dir = std::env::temp_dir().join(format!("farm-seed-clean-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let old = dir.join("1.0.lpkg");
        let cur = dir.join("1.1+2.lpkg");
        let unrelated = dir.join("readme.txt");
        fs::write(&old, b"x").unwrap();
        fs::write(&cur, b"x").unwrap();
        fs::write(&unrelated, b"x").unwrap();

        keep_only_current_lpkg(&dir, &cur);
        assert!(cur.exists(), "当前版本应保留");
        assert!(!old.exists(), "旧版本应被清理");
        assert!(unrelated.exists(), "非 .lpkg 文件不应被碰");

        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn parse_keeps_full_needed_so() {
        // seed 不再剥 needed_so：Index::parse 直接拿到完整 SONAME（ABI 传播的单一真源）
        let idx = Index::parse("pkg|1.0:h:deps:liba.so.1,libb.so:libc.so.6,libm.so.6\n");
        let p = &idx.packages["pkg"];
        assert_eq!(p.needed_so, vec!["libc.so.6", "libm.so.6"]);
        assert_eq!(p.provides, vec!["liba.so.1", "libb.so"]);
        assert_eq!(p.sha256, "h");
    }
}
