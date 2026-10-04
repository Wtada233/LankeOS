//! `src/graph.rs` 的单元测试（从生产文件搬出：那里测试比生产还长，正文被淹没）。
//! 仍是同一父模块的子模块 ⇒ `use super::*` 与 crate 内私有项照旧可用。

use super::*;

const CHAIN: &str = "\
libxml2|2.9.0:hash:::libxml2.so,libxml2.so.2:ld-linux.so.2,libc.so.6|
llvm|18.1.0:hash:::libLLVM.so,libLLVM.so.18:libxml2.so.2,libc.so.6|
rust|1.80.0:hash::rustc::libLLVM.so.18,libc.so.6|
glibc|2.39:hash:::libc.so,libc.so.6,ld-linux.so.2:|
";

#[test]
fn parse_index_latest_version_block() {
    let idx = Index::parse(CHAIN);
    assert_eq!(idx.packages.len(), 4);
    let llvm = &idx.packages["llvm"];
    assert_eq!(llvm.version, "18.1.0");
    assert_eq!(llvm.needed_so, vec!["libxml2.so.2", "libc.so.6"]);
    assert_eq!(llvm.provides_soname, vec!["libLLVM.so", "libLLVM.so.18"]);
    // `rust` 的 provides 是**纯虚拟 provider**（rustc），与 SONAME 无关
    assert_eq!(idx.packages["rust"].provides, vec!["rustc"]);
    assert!(idx.packages["rust"].provides_soname.is_empty());
}

#[test]
fn parse_rejects_legacy_field_count() {
    // 旧 5 字段格式（无 provides_soname）不再被容忍：整行跳过
    let idx = Index::parse("old|1.0:h::liba.so.1:libc.so.6|\n");
    assert!(
        idx.is_empty(),
        "旧格式行应被跳过: {:?}",
        idx.packages.keys()
    );
    // 旧格式带包级 provides 第三段也不认
    let idx2 = Index::parse("old|1.0:h::liba.so.1|libvirt|\n");
    assert!(idx2.is_empty());
}

#[test]
fn soname_filter_excludes_dev_links_and_virtuals() {
    let idx = Index::parse(CHAIN);
    assert_eq!(
        idx.soname_provides("libxml2"),
        HashSet::from(["libxml2.so.2".to_string()])
    );
    assert!(idx.soname_provides("rust").is_empty());
}

#[test]
fn link_deps_forward() {
    let idx = Index::parse(CHAIN);
    assert_eq!(link_deps(&idx, "llvm"), vec!["glibc", "libxml2"]);
    assert_eq!(link_deps(&idx, "rust"), vec!["glibc", "llvm"]);
    assert_eq!(link_deps(&idx, "libxml2"), vec!["glibc"]);
}

#[test]
fn is_soname_versioned_cases() {
    assert!(is_soname_versioned("libfoo.so.1"));
    assert!(is_soname_versioned("libsystemd.so.0"));
    assert!(is_soname_versioned("libc.so.6.1"));
    assert!(is_soname_versioned("ld-linux-x86-64.so.2"));
    assert!(!is_soname_versioned("libfoo.so"));
    assert!(!is_soname_versioned("rustc"));
    assert!(!is_soname_versioned("libfoo.soX"));
}
