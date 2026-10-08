//! `src/graph.rs` 的单元测试。
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

/// **规格解析 / 覆盖**：与 lpkg 同一份真值表（`lpkg/tests/unit/test_so_spec.cpp` +
/// `check_index_conformance.py` 的 `SO_SPEC_VECTORS`）—— 跨语言契约，两边各钉一份。
#[test]
fn so_spec_vectors_match_lpkg() {
    // 解析：裸 / 单版本 / 花括号（**排序去重**）
    assert_eq!(
        parse_so_spec("libc.so.6"),
        ("libc.so.6".to_string(), vec![])
    );
    assert_eq!(
        parse_so_spec("libc.so.6@GLIBC_2.40"),
        ("libc.so.6".to_string(), vec!["GLIBC_2.40".to_string()])
    );
    assert_eq!(
        parse_so_spec("X@{B,A,A}"),
        ("X".to_string(), vec!["A".to_string(), "B".to_string()])
    );
    // 畸形一律整串当裸名（与 lpkg 的宽容分支同口径）
    for bad in [
        "", "@A", "X@", "X@{}", "X@{A,}", "X@{,A}", "X@{A", "X@A}", "X@{A}{B}", "X@{A{B}}", "X@@A",
        "X@{A, B}", "X @A", "X@A B",
    ] {
        assert_eq!(
            parse_so_spec(bad),
            (bad.to_string(), vec![]),
            "畸形判定不符：{bad:?}"
        );
    }
    // 覆盖（保守）
    for (p, n) in [
        ("X", "X"),
        ("X@{A,B}", "X"),
        ("X@{A,B}", "X@A"),
        ("X@{A,B}", "X@{A,B}"),
        ("X@{A,B}", "X@{B,A}"), // 集合语义，与次序无关
        ("X@{A,B", "X@{A,B"),   // 畸形 → 整串相等才算
    ] {
        assert!(so_covers(p, n), "{p} 应覆盖 {n}");
    }
    for (p, n) in [
        ("X@{A,B}", "X@{A,C}"), // 缺 C
        ("X", "X@A"),           // **保守**：裸 provider 不算
        ("X@A", "X@B"),
        ("X@{A}", "Y@A"), // SONAME 不同
        ("X", "X@{A,B"),
        ("X@{A,B", "X"),
    ] {
        assert!(!so_covers(p, n), "{p} 不应覆盖 {n}");
    }
}

/// 版本级移除只报"**库还在、版本没了**"；整个 SONAME 消失归 `removed_sonames()`（裸名那条）。
#[test]
fn removed_provided_versions_only_reports_surviving_sonames() {
    let old = vec![
        "libc.so.6@{GLIBC_2.39,GLIBC_2.40}".to_string(),
        "libgone.so.1@V1".to_string(),
    ];
    let new = vec!["libc.so.6@GLIBC_2.40".to_string()];
    assert_eq!(
        removed_provided_versions(&old, &new),
        vec!["libc.so.6@GLIBC_2.39"]
    );
    // 整个 SONAME 消失 ⇒ 不在这里重复报
    assert!(removed_provided_versions(&["libgone.so.1@V1".to_string()], &[]).is_empty());
    // 新增版本 / 不变 ⇒ 无移除
    assert!(removed_provided_versions(
        &["libc.so.6@A".to_string()],
        &["libc.so.6@{A,B}".to_string()]
    )
    .is_empty());
}

/// 反图的**版本级**查询：need 声明里的每个版本各登记一次。
#[test]
fn revmap_indexes_needed_versions() {
    let idx = Index::parse(
        "prov|1.0:h:::libx.so.1@{V1,V2}:|\n\
         a|1.0:h::::libx.so.1@{V1,V2}|\n\
         b|1.0:h::::libx.so.1@V2|\n\
         c|1.0:h::::libx.so.1|\n",
    );
    let rev = RevMap::build(&idx);
    // 顺序未定义（HashMap 遍历序）⇒ 只比集合
    let v1: Vec<&str> = rev
        .version_needers("libx.so.1@V1")
        .iter()
        .map(String::as_str)
        .collect();
    assert_eq!(v1, vec!["a"]);
    let mut v2: Vec<&str> = rev
        .version_needers("libx.so.1@V2")
        .iter()
        .map(String::as_str)
        .collect();
    v2.sort_unstable();
    assert_eq!(v2, vec!["a", "b"]);
    // 裸名查询含所有需要它的人（含只声明裸名的 c）—— 顺序未定义（HashMap 遍历序），比集合
    let mut bare: Vec<&str> = rev
        .needers("libx.so.1")
        .iter()
        .map(String::as_str)
        .collect();
    bare.sort_unstable();
    assert_eq!(bare, vec!["a", "b", "c"]);
}
