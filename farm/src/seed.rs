//! seed.rs — 冷启动播种（§8）。
//!
//! 从远程 lankerepo 播种本地 repo：
//! 1. 下载 `<remote>/<arch>/index.txt`（graph.rs 解析，得每包版本 + SHA256）；
//! 2. 逐包下载 `<remote>/<arch>/<pkg>/<ver>.lpkg`（URL 模式对齐 installation_task.cpp:380），
//!    **SHA256 校验**（index 里的 hash，防破损/篡改）；
//! 3. 落本地 repo `out/<arch>/<pkg>/<ver>.lpkg`，index.txt 原样保留（已含全部字段）。
//!
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
    // 1. 下载 + 解析 index.txt
    let index_url = format!("{remote}/{arch}/index.txt");
    let index_text = fetch(&index_url)?;
    let index = Index::parse(&index_text);
    // farm 自己的 ABI 数据库：完整 provides + needed_so 灌入（容器索引随后剥掉 needed_so，
    // 传播基线从这里读，见 abidb.rs）。
    crate::abidb::write_all(out, arch, &index)?;
    let index = &index; // 借用进各线程闭包（&Index 是 Copy，可被 move 捕获）
    let total = index.packages.len();
    let names = index.sorted_names();

    let dest_arch = out.join(arch);
    fs::create_dir_all(&dest_arch).map_err(|e| format!("创建 {dest_arch:?} 失败: {e}"))?;

    // 2. 并行处理：下载 + 校验 + 剥 needed_so + 清旧版。每包目录独立，线程间无共享写。
    //    `stripped_hashes` 收集剥离后 .lpkg 的 SHA256（index.txt 要写它，lpkg 装包校验才过）。
    let jobs = jobs.clamp(1, 64);
    let chunk_size = names.len().div_ceil(jobs);
    let stripped_hashes: std::sync::Mutex<std::collections::HashMap<String, String>> =
        std::sync::Mutex::new(std::collections::HashMap::new());
    let hashes_ref = &stripped_hashes; // 引用捕获（&Mutex 是 Copy，move 闭包不夺所有权）
    let mut report = SeedReport { total, ok: 0, failed: Vec::new() };
    let results: Vec<SeedReport> = std::thread::scope(|s| {
        let mut handles = Vec::new();
        for chunk in names.chunks(chunk_size) {
            let names = chunk.to_vec();
            let remote = remote.to_string();
            let dest_arch = dest_arch.clone();
            handles.push(s.spawn(move || {
                seed_chunk(&remote, arch, &dest_arch, index, &names, hashes_ref)
            }));
        }
        handles.into_iter().map(|h| h.join().unwrap_or_default()).collect()
    });
    for r in results {
        report.ok += r.ok;
        report.failed.extend(r.failed);
    }

    // 3. 本地 index.txt：剥掉 needed_so + 写剥离后哈希（容器可见，lpkg 的一致性检查失效、
    //    装包哈希校验通过；完整 needed_so 在 abidb）
    let hashes = stripped_hashes.into_inner().unwrap_or_default();
    fs::write(dest_arch.join("index.txt"), build_index_text(&index_text, &hashes))
        .map_err(|e| format!("写本地 index.txt 失败: {e}"))?;
    Ok(report)
}

/// 一个线程的包子集：下载（如缺）→ SHA256 校验 → 剥 needed_so → 记录剥离后哈希 → 清旧版本。
/// **增量**：本地已有该版本 .lpkg 且 metadata 的 needed_so 已剥空 → 跳过（不重下，省流量）；
/// 已下载未剥 → 只补剥。`hashes` 收集剥离后 SHA256（index.txt 用）。
fn seed_chunk(
    remote: &str,
    arch: &str,
    dest_arch: &Path,
    index: &Index,
    names: &[String],
    hashes: &std::sync::Mutex<std::collections::HashMap<String, String>>,
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
        let extract = dest_arch.join(".seed-strip").join(name);
        // 剥离完立即清掉解包目录（387 个包的 content 累起来是十几个 G）
        let strip_and_cleanup = |dest: &Path, extract: &Path| -> Result<(), String> {
            let r = strip_lpkg_needed_so(dest, extract);
            let _ = fs::remove_dir_all(extract);
            r
        };

        let mut present = false;
        if dest.exists() {
            // 本地已有该版本：读 metadata 判断是否已剥。已剥 → 增量跳过（不重下）。
            match crate::scan::read_lpkg_metadata(&dest) {
                Ok(meta) => {
                    present = true;
                    let stripped = meta["needed_so"]
                        .as_array()
                        .map(|a| a.is_empty())
                        .unwrap_or(false);
                    if !stripped {
                        if let Err(e) = strip_and_cleanup(&dest, &extract) {
                            report.failed.push((name.clone(), format!("剥离 needed_so 失败: {e}")));
                        }
                    }
                }
                Err(e) => {
                    report.failed.push((name.clone(), format!("读本地 .lpkg 失败: {e}")));
                }
            }
        } else {
            // 下载 + 远端哈希校验
            match download(&url, &dest) {
                Ok(()) => match sha256_file(&dest) {
                    Ok(h) if h == info.sha256 => {
                        println!("{}", tr!("seed.progress", name, info.version));
                        present = true;
                        if let Err(e) = strip_and_cleanup(&dest, &extract) {
                            report.failed.push((name.clone(), format!("剥离 needed_so 失败: {e}")));
                        }
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

        if present {
            // 记录剥离后（或已剥）哈希 → index.txt 写它，lpkg 装包哈希校验通过
            if let Ok(h) = sha256_file(&dest) {
                hashes.lock().unwrap().insert(name.clone(), h);
            }
            report.ok += 1;
            keep_only_current_lpkg(&pkg_dir, &dest);
        }
    }
    report
}

/// 生成容器可见的 index.txt：每行剥掉 needed_so，并用**剥离后 .lpkg 的哈希**替换远端哈希
/// （lpkg 装包校验哈希，写远端哈希会对不上剥离后的本地 .lpkg）。
/// 格式：`pkg|ver:hash:deps:provides:needed_so[;ver2...]|pkg_level`。
fn build_index_text(text: &str, hashes: &std::collections::HashMap<String, String>) -> String {
    let mut out = String::new();
    for line in text.lines() {
        if line.trim().is_empty() || line.starts_with('#') {
            out.push_str(line);
            out.push('\n');
            continue;
        }
        let mut parts = line.splitn(3, '|');
        let name = parts.next().unwrap_or("");
        let rest = parts.next().unwrap_or("");
        let pkg_level = parts.next().unwrap_or("");
        let mut blocks: Vec<String> = Vec::new();
        for block in rest.split(';') {
            let v: Vec<&str> = block.splitn(6, ':').collect();
            let ver = v.first().copied().unwrap_or("");
            let deps = v.get(2).copied().unwrap_or("");
            let prov = v.get(3).copied().unwrap_or("");
            let hash = hashes
                .get(name)
                .map(|s| s.as_str())
                .unwrap_or_else(|| v.get(1).copied().unwrap_or(""));
            blocks.push(format!("{ver}:{hash}:{deps}:{prov}:"));
        }
        out.push_str(&format!("{name}|{}|{pkg_level}\n", blocks.join(";")));
    }
    out
}

/// 剥掉 .lpkg 内 metadata.json 的 needed_so（保留 provides），重打覆盖。
fn strip_lpkg_needed_so(lpkg_path: &Path, extract_dir: &Path) -> Result<(), String> {
    crate::scan::extract_lpkg(lpkg_path, extract_dir)?;
    let meta = crate::scan::read_metadata_json(&extract_dir.join("metadata.json"))?;
    let provides: Vec<String> = meta["provides"]
        .as_array()
        .map(|a| a.iter().filter_map(|v| v.as_str().map(String::from)).collect())
        .unwrap_or_default();
    crate::repack::repack_with_metadata(lpkg_path, extract_dir, &[], &provides)
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
    fn build_index_text_strips_soname_and_rewrites_hash() {
        use std::collections::HashMap;
        let text = "pkg|1.0:h:deps:liba.so.1,libb.so:libc.so.6,libm.so.6|extra\n\
                    # comment\n\
                    lib2|2.0:hh::prov:need1,need2|\n";
        let mut hashes = HashMap::new();
        hashes.insert("pkg".to_string(), "stripped-hash".to_string());
        let out = build_index_text(text, &hashes);
        assert!(
            out.contains("pkg|1.0:stripped-hash:deps:liba.so.1,libb.so:|extra"),
            "needed_so 剥掉 + 哈希替换: {out}"
        );
        assert!(!out.contains("libc.so.6"), "needed_so 内容不应残留: {out}");
        assert!(out.contains("# comment"), "注释应保留");
        // 无剥离哈希的包（下载失败）：保留远端哈希，剥 needed_so
        assert!(out.contains("lib2|2.0:hh::prov:|"), "第二行剥 needed_so、保留远端哈希: {out}");
    }
}
