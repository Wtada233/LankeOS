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

    let (_, provides_soname) = scan_content(&content, &Default::default());
    // 去重仍是本用例的主题：符号链接与真身各贡献一次 → 归并成两条（`libc.so` 与 `libc.so.6`）
    assert_eq!(provides_soname.len(), 2, "应只有两条：{provides_soname:?}");
    let mut bare: Vec<&str> = provides_soname
        .iter()
        .map(|s| crate::graph::so_bare(s))
        .collect();
    bare.sort_unstable();
    assert_eq!(bare, vec!["libc.so", "libc.so.6"]);
    // 新增的可观测性：符号链接那一条的符号版本**来自目标文件**（链接本身没有节区）⇒
    // 两条的 `@…` 后缀必须逐字相同。宿主 libc 有没有 verdef 由它自己决定，所以这里只比
    // "两条一致"，不比具体版本号（夹具那一条用例负责钉死版本语义）。
    let tail = |s: &str| s[crate::graph::so_bare(s).len()..].to_string();
    assert_eq!(
        tail(&provides_soname[0]),
        tail(&provides_soname[1]),
        "符号链接与真身的版本集合必须同源：{provides_soname:?}"
    );

    let _ = std::fs::remove_dir_all(&tmp);
}

/// **符号版本（提供端）**：夹具 `tests/fixtures/abi/libx.so.1` 定义了 `V1`/`V2`
/// （外加一个 BASE 节点，名字就是 SONAME 本身 —— 那个不算版本）。
#[test]
fn scan_content_emits_provides_symbol_versions() {
    let fix = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/abi");
    let tmp = std::env::temp_dir().join(format!("farm-scan-ver-prov-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&tmp);
    let content = tmp.join("content");
    std::fs::create_dir_all(content.join("usr/lib")).unwrap();
    std::fs::copy(fix.join("libx.so.1"), content.join("usr/lib/libx.so.1")).unwrap();

    let (needed, provides) = scan_content(&content, &Default::default());
    assert_eq!(
        provides,
        vec!["libx.so.1@{V1,V2}"],
        "verdef 的 V1/V2 必须进规格串；BASE 节点（名字 = SONAME）不算版本"
    );
    assert!(
        needed.is_empty(),
        "只放了提供者，不该有 needed_so：{needed:?}"
    );

    let _ = std::fs::remove_dir_all(&tmp);
}

/// **符号版本（消费端）**：`tests/fixtures/abi/foo` 从 `libx.so.1` 需要 `V2`。
///
/// ⚠️ 提供者**不能**放进同一个包：包里某个 ELF 的 SONAME 与该需求同名会被判成"自提供"
/// 而过滤掉（既有语义，见 `scan_content` 的三类过滤）—— 所以这条用例只放消费者。
#[test]
fn scan_content_emits_needed_symbol_versions() {
    let fix = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/abi");
    let tmp = std::env::temp_dir().join(format!("farm-scan-ver-need-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&tmp);
    let content = tmp.join("content");
    std::fs::create_dir_all(content.join("usr/bin")).unwrap();
    std::fs::copy(fix.join("foo"), content.join("usr/bin/foo")).unwrap();

    // provider 集合按**裸名**给（索引侧的键也是裸名）
    let rp: HashSet<String> = ["libx.so.1".to_string()].into_iter().collect();
    let (needed, provides) = scan_content(&content, &rp);

    assert_eq!(
        needed,
        vec!["libx.so.1@V2"],
        "verneed 点名的版本必须进 needed_so（只声明裸名的提供者满足不了它）"
    );
    assert!(provides.is_empty(), "只放了消费者：{provides:?}");

    let _ = std::fs::remove_dir_all(&tmp);
}

/// **H1（承重）**：绝对符号链接目标必须映射回**包内** content 根（归档里的 `/` == content/），
/// **绝不落到宿主机**。
///
/// 场景：`content/usr/lib/libzzz.so -> /usr/lib/libzzz.so.1`（绝对），真身 ELF 在
/// `content/usr/lib/libzzz.so.1`（夹具 `libx.so.1`，定义 V1/V2）。
///
/// **修前为什么红**：旧实现把绝对目标原样 `canonicalize` ⇒ 解析到**宿主**
/// `/usr/lib/libzzz.so.1`（该文件不存在）⇒ `is_elf` false ⇒ 该符号链接静默丢条目，
/// 版本集合读不到。真实实例：keyutils 的
/// `content/usr/lib/libkeyutils.so -> /lib/libkeyutils.so.1`（该包不含 `content/lib/`）。
/// 修后 `resolve_link_within_content` 把 `/usr/lib/libzzz.so.1` 拼成
/// `content/usr/lib/libzzz.so.1` ⇒ 读到真身 ⇒ 产出 `libzzz.so@…`。
#[test]
fn scan_content_resolves_absolute_symlink_within_content() {
    let fix = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/abi");
    let tmp = std::env::temp_dir().join(format!("farm-scan-abs-in-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&tmp);
    let content = tmp.join("content");
    std::fs::create_dir_all(content.join("usr/lib")).unwrap();
    // 真身放在包内 —— 正是绝对链接 `/usr/lib/libzzz.so.1` 应映射到的位置
    std::fs::copy(fix.join("libx.so.1"), content.join("usr/lib/libzzz.so.1")).unwrap();
    std::os::unix::fs::symlink("/usr/lib/libzzz.so.1", content.join("usr/lib/libzzz.so")).unwrap();

    // 先钉住解析本身的语义（词法映射，不碰宿主机）
    let resolved =
        resolve_link_within_content(&content.join("usr/lib/libzzz.so"), &content).unwrap();
    assert_eq!(
        resolved,
        content.join("usr/lib/libzzz.so.1"),
        "绝对目标 `/usr/lib/libzzz.so.1` 必须映射回 content/usr/lib/（而非宿主 /usr/lib/）"
    );

    let (_, provides) = scan_content(&content, &Default::default());
    assert!(
        provides.iter().any(|s| s == "libzzz.so@{V1,V2}"),
        "绝对链接目标映回包内后，`libzzz.so` 的版本集合必须从真身读到（V1/V2）；\
         修前 canonicalize 落到宿主 /usr/lib/libzzz.so.1 ⇒ 条目静默消失。实得: {provides:?}"
    );
    let _ = std::fs::remove_dir_all(&tmp);
}

/// **H1（密闭性）**：绝对链接目标落在 **content 之外** 时**绝不读取** content 外的文件
/// （宿主 / 仓库外）。
///
/// 真实实例：keyutils 的 `content/usr/lib/libkeyutils.so -> /lib/libkeyutils.so.1`，而该包
/// **不含** `content/lib/`。修前把绝对目标原样 `canonicalize` ⇒ 读 content 外的同名库，把
/// **它的版本集合**烤进产物；content 外没有那个文件时反而静默丢条目 —— 同一缺陷的两个方向。
///
/// 诱饵用**明确存在的 content 外文件**（`<tmp>/outside/usr/lib/libyyy.so.1`，内容 = 夹具
/// `libx.so.1`），而不是钉死 `/usr/lib/libyyy.so.1`：后者在没装该库的宿主上修前也读不到 ⇒
/// 断言会退化成恒真。用一个**确定存在**的 content 外文件，才能确定性地证明"绝对目标不许
/// 穿透 content 根"。
#[test]
fn scan_content_never_reads_absolute_target_outside_content() {
    let fix = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/abi");
    let tmp = std::env::temp_dir().join(format!("farm-scan-abs-out-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&tmp);
    let content = tmp.join("content");
    std::fs::create_dir_all(content.join("usr/lib")).unwrap();
    // content 之外的“宿主/仓库外”文件（绝对目标会指向它）
    let outside = tmp.join("outside/usr/lib");
    std::fs::create_dir_all(&outside).unwrap();
    let decoy = outside.join("libyyy.so.1");
    std::fs::copy(fix.join("libx.so.1"), &decoy).unwrap();
    // 绝对目标指向 content 之外（模拟宿主 /lib/libyyy.so.1）；包内**没有** libyyy.so.1
    std::os::unix::fs::symlink(&decoy, content.join("usr/lib/libyyy.so")).unwrap();

    let (_, provides) = scan_content(&content, &Default::default());
    assert!(
        !provides
            .iter()
            .any(|s| crate::graph::so_bare(s) == "libyyy.so"),
        "绝对链接目标落在 content 之外，绝不能被读到；\
         修前 canonicalize 会读它并把版本集合(V1/V2)烤进产物。实得: {provides:?}"
    );
    let _ = std::fs::remove_dir_all(&tmp);
}

/// **H1（单元）**：`normalize_lexically` 只做词法归一（消 `.`/`..`），不碰文件系统。
#[test]
fn normalize_lexically_collapses_dot_segments() {
    assert_eq!(
        normalize_lexically(std::path::Path::new("/a/b/../c/./d")),
        std::path::PathBuf::from("/a/c/d")
    );
}

/// **H1（单元）**：相对目标借 `..` 逃出 content 根 → `None`（绝不越界解析）。
#[test]
fn resolve_link_within_content_rejects_escape() {
    let tmp = std::env::temp_dir().join(format!("farm-scan-escape-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&tmp);
    let content = tmp.join("content");
    std::fs::create_dir_all(content.join("usr/lib")).unwrap();
    std::os::unix::fs::symlink("../../../../../etc/passwd", content.join("usr/lib/esc.so"))
        .unwrap();
    assert!(
        resolve_link_within_content(&content.join("usr/lib/esc.so"), &content).is_none(),
        "借 .. 逃出 content 根必须返回 None（绝不越界解析）"
    );
    let _ = std::fs::remove_dir_all(&tmp);
}
