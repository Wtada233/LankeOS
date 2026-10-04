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

/// 花括号感知切分：`provides_soname` / `needed_so` 里的 `X@{A,B}` 中，花括号内的逗号
/// **不是**字段分隔符。与 lpkg 的 `split_so_list()`（`base/so_spec.cpp`）是同一条规则，
/// 两侧共用 `main/scripts/check_index_conformance.py` 那份 fixture 的用例形态。
#[test]
fn split_field_is_brace_aware() {
    assert_eq!(
        split_field(Some("libc.so.6@{GLIBC_2.40,GLIBC_2.39},libm.so.6")),
        vec!["libc.so.6@{GLIBC_2.40,GLIBC_2.39}", "libm.so.6"]
    );
    // 与旧行为逐条一致的部分：空片段丢弃、两端空白剥掉
    assert_eq!(
        split_field(Some("a.so.1,,b.so.2,")),
        vec!["a.so.1", "b.so.2"]
    );
    assert_eq!(
        split_field(Some(" a.so.1 , b.so.2 ")),
        vec!["a.so.1", "b.so.2"]
    );
    assert!(split_field(Some("")).is_empty());
    assert!(split_field(None).is_empty());
    // 花括号不配对：退化成普通逗号切分（有界、确定），由 lpkg 读入处整块跳过
    assert_eq!(
        split_field(Some("X@{A,B,libm.so.6")),
        vec!["X@{A,B,libm.so.6"]
    );
    // 游离的 `}`（深度已经是 0）**不吞**后面的逗号：它只是个普通字符
    assert_eq!(split_field(Some("X}A,b.so.1")), vec!["X}A", "b.so.1"]);
}

/// **基线归一**：含符号版本的声明/需求在 farm 侧一律按**裸 SONAME**参与查找与比较。
/// 不归一的话：`verify` 会把"手写符号版本"判成漂移（repack 涂掉它），`abi` 会误报断裂。
#[test]
fn symbol_version_specs_are_normalised_to_bare_sonames() {
    // 提供者声明的是带版本的规格，消费者需要的是其中一个符号版本
    let idx = Index::parse(
        "prov|1.0:h:::libc.so.6@{GLIBC_2.39,GLIBC_2.40}:|\n\
         cons|1.0:h::::libc.so.6@GLIBC_2.40|\n",
    );
    // provider 查找：按裸名命中（不是整串）
    assert_eq!(
        idx.providers_of("libc.so.6@GLIBC_2.40"),
        vec!["prov".to_string()]
    );
    assert_eq!(idx.providers_of("libc.so.6"), vec!["prov".to_string()]);
    // 反图：需要 `X@V` 的包挂在**裸名**键上
    let rev = RevMap::build(&idx);
    assert_eq!(
        rev.needers("libc.so.6@GLIBC_2.40"),
        vec!["cons".to_string()]
    );
    assert_eq!(rev.needers("libc.so.6"), vec!["cons".to_string()]);
    // 前向链接边（排序用）同样成立
    assert_eq!(link_deps(&idx, "cons"), vec!["prov".to_string()]);
    // ABI 面：带版本的规格仍被判为版本化 SONAME（形状按裸名判）
    let face = idx.soname_provides("prov");
    assert!(face.contains("libc.so.6"), "ABI 面应含裸名，实得 {face:?}");
    // 原始规格串**逐字保留**（farm 只转录，不改写用户写法）
    assert_eq!(
        idx.packages["prov"].provides_soname,
        vec!["libc.so.6@{GLIBC_2.39,GLIBC_2.40}".to_string()]
    );
}

#[test]
fn so_bare_strips_only_the_symbol_version_suffix() {
    assert_eq!(so_bare("libc.so.6"), "libc.so.6");
    assert_eq!(so_bare("libc.so.6@GLIBC_2.40"), "libc.so.6");
    assert_eq!(so_bare("libc.so.6@{GLIBC_2.40,GLIBC_2.39}"), "libc.so.6");
}
