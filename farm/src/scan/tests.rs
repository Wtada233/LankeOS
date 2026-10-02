//! `src/scan.rs` 的单元测试（从生产文件搬出：那里测试比生产还长，正文被淹没）。
//! 仍是同一父模块的子模块 ⇒ `use super::*` 与 crate 内私有项照旧可用。

use super::*;

#[test]
fn basename_strips_path() {
    assert_eq!(basename("libc.so.6"), "libc.so.6");
    assert_eq!(basename("/usr/lib/libm.so.6"), "libm.so.6");
}

#[test]
fn in_system_lib_dir_matches_standard_paths() {
    let c = Path::new("/x/content");
    assert!(in_system_lib_dir(
        Path::new("/x/content/usr/lib/libc.so.6"),
        c
    ));
    assert!(in_system_lib_dir(Path::new("/x/content/lib/libz.so.1"), c));
    assert!(in_system_lib_dir(
        Path::new("/x/content/usr/lib64/libm.so"),
        c
    ));
    // 捆绑路径排除
    assert!(!in_system_lib_dir(
        Path::new("/x/content/usr/lib/chromium/libnss3.so"),
        c
    ));
    assert!(!in_system_lib_dir(Path::new("/x/content/usr/bin/lpkg"), c));
}

#[test]
fn collect_files_includes_symlinks_and_broken() {
    let tmp = std::env::temp_dir().join(format!("farm-scan-collect-{}", std::process::id()));
    let _ = fs::remove_dir_all(&tmp);
    let content = tmp.join("content");
    fs::create_dir_all(content.join("sub")).unwrap();
    fs::write(content.join("b.txt"), b"x").unwrap();
    fs::write(content.join("sub/a.txt"), b"x").unwrap();
    std::os::unix::fs::symlink("b.txt", content.join("link")).unwrap();
    std::os::unix::fs::symlink("nope", content.join("broken")).unwrap();

    let mut out = Vec::new();
    collect_files(&content, &mut out);
    // 含普通文件与（含损坏的）符号链接，且只对目录递归
    let names: std::collections::HashSet<String> = out
        .iter()
        .map(|p| p.file_name().unwrap().to_string_lossy().into_owned())
        .collect();
    assert_eq!(
        names,
        ["b.txt", "a.txt", "link", "broken"]
            .iter()
            .map(|s| s.to_string())
            .collect(),
        "应含普通文件与符号链接（含损坏的）: {out:?}"
    );
    // 顺序无关（下游是 HashSet），但同输入两次运行必须一致
    let mut again = Vec::new();
    collect_files(&content, &mut again);
    assert_eq!(out, again, "同输入两次遍历顺序应一致");
    let _ = fs::remove_dir_all(&tmp);
}

#[test]
fn is_elf_magic_check() {
    let f = std::env::temp_dir().join("farm-scan-elf-test");
    std::fs::write(&f, [0x7f, b'E', b'L', b'F', 2, 1, 1]).unwrap();
    assert!(is_elf(&f));
    std::fs::write(&f, b"#!/bin/sh\n").unwrap();
    assert!(!is_elf(&f));
    std::fs::remove_file(&f).ok();
}

/// 复现 libmagic 重复 provides：同一 SONAME 被符号链接分支（文件名）和 ELF 分支（SONAME）
/// 各贡献一次。构造 content/usr/lib/libc.so.6（真实 ELF）+ lib/libc.so.6 与
/// usr/lib/libc.so 两个符号链接 → 无去重时 libc.so.6 出现两次。
#[test]
fn scan_content_dedups_provides_from_symlink_and_soname() {
    let host_lib = [
        "/usr/lib/libc.so.6",
        "/lib/x86_64-linux-gnu/libc.so.6",
        "/usr/lib/x86_64-linux-gnu/libc.so.6",
        "/lib/libc.so.6",
    ]
    .iter()
    .find_map(|p| std::fs::canonicalize(p).ok());
    let Some(src) = host_lib else {
        eprintln!("{}", crate::tr!("test.skip_host_libc"));
        return;
    };
    let tmp = std::env::temp_dir().join(format!("farm-scan-dedup-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&tmp);
    let content = tmp.join("content");
    std::fs::create_dir_all(content.join("usr/lib")).unwrap();
    std::fs::create_dir_all(content.join("lib")).unwrap();
    std::fs::copy(&src, content.join("usr/lib/libc.so.6")).unwrap();
    // 符号链接 basename=libc.so.6，与真实 ELF 的 SONAME（或回退文件名）相同 → 撞车
    std::os::unix::fs::symlink("../usr/lib/libc.so.6", content.join("lib/libc.so.6")).unwrap();
    std::os::unix::fs::symlink("libc.so.6", content.join("usr/lib/libc.so")).unwrap();

    let (_, provides) = scan_content(&content, &Default::default());
    assert_eq!(provides, vec!["libc.so", "libc.so.6"]); // libc.so.6 只出现一次

    let _ = std::fs::remove_dir_all(&tmp);
}

/// perl 的 RPATH → /usr/lib/perl5/.../CORE（绝对），libperl.so 随包安装在该目录（无 SONAME）。
/// 扫描应判定"自提供"→ 从 needed_so 忽略 libperl.so（否则 lpkg 装 perl 都报无提供者）。
#[test]
fn scan_content_ignores_self_provided_via_abs_rpath() {
    let host_lib = [
        "/usr/lib/libc.so.6",
        "/lib/x86_64-linux-gnu/libc.so.6",
        "/usr/lib/x86_64-linux-gnu/libc.so.6",
        "/lib/libc.so.6",
    ]
    .iter()
    .find_map(|p| std::fs::canonicalize(p).ok());
    let Some(src) = host_lib else {
        eprintln!("{}", crate::tr!("test.skip_host_libc"));
        return;
    };
    let tmp = std::env::temp_dir().join(format!("farm-scan-rpath-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&tmp);
    let content = tmp.join("content");
    let core = content.join("usr/lib/perl5/5.44/core_perl/CORE");
    std::fs::create_dir_all(&core).unwrap();
    std::fs::create_dir_all(content.join("usr/bin")).unwrap();
    // 用宿主 libc 充当"libperl.so"（ELF、无 SONAME）与"perl"（含 RPATH 的 ELF）
    std::fs::copy(&src, core.join("libperl.so")).unwrap();
    std::fs::copy(&src, content.join("usr/bin/perl")).unwrap();

    let (needed, provides) = scan_content(&content, &Default::default());
    // 非标准目录的 ELF .so（子目录）不加入 provides（回归原逻辑：只提供搜索路径 .so）
    assert!(!provides.contains(&"/usr/lib/perl5/5.44/core_perl/CORE/libperl.so".to_string()));
    // 包内同名 .so（任何路径）→ needed_so 排除（not-found/自提供）
    assert!(!needed.contains(&"libperl.so".to_string()));

    let _ = std::fs::remove_dir_all(&tmp);
}

/// 仓库 provider map 的 not-found 过滤：needed_so 条目不在 repo_provides → 判 not found → 不进
/// needed_so（postgresql plperl.so 链接 libperl.so，perl 不提供 → 剔除，否则 redland 构建
/// 装 postgresql 时报 "libperl.so 无提供者"）。
#[test]
fn scan_content_filters_needed_by_repo_provides() {
    let host_lib = [
        "/usr/lib/libc.so.6",
        "/lib/x86_64-linux-gnu/libc.so.6",
        "/usr/lib/x86_64-linux-gnu/libc.so.6",
        "/lib/libc.so.6",
    ]
    .iter()
    .find_map(|p| std::fs::canonicalize(p).ok());
    let Some(src) = host_lib else {
        eprintln!("{}", crate::tr!("test.skip_host_libc"));
        return;
    };
    let tmp = std::env::temp_dir().join(format!("farm-scan-notfound-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&tmp);
    let content = tmp.join("content");
    std::fs::create_dir_all(content.join("usr/bin")).unwrap();
    std::fs::copy(&src, content.join("usr/bin/testbin")).unwrap();

    // 空 repo_provides → 全部 not-found → needed_so 空
    let (needed, _) = scan_content(&content, &Default::default());
    assert!(
        needed.is_empty(),
        "空 provider map 应全部 not-found: {needed:?}"
    );

    // repo_provides = 二进制实际 NEEDED → 保留
    let bytes = std::fs::read(content.join("usr/bin/testbin")).unwrap();
    let (_, bin_needed) = parse_elf_dynamic(&bytes);
    let rp: HashSet<String> = bin_needed.iter().map(|n| basename(n)).collect();
    let (needed2, _) = scan_content(&content, &rp);
    assert!(!needed2.is_empty(), "provider 齐全时 needed_so 不应为空");
    assert!(needed2.iter().all(|n| rp.contains(n)));

    let _ = std::fs::remove_dir_all(&tmp);
}
