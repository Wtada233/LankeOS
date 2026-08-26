//! gnome 模板：两级版本目录探测（download.gnome.org/sources/{name}/{x.y}/）。
//!
//! **被动触发**：source 条目里 `tracker-template: gnome` + `template`。
//! 稳定分支惯例：偶 minor 为稳定版（2.88 稳定，2.89 开发），优先取最大偶 minor 目录。

use regex::Regex;

use crate::net::Fetcher;
use crate::track::templates::{self, minor_is_even};
use crate::track::vercmp;
use crate::track::{need, EntryProbe, SourceConfig};

/// 探测最新稳定版本：第一级版本目录（偶 minor 优先）→ 第二级文件。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &SourceConfig,
    major: Option<&str>,
    pkg_name: &str,
) -> Result<EntryProbe, String> {
    let name = cfg.effective_name(pkg_name); // source-name 覆盖上游目录名（gtk3 → gtk）
    let template = need(&cfg.template, "template")?;

    // 第一级：sources/{name}/ → 版本目录 x.y/
    let level1 = format!("https://download.gnome.org/sources/{name}/");
    let html = fetcher.get(&level1)?;
    let dir_re = Regex::new(r"(\d+(?:\.\d+)*)/").map_err(|e| e.to_string())?;
    let dirs: Vec<String> = dir_re
        .captures_iter(&html)
        .filter_map(|c| c.get(1).map(|m| m.as_str().to_string()))
        .collect();
    // 偶 minor 为稳定分支，优先取最大稳定目录（2.89 是开发版）。
    // 但该假设对非核心 GNOME 库不成立（libepoxy 1.5 是稳定版）——yaml 可用 stable-minor: all 关闭。
    let even_only = cfg.stable_minor.as_deref().unwrap_or("even") == "even";
    let mut candidates: Vec<&String> = if even_only {
        let even: Vec<&String> = dirs.iter().filter(|d| minor_is_even(d)).collect();
        if even.is_empty() {
            dirs.iter().collect()
        } else {
            even
        }
    } else {
        dirs.iter().collect()
    };
    if candidates.is_empty() {
        return Err("gnome 目录列表无版本子目录".into());
    }
    // major-version-lock / major-of：只保留该主版本的目录（gtk3 锁 3 → 只留 3.x，不误入 4.x）
    if let Some(m) = major {
        candidates.retain(|d| templates::matches_major(d, Some(m)));
    }
    // max-version：只保留不超过封顶版本的目录（gtk3 的 3.98 是历史 dev 分支，封顶 3.24）
    if let Some(cap) = &cfg.max_version {
        candidates.retain(|d| vercmp::cmp_version(d, cap) != std::cmp::Ordering::Greater);
    }
    if candidates.is_empty() {
        return Err("gnome 目录无匹配主版本的子目录".into());
    }
    // 降序遍历候选目录，取第一个有稳定版本文件的：2.90 可能只有 alpha 快照 → 落到 2.80
    candidates.sort_by(|a, b| vercmp::cmp_version(b, a));
    let file_re = Regex::new(&format!(
        r"{}-(\d[\d.]*)\.tar\.(?:xz|gz)",
        regex::escape(name)
    ))
    .map_err(|e| e.to_string())?;
    let mut found: Option<(String, String)> = None; // (dir, version)
    for d in &candidates {
        let level2 = format!("{level1}{d}/");
        let html2 = match fetcher.get(&level2) {
            Ok(h) => h,
            Err(_) => continue,
        };
        if let Some(v) = templates::max_match(&file_re, &html2, major, None) {
            found = Some((d.to_string(), v));
            break;
        }
    }
    let (dir, version) = found.ok_or("gnome 目录无匹配稳定版本文件")?;
    let url = templates::substitute(
        template,
        &[
            ("name", name),
            ("path_version", &dir),
            ("version", &version),
        ],
    );
    Ok(EntryProbe { version, url })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::net::MockFetcher;
    use std::collections::HashMap;

    #[test]
    fn probe_two_level() {
        let f = MockFetcher::new(HashMap::new())
            .entry(
                "https://download.gnome.org/sources/glib/",
                "2.82/\n2.84/\n2.80/\n",
            )
            .entry(
                "https://download.gnome.org/sources/glib/2.84/",
                "glib-2.84.0.tar.xz\nglib-2.83.0.tar.xz\n",
            );
        let cfg = SourceConfig {
            tracker_template: "gnome".into(),
            template: Some(
                "https://download.gnome.org/sources/{name}/{path_version}/{name}-{version}.tar.xz"
                    .into(),
            ),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "glib").unwrap();
        assert_eq!(r.version, "2.84.0");
        assert_eq!(
            r.url,
            "https://download.gnome.org/sources/glib/2.84/glib-2.84.0.tar.xz"
        );
    }

    #[test]
    fn probe_prefers_even_minor_stable() {
        // 2.89 是开发分支（odd minor），最新稳定应选 2.88
        let f = MockFetcher::new(HashMap::new())
            .entry(
                "https://download.gnome.org/sources/glib/",
                "2.88/\n2.89/\n2.86/\n",
            )
            .entry(
                "https://download.gnome.org/sources/glib/2.88/",
                "glib-2.88.3.tar.xz\nglib-2.88.0.tar.xz\n",
            );
        let cfg = SourceConfig {
            tracker_template: "gnome".into(),
            template: Some(
                "https://download.gnome.org/sources/{name}/{path_version}/{name}-{version}.tar.xz"
                    .into(),
            ),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "glib").unwrap();
        assert_eq!(r.version, "2.88.3");
    }

    #[test]
    fn probe_source_name_and_major_lock() {
        // gtk3：source-name 探测 gtk 目录，major-version-lock 只留 3.x（不误入 4.x）
        let f = MockFetcher::new(HashMap::new())
            .entry(
                "https://download.gnome.org/sources/gtk/",
                "3.24/\n4.19/\n4.20/\n",
            )
            .entry(
                "https://download.gnome.org/sources/gtk/3.24/",
                "gtk-3.24.50.tar.xz\n",
            );
        let cfg = SourceConfig {
            tracker_template: "gnome".into(),
            source_name: Some("gtk".into()),
            major_version_lock: Some("3".into()),
            template: Some(
                "https://download.gnome.org/sources/gtk/{path_version}/gtk-{version}.tar.xz".into(),
            ),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "gtk3").unwrap();
        assert_eq!(r.version, "3.24.50");
        assert_eq!(
            r.url,
            "https://download.gnome.org/sources/gtk/3.24/gtk-3.24.50.tar.xz"
        );
    }

    #[test]
    fn probe_falls_through_when_max_dir_only_alpha() {
        // 2.90 只有 alpha 快照（无稳定文件）→ 降级落到 2.80 稳定版
        let f = MockFetcher::new(HashMap::new())
            .entry(
                "https://download.gnome.org/sources/glib-networking/",
                "2.74/\n2.76/\n2.78/\n2.80/\n2.90/\n",
            )
            .entry(
                "https://download.gnome.org/sources/glib-networking/2.90/",
                "glib-networking-2.90.alpha.tar.xz\n",
            )
            .entry(
                "https://download.gnome.org/sources/glib-networking/2.80/",
                "glib-networking-2.80.1.tar.xz\n",
            );
        let cfg = SourceConfig {
            tracker_template: "gnome".into(),
            template: Some(
                "https://download.gnome.org/sources/{name}/{path_version}/{name}-{version}.tar.xz"
                    .into(),
            ),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "glib-networking").unwrap();
        assert_eq!(r.version, "2.80.1");
        assert_eq!(
            r.url,
            "https://download.gnome.org/sources/glib-networking/2.80/glib-networking-2.80.1.tar.xz"
        );
    }

    #[test]
    fn probe_single_segment_dirs_no_parity_filter() {
        // 单段目录每个版本号都是正式版：奇数 45 有真实发布也应选中（不按奇偶排除）；
        // 51 只有 alpha → 降级跳过
        let f = MockFetcher::new(HashMap::new())
            .entry(
                "https://download.gnome.org/sources/gnome-desktop/",
                "44/\n45/\n51/\n",
            )
            .entry(
                "https://download.gnome.org/sources/gnome-desktop/45/",
                "gnome-desktop-45.1.tar.xz\n",
            )
            .entry(
                "https://download.gnome.org/sources/gnome-desktop/51/",
                "gnome-desktop-51.alpha.tar.xz\n",
            );
        let cfg = SourceConfig {
            tracker_template: "gnome".into(),
            template: Some(
                "https://download.gnome.org/sources/{name}/{path_version}/{name}-{version}.tar.xz"
                    .into(),
            ),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "gnome-desktop").unwrap();
        assert_eq!(r.version, "45.1");
        assert_eq!(
            r.url,
            "https://download.gnome.org/sources/gnome-desktop/45/gnome-desktop-45.1.tar.xz"
        );
    }

    #[test]
    fn probe_single_segment_even_dirs() {
        // 现代 GNOME 单段目录：44(偶=稳定)/51(奇=开发)；旧式 3.38 不再被误选为最新
        let f = MockFetcher::new(HashMap::new())
            .entry(
                "https://download.gnome.org/sources/gnome-desktop/",
                "3.38/\n40/\n41/\n42/\n43/\n44/\n51/\n",
            )
            .entry(
                "https://download.gnome.org/sources/gnome-desktop/44/",
                "gnome-desktop-44.5.tar.xz\n",
            );
        let cfg = SourceConfig {
            tracker_template: "gnome".into(),
            template: Some(
                "https://download.gnome.org/sources/{name}/{path_version}/{name}-{version}.tar.xz"
                    .into(),
            ),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "gnome-desktop").unwrap();
        assert_eq!(r.version, "44.5");
    }

    #[test]
    fn probe_stable_minor_all_ignores_parity() {
        // libepoxy：1.4(even)/1.5(odd)，odd 才是稳定版 → stable-minor: all 应选 1.5
        let f = MockFetcher::new(HashMap::new())
            .entry(
                "https://download.gnome.org/sources/libepoxy/",
                "1.4/\n1.5/\n",
            )
            .entry(
                "https://download.gnome.org/sources/libepoxy/1.5/",
                "libepoxy-1.5.10.tar.xz\n",
            );
        let cfg = SourceConfig {
            tracker_template: "gnome".into(),
            stable_minor: Some("all".into()),
            template: Some(
                "https://download.gnome.org/sources/{name}/{path_version}/{name}-{version}.tar.xz"
                    .into(),
            ),
            ..Default::default()
        };
        let r = probe(&f, &cfg, None, "libepoxy").unwrap();
        assert_eq!(r.version, "1.5.10");
    }
}
