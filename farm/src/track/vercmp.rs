//! 版本比较（对齐 lpkg `vercmp/version.cpp` 语义）。
//!
//! 格式：`主版本号[补丁后缀][-预发布][+发行修订号]`
//!   - 主版本号: `(\d+)(\.\d+)*`
//!   - 补丁后缀: `[a-zA-Z]\d*`（如 `p2`、`b`，最高）
//!   - 发行修订号: `+[0-9A-Za-z]+(\.[0-9A-Za-z]+)*`（LankeOS release，高于基础版）
//!   - 预发布: `-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*`（低于基础版）
//!
//! 比较优先级（主版本号相等时）：补丁后缀 > 发行修订号 > 基础版 > 预发布。
//! 与 lpkg 一致：`1.0 > 1.0-rc1`、`2.3.2+2 > 2.3.2`、`1.0p2 > 1.0`。
//!
//! 本实现比 lpkg 宽松（不抛格式校验异常）——上游版本五花八门，track 取宽松解析。

use std::cmp::Ordering;

#[derive(Debug, Default)]
struct Version {
    main_part: Vec<u64>,
    patch_suffix: String,
    release_part: Vec<String>,
    pre_release_part: Vec<String>,
}

impl Version {
    fn parse(s: &str) -> Version {
        let pre_pos = s.find('-');
        let build_pos = s.find('+');
        let main_end = match (pre_pos, build_pos) {
            (Some(p), Some(b)) => p.min(b),
            (Some(p), None) => p,
            (None, Some(b)) => b,
            (None, None) => s.len(),
        };
        let main_str = &s[..main_end];

        let mut v = Version::default();
        let segs: Vec<&str> = main_str.split('.').collect();
        for (i, seg) in segs.iter().enumerate() {
            if i + 1 < segs.len() {
                v.main_part.push(seg.parse().unwrap_or(0));
            } else {
                // 最后一段：数字 + 可选补丁后缀（如 "17p2" → 17 + "p2"）
                let mut num_end = 0;
                while num_end < seg.len() && seg.as_bytes()[num_end].is_ascii_digit() {
                    num_end += 1;
                }
                v.main_part.push(seg[..num_end].parse().unwrap_or(0));
                let tail = &seg[num_end..];
                let is_patch = !tail.is_empty()
                    && tail.as_bytes()[0].is_ascii_alphabetic()
                    && tail[1..].bytes().all(|b| b.is_ascii_digit());
                if is_patch {
                    v.patch_suffix = tail.to_string();
                }
            }
        }

        if let Some(p) = pre_pos {
            let end = build_pos.filter(|b| *b > p).unwrap_or(s.len());
            v.pre_release_part = s[p + 1..end]
                .split('.')
                .filter(|x| !x.is_empty())
                .map(String::from)
                .collect();
        }
        if let Some(b) = build_pos {
            let end = pre_pos.filter(|p| *p > b).unwrap_or(s.len());
            v.release_part = s[b + 1..end]
                .split('.')
                .filter(|x| !x.is_empty())
                .map(String::from)
                .collect();
        }
        v
    }
}

/// 比较两个版本字符串。
pub fn cmp_version(a: &str, b: &str) -> Ordering {
    compare(&Version::parse(a), &Version::parse(b))
}

/// `a` 是否比 `b` 新（严格大于）。
pub fn is_newer(a: &str, b: &str) -> bool {
    cmp_version(a, b) == Ordering::Greater
}

fn compare(a: &Version, b: &Version) -> Ordering {
    let n = a.main_part.len().max(b.main_part.len());
    for i in 0..n {
        let na = a.main_part.get(i).copied().unwrap_or(0);
        let nb = b.main_part.get(i).copied().unwrap_or(0);
        if na != nb {
            return na.cmp(&nb);
        }
    }
    let ord = cmp_patch(&a.patch_suffix, &b.patch_suffix);
    if ord != Ordering::Equal {
        return ord;
    }
    let a_rel = !a.release_part.is_empty();
    let b_rel = !b.release_part.is_empty();
    if a_rel && !b_rel {
        return Ordering::Greater;
    }
    if !a_rel && b_rel {
        return Ordering::Less;
    }
    if a_rel && b_rel {
        let ord = compare_segments(&a.release_part, &b.release_part);
        if ord != Ordering::Equal {
            return ord;
        }
    }
    let a_pre = !a.pre_release_part.is_empty();
    let b_pre = !b.pre_release_part.is_empty();
    if !a_pre && b_pre {
        return Ordering::Greater;
    }
    if a_pre && !b_pre {
        return Ordering::Less;
    }
    if a_pre && b_pre {
        return compare_segments(&a.pre_release_part, &b.pre_release_part);
    }
    Ordering::Equal
}

fn cmp_patch(a: &str, b: &str) -> Ordering {
    match (a.is_empty(), b.is_empty()) {
        (true, false) => return Ordering::Less,
        (false, true) => return Ordering::Greater,
        (false, false) => {
            let ca = a.as_bytes()[0];
            let cb = b.as_bytes()[0];
            if ca != cb {
                return ca.cmp(&cb);
            }
            let na: u64 = a[1..].parse().unwrap_or(0);
            let nb: u64 = b[1..].parse().unwrap_or(0);
            if na != nb {
                return na.cmp(&nb);
            }
        }
        (true, true) => {}
    }
    Ordering::Equal
}

/// 分段比较（语义化规范）：数字段按数值，数字 < 字母，更多分段更高。
fn compare_segments(a: &[String], b: &[String]) -> Ordering {
    let n = a.len().min(b.len());
    for i in 0..n {
        let ai = a[i].as_str();
        let bi = b[i].as_str();
        let a_num = !ai.is_empty() && ai.bytes().all(|c| c.is_ascii_digit());
        let b_num = !bi.is_empty() && bi.bytes().all(|c| c.is_ascii_digit());
        let ord = match (a_num, b_num) {
            (true, true) => {
                let na: u128 = ai.parse().unwrap_or(0);
                let nb: u128 = bi.parse().unwrap_or(0);
                na.cmp(&nb)
            }
            (true, false) => Ordering::Less,
            (false, true) => Ordering::Greater,
            (false, false) => ai.cmp(bi),
        };
        if ord != Ordering::Equal {
            return ord;
        }
    }
    a.len().cmp(&b.len())
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
    fn trailing_zero_segments_equal() {
        assert_eq!(cmp_version("1.2", "1.2.0"), Ordering::Equal);
        assert_eq!(cmp_version("1.2.3", "1.2"), Ordering::Greater);
    }

    #[test]
    fn prerelease_lower_than_release() {
        assert_eq!(cmp_version("1.0", "1.0-rc1"), Ordering::Greater);
        assert_eq!(cmp_version("261", "261-rc4"), Ordering::Greater);
        assert_eq!(cmp_version("1.0-rc1", "1.0"), Ordering::Less);
    }

    #[test]
    fn prerelease_segments() {
        assert_eq!(cmp_version("1.0-rc1", "1.0-rc2"), Ordering::Less);
        assert_eq!(cmp_version("1.0-rc2", "1.0-rc1"), Ordering::Greater);
        assert_eq!(cmp_version("1.0-alpha1", "1.0-alpha1.1"), Ordering::Less);
        assert_eq!(cmp_version("1.0-1", "1.0-alpha"), Ordering::Less);
    }

    #[test]
    fn release_revision_higher() {
        assert_eq!(cmp_version("2.3.2+2", "2.3.2"), Ordering::Greater);
        assert_eq!(cmp_version("2.3.2", "2.3.2+1"), Ordering::Less);
        assert_eq!(cmp_version("2.3.2+2", "2.3.2+1"), Ordering::Greater);
    }

    #[test]
    fn patch_suffix() {
        assert_eq!(cmp_version("1.0p2", "1.0"), Ordering::Greater);
        assert_eq!(cmp_version("3.7b", "3.7"), Ordering::Greater);
        assert_eq!(cmp_version("3.7b", "3.8"), Ordering::Less);
    }

    #[test]
    fn alpha_segments() {
        assert_eq!(cmp_version("1.0.a", "1.0.b"), Ordering::Less);
    }
}
