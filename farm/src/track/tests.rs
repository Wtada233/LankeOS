//! `track` 的单元测试。**从 `mod.rs` 搬出来**：那边 54% 的行数是测试，生产代码被淹没了。
//! 作为 `track` 的子模块，`use super::*` 与 crate 内私有项照旧可用。

use super::*;
use crate::net::MockFetcher;
use std::collections::HashMap;

#[test]
fn tracker_yaml_roundtrip() {
    let yaml = r#"
pkg-name: glibc
version-source: sources[0]
after: tzdata
sources:
  - tracker-template: html-index
    url: https://ftp.gnu.org/gnu/glibc/
    pattern: 'glibc-(\d[\d.]*)\.tar\.xz'
    template: https://ftp.gnu.org/gnu/glibc/{name}-{version}.tar.xz
work_sources:
  - tracker-template: html-index
    url: https://www.iana.org/time-zones/repository/releases/
    pattern: 'tzdata(\d{4}[a-z])\.tar\.gz'
    template: https://www.iana.org/time-zones/repository/releases/tzdata{version}.tar.gz
"#;
    let cfg: TrackerConfig = serde_yaml_ng::from_str(yaml).unwrap();
    assert_eq!(cfg.pkg_name, "glibc");
    assert_eq!(cfg.version_source.as_deref(), Some("sources[0]"));
    assert_eq!(cfg.after.as_deref(), Some("tzdata"));
    assert_eq!(cfg.sources.len(), 1);
    assert_eq!(cfg.sources[0].tracker_template, "html-index");
    assert_eq!(
        cfg.sources[0].url.as_deref(),
        Some("https://ftp.gnu.org/gnu/glibc/")
    );
    assert_eq!(cfg.work_sources.len(), 1);
    assert_eq!(
        cfg.work_sources[0].template.as_deref(),
        Some("https://www.iana.org/time-zones/repository/releases/tzdata{version}.tar.gz")
    );
}

#[test]
fn script_entry_yaml_roundtrip() {
    let yaml = r#"
pkg-name: rhino
after: base
sources:
  - tracker-template: script
    script: |
      #!/bin/bash
      echo "1.7.15|https://github.com/mozilla/rhino/releases/download/rhino1.7.15/rhino-1.7.15.zip"
"#;
    let cfg = TrackerConfig::from_yaml(yaml).unwrap();
    assert_eq!(cfg.kind(), "script");
    assert_eq!(cfg.after.as_deref(), Some("base"));
    assert!(!cfg.sources[0].expand, "expand 缺省 false");
    assert!(cfg.sources[0]
        .script
        .as_ref()
        .unwrap()
        .contains("echo \"1.7.15|"));
}

#[test]
fn legacy_package_level_type_and_script_content_are_unknown_fields() {
    // 迁移期已结束（仓库 850 个 tracker 全部迁完，零使用）→ 那两个字段连同迁移守卫一起删掉了，
    // 旧写法落回 `deny_unknown_fields`（仍是**报错**，只是不再有迁移指引）。
    for yaml in [
        "pkg-name: rhino\ntype: script\n",
        "pkg-name: rhino\nscript-content: |\n  echo x\n",
    ] {
        let e = TrackerConfig::from_yaml(yaml).unwrap_err().to_string();
        assert!(e.contains("unknown field"), "{e}");
    }
}

#[test]
fn kind_lists_distinct_templates() {
    let cfg = TrackerConfig {
        pkg_name: "p".into(),
        sources: vec![SourceConfig {
            tracker_template: "github".into(),
            ..Default::default()
        }],
        work_sources: vec![SourceConfig {
            tracker_template: "script".into(),
            script: Some("x".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    assert_eq!(cfg.kind(), "github+script");
}

#[test]
fn serde_projection_skips_default_fields() {
    let cfg = TrackerConfig {
        pkg_name: "bash".into(),
        sources: vec![SourceConfig {
            tracker_template: "html-index".into(),
            url: Some("https://ftp.gnu.org/gnu/bash/".into()),
            pattern: Some(r"bash[-_]?(\d[\d.]*)\.tar\.(?:xz|gz|bz2)".into()),
            template: Some("https://ftp.gnu.org/gnu/bash/{name}-{version}.tar.gz".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    // **默认值不得出现在序列化结果里**——这条不是"yaml 好看"，而是白名单的**机制**：
    // `validate_supported_fields` 用 `serde_json::to_value(cfg)` 的**键集**判断"用户设了哪些字段"，
    // 靠的就是各字段的 `skip_serializing_if`。属性与格式无关，所以这里用 JSON 验（生产侧唯一消费
    // 序列化的地方也是 JSON）。
    let v = serde_json::to_value(&cfg).unwrap();
    let keys: Vec<&String> = v.as_object().unwrap().keys().collect();
    assert!(keys.iter().any(|k| k.as_str() == "pkg-name"));
    assert!(!keys.iter().any(|k| k.as_str() == "repo")); // 未设 → 不出现
    assert!(!keys.iter().any(|k| k.as_str() == "expand")); // 缺省 false → 不出现
    assert!(!keys.iter().any(|k| k.as_str() == "after"));
    assert!(!keys.iter().any(|k| k.as_str() == "version-source"));
    let src = &v["sources"][0];
    assert!(src.get("tracker-template").is_some());
    assert!(src.get("url").is_some() && src.get("pattern").is_some());
    assert!(src.get("repo").is_none());

    // 反过来：非默认值必须出现（否则白名单会把"设过了"当成"没设"而放行/误报）
    let mut cfg2 = cfg.clone();
    cfg2.sources[0].tracker_template = "script".into();
    cfg2.sources[0].script = Some("echo x".into());
    cfg2.sources[0].expand = true;
    let v2 = serde_json::to_value(&cfg2).unwrap();
    assert_eq!(v2["sources"][0]["expand"], serde_json::json!(true));
}

#[test]
fn parse_version_source_selectors() {
    assert_eq!(
        parse_version_source("sources[0]").unwrap(),
        (SlotList::Sources, 0)
    );
    assert_eq!(
        parse_version_source("sources[3]").unwrap(),
        (SlotList::Sources, 3)
    );
    assert_eq!(
        parse_version_source("work_sources[0]").unwrap(),
        (SlotList::WorkSources, 0)
    );
    assert!(parse_version_source("sources[]").is_err());
    assert!(parse_version_source("sources[abc]").is_err());
    assert!(parse_version_source("source[0]").is_err());
    assert!(parse_version_source("0").is_err());
}

#[test]
fn package_probe_multi_source_version_source() {
    // 版本由 work_sources[0] 提供，sources 两条各自探测出 URL
    let f = MockFetcher::new(HashMap::new())
        .tags("https://github.com/a/main.git", &["v2.0", "v1.0"])
        .tags("https://github.com/b/vendored.git", &["v9.0"])
        .tags("https://github.com/c/ver.git", &["v3.1", "v3.0"]);
    let cfg = TrackerConfig {
        pkg_name: "pkg".into(),
        version_source: Some("work_sources[0]".into()),
        sources: vec![
            SourceConfig {
                tracker_template: "github".into(),
                repo: Some("a/main".into()),
                mode: Some("tags".into()),
                tag_prefix: Some("v".into()),
                template: Some("https://github.com/a/main/archive/refs/tags/{tag}.tar.gz".into()),
                ..Default::default()
            },
            SourceConfig {
                tracker_template: "github".into(),
                repo: Some("b/vendored".into()),
                mode: Some("tags".into()),
                tag_prefix: Some("v".into()),
                template: Some(
                    "https://github.com/b/vendored/archive/refs/tags/{tag}.tar.gz".into(),
                ),
                ..Default::default()
            },
        ],
        work_sources: vec![SourceConfig {
            tracker_template: "github".into(),
            repo: Some("c/ver".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            template: Some("https://github.com/c/ver/archive/refs/tags/{tag}.tar.gz".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    let r = cfg.probe(&f).unwrap();
    assert_eq!(r.version, "3.1"); // 版本来自 work_sources[0]
    assert_eq!(
        r.sources,
        vec![
            "https://github.com/a/main/archive/refs/tags/v2.0.tar.gz",
            "https://github.com/b/vendored/archive/refs/tags/v9.0.tar.gz"
        ]
    );
    assert_eq!(
        r.work_sources,
        vec!["https://github.com/c/ver/archive/refs/tags/v3.1.tar.gz"]
    );
}

#[test]
fn package_probe_defaults_version_to_sources0() {
    let f = MockFetcher::new(HashMap::new()).tags("https://github.com/a/main.git", &["v2.0"]);
    let cfg = TrackerConfig {
        pkg_name: "pkg".into(),
        sources: vec![SourceConfig {
            tracker_template: "github".into(),
            repo: Some("a/main".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            template: Some("https://github.com/a/main/archive/refs/tags/{tag}.tar.gz".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    let r = cfg.probe(&f).unwrap();
    assert_eq!(r.version, "2.0");
    assert_eq!(
        r.sources,
        vec!["https://github.com/a/main/archive/refs/tags/v2.0.tar.gz"]
    );
    assert!(r.work_sources.is_empty());
}

#[test]
fn package_probe_atomic_fails_on_entry_error() {
    // 任一条目探测失败 → 整包失败（原子性，不产出半截清单）
    let f = MockFetcher::new(HashMap::new()); // 无任何响应 → github tags 抓取失败
    let cfg = TrackerConfig {
        pkg_name: "pkg".into(),
        sources: vec![
            SourceConfig {
                tracker_template: "github".into(),
                repo: Some("a/main".into()),
                tag_prefix: Some("v".into()),
                template: Some("https://x/{tag}".into()),
                ..Default::default()
            },
            SourceConfig {
                tracker_template: "github".into(),
                repo: Some("b/broken".into()),
                tag_prefix: Some("v".into()),
                template: Some("https://x/{tag}".into()),
                ..Default::default()
            },
        ],
        ..Default::default()
    };
    let err = cfg.probe(&f).unwrap_err();
    assert!(
        err.to_string().contains("sources[0] 探测失败"),
        "err: {err}"
    );
}

#[test]
fn version_source_out_of_range_errors() {
    let f = MockFetcher::new(HashMap::new()).tags("https://github.com/a/main.git", &["v2.0"]);
    let cfg = TrackerConfig {
        pkg_name: "pkg".into(),
        version_source: Some("sources[5]".into()),
        sources: vec![SourceConfig {
            tracker_template: "github".into(),
            repo: Some("a/main".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            template: Some("https://x/{tag}".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    let err = cfg.probe(&f).unwrap_err();
    assert!(err.to_string().contains("越界"), "err: {err}");
}

#[test]
fn entry_same_version_locks_version_and_builds_url() {
    // 条目级 same-version：锁定另一包版本，URL 全写在 template（tag 前缀/仓库路径烘进），无网络
    let cfg = TrackerConfig {
            pkg_name: "SPIRV-Headers".into(),
            sources: vec![SourceConfig {
                tracker_template: "same-version".into(),
                template: Some(
                    "https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-{version}.tar.gz"
                        .into(),
                ),
                same_version_of: Some("vulkan-headers".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
    let r = cfg
        .probe_with(
            &crate::net::RealFetcher::default(), // same-version 模板不联网
            &|pkg| (pkg == "vulkan-headers").then(|| "1.4.350.1".to_string()),
        )
        .unwrap();
    assert_eq!(r.version, "1.4.350.1");
    assert_eq!(
            r.sources,
            vec!["https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz"]
        );
}

#[test]
fn entry_same_version_major_minor_for_dir_paths() {
    // qt6 风格：{major_minor}/{version} 拼目录（qt/<6.11>/<6.11.1>/）
    let cfg = TrackerConfig {
            pkg_name: "qt6-declarative".into(),
            sources: vec![SourceConfig {
                tracker_template: "same-version".into(),
                same_version_of: Some("qt6-base".into()),
                template: Some(
                    "https://download.qt.io/official_releases/qt/{major_minor}/{version}/submodules/qtdeclarative-everywhere-src-{version}.tar.xz"
                        .into(),
                ),
                ..Default::default()
            }],
            ..Default::default()
        };
    let r = cfg
        .probe_with(
            &crate::net::RealFetcher::default(), // same-version 模板不联网
            &|pkg| (pkg == "qt6-base").then(|| "6.12.1".to_string()),
        )
        .unwrap();
    assert_eq!(r.version, "6.12.1");
    assert_eq!(
            r.sources,
            vec!["https://download.qt.io/official_releases/qt/6.12/6.12.1/submodules/qtdeclarative-everywhere-src-6.12.1.tar.xz"]
        );
}

#[test]
fn entry_same_version_missing_lookup_errors() {
    let cfg = TrackerConfig {
        pkg_name: "SPIRV-Headers".into(),
        sources: vec![SourceConfig {
            tracker_template: "same-version".into(),
            same_version_of: Some("nonexistent".into()),
            template: Some("https://x/{tag}".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    let err = cfg
        .probe_with(&crate::net::RealFetcher::default(), &|_| None)
        .unwrap_err();
    assert!(err.to_string().contains("same-version-of"), "err: {err}");
}

#[test]
fn legacy_same_version_key_is_unknown_field() {
    // 旧写法 `same-version:`（无 -of）已是未知字段 → deny_unknown_fields 解析即拒
    let yaml = "tracker-template: github\nrepo: a/b\nsame-version: other\n";
    let err = serde_yaml_ng::from_str::<SourceConfig>(yaml).unwrap_err();
    assert!(err.to_string().contains("same-version"), "err: {err}");
}

#[test]
fn entry_same_version_of_rejected_on_probing_template() {
    // same-version-of 是 same-version 模板专属字段：github 上写它 → 报错
    let cfg = TrackerConfig {
        pkg_name: "x".into(),
        sources: vec![SourceConfig {
            tracker_template: "github".into(),
            repo: Some("a/b".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            same_version_of: Some("other".into()),
            template: Some("https://x/{tag}".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    let err = cfg.probe(&MockFetcher::new(HashMap::new())).unwrap_err();
    assert!(
        err.to_string().contains("不支持字段: same-version-of"),
        "err: {err}"
    );
}

#[test]
fn field_whitelist_uses_serde_projection() {
    // "设了哪些字段"由 serde 投影得到（各字段的 skip_serializing_if）——默认值必须**不出现**，
    // 非默认值必须**出现**。这条测试就是钉这个机制的（它替掉了原来那张手写的 19 项 set 表）。
    let with = |t: &str, f: fn(&mut SourceConfig)| {
        let mut c = SourceConfig {
            tracker_template: t.into(),
            ..Default::default()
        };
        f(&mut c);
        c
    };
    // 默认值不误报：script 模板 + 只设 script 字段（expand=false / 空 version-var）→ 合法
    assert!(validate_supported_fields(&with("script", |c| {
        c.script = Some("#!/bin/sh\necho 1|http://x".into())
    }))
    .is_ok());
    // expand=true 必须被看见（写在 github 上 → 报不支持）
    let e = validate_supported_fields(&with("github", |c| c.expand = true))
        .unwrap_err()
        .to_string();
    assert!(e.contains("不支持字段: expand"), "{e}");
    // version-var（BTreeMap）非空同理
    let e = validate_supported_fields(&with("github", |c| {
        c.version_var.insert("main".into(), "sources[0]".into());
    }))
    .unwrap_err()
    .to_string();
    assert!(e.contains("不支持字段: version-var"), "{e}");
    // levels（Vec）非空同理
    let e = validate_supported_fields(&with("github", |c| {
        c.levels = vec![LevelConfig {
            name: Some("version".into()),
            ..Default::default()
        }];
    }))
    .unwrap_err()
    .to_string();
    assert!(e.contains("不支持字段: levels"), "{e}");
    // 版本约束只给探测模板：github 上写 major-version-lock → 合法；script 上写 → 报错
    assert!(validate_supported_fields(&with("github", |c| {
        c.major_version_lock = Some("3".into())
    }))
    .is_ok());
    let e = validate_supported_fields(&with("script", |c| c.major_version_lock = Some("3".into())))
        .unwrap_err()
        .to_string();
    assert!(e.contains("不支持字段: major-version-lock"), "{e}");
}

#[test]
fn template_registry_is_consistent() {
    use crate::track::templates::{spec, TEMPLATES};
    let mut names: Vec<&str> = TEMPLATES.iter().map(|s| s.name).collect();
    let n = names.len();
    names.sort_unstable();
    names.dedup();
    assert_eq!(names.len(), n, "注册表里模板名重复");
    for s in TEMPLATES {
        assert!(spec(s.name).is_some_and(|x| x.name == s.name));
        // 每个模板的**最小配置**（只有 tracker-template）必须过自己的白名单
        let cfg = SourceConfig {
            tracker_template: s.name.into(),
            ..Default::default()
        };
        assert!(
            validate_supported_fields(&cfg).is_ok(),
            "模板 {} 的最小配置被自己的白名单拒了",
            s.name
        );
    }
    assert!(spec("no-such-template").is_none());
}

#[test]
fn entry_same_version_of_source_takes_this_round_version() {
    // 第二槽位用**本轮** sources[0] 解出的版本拼 URL，而不是 LankeBUILD.json 里的旧版本 ——
    // 这正是它相对 `same-version-of: <本包名>` 的意义：同一次探测内自洽，没有跨包/跨轮窗口。
    let cfg = TrackerConfig {
        pkg_name: "docker".into(),
        sources: vec![
            script_entry(
                "#!/bin/bash\necho \"28.4.0|https://x/docker-28.4.0.tgz\"\n",
                false,
            ),
            SourceConfig {
                tracker_template: "same-version-of-source".into(),
                same_version_of_source: Some("sources[0]".into()),
                template: Some("https://x/cli-{version}.tar.gz".into()),
                ..Default::default()
            },
        ],
        ..Default::default()
    };
    let r = cfg.probe(&crate::net::RealFetcher::default()).unwrap();
    assert_eq!(r.version, "28.4.0");
    assert_eq!(
        r.sources,
        vec!["https://x/docker-28.4.0.tgz", "https://x/cli-28.4.0.tar.gz"]
    );
}

#[test]
fn entry_same_version_of_source_forward_reference_errors() {
    // 只能引用**位于它之前**的槽位：sources[0] 引用 sources[1] → 报错，且带槽位上下文
    let cfg = TrackerConfig {
        pkg_name: "x".into(),
        sources: vec![
            SourceConfig {
                tracker_template: "same-version-of-source".into(),
                same_version_of_source: Some("sources[1]".into()),
                template: Some("https://x/{version}".into()),
                ..Default::default()
            },
            script_entry("#!/bin/bash\necho \"1.0|https://x/a-1.0.tar.gz\"\n", false),
        ],
        ..Default::default()
    };
    let err = cfg
        .probe(&crate::net::RealFetcher::default())
        .unwrap_err()
        .to_string();
    assert!(err.contains("sources[0] 探测失败"), "err: {err}");
    assert!(err.contains("尚不可用"), "err: {err}");
}

#[test]
fn same_version_of_source_field_is_exclusive_to_its_template() {
    // 新字段写到探测模板上 → 报错
    let cfg = TrackerConfig {
        pkg_name: "x".into(),
        sources: vec![SourceConfig {
            tracker_template: "github".into(),
            repo: Some("a/b".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            template: Some("https://x/{tag}".into()),
            same_version_of_source: Some("sources[0]".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    let err = cfg
        .probe(&MockFetcher::new(HashMap::new()))
        .unwrap_err()
        .to_string();
    assert!(
        err.contains("不支持字段: same-version-of-source"),
        "err: {err}"
    );

    // 反过来：跨包那个 `same-version-of` 写到新模板上 → 也报错
    let cfg = TrackerConfig {
        pkg_name: "x".into(),
        sources: vec![SourceConfig {
            tracker_template: "same-version-of-source".into(),
            same_version_of_source: Some("sources[0]".into()),
            same_version_of: Some("other".into()),
            template: Some("https://x/{version}".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    let err = cfg
        .probe(&MockFetcher::new(HashMap::new()))
        .unwrap_err()
        .to_string();
    assert!(err.contains("不支持字段: same-version-of"), "err: {err}");
}

#[test]
fn same_version_of_source_yaml_roundtrip() {
    let yaml = r#"
pkg-name: docker
sources:
  - tracker-template: script
    script: |
      #!/bin/bash
      echo "28.4.0|https://x/docker-28.4.0.tgz"
  - tracker-template: same-version-of-source
    same-version-of-source: sources[0]
    template: https://x/cli-{version}.tar.gz
"#;
    let cfg = TrackerConfig::from_yaml(yaml).unwrap();
    assert_eq!(
        cfg.sources[1].same_version_of_source.as_deref(),
        Some("sources[0]")
    );
    assert_eq!(cfg.kind(), "same-version-of-source+script");
    // 键名就是对外契约：解析后字段必须落到正确的槽位
    assert_eq!(
        cfg.sources[1].same_version_of_source.as_deref(),
        Some("sources[0]")
    );
}

#[test]
fn entry_major_of_filters_by_major() {
    // 条目级 major-of：只匹配指定包主版本的 tag
    let f = MockFetcher::new(HashMap::new()).tags(
        "https://github.com/KhronosGroup/SPIRV-LLVM-Translator.git",
        &["v21.1.0", "v22.1.2", "v22.0.0", "v23.0.0"],
    );
    let cfg = TrackerConfig {
            pkg_name: "SPIRV-LLVM-Translator".into(),
            sources: vec![SourceConfig {
                tracker_template: "github".into(),
                repo: Some("KhronosGroup/SPIRV-LLVM-Translator".into()),
                mode: Some("tags".into()),
                tag_prefix: Some("v".into()),
                template: Some(
                    "https://github.com/KhronosGroup/SPIRV-LLVM-Translator/archive/refs/tags/{tag}.tar.gz"
                        .into(),
                ),
                major_of: Some("llvm".into()),
                ..Default::default()
            }],
            ..Default::default()
        };
    let r = cfg
        .probe_with(&f, &|pkg| (pkg == "llvm").then(|| "22.1.7".to_string()))
        .unwrap();
    assert_eq!(r.version, "22.1.2");
    assert_eq!(
            r.sources,
            vec!["https://github.com/KhronosGroup/SPIRV-LLVM-Translator/archive/refs/tags/v22.1.2.tar.gz"]
        );
}

#[test]
fn max_version_cap_honored_by_html_index() {
    // 曾对 html-index/gcs 是死字段：max-version 必须生效（tcl 锁 8.6.x 场景）
    let f = MockFetcher::new(HashMap::new()).entry(
        "https://ftp.gnu.org/gnu/tcl/",
        "tcl8.6.16-src.tar.gz\ntcl9.0.4-src.tar.gz\n",
    );
    let cfg = TrackerConfig {
        pkg_name: "tcl".into(),
        sources: vec![SourceConfig {
            tracker_template: "html-index".into(),
            url: Some("https://ftp.gnu.org/gnu/tcl/".into()),
            pattern: Some(r"tcl([\d.]+)-src\.tar\.gz".into()),
            max_version: Some("8.6.16".into()),
            template: Some("https://ftp.gnu.org/gnu/tcl/tcl{version}-src.tar.gz".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    let r = cfg.probe(&f).unwrap();
    assert_eq!(r.version, "8.6.16");
}

#[test]
fn template_leftover_placeholder_is_rejected() {
    // 模板引用未提供的占位符 → URL 残留 {unknown} → 探测报错，而非静默生成坏 URL
    let cfg = TrackerConfig {
        pkg_name: "x".into(),
        sources: vec![SourceConfig {
            tracker_template: "github".into(),
            repo: Some("a/b".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            template: Some("https://example.com/{repo}/{unknown}/{version}.tar.gz".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    let f = MockFetcher::new(HashMap::new()).tags("https://github.com/a/b.git", &["v1.2"]);
    let err = cfg.probe(&f).unwrap_err();
    assert!(err.to_string().contains("残留未替换占位符"), "err: {err}");
}

#[test]
fn entry_unsupported_field_is_explicit_error() {
    // github 不支持 host（模板从 repo 拼 api.github.com URL）：设置 → 显式报错而非静默忽略
    let cfg = TrackerConfig {
        pkg_name: "x".into(),
        sources: vec![SourceConfig {
            tracker_template: "github".into(),
            repo: Some("a/b".into()),
            host: Some("github.example".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            template: Some("https://x/{tag}".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    let err = cfg.probe(&MockFetcher::new(HashMap::new())).unwrap_err();
    assert!(err.to_string().contains("不支持字段: host"), "err: {err}");
    assert!(err.to_string().contains("github"), "err: {err}");
}

#[test]
fn github_entry_accepts_max_version_and_caps() {
    // github 模板支持 max-version：tags 列表封顶生效（v261 被过滤取 v256）
    let f = MockFetcher::new(HashMap::new()).tags(
        "https://github.com/systemd/systemd.git",
        &["v254", "v256", "v255", "v261"],
    );
    let cfg = TrackerConfig {
        pkg_name: "systemd".into(),
        sources: vec![SourceConfig {
            tracker_template: "github".into(),
            repo: Some("systemd/systemd".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            max_version: Some("256".into()),
            template: Some(
                "https://github.com/systemd/systemd/archive/refs/tags/{tag}.tar.gz".into(),
            ),
            ..Default::default()
        }],
        ..Default::default()
    };
    let r = cfg.probe(&f).unwrap();
    assert_eq!(r.version, "256");
    assert_eq!(
        r.sources,
        vec!["https://github.com/systemd/systemd/archive/refs/tags/v256.tar.gz"]
    );
}

#[test]
fn gitlab_entry_accepts_max_version_and_caps() {
    // gitlab 模板支持 max-version：不报"不支持字段"，且封顶生效（v2.0.0 被过滤取 1.5.0）
    let f = MockFetcher::new(HashMap::new()).tags(
        "https://gitlab.com/a/b.git",
        &["v2.0.0", "v1.5.0", "v1.2.0"],
    );
    let cfg = TrackerConfig {
        pkg_name: "x".into(),
        sources: vec![SourceConfig {
            tracker_template: "gitlab".into(),
            host: Some("gitlab.com".into()),
            project: Some("a/b".into()),
            mode: Some("tags".into()),
            tag_prefix: Some("v".into()),
            max_version: Some("1.5.0".into()),
            template: Some("https://gitlab.com/{project}/-/archive/{tag}/x.tar.gz".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    let r = cfg.probe(&f).unwrap();
    assert_eq!(r.version, "1.5.0");
    assert_eq!(
        r.sources,
        vec!["https://gitlab.com/a/b/-/archive/v1.5.0/x.tar.gz"]
    );
}

#[test]
fn entry_pypi_rejects_template() {
    // pypi 模板不用 template（URL 来自 API），设置 → 报错
    let f = MockFetcher::new(HashMap::new()).entry(
            "https://pypi.org/pypi/setuptools/json",
            r#"{"info":{"version":"1.0"},"urls":[{"packagetype":"sdist","url":"https://x/1.0.tar.gz"}],"releases":{}}"#,
        );
    let cfg = TrackerConfig {
        pkg_name: "x".into(),
        sources: vec![SourceConfig {
            tracker_template: "pypi".into(),
            project: Some("setuptools".into()),
            template: Some("https://x/{version}".into()),
            ..Default::default()
        }],
        ..Default::default()
    };
    let err = cfg.probe(&f).unwrap_err();
    assert!(
        err.to_string().contains("不支持字段: template"),
        "err: {err}"
    );
}

#[test]
fn entry_unknown_field_in_yaml_is_rejected() {
    // deny_unknown_fields：typo 字段名（tag-prefx）解析即报错，而非静默忽略
    let yaml = "tracker-template: github\nrepo: a/b\ntag-prefx: v\n";
    let err = serde_yaml_ng::from_str::<SourceConfig>(yaml).unwrap_err();
    assert!(err.to_string().contains("tag-prefx"), "err: {err}");
}

/// 造一个条目级 script 条目（`expand` 缺省 false）。
fn script_entry(script: &str, expand: bool) -> SourceConfig {
    SourceConfig {
        tracker_template: "script".into(),
        script: Some(script.into()),
        expand,
        ..Default::default()
    }
}

#[test]
fn script_entry_produces_one_slot() {
    let cfg = TrackerConfig {
        pkg_name: "pkg".into(),
        sources: vec![script_entry(
            "#!/bin/bash\necho \"2.0|https://x/a-2.0.tar.gz\"\n",
            false,
        )],
        ..Default::default()
    };
    let r = cfg.probe(&crate::net::RealFetcher::default()).unwrap();
    assert_eq!(r.version, "2.0");
    assert_eq!(r.sources, vec!["https://x/a-2.0.tar.gz"]);
    assert!(r.work_sources.is_empty());
}

#[test]
fn script_entry_works_in_work_sources_and_can_expand() {
    // 同一个模型的三个要点：script 也能放 work_sources；expand 产多槽位；
    // sources/work_sources 各归各的（条目在哪个列表就填哪个列表）。
    let cfg = TrackerConfig {
            pkg_name: "libreoffice".into(),
            sources: vec![script_entry(
                "#!/bin/bash\necho \"26.8.0.3|https://x/main.tar.xz\"\n",
                false,
            )],
            work_sources: vec![script_entry(
                "#!/bin/bash\nprintf '%s\\n' \"26.8.0.3|https://x/v1\" \"26.8.0.3|https://x/v2\" \"26.8.0.3|https://x/v3\"\n",
                true,
            )],
            ..Default::default()
        };
    let r = cfg.probe(&crate::net::RealFetcher::default()).unwrap();
    assert_eq!(r.version, "26.8.0.3");
    assert_eq!(r.sources, vec!["https://x/main.tar.xz"]);
    assert_eq!(
        r.work_sources,
        vec!["https://x/v1", "https://x/v2", "https://x/v3"]
    );
}

#[test]
fn version_source_indexes_into_expanded_slots() {
    // expand 之后 `version-source` 索引的是**扁平槽位表**（= LankeBUILD.json 的 sources 数组）：
    // 第 0 条展开成 3 个槽位，则 sources[2] 是它的第 3 个槽位，sources[3] 才轮到第 1 条。
    let cfg = TrackerConfig {
            pkg_name: "pkg".into(),
            version_source: Some("sources[2]".into()),
            sources: vec![
                script_entry(
                    "#!/bin/bash\nprintf '%s\\n' \"1|https://x/a\" \"2|https://x/b\" \"3|https://x/c\"\n",
                    true,
                ),
                script_entry("#!/bin/bash\necho \"9|https://x/d\"\n", false),
            ],
            ..Default::default()
        };
    let r = cfg.probe(&crate::net::RealFetcher::default()).unwrap();
    assert_eq!(r.version, "3", "sources[2] 应落到展开出的第 3 个槽位");
    assert_eq!(
        r.sources,
        vec!["https://x/a", "https://x/b", "https://x/c", "https://x/d"]
    );
}

#[test]
fn script_template_rejects_other_fields() {
    // script 只认 script/expand（连 CORE 的 major-of/max-version 都不认——脚本自己过滤版本）
    let mut e = script_entry("#!/bin/bash\necho \"1|https://x/a\"\n", false);
    e.max_version = Some("1.0".into());
    let cfg = TrackerConfig {
        pkg_name: "p".into(),
        sources: vec![e],
        ..Default::default()
    };
    let err = cfg.probe(&crate::net::RealFetcher::default()).unwrap_err();
    assert!(err.to_string().contains("不支持字段"), "{err}");

    // expand 只属于 script：放到别的模板上必须报错（否则会静默无效）
    let mut e2 = SourceConfig {
        tracker_template: "html-index".into(),
        url: Some("https://x/".into()),
        pattern: Some(r"v([0-9.]+)".into()),
        template: Some("https://x/{version}".into()),
        ..Default::default()
    };
    e2.expand = true;
    let cfg2 = TrackerConfig {
        pkg_name: "p".into(),
        sources: vec![e2],
        ..Default::default()
    };
    let err2 = cfg2.probe(&crate::net::RealFetcher::default()).unwrap_err();
    assert!(err2.to_string().contains("expand"), "{err2}");
}

#[test]
fn version_var_injects_upstream_resolved_version() {
    // work_sources 的脚本用 $main 取 sources[0] **本轮解析出**的版本——
    // 不再自己重探上游，两个列表因此必然描述同一个版本。
    let cfg = TrackerConfig {
        pkg_name: "p".into(),
        sources: vec![script_entry(
            "#!/bin/bash\necho \"26.8.0.3|https://x/main.tar.xz\"\n",
            false,
        )],
        work_sources: vec![SourceConfig {
            tracker_template: "script".into(),
            script: Some("#!/bin/bash\necho \"$main|https://x/vendor-$main.tar.gz\"\n".into()),
            version_var: BTreeMap::from([("main".to_string(), "sources[0]".to_string())]),
            ..Default::default()
        }],
        ..Default::default()
    };
    let r = cfg.probe(&crate::net::RealFetcher::default()).unwrap();
    assert_eq!(r.version, "26.8.0.3");
    assert_eq!(r.work_sources, vec!["https://x/vendor-26.8.0.3.tar.gz"]);
}

#[test]
fn version_var_only_references_earlier_slots() {
    // sources[0] 引用 sources[1]（前向）→ 取不到 → 报错
    let fwd = TrackerConfig {
        pkg_name: "p".into(),
        sources: vec![
            SourceConfig {
                tracker_template: "script".into(),
                script: Some("#!/bin/bash\necho \"1|https://x/a\"\n".into()),
                version_var: BTreeMap::from([("v".to_string(), "sources[1]".to_string())]),
                ..Default::default()
            },
            script_entry("#!/bin/bash\necho \"2|https://x/b\"\n", false),
        ],
        ..Default::default()
    };
    let e = fwd
        .probe(&crate::net::RealFetcher::default())
        .unwrap_err()
        .to_string();
    assert!(e.contains("位于它之前"), "{e}");

    // sources 条目不引用 work_sources（后者整列表都在它之后才探测）
    let cross = TrackerConfig {
        pkg_name: "p".into(),
        sources: vec![SourceConfig {
            tracker_template: "script".into(),
            script: Some("#!/bin/bash\necho \"1|https://x/a\"\n".into()),
            version_var: BTreeMap::from([("v".to_string(), "work_sources[0]".to_string())]),
            ..Default::default()
        }],
        work_sources: vec![script_entry("#!/bin/bash\necho \"2|https://x/b\"\n", false)],
        ..Default::default()
    };
    let e2 = cross
        .probe(&crate::net::RealFetcher::default())
        .unwrap_err()
        .to_string();
    assert!(e2.contains("位于它之前"), "{e2}");
}

#[test]
fn version_var_validates_name_and_belongs_to_script_only() {
    let with_name = |name: &str| TrackerConfig {
        pkg_name: "p".into(),
        sources: vec![SourceConfig {
            tracker_template: "script".into(),
            script: Some("#!/bin/bash\necho \"1|https://x/a\"\n".into()),
            version_var: BTreeMap::from([(name.to_string(), "sources[0]".to_string())]),
            ..Default::default()
        }],
        ..Default::default()
    };
    for bad in ["1bad", "has-dash", "PKG_NAME"] {
        assert!(
            with_name(bad)
                .probe(&crate::net::RealFetcher::default())
                .is_err(),
            "{bad} 应被拒绝"
        );
    }
    // version-var 只属于 script：放到探测模板上 → 白名单报错
    let cfg = TrackerConfig {
        pkg_name: "p".into(),
        sources: vec![SourceConfig {
            tracker_template: "html-index".into(),
            url: Some("https://x/".into()),
            pattern: Some(r"v([0-9.]+)".into()),
            template: Some("https://x/{version}".into()),
            version_var: BTreeMap::from([("v".to_string(), "sources[0]".to_string())]),
            ..Default::default()
        }],
        ..Default::default()
    };
    let e = cfg
        .probe(&crate::net::RealFetcher::default())
        .unwrap_err()
        .to_string();
    assert!(e.contains("version-var"), "{e}");
}

#[test]
fn order_entries_after_and_nested_edges() {
    let mut trackers = HashMap::new();
    for n in ["llvm", "vulkan-headers"] {
        trackers.insert(
            n.to_string(),
            TrackerConfig {
                pkg_name: n.to_string(),
                ..Default::default()
            },
        );
    }
    // SPIRV-Headers：after + 条目级 same-version 模板
    trackers.insert(
        "SPIRV-Headers".into(),
        TrackerConfig {
            pkg_name: "SPIRV-Headers".into(),
            after: Some("vulkan-headers".into()),
            sources: vec![SourceConfig {
                tracker_template: "same-version".into(),
                same_version_of: Some("vulkan-headers".into()),
                template: Some("https://x/{tag}".into()),
                ..Default::default()
            }],
            ..Default::default()
        },
    );
    // SPIRV-LLVM-Translator：after + 条目级 major-of
    trackers.insert(
        "SPIRV-LLVM-Translator".into(),
        TrackerConfig {
            pkg_name: "SPIRV-LLVM-Translator".into(),
            after: Some("llvm".into()),
            sources: vec![SourceConfig {
                tracker_template: "github".into(),
                major_of: Some("llvm".into()),
                template: Some("https://x/{tag}".into()),
                ..Default::default()
            }],
            ..Default::default()
        },
    );

    let names = vec![
        "SPIRV-LLVM-Translator".to_string(),
        "llvm".to_string(),
        "vulkan-headers".to_string(),
        "SPIRV-Headers".to_string(),
    ];
    let ordered = order_entries(names, &trackers);
    let pos = |p: &str| ordered.iter().position(|n| n == p).unwrap();
    assert!(
        pos("llvm") < pos("SPIRV-LLVM-Translator"),
        "ordered: {ordered:?}"
    );
    assert!(
        pos("vulkan-headers") < pos("SPIRV-Headers"),
        "ordered: {ordered:?}"
    );
}

#[test]
fn order_entries_last_goes_after_all_normal() {
    let mut trackers = HashMap::new();
    for n in ["aa", "bb", "zz"] {
        trackers.insert(
            n.to_string(),
            TrackerConfig {
                pkg_name: n.to_string(),
                ..Default::default()
            },
        );
    }
    trackers.insert(
        "lastpkg".into(),
        TrackerConfig {
            pkg_name: "lastpkg".into(),
            last: true,
            ..Default::default()
        },
    );
    let names = vec!["zz".into(), "aa".into(), "lastpkg".into(), "bb".into()];
    let ordered = order_entries(names, &trackers);
    assert_eq!(&ordered[..3], &["aa", "bb", "zz"]);
    assert_eq!(ordered[3], "lastpkg");
}

/// 钉死：字符串字段（`major-version-lock` 等）写**裸数字**时 serde_yaml_ng **会强转成字符串**。
/// 仓库里两种写法并存（`'3'` 带引号 vs 裸 `6`），都有效——这条把该依赖行为钉住：一旦将来
/// serde_yaml_ng 改成报错，`qt6-base`/`tcl` 这类 tracker 会被 `cli::load_trackers` 的
/// `if let Ok` **静默丢弃**（配置看着在、实际不生效），必须先在此暴露。
#[test]
fn string_field_accepts_bare_number() {
    let yaml = "\
pkg-name: p
sources:
- tracker-template: html-index
  url: https://example.com/
  pattern: 'a([0-9]+)'
  template: https://example.com/{version}.tar.gz
  major-version-lock: 6
";
    let cfg = serde_yaml_ng::from_str::<TrackerConfig>(yaml).expect("裸数字应能解析");
    assert_eq!(
        cfg.sources[0].major_version_lock.as_deref(),
        Some("6"),
        "裸数字应被强转为 \"6\""
    );
}
