//! same-version-of-source 模板：**锁本 tracker 中更早槽位的版本**（不经任何网络探测）。
//!
//! 与 `same-version` 是同一抽象层的两个入口，差别只在"版本从哪来"：
//! - `same-version`（`same-version-of: <包名>`）：读**另一个包**的已解析版本——本轮解析出的优先，
//!   否则 LankeBUILD.json。跨包，因此受"那个包本轮是否已处理完"影响。
//! - `same-version-of-source`（`same-version-of-source: sources[i]`）：读**同一 tracker 里、位于
//!   本条目之前**的槽位在**本轮**解析出的版本。不跨包，也就没有"另一个包还没轮到"的问题。
//!
//! 它治的具体 bug：某包有多个 source，第二个 source 的 URL 里嵌着**本包自己的**版本号，那条于是写成
//! `same-version-of: <本包名>`；而本包这一轮刚探出新版本时，`same-version-of` 取到的仍是
//! LankeBUILD.json 里的**旧**版本 → 一次探测产出"新版本号 + 用旧版本拼的 URL"这种自相矛盾的清单
//! （与 `version-var` 治的是同一类病：两次取版本之间有窗口）。
//!
//! 排序规则与 `version-var` / multi-level 的"占位符只能引用前面的级"完全一致：`sources` 先于
//! `work_sources` 探测，列表内从左到右；**只能引用位于它之前的槽位**，前向 / 自引用 / 越界在探测时
//! 报错并给出可用范围（不猜、不静默给空值）。
//!
//! 字段与 `same-version` 一样收到最简：只认 `same-version-of-source` + `template`，占位符也完全一致
//! （`{version}` / `{major_minor}` = 前 2 段 / `{major_minor_patch}` = 前 3 段 / `{version:N}` 前 N 段；
//! tag 前缀 / 仓库路径 / 上游名都烘进 template）。libreoffice 的 help/translations 两个源就是本模板的
//! 用例：版本 4 段 `26.8.0.3`、目录段 3 段 `26.8.0` → template 写
//! `…/src/{major_minor_patch}/libreoffice-help-{version}.tar.xz`。

use crate::error::FarmError;
use crate::track::templates;
use crate::track::{need, resolved_slot_version, EntryProbe, ResolvedSlots, SourceConfig};

/// 探测：取指定槽位**本轮**的版本，用 template 拼出本槽位 URL。
/// 签名与其它模板不同（`resolved` 取代 `fetcher`/`major`）：它不联网，直接读已探测槽位。
/// `pub(crate)`（而非 `pub`）：参数 `ResolvedSlots` 本身是 crate 内类型。
pub(crate) fn probe(cfg: &SourceConfig, resolved: &ResolvedSlots) -> Result<EntryProbe, FarmError> {
    let sel = need(&cfg.same_version_of_source, "same-version-of-source")?;
    let v = resolved_slot_version(sel, resolved, &format!("same-version-of-source {sel}"))?;
    let template = need(&cfg.template, "template")?;
    // 残留占位符不在这里判：`track::probe_with` 对**每个产出槽位**统一 validate_url（唯一一处）。
    let url = templates::substitute_locked_version(template, &v);
    Ok(EntryProbe { version: v, url })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn probe_at(
        sel: &str,
        template: &str,
        resolved: &ResolvedSlots,
    ) -> Result<EntryProbe, FarmError> {
        let cfg = SourceConfig {
            tracker_template: "same-version-of-source".into(),
            same_version_of_source: Some(sel.into()),
            template: Some(template.into()),
            ..Default::default()
        };
        probe(&cfg, resolved)
    }

    /// sources 槽位池（version, url 成对）。
    fn src_slots(pairs: &[(&str, &str)]) -> ResolvedSlots {
        ResolvedSlots {
            sources: pairs
                .iter()
                .map(|(v, u)| EntryProbe {
                    version: (*v).into(),
                    url: (*u).into(),
                })
                .collect(),
            work_sources: Vec::new(),
        }
    }

    #[test]
    fn takes_version_from_earlier_slot() {
        // 目标场景：sources[1] 的 URL 里嵌的是 sources[0] **本轮**解出的版本，而不是 json 里的旧版本。
        let r = probe_at(
            "sources[0]",
            "https://download.docker.com/linux/static/stable/x86_64/docker-{version}.tgz",
            &src_slots(&[("28.4.0", "https://example.invalid/docker-28.4.0.tgz")]),
        )
        .unwrap();
        assert_eq!(r.version, "28.4.0");
        assert_eq!(
            r.url,
            "https://download.docker.com/linux/static/stable/x86_64/docker-28.4.0.tgz"
        );
    }

    #[test]
    fn major_minor_placeholder() {
        let r = probe_at(
            "sources[0]",
            "https://download.qt.io/official_releases/qt/{major_minor}/{version}/submodules/qtdeclarative-everywhere-src-{version}.tar.xz",
            &src_slots(&[("6.12.1", "https://example.invalid/qt-6.12.1.tar.xz")]),
        )
        .unwrap();
        assert_eq!(
            r.url,
            "https://download.qt.io/official_releases/qt/6.12/6.12.1/submodules/qtdeclarative-everywhere-src-6.12.1.tar.xz"
        );
    }

    #[test]
    fn unreachable_slot_errors_with_available_range() {
        // 没有更早的槽位（自引用/前向/越界）→ 报错，并说明已探测了多少条
        let err = probe_at(
            "sources[0]",
            "https://x.invalid/{version}",
            &ResolvedSlots::default(),
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("尚不可用"), "{err}");
        assert!(err.contains("sources 已探测 0 条"), "{err}");

        // 越界同理（池里只有 1 条，却引用 sources[3]）
        let err = probe_at(
            "sources[3]",
            "https://x.invalid/{version}",
            &src_slots(&[("1.0", "https://example.invalid/1.0")]),
        )
        .unwrap_err()
        .to_string();
        assert!(err.contains("尚不可用"), "{err}");
        assert!(err.contains("sources 已探测 1 条"), "{err}");
    }

    #[test]
    fn bad_selector_and_missing_template_error() {
        let slots = src_slots(&[("1.0", "https://example.invalid/1.0")]);
        assert!(probe_at("source[0]", "https://x.invalid/{version}", &slots).is_err());
        // template 缺失 → 报"缺 template"（版本已解出，也不会静默产出一个空 URL）
        let cfg = SourceConfig {
            tracker_template: "same-version-of-source".into(),
            same_version_of_source: Some("sources[0]".into()),
            ..Default::default()
        };
        let e = probe(&cfg, &slots).unwrap_err().to_string();
        assert!(e.contains("缺 template"), "{e}");
    }
}
