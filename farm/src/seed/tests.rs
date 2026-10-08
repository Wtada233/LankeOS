//! `src/seed.rs` 的单元测试。
//! 仍是同一父模块的子模块 ⇒ `use super::*` 与 crate 内私有项照旧可用。

use super::*;
use crate::build::sha256_file;
use sha2::{Digest, Sha256};

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
    let cur = dir.join("1.1-2.lpkg");
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
    let idx = Index::parse("pkg|1.0:h:deps::liba.so.1,libb.so:libc.so.6,libm.so.6|\n");
    let p = &idx.packages["pkg"];
    assert_eq!(p.needed_so, vec!["libc.so.6", "libm.so.6"]);
    assert_eq!(p.provides_soname, vec!["liba.so.1", "libb.so"]);
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
        provides_soname: vec![],
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
