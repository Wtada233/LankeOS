//! 版本比较（**rpm 语义**：`[epoch:]version[-release]`）。
//!
//! ## 为什么是这一份实现
//!
//! lpkg 从 8.0.0 起**原生就是 rpm 的 EVR**：它把版本串**原样**交给 libsolv 的
//! `pool_evrcmp_str()`，再没有"自己的语义 + 编码桥"（见 lpkg `vercmp/version.hpp` 的订正块）。
//! farm 这边没有 libsolv 绑定（Rust + 静态链接的 C 库不便），所以这里**移植** libsolv 的
//! 同一套算法 —— 移植的是 `evr.c` 的 `solv_vercmp_rpm()` 与 `pool_evrcmp_str()` 的
//! COMPARE 分支（epoch 处理、**最后一个** `-` 切 release、"有 release 者更大"）。
//!
//! ⚠️ **这份是"移植"，不是"另一个实现"** —— 判据来自外部（libsolv/rpm），本文件只负责
//! 逐值对齐。对齐不是靠嘴说的：`tests/fixtures/vercmp_rpm.txt` 里每一条期望值都是**用
//! 容器里的真 libsolv 算出来的**（含全部真实包版本两两组合的抽样 + 边界形态），下面的
//! `diff_against_real_libsolv_fixture` 逐条比对。改这个文件时那条用例必须仍然绿。
//!
//! ## 语义（与 rpm 一致）
//!
//! - `~` 是**预发布**：`1.0~rc1 < 1.0`；
//! - `-` 之后是 **release**（最后一个 `-` 切分）：`1.0-1 > 1.0`、`1.0-1 < 1.0-2`；
//! - `^` 介于"基础版"与"任何真实下一段"之间（rpm 的 caret 语义）；
//! - 段按数字比大小、数字段 > 字母段、字母段按字典序；数字段去掉前导 0 再比位数；
//! - 有 epoch 且非 0 者更大；`0:` 与无 epoch 等价。
//!
//! ⚠️ **旧实现（`主版本[补丁后缀][-预发布][+发行修订号]`）已删除** —— 那是 lpkg 旧语义的
//! 影子（`-` 当预发布、`+N` 当发行修订号），随 lpkg 的破坏性改动一起作废。farm 写进
//! `LankeBUILD.json` 的版本也不再拼 `<ver>+<release>`，而是 `<ver>-<release>`
//! （`build/repo.rs` 的 `effective_version`）。

use std::cmp::Ordering;

/// 比较两个版本字符串（rpm EVR 语义）。
pub fn cmp_version(a: &str, b: &str) -> Ordering {
    match evr_cmp(a, b) {
        r if r < 0 => Ordering::Less,
        0 => Ordering::Equal,
        _ => Ordering::Greater,
    }
}

/// `a` 是否比 `b` 新（严格大于）。
pub fn is_newer(a: &str, b: &str) -> bool {
    cmp_version(a, b) == Ordering::Greater
}

/// `solv_vercmp_rpm()` 的移植：比较两段**不含 epoch/release 语义**的字符串（version 段或
/// release 段），返回 <0 / 0 / >0。
fn vercmp_rpm(s1: &[u8], s2: &[u8]) -> i32 {
    let (mut i, mut j) = (0usize, 0usize);
    let is_digit = |c: u8| c.is_ascii_digit();
    let is_alpha = |c: u8| c.is_ascii_alphabetic();
    loop {
        // 跳过分隔符（但 `~`/`^` 不是分隔符，它们有语义）
        while i < s1.len() && !is_digit(s1[i]) && !is_alpha(s1[i]) && s1[i] != b'~' && s1[i] != b'^'
        {
            i += 1;
        }
        while j < s2.len() && !is_digit(s2[j]) && !is_alpha(s2[j]) && s2[j] != b'~' && s2[j] != b'^'
        {
            j += 1;
        }
        if i < s1.len() && s1[i] == b'~' {
            if j < s2.len() && s2[j] == b'~' {
                i += 1;
                j += 1;
                continue;
            }
            return -1;
        }
        if j < s2.len() && s2[j] == b'~' {
            return 1;
        }
        if i < s1.len() && s1[i] == b'^' {
            if j < s2.len() && s2[j] == b'^' {
                i += 1;
                j += 1;
                continue;
            }
            return if j < s2.len() { -1 } else { 1 };
        }
        if j < s2.len() && s2[j] == b'^' {
            return if i < s1.len() { 1 } else { -1 };
        }
        if i >= s1.len() || j >= s2.len() {
            break;
        }
        if is_digit(s1[i]) || is_digit(s2[j]) {
            // 数字段：去前导 0（至少留一位），先比位数、再逐字符比
            while s1[i] == b'0' && i + 1 < s1.len() && is_digit(s1[i + 1]) {
                i += 1;
            }
            while s2[j] == b'0' && j + 1 < s2.len() && is_digit(s2[j + 1]) {
                j += 1;
            }
            let mut e1 = i;
            while e1 < s1.len() && is_digit(s1[e1]) {
                e1 += 1;
            }
            let mut e2 = j;
            while e2 < s2.len() && is_digit(s2[e2]) {
                e2 += 1;
            }
            let r = (e1 - i) as i32 - (e2 - j) as i32;
            if r != 0 {
                return if r > 0 { 1 } else { -1 };
            }
            let c = s1[i..e1].cmp(&s2[j..e2]);
            if c != Ordering::Equal {
                return if c == Ordering::Greater { 1 } else { -1 };
            }
            i = e1;
            j = e2;
        } else {
            // 字母段：短的那段是长的前缀时，**长的大**
            let mut e1 = i;
            while e1 < s1.len() && is_alpha(s1[e1]) {
                e1 += 1;
            }
            let mut e2 = j;
            while e2 < s2.len() && is_alpha(s2[e2]) {
                e2 += 1;
            }
            let len1 = (e1 - i) as i32;
            let len2 = (e2 - j) as i32;
            if len1 > len2 {
                let c = s1[i..i + (len2 as usize)].cmp(&s2[j..e2]);
                return if c != Ordering::Less { 1 } else { -1 };
            }
            if len1 < len2 {
                let c = s1[i..e1].cmp(&s2[j..j + (len1 as usize)]);
                return if c != Ordering::Greater { -1 } else { 1 };
            }
            let c = s1[i..e1].cmp(&s2[j..e2]);
            if c != Ordering::Equal {
                return if c == Ordering::Greater { 1 } else { -1 };
            }
            i = e1;
            j = e2;
        }
    }
    if i < s1.len() {
        1
    } else if j < s2.len() {
        -1
    } else {
        0
    }
}

/// `pool_evrcmp_str(..., EVRCMP_COMPARE)` 的移植（RPM disttype、`promoteepoch=false`）。
fn evr_cmp(evr1: &str, evr2: &str) -> i32 {
    if evr1 == evr2 {
        return 0;
    }
    let e1 = evr1.as_bytes();
    let e2 = evr2.as_bytes();

    // ── epoch ────────────────────────────────────────────────────────────────
    let mut s1 = 0usize;
    while s1 < e1.len() && e1[s1].is_ascii_digit() {
        s1 += 1;
    }
    let mut s2 = 0usize;
    while s2 < e2.len() && e2[s2].is_ascii_digit() {
        s2 += 1;
    }
    let ep1 = s1 != 0 && s1 < e1.len() && e1[s1] == b':';
    let ep2 = s2 != 0 && s2 < e2.len() && e2[s2] == b':';
    let (mut v1, mut v2) = (0usize, 0usize); // version 起点（跳过 epoch）
    if ep1 && ep2 {
        let r = vercmp_rpm(&e1[..s1], &e2[..s2]);
        if r != 0 {
            return r;
        }
        v1 = s1 + 1;
        v2 = s2 + 1;
    } else if ep1 {
        // 只有一侧有 epoch：全 0 的 epoch 等价于没有；非 0 → 这一侧更大
        let mut p = 0usize;
        while p < e1.len() && e1[p] == b'0' {
            p += 1;
        }
        if p >= e1.len() || e1[p] != b':' {
            return 1;
        }
        v1 = p + 1;
    } else if ep2 {
        let mut p = 0usize;
        while p < e2.len() && e2[p] == b'0' {
            p += 1;
        }
        if p >= e2.len() || e2[p] != b':' {
            return -1;
        }
        v2 = p + 1;
    }

    // ── version / release：在**最后一个** `-` 处切 ─────────────────────────────
    let dash1 = e1[v1..].iter().rposition(|&c| c == b'-').map(|p| v1 + p);
    let dash2 = e2[v2..].iter().rposition(|&c| c == b'-').map(|p| v2 + p);
    let end1 = dash1.unwrap_or(e1.len());
    let end2 = dash2.unwrap_or(e2.len());

    let r = vercmp_rpm(&e1[v1..end1], &e2[v2..end2]);
    if r != 0 {
        return r;
    }
    // COMPARE 模式：有 release 的**更大**（`1.0-1 > 1.0`）
    match (dash1, dash2) {
        (None, Some(_)) => return -1,
        (Some(_), None) => return 1,
        (None, None) => return 0,
        (Some(_), Some(_)) => {}
    }
    vercmp_rpm(&e1[dash1.unwrap() + 1..], &e2[dash2.unwrap() + 1..])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn numeric_dot_compare() {
        assert_eq!(cmp_version("2.3.2", "2.3.2"), Ordering::Equal);
        assert_eq!(cmp_version("2.3.2", "2.10.0"), Ordering::Less);
        assert_eq!(cmp_version("2024.10.16", "2023.1.1"), Ordering::Greater);
        assert_eq!(cmp_version("1.4.350.1", "22.1.2"), Ordering::Less);
        assert_eq!(cmp_version("5.3", "5.3.1"), Ordering::Less);
        assert_eq!(cmp_version("0.16.2", "0.16.1"), Ordering::Greater);
    }

    #[test]
    fn trailing_zero_segments_are_not_equal() {
        // ⚠️ rpm **不补段**：`1.2` 与 `1.2.0` 是不同版本（后者更大）。旧实现按"尾随 0 段
        // 相等"处理，那条随语义切换一起废掉了。
        assert_eq!(cmp_version("1.2", "1.2.0"), Ordering::Less);
        assert_eq!(cmp_version("1.2.0", "1.2"), Ordering::Greater);
        assert_eq!(cmp_version("1.2.3", "1.2"), Ordering::Greater);
    }

    #[test]
    fn prerelease_is_tilde() {
        // `~` = 预发布（旧写法的 `-rc1` 现在是 release，比基础版**新**）
        assert_eq!(cmp_version("1.0~rc1", "1.0"), Ordering::Less);
        assert_eq!(cmp_version("1.0", "1.0~rc1"), Ordering::Greater);
        assert_eq!(cmp_version("1.0~rc1", "1.0~rc2"), Ordering::Less);
        assert_eq!(cmp_version("1.0~alpha", "1.0~beta"), Ordering::Less);
        assert_eq!(cmp_version("261~rc4", "261"), Ordering::Less);
    }

    #[test]
    fn release_is_dash() {
        assert_eq!(cmp_version("1.0-1", "1.0"), Ordering::Greater);
        assert_eq!(cmp_version("1.0", "1.0-1"), Ordering::Less);
        assert_eq!(cmp_version("1.0-1", "1.0-2"), Ordering::Less);
        assert_eq!(cmp_version("1.0~rc1", "1.0-1"), Ordering::Less);
        // 版本升级主导 release
        assert_eq!(cmp_version("261-3", "261.2-3"), Ordering::Less);
        assert_eq!(cmp_version("1.0-9", "1.0.1"), Ordering::Less);
        assert_eq!(cmp_version("3.7-2", "3.7b-2"), Ordering::Less);
    }

    #[test]
    fn caret_sits_between() {
        // rpm 的 `^`："比基础版新、比任何真实下一段旧"
        assert_eq!(cmp_version("1.0^git1", "1.0"), Ordering::Greater);
        assert_eq!(cmp_version("1.0^git1", "1.0.1"), Ordering::Less);
    }

    #[test]
    fn alpha_and_patch_suffixes() {
        assert_eq!(cmp_version("1.0p2", "1.0"), Ordering::Greater);
        assert_eq!(cmp_version("3.7b", "3.7"), Ordering::Greater);
        assert_eq!(cmp_version("3.7b", "3.8"), Ordering::Less);
        assert_eq!(cmp_version("1.9.17", "1.9.17p2"), Ordering::Less);
        assert_eq!(cmp_version("1.0p1", "1.0p2"), Ordering::Less);
        assert_eq!(cmp_version("1.0p1", "1.0p10"), Ordering::Less); // 数字段按位数/值，不是字典序
        assert_eq!(cmp_version("1.0beta", "1.0"), Ordering::Greater);
        assert_eq!(cmp_version("1.0beta", "1.0alpha"), Ordering::Greater);
    }

    #[test]
    fn epoch_beats_absent_and_zero_epoch_is_absent() {
        assert_eq!(cmp_version("1:2.0", "2.0"), Ordering::Greater);
        assert_eq!(cmp_version("2.0", "1:2.0"), Ordering::Less);
        assert_eq!(cmp_version("0:1.0", "1.0"), Ordering::Equal);
        assert_eq!(cmp_version("1:1.0", "2:1.0"), Ordering::Less);
    }

    #[test]
    fn is_newer_helper() {
        assert!(is_newer("1.0-2", "1.0-1"));
        assert!(!is_newer("1.0-1", "1.0-2"));
        assert!(!is_newer("1.0", "1.0"));
    }

    /// **对齐闸门**：逐条比对真 libsolv 算出来的期望值。
    ///
    /// 期望值的生成方式（fixture 头部也写着）：把真实仓库的全部包版本两两抽样、再补一批
    /// 边界形态（`~`/`^`/epoch/前导 0/字母段/多段 release），喂给容器里的
    /// `pool_evrcmp_str(pool, a, b, EVRCMP_COMPARE)` 取符号。因此**这份 fixture 是"移植
    /// 是否忠实"的唯一证据** —— 只改本文件而不重算 fixture，这条用例就会红。
    #[test]
    fn diff_against_real_libsolv_fixture() {
        let fixture = include_str!("../../tests/fixtures/vercmp_rpm.txt");
        let mut checked = 0usize;
        for (lineno, line) in fixture.lines().enumerate() {
            if line.starts_with('#') || line.trim().is_empty() {
                continue;
            }
            let mut it = line.split('\t');
            let (a, b, want) = match (it.next(), it.next(), it.next()) {
                (Some(a), Some(b), Some(w)) => (a, b, w.parse::<i32>().expect("cmp 必须是 -1/0/1")),
                _ => panic!("fixture 第 {} 行不是 a<TAB>b<TAB>cmp：{}", lineno + 1, line),
            };
            let got = match cmp_version(a, b) {
                Ordering::Less => -1,
                Ordering::Equal => 0,
                Ordering::Greater => 1,
            };
            assert_eq!(
                got,
                want,
                "第 {} 行与真 libsolv 不一致：cmp({a:?}, {b:?}) = {got}，libsolv 说 {want}",
                lineno + 1
            );
            // 反对称性：反向必须取反（顺带证明判据不是"恒返回某个值"）
            let rev = match cmp_version(b, a) {
                Ordering::Less => -1,
                Ordering::Equal => 0,
                Ordering::Greater => 1,
            };
            assert_eq!(
                rev, -want,
                "反向比较不对称：cmp({b:?}, {a:?}) = {rev}，应为 {}",
                -want
            );
            checked += 1;
        }
        assert!(
            checked > 500,
            "fixture 太小（{} 条），对齐闸门形同虚设",
            checked
        );
    }
}
