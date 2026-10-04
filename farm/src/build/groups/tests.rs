//! `src/build/groups.rs` 的单元测试（从生产文件搬出：那里测试比生产还长，正文被淹没）。
//! 仍是同一父模块的子模块 ⇒ `use super::*` 与 crate 内私有项照旧可用。

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
    // 回归：从真实 YAML 内容走 serde_yaml_ng 路径（不是手动 map.insert），
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
        "serde_yaml_ng 必须解析出全部 4 个 glob: {v:?}"
    );
    std::fs::remove_dir_all(&dir).ok();
}

#[test]
fn trigger_edges_only_when_on_in_names() {
    let mut groups = RebuildGroups::default();
    groups
        .abi
        .insert("python".into(), vec!["python-*".into(), "meson".into()]);
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
    let without_on: Vec<String> = ["python-cairo", "meson"]
        .iter()
        .map(|s| s.to_string())
        .collect();
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
    let all: Vec<String> = [
        "python",
        "python-cairo",
        "python-gobject",
        "meson",
        "blueman",
        "glib",
    ]
    .iter()
    .map(|s| s.to_string())
    .collect();
    let v = groups.victims_for("python", &all);
    assert_eq!(
        v,
        vec!["blueman", "meson", "python-cairo", "python-gobject"]
    );
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
        VersionChangeGroup {
            script: script.into(),
            globs: vec!["perl-*".into()],
        },
    );
    let v = g
        .version_victims_ctx("perl", "5.44.0", "5.45.0", &all, None)
        .unwrap();
    assert_eq!(v, vec!["perl-file-sharedir", "perl-xml-parser"]);
    // patch 变 → 脚本 exit 1 → 空（perl 模块无需重建）
    let v = g
        .version_victims_ctx("perl", "5.44.0", "5.44.1", &all, None)
        .unwrap();
    assert!(v.is_empty(), "patch 升级不应触发 perl-* 重建: {v:?}");
    // release 修订（5.44.0-1→5.44.0-2，8.0.0 起 release 用 `-`）也不算 minor 变化
    let v = g
        .version_victims_ctx("perl", "5.44.0-1", "5.44.0-2", &all, None)
        .unwrap();
    assert!(v.is_empty());
    // 未注册的 on 包 → 空
    assert!(g
        .version_victims_ctx("python", "3.14", "3.15", &all, None)
        .unwrap()
        .is_empty());
}

#[test]
fn version_change_group_loads_and_edges() {
    // 真实 YAML 走 serde_yaml_ng 路径：rebuild-on-version-change + version-change-script + packages
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
    let all: Vec<String> = ["perl", "perl-xml-parser"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    let v = g
        .version_victims_ctx("perl", "5.44", "5.45", &all, None)
        .unwrap();
    assert_eq!(v, vec!["perl-xml-parser"]);
    // trigger_edges_in：on 在 names 里 → (victim, on) 边（perl-* 组受害者排在 perl 之后）
    let names: Vec<String> = ["perl", "perl-xml-parser"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    assert_eq!(
        g.trigger_edges_in(&names),
        vec![("perl-xml-parser".to_string(), "perl".to_string())]
    );
    std::fs::remove_dir_all(&dir).ok();
}

#[test]
fn version_change_without_script_is_skipped() {
    // rebuild-on-version-change 声明了但缺 version-change-script → 组不注册（load 告警跳过），
    // version_victims_if 返回空而非 panic。
    let dir = std::env::temp_dir().join(format!("farm-groups-vnoscript-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(
        dir.join("perl.yaml"),
        "rebuild-on-version-change: perl\npackages: perl-*\n",
    )
    .unwrap();
    let g = RebuildGroups::load(&dir);
    let all: Vec<String> = ["perl", "perl-xml-parser"]
        .iter()
        .map(|s| s.to_string())
        .collect();
    assert!(g
        .version_victims_ctx("perl", "5.44", "5.45", &all, None)
        .unwrap()
        .is_empty());
    assert!(g.trigger_edges_in(&all).is_empty());
    std::fs::remove_dir_all(&dir).ok();
}

#[test]
fn version_change_dynamic_victims_via_stdout() {
    // version-change 脚本 exit 0 时，stdout 里打印的**合法包名**并入受害者（动态计算，
    // 如"扫所有包 ELF 里 import Qt_6_PRIVATE_API 的"）；glob 仍生效；on 自身、未知名被滤掉。
    let dir = std::env::temp_dir().join(format!("farm-groups-vdyn-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let mut g = RebuildGroups::default();
    // glob 一个都不匹配；动态脚本 echo 出 a（合法）与 q/bogus（on 自身 + 未知 → 滤掉）
    g.version.insert(
        "q".into(),
        VersionChangeGroup {
            script: "echo a; echo q; echo bogus\n".into(),
            globs: vec!["unused-*".into()],
        },
    );
    let all: Vec<String> = ["q", "a", "b"].iter().map(|s| s.to_string()).collect();
    let v = g
        .version_victims_ctx(
            "q",
            "1",
            "2",
            &all,
            Some(ScriptEnv {
                pkgs_dir: &dir,
                out_dir: &dir,
                arch: "x86_64",
            }),
        )
        .unwrap();
    assert_eq!(v, vec!["a"], "stdout 动态受害者应只含合法包名 a: {v:?}");

    // exit 非零（跳过）→ 空，即使脚本打印了包名
    g.version.insert(
        "q".into(),
        VersionChangeGroup {
            script: "echo a; exit 1\n".into(),
            globs: vec![],
        },
    );
    let v = g
        .version_victims_ctx(
            "q",
            "1",
            "2",
            &all,
            Some(ScriptEnv {
                pkgs_dir: &dir,
                out_dir: &dir,
                arch: "x86_64",
            }),
        )
        .unwrap();
    assert!(v.is_empty(), "exit≠0 应跳过: {v:?}");
    std::fs::remove_dir_all(&dir).ok();
}
