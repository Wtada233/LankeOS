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
            handles.push(s.spawn(move || seed_chunk(&remote, arch, &dest_arch, index, &names)));
        }
        handles
            .into_iter()
            .map(|h| h.join().unwrap_or_default())
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
            Err(e) => report.failed.push((name.clone(), e)),
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
) -> Result<(), String> {
    // 已有文件：增量跳过，但仍须校验哈希（防半文件/损坏被永久接受）
    if let Ok(meta) = fs::metadata(dest) {
        if meta.is_file() {
            match sha256_file(dest) {
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
    if let Err(e) = download(url, dest) {
        let _ = fs::remove_file(dest);
        return Err(e);
    }

    match sha256_file(dest) {
        Ok(h) if h == info.sha256 => {
            println!("{}", tr!("seed.progress", name, info.version));
            keep_only_current_lpkg(pkg_dir, dest);
            Ok(())
        }
        Ok(_) => {
            let _ = fs::remove_file(dest);
            Err("SHA256 不匹配".to_string())
        }
        Err(e) => Err(e),
    }
}

/// 下载文本（index.txt）。
fn fetch(url: &str) -> Result<String, String> {
    let body = ureq::get(url)
        .set("User-Agent", crate::net::CURL_UA)
        .call()
        .map_err(|e| format!("GET {url}: {e}"))?;
    body.into_string().map_err(|e| format!("读 {url}: {e}"))
}

/// 流式下载到文件。
fn download(url: &str, dest: &Path) -> Result<(), String> {
    let resp = ureq::get(url)
        .set("User-Agent", crate::net::CURL_UA)
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

    /// 起一个只响应一次的本地 HTTP 服务，返回绑定端口。
    fn serve_once(bytes: Vec<u8>) -> u16 {
        use std::io::{Read, Write};
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        std::thread::spawn(move || {
            if let Ok((mut sock, _)) = listener.accept() {
                let mut buf = [0u8; 4096];
                let _ = sock.read(&mut buf);
                let head = format!(
                    "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    bytes.len()
                );
                let _ = sock.write_all(head.as_bytes());
                let _ = sock.write_all(&bytes);
            }
        });
        port
    }

    fn pkg_info(sha256: &str) -> crate::graph::PkgInfo {
        crate::graph::PkgInfo {
            name: "p".into(),
            version: "1.0".into(),
            sha256: sha256.into(),
            deps: vec![],
            provides: vec![],
            needed_so: vec![],
        }
    }

    #[test]
    fn valid_existing_file_skips_download_and_hash_matches() {
        // 已有文件哈希正确 → 直接保留，不碰网络（远端不可达也不报错）
        let dir = std::env::temp_dir().join(format!("farm-seed-valid-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let dest = dir.join("1.0.lpkg");
        fs::write(&dest, b"hello").unwrap();
        let sha = format!("{:x}", Sha256::digest(b"hello"));

        let res = seed_one_pkg(
            "http://127.0.0.1:1/unreachable.lpkg",
            &dest,
            &dir,
            "p",
            &pkg_info(&sha),
        );
        assert!(res.is_ok());
        assert_eq!(fs::read(&dest).unwrap(), b"hello", "有效已有文件不得被重写");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn corrupt_existing_file_is_removed_not_accepted() {
        // 已有文件哈希不符（半文件/损坏）→ 绝不"存在即 OK"，先删除；
        // 远端不可达时下载失败 → 也不得留下半文件
        let dir = std::env::temp_dir().join(format!("farm-seed-corrupt-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let dest = dir.join("1.0.lpkg");
        fs::write(&dest, b"partial-garbage").unwrap();
        let sha = format!("{:x}", Sha256::digest(b"hello"));

        let res = seed_one_pkg(
            "http://127.0.0.1:1/unreachable.lpkg",
            &dest,
            &dir,
            "p",
            &pkg_info(&sha),
        );
        assert!(res.is_err(), "损坏文件 + 远端不可达 → 必须失败");
        assert!(!dest.exists(), "损坏文件不得被接受，下载失败也不得留半文件");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn corrupt_existing_file_is_redownloaded_and_validated() {
        // 已有文件损坏 + 远端可达 → 删除后重下，且新文件经哈希校验
        let dir = std::env::temp_dir().join(format!("farm-seed-redl-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let dest = dir.join("1.0.lpkg");
        fs::write(&dest, b"garbage").unwrap();

        let content = b"hello".to_vec();
        let sha = format!("{:x}", Sha256::digest(&content));
        let port = serve_once(content.clone());
        let url = format!("http://127.0.0.1:{port}/p/1.0.lpkg");

        let res = seed_one_pkg(&url, &dest, &dir, "p", &pkg_info(&sha));
        assert!(res.is_ok());
        assert_eq!(fs::read(&dest).unwrap(), b"hello", "重下的文件应校验通过");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn seed_empty_index_returns_empty_report() {
        // 空 index.txt → 不得 panic（chunks(0)）
        let out = std::env::temp_dir().join(format!("farm-seed-empty-{}", std::process::id()));
        let _ = fs::remove_dir_all(&out);
        fs::create_dir_all(&out).unwrap();
        let port = serve_once(Vec::new());
        let res = seed(&format!("http://127.0.0.1:{port}"), "x86_64", &out, 8);
        assert!(res.is_ok(), "空索引应返回空报告而非 panic: {res:?}");
        assert_eq!(res.unwrap().total, 0);
        fs::remove_dir_all(&out).ok();
    }
}
