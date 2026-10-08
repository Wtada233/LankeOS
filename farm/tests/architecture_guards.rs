//! 架构守护测试：把 ARCH.md 里的分层不变量变成可执行断言（"注释/实现不漂移"的护栏）。
//! 这些断言便宜、不碰网络/容器，却能挡住"以后顺手在别处 spawn docker / 去掉超时"的回归。

/// 架构 §5：所有 docker 交互收敛在 `lpkg_binding/docker.rs` 这一个叶；CLI 与逻辑层不得直接 spawn。
#[test]
fn docker_only_spawned_in_binding_leaf() {
    for (name, src) in [
        ("src/cli/build.rs", include_str!("../src/cli/build.rs")),
        ("src/cli/mod.rs", include_str!("../src/cli/mod.rs")),
        ("src/build/mod.rs", include_str!("../src/build/mod.rs")),
        (
            "src/lpkg_binding/mod.rs",
            include_str!("../src/lpkg_binding/mod.rs"),
        ),
        ("src/serve.rs", include_str!("../src/serve.rs")),
    ] {
        assert!(
            !src.contains(r#"Command::new("docker")"#),
            "{name} 不得直接 spawn docker（应收敛到 lpkg_binding/docker.rs）"
        );
    }
}

/// HTTP 客户端必须设**读写空闲**超时：ureq 2 默认 read/write 无超时，卡死连接会永久 hang。
#[test]
fn http_client_sets_read_and_write_timeouts() {
    let src = include_str!("../src/net.rs");
    assert!(src.contains("timeout_read"), "net.rs 应设 timeout_read");
    assert!(src.contains("timeout_write"), "net.rs 应设 timeout_write");
}

/// tracker 的模板清单**只能有一处**：`templates::TEMPLATES` 注册表。
/// 重蹈覆辙的信号 = `track/mod.rs` 里又冒出按模板名分派的 `match` 臂
/// （历史上字段白名单与探测分发各有一份 12 臂 `match`，加模板要同步改 5 处）。
#[test]
fn tracker_dispatch_is_table_driven() {
    let src = include_str!("../src/track/mod.rs");
    for t in [
        "github",
        "gitlab",
        "html-index",
        "multi-level-html-index",
        "gcs",
        "gnome",
        "sourceforge",
        "pypi",
        "same-version",
        "same-version-of-source",
        "script",
    ] {
        assert!(
            !src.contains(&format!("\"{t}\" =>")),
            "src/track/mod.rs 又出现了按模板名 `{t}` 的分派臂——模板清单只能在 templates::TEMPLATES"
        );
    }
}

/// 检則的 `IGNORE_CHK_<KIND>` 只能由 label 派生（`cli::custom_checks::ChkDef::kind`）：
/// 各检則里不得再手写 `const KIND` 或传 3 参的 `is_ignored(..., "QML")`。
#[test]
fn chk_ignore_kinds_are_derived_from_labels() {
    for (name, src) in [
        ("qml", include_str!("../src/custom_checks/qml.rs")),
        ("pkgconf", include_str!("../src/custom_checks/pkgconf.rs")),
        ("pkg_err", include_str!("../src/custom_checks/pkg_err.rs")),
        ("hook", include_str!("../src/custom_checks/hook.rs")),
        (
            "introspection",
            include_str!("../src/custom_checks/introspection.rs"),
        ),
        ("vapi", include_str!("../src/custom_checks/vapi.rs")),
        ("pycache", include_str!("../src/custom_checks/pycache.rs")),
        (
            "build_deps",
            include_str!("../src/custom_checks/build_deps.rs"),
        ),
        ("abi", include_str!("../src/custom_checks/abi.rs")),
    ] {
        assert!(
            !src.contains("is_ignored(&opts.pkgs_dir") && !src.contains("is_ignored(&o.pkgs_dir"),
            "{name}.rs 又手写了 KIND 串——KIND 由 label 派生，见 ChkDef::kind"
        );
        assert!(
            !src.contains("const KIND"),
            "{name}.rs 又声明了自己的 KIND 常量——KIND 由 label 派生"
        );
    }
}

/// 版本约束只能在共享汇点实现：模板里不得再出现"自己判封顶 / 自己解析 stable-minor 默认"的写法
/// （gnome 曾重写一遍 major/封顶/even/排序，还靠 `f.cap = None` 绕开共享件）。
#[test]
fn version_constraints_have_single_implementation() {
    for (name, src) in [
        ("gnome", include_str!("../src/track/templates/gnome.rs")),
        ("github", include_str!("../src/track/templates/github.rs")),
        ("gitlab", include_str!("../src/track/templates/gitlab.rs")),
        ("gcs", include_str!("../src/track/templates/gcs.rs")),
        (
            "html_index",
            include_str!("../src/track/templates/html_index.rs"),
        ),
        (
            "multi_level_html_index",
            include_str!("../src/track/templates/multi_level_html_index.rs"),
        ),
        ("pypi", include_str!("../src/track/templates/pypi.rs")),
        (
            "sourceforge",
            include_str!("../src/track/templates/sourceforge.rs"),
        ),
    ] {
        assert!(
            !src.contains("f.cap = None"),
            "{name}.rs 又在绕开封顶（应用 VersionFilter::without_cap 或走 pool_filter）"
        );
        assert!(
            !src.contains("unwrap_or(\"even\")"),
            "{name}.rs 又自己解析 stable-minor 默认值（应交给 version_filter + pool_filter）"
        );
    }
}

/// `index.txt` 的**消费侧读取**只有一处（`build::repo::read_index` / `load_old_index`）。
/// `seed.rs` 是**生产者**（下载后写它），不在管控范围。
#[test]
fn index_txt_is_read_in_one_module() {
    for (name, src) in [
        ("build/mod.rs", include_str!("../src/build/mod.rs")),
        ("graph.rs", include_str!("../src/graph.rs")),
        ("scan.rs", include_str!("../src/scan.rs")),
        ("verify.rs", include_str!("../src/verify.rs")),
        ("abi.rs", include_str!("../src/abi.rs")),
    ] {
        assert!(
            !src.contains("join(\"index.txt\")"),
            "{name} 又直接读 index.txt——消费侧读取统一走 build::repo::read_index"
        );
    }
}

/// 测试代码不得住在生产文件里：生产文件的 `#[cfg(test)]` **只能**用于 `mod tests;` 声明
/// （测试要么在 `X/tests.rs`，要么在文件末尾的内联 `mod tests`——两者都在 `#[cfg(test)]` 之后）。
/// 反例：`sources.rs` 里 `#[cfg(test)] pub fn sources_ready`——虽然不进二进制，
/// 但它让"生产文件里混着测试专用代码"成为惯例，久了就分不清哪些是真 API。
#[test]
fn no_test_code_in_production_files() {
    let mut bad = Vec::new();
    for entry in walkdir(std::path::Path::new("src")) {
        let p = entry.to_string_lossy().into_owned();
        if p.starts_with("tests/") || p.ends_with("/tests.rs") || !p.ends_with(".rs") {
            continue;
        }
        let src = std::fs::read_to_string(&entry).unwrap();
        // 截到第一个 `#[cfg(test)]` / `mod tests`：它之后是测试区，合法
        let cut = src
            .lines()
            .position(|l| l.starts_with("#[cfg(test)]") || l.starts_with("mod tests"))
            .unwrap_or(usize::MAX);
        for (i, l) in src.lines().enumerate() {
            if i < cut && l.trim_start().starts_with("#[cfg(test)]") {
                bad.push(format!("{p}:{}", i + 1));
            }
        }
    }
    assert!(
        bad.is_empty(),
        "生产文件里出现了测试代码（应移入 X/tests.rs 或文件末尾的 mod tests）: {bad:?}"
    );
}

fn walkdir(dir: &std::path::Path) -> Vec<std::path::PathBuf> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(dir) else {
        return out;
    };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            out.extend(walkdir(&p));
        } else {
            out.push(p);
        }
    }
    out
}
