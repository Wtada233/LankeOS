//! `src/build/repo.rs` 的单元测试。
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
        provides: vec![],
        provides_soname: vec!["liba.so.1".into()],
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
        &["virtualcap".to_string()],
        &["libmypkg.so.1".to_string()],
        &["libc.so.6".to_string()],
    )
    .unwrap();

    let content = fs::read_to_string(arch_dir.join("index.txt")).unwrap();
    // 新格式：6 冒号字段 + 3 个 `|` 段（虚拟 provides 与 provides_soname 各占一列）
    assert!(
        content.contains("mypkg|1.0:hash123:glibc>=2.34,bash:virtualcap:libmypkg.so.1:libc.so.6|"),
        "index.txt 应包含新格式行（deps/provides/provides_soname/needed_so）: {content}"
    );
    // 写侧产出的行必须能被读侧吃回（往返）
    let idx = crate::graph::Index::parse(&content);
    let p = &idx.packages["mypkg"];
    assert_eq!(p.deps, vec!["glibc>=2.34", "bash"]);
    assert_eq!(p.provides, vec!["virtualcap"]);
    assert_eq!(p.provides_soname, vec!["libmypkg.so.1"]);
    assert_eq!(p.needed_so, vec!["libc.so.6"]);
    fs::remove_dir_all(&out).ok();
}

/// **行为变更的钉子**：元数据双写只更新 needed_so/provides_soname，**绝不触碰手写的虚拟
/// provides**（旧实现把扫描结果直接盖上去，真实仓库 100% 的虚拟 provider 因此永久丢失）。
#[test]
fn update_lankebuild_metadata_preserves_handwritten_provides() {
    let dir = std::env::temp_dir().join(format!("farm-lb-preserve-{}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    let pkg_dir = dir.join("p");
    fs::create_dir_all(&pkg_dir).unwrap();
    fs::write(
        pkg_dir.join("LankeBUILD.json"),
        r#"{"name":"p","version":"1.0","provides":["virtualcap"],"provides_soname":[],"needed_so":[]}"#,
    )
    .unwrap();

    let outcome = BuildOutcome {
        ok: true,
        needed_so: vec!["libc.so.6".into(), "libm.so.6".into()],
        provides: vec!["virtualcap".into()],
        provides_soname: vec!["libp.so.1".into()],
        deps: vec![],
        failure_stage: None,
        lpkg_path: None,
    };
    update_lankebuild_metadata(&dir, "p", &outcome);

    let v: serde_json::Value =
        serde_json::from_str(&fs::read_to_string(pkg_dir.join("LankeBUILD.json")).unwrap())
            .unwrap();
    assert_eq!(
        v["provides"],
        serde_json::json!(["virtualcap"]),
        "手写虚拟 provides 不得被扫描结果覆盖"
    );
    assert_eq!(
        v["provides_soname"],
        serde_json::json!(["libp.so.1"]),
        "provides_soname 应写为扫描实际值"
    );
    assert_eq!(
        v["needed_so"],
        serde_json::json!(["libc.so.6", "libm.so.6"])
    );
    fs::remove_dir_all(&dir).ok();
}
