//! gnome 模板：两级版本目录探测（download.gnome.org/sources/{name}/{x.y}/）。
//!
//! **被动触发**：source 条目里 `tracker-template: gnome` + `template`。
//! 稳定分支惯例：偶 minor 为稳定版（2.88 稳定，2.89 开发），优先取最大偶 minor 目录。

use crate::error::FarmError;
use regex::Regex;

use crate::net::Fetcher;
use crate::track::templates;
use crate::track::vercmp;
use crate::track::{need, EntryProbe, SourceConfig};

/// 探测最新稳定版本：第一级版本目录（偶 minor 优先）→ 第二级文件。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &SourceConfig,
    major: Option<&str>,
    pkg_name: &str,
) -> Result<EntryProbe, FarmError> {
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
    if dirs.is_empty() {
        return Err("gnome 目录列表无版本子目录".into());
    }
    // 约束筛选（major-of / max-version 封顶 / exclude / 奇偶偏好）**走共享汇点** —— 与其它模板
    // 同一处实现（`pool_filter`），不再在本文件里重写一遍 major/封顶/even 的判定。
    let mut f = templates::version_filter(cfg, major)?;
    // GNOME 惯例：`stable-minor` **未显式设置**时默认按偶 minor 取稳定分支（2.89 是开发版）。
    // 该假设对非核心 GNOME 库不成立（libepoxy 1.5 是稳定版）——yaml 写 `stable-minor: all` 关闭。
    f.even_minor = cfg.stable_minor.as_deref() != Some("all");
    let mut candidates: Vec<String> = templates::pool_filter(dirs, &f);
    if candidates.is_empty() {
        return Err("gnome 目录无匹配主版本/封顶的子目录".into());
    }
    // 降序遍历候选目录，取第一个有稳定版本文件的：2.90 可能只有 alpha 快照 → 落到 2.80
    candidates.sort_by(|a, b| vercmp::cmp_version(b, a));
    let file_re = Regex::new(&format!(
        r"{}-(\d[\d.]*)\.tar\.(?:xz|gz)",
        regex::escape(name)
    ))
    .map_err(|e| e.to_string())?;
    // 文件层用**去掉封顶**的副本：`max-version` 已在目录层判过，而目录里的文件版本串更长
    // （`3.24.1` vs 封顶 `3.24`），再按封顶过滤会让整个目录落空（gtk3 等封顶场景）。
    // 其余约束（major / exclude / 奇偶）保持不变。
    let f_file = f.without_cap();
    let mut found: Option<(String, String)> = None; // (dir, version)
    for d in &candidates {
        let level2 = format!("{level1}{d}/");
        let html2 = match fetcher.get(&level2) {
            Ok(h) => h,
            Err(_) => continue,
        };
        if let Some(v) = templates::max_match(&file_re, &html2, &f_file) {
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
    fn hard_constraints_apply_before_even_preference() {
        // 唯一偶 minor 的目录被 major 约束排除时**不应**整体落空：硬约束（allows）先判、奇偶偏好后判。
        // 历史实现是先算 even 池（在未过滤的目录上）再判 major ⇒ 这里会报"无匹配主版本的子目录"。
        let f = MockFetcher::new(HashMap::new())
            .entry("https://download.gnome.org/sources/glib/", "2.10/\n3.1/\n")
            .entry(
                "https://download.gnome.org/sources/glib/3.1/",
                "glib-3.1.5.tar.xz\n",
            );
        let cfg = SourceConfig {
            tracker_template: "gnome".into(),
            template: Some(
                "https://download.gnome.org/sources/{name}/{path_version}/{name}-{version}.tar.xz"
                    .into(),
            ),
            ..Default::default()
        };
        // 模板层 `major` 是**参数**（`major-version-lock` / `major-of` 在 mod.rs 里解析后才传进来）
        let r = probe(&f, &cfg, Some("3"), "glib").unwrap();
        assert_eq!(r.version, "3.1.5");
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
