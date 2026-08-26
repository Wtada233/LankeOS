//! same-version 模板：**直接锁定另一包的版本**（不经任何上游网络探测）。
//!
//! 与其它模板不同：它不抓取上游，而是读 `same-version-of` 指定包的已解析版本
//! （本轮 `--all` 解析出的新版本优先，否则 LankeBUILD.json 版本），用它构建下载 URL。
//! 因此它**直接确定版本号**，是一个独立探测来源，而不是其它模板的"约束"——所以它
//! 是独立模板，`same-version-of` 只能在这里用，其它模板写 `same-version`/`same-version-of` 都报错。
//!
//! 字段收得最简：只认 `same-version-of` + `template`，占位符仅 `{version}` 与 `{major_minor}`
//! （tag 前缀 / 仓库路径 / 上游名都烘进 template，不支持 tag-prefix/repo/source-name/{tag}/{name}）。

use crate::track::templates;
use crate::track::{need, validate_url, EntryProbe, SourceConfig};

/// 探测：锁定 `same-version-of` 指定包的版本，用 template 拼出该槽位 URL。
/// 签名与其它模板不同（`lookup` 取代 `major`）：它不联网，直接读版本输入。
pub fn probe(
    cfg: &SourceConfig,
    lookup: &dyn Fn(&str) -> Option<String>,
    _pkg_name: &str,
) -> Result<EntryProbe, String> {
    let target = need(&cfg.same_version_of, "same-version-of")?;
    let v = lookup(target).ok_or_else(|| {
        format!("same-version-of 依赖 {target} 无版本（读 LankeBUILD.json/已解析版本失败）")
    })?;
    let template = need(&cfg.template, "template")?;
    // {major_minor} = 版本前两段（qt6 等目录结构 qt/<6.11>/<6.11.1>/，不能锁死 minor）。
    let major_minor: String = v.split('.').take(2).collect::<Vec<_>>().join(".");
    let vars = vec![("version", v.as_str()), ("major_minor", major_minor.as_str())];
    let url = templates::substitute(template, &vars);
    validate_url(&url)?;
    Ok(EntryProbe { version: v, url })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn locks_version_and_builds_url() {
        let cfg = SourceConfig {
            tracker_template: "same-version".into(),
            same_version_of: Some("vulkan-headers".into()),
            template: Some(
                "https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-{version}.tar.gz"
                    .into(),
            ),
            ..Default::default()
        };
        let r = probe(
            &cfg,
            &|pkg| (pkg == "vulkan-headers").then(|| "1.4.350.1".to_string()),
            "SPIRV-Headers",
        )
        .unwrap();
        assert_eq!(r.version, "1.4.350.1");
        assert_eq!(
            r.url,
            "https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz"
        );
    }

    #[test]
    fn major_minor_placeholder() {
        let cfg = SourceConfig {
            tracker_template: "same-version".into(),
            same_version_of: Some("qt6-base".into()),
            template: Some(
                "https://download.qt.io/official_releases/qt/{major_minor}/{version}/submodules/qtdeclarative-everywhere-src-{version}.tar.xz"
                    .into(),
            ),
            ..Default::default()
        };
        let r = probe(
            &cfg,
            &|pkg| (pkg == "qt6-base").then(|| "6.12.1".to_string()),
            "qt6-declarative",
        )
        .unwrap();
        assert_eq!(
            r.url,
            "https://download.qt.io/official_releases/qt/6.12/6.12.1/submodules/qtdeclarative-everywhere-src-6.12.1.tar.xz"
        );
    }
}
