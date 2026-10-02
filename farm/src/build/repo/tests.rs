//! `src/build/repo.rs` 的单元测试（从生产文件搬出：那里测试比生产还长，正文被淹没）。
//! 仍是同一父模块的子模块 ⇒ `use super::*` 与 crate 内私有项照旧可用。

use super::*;

#[test]
fn removed_soname_file_matches_exact_soname() {
    let removed = ["libfoo.so.2"];
    // SONAME 本体 + 其实体版本文件 → 匹配
    assert!(is_removed_soname_file("libfoo.so.2", &removed));
    assert!(is_removed_soname_file("libfoo.so.2.1.3", &removed));
    // 精确前缀（`r.`），绝不误匹配别的 major：libfoo.so.2 不该吞掉 libfoo.so.20
    assert!(
        !is_removed_soname_file("libfoo.so.20", &removed),
        "不应误匹配 libfoo.so.20"
    );
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

#[test]
fn repack_if_drift_errors_when_metadata_missing() {
    // 曾静默返回 false（当作"无漂移"）→ 照发陈旧 metadata，.lpkg 与 index 永久失配。
    // 现在 metadata.json 不可读必须 Err（上层 BLOCK，不得静默发布）。
    let out = std::env::temp_dir().join(format!("farm-repack-meta-{}", std::process::id()));
    let _ = fs::remove_dir_all(&out);
    fs::create_dir_all(out.join("extract").join("p")).unwrap();
    fs::create_dir_all(out.join(".staging").join("p")).unwrap();
    let lpkg = out.join(".staging").join("p").join("p-1.0.lpkg");
    fs::write(&lpkg, b"not-a-real-lpkg").unwrap();

    let opts = BuildOptions {
        pkgs_dir: out.join("pkgs"),
        out_dir: out.clone(),
        targets: vec!["p".into()],
        arch: "x86_64".into(),
        image: String::new(),
        download_retries: 1,
        interactive: false,
        build_data_dir: std::path::PathBuf::from("data/build"),
        validate: false,
        manual_sort: false,
    };
    let outcome = BuildOutcome {
        ok: true,
        needed_so: vec![],
        provides: vec!["liba.so.1".into()],
        deps: vec![],
        failure_stage: None,
        lpkg_path: Some(lpkg),
    };

    let res = repack_if_drift(&outcome, &opts, "p");
    assert!(res.is_err(), "metadata.json 缺失必须报错而非静默当无漂移");
    fs::remove_dir_all(&out).ok();
}

#[test]
fn update_repo_index_writes_deps() {
    let out = std::env::temp_dir().join(format!("farm-repo-deps-test-{}", std::process::id()));
    let _ = fs::remove_dir_all(&out);
    let arch_dir = out.join("x86_64");
    fs::create_dir_all(&arch_dir).unwrap();
    fs::write(arch_dir.join("index.txt"), "# index\n").unwrap();

    update_repo_index(
        &out,
        "x86_64",
        "mypkg",
        "1.0",
        "hash123",
        &["glibc>=2.34".to_string(), "bash".to_string()],
        &["libmypkg.so.1".to_string()],
        &["libc.so.6".to_string()],
    )
    .unwrap();

    let content = fs::read_to_string(arch_dir.join("index.txt")).unwrap();
    assert!(
        content.contains("mypkg|1.0:hash123:glibc>=2.34,bash:libmypkg.so.1:libc.so.6|"),
        "index.txt 应包含转述的 deps: {content}"
    );
    fs::remove_dir_all(&out).ok();
}
