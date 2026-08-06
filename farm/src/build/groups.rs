//! groups.rs — `data/build/*.yaml` 声明式重建组。
//!
//! 构建顺序**只根据 needed_so**（链接依赖才必须重建）；但"不链 libpython 但 ABI 敏感"的包
//! （python-cairo / python-gobject / blueman / meson …）不产生链接边 → 不会自动成为 ABI 受害者。
//! 用声明式 YAML 声明：某包 SONAME 断裂时强制重建哪些包（`*` glob）。
//!
//! ```yaml
//! # data/build/python.yaml
//! rebuild-on-abichange: python
//! packages: python-* meson gobject-introspection blueman
//! ```
//!
//! **version-change 组**（纯脚本解释器如 perl 没有 libperl.so，SONAME 检测无从谈起 → abichange
//! 永不触发）：`rebuild-on-version-change` + `version-change-script`，脚本接收 `OLD_VER`/`NEW_VER`
//! 环境变量，exit 0 = 重建组受害者（如 minor 变才重建），非零 = 跳过。
//!
//! ```yaml
//! # data/build/perl.yaml
//! rebuild-on-version-change: perl
//! version-change-script: |
//!   #!/bin/bash
//!   [ "$(printf '%s' "$OLD_VER" | cut -d. -f1-2)" != "$(printf '%s' "$NEW_VER" | cut -d. -f1-2)" ]
//! packages: perl-*
//! ```
//!
//! 与 data/trackers 同一套模式：触发包按包名索引，`packages` 是空格分隔的 glob。

use std::collections::{HashMap, HashSet};
use std::path::Path;
use std::sync::atomic::{AtomicU32, Ordering};

use crate::tr;

#[derive(Debug, Clone, serde::Deserialize)]
struct RebuildGroupRaw {
    #[serde(rename = "rebuild-on-abichange")]
    on_abi: Option<String>,
    #[serde(rename = "rebuild-on-version-change")]
    on_version: Option<String>,
    /// version-change 组专用：bash 脚本，`OLD_VER`/`NEW_VER` 环境变量，exit 0 = 重建。
    #[serde(rename = "version-change-script", default)]
    version_script: Option<String>,
    /// 空格分隔的 glob 列表（如 `python-* meson blueman`）
    #[serde(default)]
    packages: String,
}

/// version-change 组：`on` 包版本变化时用脚本判定是否重建。
#[derive(Debug, Clone)]
struct VersionChangeGroup {
    /// 判定脚本（OLD_VER/NEW_VER 环境变量，exit 0 = 重建）
    script: String,
    /// 空格分隔的 glob 列表
    globs: Vec<String>,
}

/// 全部重建组：`on` 包 → 需要重建的 glob 模式。
#[derive(Debug, Default)]
pub struct RebuildGroups {
    /// abichange 组：`on` 包 SONAME 断裂 → 组受害者重建
    abi: HashMap<String, Vec<String>>,
    /// version-change 组：`on` 包版本变化且脚本判定通过 → 组受害者重建
    version: HashMap<String, VersionChangeGroup>,
}

impl RebuildGroups {
    /// 扫描 `data/build/*.yaml` 加载全部重建组。目录缺失/空 → 空组（无害）。
    pub fn load(data_dir: &Path) -> RebuildGroups {
        let mut groups = RebuildGroups::default();
        if let Ok(rd) = std::fs::read_dir(data_dir) {
            for entry in rd.flatten() {
                let path = entry.path();
                if path.extension().and_then(|e| e.to_str()) != Some("yaml") {
                    continue;
                }
                let Ok(content) = std::fs::read_to_string(&path) else { continue };
                let Ok(cfg) = serde_yaml::from_str::<RebuildGroupRaw>(&content) else {
                    eprintln!("{}", tr!("build.group_parse_fail", path.display()));
                    continue;
                };
                let globs: Vec<String> = cfg.packages.split_whitespace().map(String::from).collect();
                if let Some(on) = cfg.on_abi {
                    groups.abi.entry(on).or_default().extend(globs.clone());
                }
                if let Some(on) = cfg.on_version {
                    match cfg.version_script {
                        Some(script) => {
                            groups.version.insert(on, VersionChangeGroup { script, globs });
                        }
                        None => {
                            eprintln!("{}", tr!("build.group_parse_fail", path.display()));
                        }
                    }
                }
            }
        }
        groups
    }

    /// `on` 包 ABI 断裂时，匹配 `packages` glob 的包集合（名字升序，确定性）。
    /// `all_pkgs` = 全部配方包名（`sorted_pkg_names`）；只匹配存在的包。
    pub fn victims_for(&self, on: &str, all_pkgs: &[String]) -> Vec<String> {
        let mut set = HashSet::new();
        if let Some(globs) = self.abi.get(on) {
            for g in globs {
                for p in all_pkgs {
                    if glob_match(g, p) {
                        set.insert(p.clone());
                    }
                }
            }
        }
        let mut v: Vec<String> = set.into_iter().collect();
        v.sort();
        v
    }

    /// `on` 包版本变化时，按 version-change 脚本判定是否重建组受害者。
    /// 脚本接收 `OLD_VER`/`NEW_VER` 环境变量，exit 0 = 重建（返回全部 glob 匹配包），
    /// 非零 = 跳过（返回空）。脚本运行失败（bash 不存在等）→ Err（调用方告警后跳过）。
    pub fn version_victims_if(
        &self,
        on: &str,
        old_ver: &str,
        new_ver: &str,
        all_pkgs: &[String],
    ) -> Result<Vec<String>, String> {
        let Some(g) = self.version.get(on) else {
            return Ok(Vec::new());
        };
        if !script_decides_rebuild(&g.script, old_ver, new_ver)? {
            return Ok(Vec::new());
        }
        let mut set = HashSet::new();
        for glob in &g.globs {
            for p in all_pkgs {
                if p != on && glob_match(glob, p) {
                    set.insert(p.clone());
                }
            }
        }
        let mut v: Vec<String> = set.into_iter().collect();
        v.sort();
        Ok(v)
    }

    /// (victim, on) 依赖边：`on` 的组受害者必须在 `on` 构建**之后**构建。
    /// 这与 needed_so 链接边同等对待——**`--all` 模式下组受害者已在初始队列**，
    /// 若没有这条边，topo 只按链接边排，python-cairo（不链 libpython）会被排到
    /// python 之前，容器里 `lpkg upgrade` 时本地 repo 还是旧 python，构建白跑。
    /// version-change 组同样参与：on 包在本轮重建时，组受害者必须排在其后。
    ///
    /// 只包含**两边都在 `names` 中**的边（`on` 不在本轮 targets 里就没有排序约束）；
    /// 去重且 (victim, on) 字典序排序，确定性。
    pub fn trigger_edges_in(&self, names: &[String]) -> Vec<(String, String)> {
        let present: HashSet<&str> = names.iter().map(String::as_str).collect();
        let mut edges: Vec<(String, String)> = Vec::new();
        for (on, globs) in &self.abi {
            if !present.contains(on.as_str()) {
                continue;
            }
            for g in globs {
                for p in names {
                    if p.as_str() != on.as_str() && glob_match(g, p) {
                        edges.push((p.clone(), on.clone()));
                    }
                }
            }
        }
        for (on, g) in &self.version {
            if !present.contains(on.as_str()) {
                continue;
            }
            for glob in &g.globs {
                for p in names {
                    if p.as_str() != on.as_str() && glob_match(glob, p) {
                        edges.push((p.clone(), on.clone()));
                    }
                }
            }
        }
        edges.sort();
        edges.dedup();
        edges
    }
}

/// version-change 脚本运行序号（进程内唯一，保证临时脚本名互不冲突）。
static SCRIPT_SEQ: AtomicU32 = AtomicU32::new(0);

/// 运行 version-change 判定脚本：`OLD_VER`/`NEW_VER` 环境变量，exit 0 = 重建，非零 = 跳过。
/// 脚本执行失败（bash 不存在等）→ Err；脚本自身非零退出（minor 未变）**不是错误**，返回 Ok(false)。
fn script_decides_rebuild(script: &str, old_ver: &str, new_ver: &str) -> Result<bool, String> {
    let tmp = std::env::temp_dir().join(format!(
        "lankefarm-version-change-{}-{}.sh",
        std::process::id(),
        SCRIPT_SEQ.fetch_add(1, Ordering::Relaxed),
    ));
    std::fs::write(&tmp, script).map_err(|e| format!("写 version-change 脚本失败: {e}"))?;
    let out = std::process::Command::new("bash")
        .arg(&tmp)
        .env("OLD_VER", old_ver)
        .env("NEW_VER", new_ver)
        .output()
        .map_err(|e| format!("运行 version-change 脚本失败: {e}"))?;
    let _ = std::fs::remove_file(&tmp);
    if !out.status.success() {
        // 非零 = 明确"不重建"（如 minor 未变）；stderr 非空时打印，辅助排查脚本错误
        let stderr = String::from_utf8_lossy(&out.stderr);
        if !stderr.trim().is_empty() {
            eprintln!("  [version-change] {}→{} 判定脚本 stderr: {}", old_ver, new_ver, stderr.trim());
        }
        return Ok(false);
    }
    Ok(true)
}

/// 简单 glob：只支持 `*`（任意字符序列）。确定性、无 panic。
/// `python-*` 匹配 `python-cairo`（不匹配 `python` 本身）。
pub fn glob_match(pattern: &str, s: &str) -> bool {
    let p: Vec<char> = pattern.chars().collect();
    let t: Vec<char> = s.chars().collect();
    let (mut pi, mut ti) = (0usize, 0usize);
    let (mut star, mut star_ti) = (None, 0usize);
    while ti < t.len() {
        if pi < p.len() && p[pi] == t[ti] {
            pi += 1;
            ti += 1;
        } else if pi < p.len() && p[pi] == '*' {
            star = Some(pi);
            star_ti = ti;
            pi += 1;
        } else if let Some(sp) = star {
            pi = sp + 1;
            star_ti += 1;
            ti = star_ti;
        } else {
            return false;
        }
    }
    while pi < p.len() && p[pi] == '*' {
        pi += 1;
    }
    pi == p.len()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn glob_match_star_wildcard() {
        assert!(glob_match("python-*", "python-cairo"));
        assert!(glob_match("python-*", "python-gobject"));
        assert!(!glob_match("python-*", "python"), "需要 `python-` 前缀");
        assert!(!glob_match("python-*", "python3"));
        assert!(glob_match("*", "anything"));
        assert!(glob_match("blueman", "blueman"));
        assert!(!glob_match("blueman", "blue"));
        assert!(glob_match("a*c", "abc"));
        assert!(glob_match("a*c", "ac"));
        assert!(!glob_match("a*c", "ab"));
    }

    #[test]
    fn load_parses_multiple_space_separated_globs() {
        // 回归：从真实 YAML 内容走 serde_yaml 路径（不是手动 map.insert），
        // 确认 `packages: python-* meson gobject-introspection blueman` 解析出全部 4 个 glob，
        // 而不是只解析出第一个。
        let dir = std::env::temp_dir().join(format!("farm-groups-load-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(
            dir.join("python.yaml"),
            "rebuild-on-abichange: python\npackages: python-* meson gobject-introspection blueman\n",
        )
        .unwrap();
        let g = RebuildGroups::load(&dir);
        let all: Vec<String> = [
            "python",
            "python-cairo",
            "python-gobject",
            "meson",
            "gobject-introspection",
            "blueman",
            "glib",
        ]
        .iter()
        .map(|s| s.to_string())
        .collect();
        let v = g.victims_for("python", &all);
        assert_eq!(
            v,
            vec![
                "blueman",
                "gobject-introspection",
                "meson",
                "python-cairo",
                "python-gobject"
            ],
            "serde_yaml 必须解析出全部 4 个 glob: {v:?}"
        );
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn trigger_edges_only_when_on_in_names() {
        let mut groups = RebuildGroups::default();
        groups.abi.insert(
            "python".into(),
            vec!["python-*".into(), "meson".into()],
        );
        // 两边都在 names → 产出边
        let all: Vec<String> = ["python", "python-cairo", "python-gobject", "meson"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let e = groups.trigger_edges_in(&all);
        assert_eq!(
            e,
            vec![
                ("meson".to_string(), "python".to_string()),
                ("python-cairo".to_string(), "python".to_string()),
                ("python-gobject".to_string(), "python".to_string()),
            ]
        );
        // on 不在 names → 无约束（python 不重建，组受害者不强制排序）
        let without_on: Vec<String> = ["python-cairo", "meson"].iter().map(|s| s.to_string()).collect();
        assert!(groups.trigger_edges_in(&without_on).is_empty());
        // 去重：同一 (victim,on) 不会被重复产出
        let dup: Vec<String> = ["python", "python-cairo", "python-cairo", "meson"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let e2 = groups.trigger_edges_in(&dup);
        assert_eq!(e2.len(), 2, "python-cairo 只应出现一次: {e2:?}");
    }

    #[test]
    fn victims_for_matches_globs_and_sorts() {
        let mut groups = RebuildGroups::default();
        groups.abi.insert(
            "python".into(),
            vec!["python-*".into(), "meson".into(), "blueman".into()],
        );
        let all: Vec<String> = ["python", "python-cairo", "python-gobject", "meson", "blueman", "glib"]
            .iter()
            .map(|s| s.to_string())
            .collect();
        let v = groups.victims_for("python", &all);
        assert_eq!(v, vec!["blueman", "meson", "python-cairo", "python-gobject"]);
        assert!(groups.victims_for("unlisted", &all).is_empty());
    }

    #[test]
    fn version_change_script_decides_by_minor() {
        // 纯解释器（perl 无 libperl.so）：SONAME 检测无从谈起，改用 version-change 组。
        // 脚本比对 OLD_VER/NEW_VER 的 minor：minor 变（5.44→5.45）→ 重建；patch（5.44.0→5.44.1）→ 跳过。
        let script = r#"#!/bin/bash
[ "$(printf '%s' "$OLD_VER" | cut -d. -f1-2)" != "$(printf '%s' "$NEW_VER" | cut -d. -f1-2)" ]
"#;
        let all: Vec<String> = vec![
            "perl".to_string(),
            "perl-xml-parser".to_string(),
            "perl-file-sharedir".to_string(),
            "glib".to_string(),
        ];
        // minor 变 → 脚本 exit 0 → 全部 perl-* 受害者
        let mut g = RebuildGroups::default();
        g.version.insert(
            "perl".into(),
            VersionChangeGroup { script: script.into(), globs: vec!["perl-*".into()] },
        );
        let v = g.version_victims_if("perl", "5.44.0", "5.45.0", &all).unwrap();
        assert_eq!(v, vec!["perl-file-sharedir", "perl-xml-parser"]);
        // patch 变 → 脚本 exit 1 → 空（perl 模块无需重建）
        let v = g.version_victims_if("perl", "5.44.0", "5.44.1", &all).unwrap();
        assert!(v.is_empty(), "patch 升级不应触发 perl-* 重建: {v:?}");
        // release 修订（5.44.0+1→5.44.0+2）也不算 minor 变化
        let v = g.version_victims_if("perl", "5.44.0+1", "5.44.0+2", &all).unwrap();
        assert!(v.is_empty());
        // 未注册的 on 包 → 空
        assert!(g.version_victims_if("python", "3.14", "3.15", &all).unwrap().is_empty());
    }

    #[test]
    fn version_change_group_loads_and_edges() {
        // 真实 YAML 走 serde_yaml 路径：rebuild-on-version-change + version-change-script + packages
        let dir = std::env::temp_dir().join(format!("farm-groups-vload-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(
            dir.join("perl.yaml"),
            r#"rebuild-on-version-change: perl
version-change-script: |
  #!/bin/bash
  [ "$OLD_VER" != "$NEW_VER" ]
packages: perl-*
"#,
        )
        .unwrap();
        let g = RebuildGroups::load(&dir);
        // version_victims_if 走真实脚本：版本不同 → 重建
        let all: Vec<String> = ["perl", "perl-xml-parser"].iter().map(|s| s.to_string()).collect();
        let v = g.version_victims_if("perl", "5.44", "5.45", &all).unwrap();
        assert_eq!(v, vec!["perl-xml-parser"]);
        // trigger_edges_in：on 在 names 里 → (victim, on) 边（perl-* 组受害者排在 perl 之后）
        let names: Vec<String> = ["perl", "perl-xml-parser"].iter().map(|s| s.to_string()).collect();
        assert_eq!(g.trigger_edges_in(&names), vec![("perl-xml-parser".to_string(), "perl".to_string())]);
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn version_change_without_script_is_skipped() {
        // rebuild-on-version-change 声明了但缺 version-change-script → 组不注册（load 告警跳过），
        // version_victims_if 返回空而非 panic。
        let dir = std::env::temp_dir().join(format!("farm-groups-vnoscript-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("perl.yaml"), "rebuild-on-version-change: perl\npackages: perl-*\n").unwrap();
        let g = RebuildGroups::load(&dir);
        let all: Vec<String> = ["perl", "perl-xml-parser"].iter().map(|s| s.to_string()).collect();
        assert!(g.version_victims_if("perl", "5.44", "5.45", &all).unwrap().is_empty());
        assert!(g.trigger_edges_in(&all).is_empty());
        std::fs::remove_dir_all(&dir).ok();
    }
}
