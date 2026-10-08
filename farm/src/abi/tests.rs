//! `src/abi.rs` 的单元测试。
//! 仍是同一父模块的子模块 ⇒ `use super::*` 与 crate 内私有项照旧可用。

use super::*;

const CHAIN: &str = "\
libxml2|2.9.0:hash:::libxml2.so,libxml2.so.2:ld-linux.so.2,libc.so.6|
llvm|18.1.0:hash:::libLLVM.so,libLLVM.so.18:libxml2.so.2,libc.so.6|
rust|1.80.0:hash::rustc::libLLVM.so.18,libc.so.6|
glibc|2.39:hash:::libc.so,libc.so.6,ld-linux.so.2:|
";

#[test]
fn removed_sonames_detects_bump() {
    let old = Index::parse(CHAIN);
    let new_provides_soname = vec!["libxml2.so".to_string(), "libxml2.so.3".to_string()];
    assert_eq!(
        removed_sonames(&old, "libxml2", &new_provides_soname),
        vec!["libxml2.so.2"]
    );
}

#[test]
fn removed_sonames_detects_unversioned_soname_change() {
    // tcl 8.6 → 9.0：无 SONAME 实体库 libtcl8.6.so → libtcl9.0.so 必须被识别为 ABI 断裂
    //（tcl 的 provides_soname 只有裸 .so，没有版本化兄弟项 → 是实体库不是 dev symlink）。
    let old = Index::parse("tcl|8.6.16:hash:::libtcl8.6.so:libc.so.6,libtcl8.6.so|\n");
    assert_eq!(
        removed_sonames(&old, "tcl", &["libtcl9.0.so".to_string()]),
        vec!["libtcl8.6.so"]
    );
    // dev symlink（libfoo.so 有版本化兄弟 libfoo.so.1）不算 ABI 信号，不能被误报
    assert_eq!(
        removed_sonames(&old, "tcl", &["libtcl8.6.so".to_string()]),
        Vec::<String>::new()
    );
}

#[test]
fn detection_and_backup_share_abi_soname_set() {
    // 对称性锁死：备份端（repo::backup_removed_sonames 内部）与检测端（removed_sonames）
    // 必须用同一个 ABI 面集合（soname_provides_of）。dev symlink（libfoo.so 有版本化兄弟
    // libfoo.so.1）两端都排除；无 SONAME 实体库（libtcl8.6.so）两端都纳入。
    let old = Index::parse(
        "libfoo|1.0:hash:::libfoo.so,libfoo.so.1:libc.so.6|\n\
             tcl|8.6.16:hash:::libtcl8.6.so:libc.so.6,libtcl8.6.so|\n",
    );
    // 检测端
    let det = removed_sonames(
        &old,
        "libfoo",
        &["libfoo.so".to_string(), "libfoo.so.2".to_string()],
    );
    // 备份端（同一计算：ABI(old) − ABI(new)）
    let old_s = soname_provides_of(&old.packages["libfoo"].provides_soname);
    let new_s = soname_provides_of(&["libfoo.so".to_string(), "libfoo.so.2".to_string()]);
    let bak: Vec<String> = old_s.difference(&new_s).cloned().collect();
    assert_eq!(det, bak, "dev symlink 场景：检测与备份必须一致");
    assert_eq!(
        det,
        vec!["libfoo.so.1"],
        "libfoo.so（dev symlink）不应是 ABI 面"
    );

    // tcl 无 SONAME 实体库：两端都识别 libtcl8.6.so 为 ABI 面
    let det2 = removed_sonames(&old, "tcl", &["libtcl9.0.so".to_string()]);
    let old_s2 = soname_provides_of(&old.packages["tcl"].provides_soname);
    let new_s2 = soname_provides_of(&["libtcl9.0.so".to_string()]);
    let bak2: Vec<String> = old_s2.difference(&new_s2).cloned().collect();
    assert_eq!(det2, bak2, "无 SONAME 实体库场景：检测与备份必须一致");
    assert_eq!(det2, vec!["libtcl8.6.so"]);
}
