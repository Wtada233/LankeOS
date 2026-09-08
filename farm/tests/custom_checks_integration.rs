//! custom-checks 集成：合成小仓库（纯文本 .lpkg，无需真 ELF）跑四类检則 + IGNORE_CHK_* + 缓存。
use std::collections::BTreeMap;
use std::path::PathBuf;

use lankefarm::custom_checks::{hook, pkg_err, pkgconf, qml, ChkOpts};

struct Repo {
    base: PathBuf,
    repo: PathBuf,
    pkgs: PathBuf,
    arch: &'static str,
    entries: BTreeMap<String, Vec<String>>, // name -> binpkg deps（写进 index.txt）
}

impl Repo {
    fn new(tag: &str) -> Repo {
        let base = std::env::temp_dir().join(format!("farm-cchk-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&base);
        let repo = base.join("repo");
        let pkgs = base.join("pkgs");
        std::fs::create_dir_all(repo.join("x86_64")).unwrap();
        std::fs::create_dir_all(&pkgs).unwrap();
        Repo {
            base,
            repo,
            pkgs,
            arch: "x86_64",
            entries: BTreeMap::new(),
        }
    }
    /// 写一个包：content 条目 + binpkg deps（index）+ 配方 deps/farm_flags + postinst。
    fn add(
        &mut self,
        name: &str,
        content: &[(&str, &str)],
        deps: &[&str],
        flags: &[&str],
        postinst: &str,
    ) {
        self.entries.insert(
            name.to_string(),
            deps.iter().map(|s| s.to_string()).collect(),
        );
        let root = self.base.join(format!("pkg-{name}"));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(root.join("content")).unwrap();
        for (rel, txt) in content {
            let p = root.join("content").join(rel);
            std::fs::create_dir_all(p.parent().unwrap()).unwrap();
            std::fs::write(p, txt).unwrap();
        }
        std::fs::write(
            root.join("metadata.json"),
            format!(r#"{{"name":"{name}","version":"1.0"}}"#),
        )
        .unwrap();
        std::fs::create_dir_all(root.join("hooks")).unwrap();
        std::fs::write(root.join("hooks/postinst.sh"), postinst).unwrap();
        let pkgdir = self.repo.join(self.arch).join(name);
        std::fs::create_dir_all(&pkgdir).unwrap();
        let f = std::fs::File::create(pkgdir.join("1.0.lpkg")).unwrap();
        let enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
        let mut b = tar::Builder::new(enc);
        b.append_dir_all(".", &root).unwrap();
        let enc = b.into_inner().unwrap();
        enc.finish().unwrap();
        std::fs::remove_dir_all(&root).unwrap();
        self.write_recipe(name, deps, flags);
    }
    fn write_recipe(&self, name: &str, deps: &[&str], flags: &[&str]) {
        std::fs::create_dir_all(self.pkgs.join(name)).unwrap();
        let mut m = serde_json::json!({ "name": name, "version": "1.0", "deps": deps });
        if !flags.is_empty() {
            m["farm_flags"] = serde_json::json!(flags);
        }
        std::fs::write(
            self.pkgs.join(format!("{name}/LankeBUILD.json")),
            serde_json::to_string(&m).unwrap(),
        )
        .unwrap();
    }
    /// 重建 index.txt（binpkg deps 真源）。checks 的 deps 判定只读 index。
    fn sync_index(&self) {
        let mut lines = Vec::new();
        for (n, deps) in &self.entries {
            let deps = deps.join(",");
            lines.push(format!("{n}|1.0:h:{deps}::|"));
        }
        lines.sort();
        let mut s = lines.join("\n");
        if !s.is_empty() {
            s.push('\n');
        }
        std::fs::write(self.repo.join(self.arch).join("index.txt"), s).unwrap();
    }
    fn opts(&mut self, cache_tag: &str) -> ChkOpts {
        self.sync_index();
        ChkOpts {
            source: self.repo.clone(),
            arch: self.arch.into(),
            cache: self.base.join(cache_tag),
            pkgs_dir: self.pkgs.clone(),
            subset: vec![],
            full_rescan: false,
        }
    }
    /// 改 binpkg deps（重建 = 更新 index 与配方 deps）。
    fn recipe(&mut self, name: &str, deps: &[&str], flags: &[&str]) {
        self.entries.insert(
            name.to_string(),
            deps.iter().map(|s| s.to_string()).collect(),
        );
        self.write_recipe(name, deps, flags);
    }
    fn cleanup(self) {
        let _ = std::fs::remove_dir_all(&self.base);
    }
}

fn has(r: &lankefarm::custom_checks::Report, pkg: &str, needle: &str) -> bool {
    r.findings
        .get(pkg)
        .map(|v| v.iter().any(|f| f.what.contains(needle)))
        .unwrap_or(false)
}

#[test]
fn qmlchk_reports_missing_dep_then_satisfied() {
    let mut r = Repo::new("qml");
    r.add(
        "libA",
        &[(
            "usr/lib/qt6/qml/org/kde/kirigami/qmldir",
            "module org.kde.kirigami\n",
        )],
        &[],
        &[],
        "",
    );
    r.add(
        "appB",
        &[(
            "usr/share/app/b.qml",
            "import org.kde.kirigami 2.0 as K\nimport org.kde.ghost\nItem {}\n",
        )],
        &[],
        &[],
        "",
    );
    let rep = qml::run(&r.opts("c1")).unwrap();
    assert!(
        has(&rep, "appB", "提供者 libA"),
        "缺 deps 应报(W): {:?}",
        rep.findings
    );
    // org.kde.ghost 仓库里没有任何包提供 → 三段判定 Critical
    let ghost = rep
        .findings
        .get("appB")
        .unwrap()
        .iter()
        .find(|f| f.what.contains("org.kde.ghost"))
        .expect("org.kde.ghost 应报 critical");
    assert_eq!(ghost.severity, lankefarm::custom_checks::Severity::Critical);
    // 补 deps 后 → 消
    r.recipe("appB", &["libA"], &[]);
    let rep2 = qml::run(&r.opts("c2")).unwrap();
    assert!(
        !has(&rep2, "appB", "提供者 libA"),
        "补 deps 后不应报: {:?}",
        rep2.findings
    );
    // IGNORE_CHK_QML 豁免
    r.recipe("appB", &[], &["IGNORE_CHK_QML"]);
    let rep3 = qml::run(&r.opts("c3")).unwrap();
    assert!(
        !rep3.findings.contains_key("appB"),
        "IGNORE_CHK_QML 应豁免: {:?}",
        rep3.findings
    );
    r.cleanup();
}

#[test]
fn pkgconfchk_reports_missing_requires_then_satisfied() {
    let mut r = Repo::new("pc");
    r.add(
        "pcre2",
        &[(
            "usr/lib/pkgconfig/libpcre2-8.pc",
            "Name: libpcre2-8\nVersion: 10\n",
        )],
        &[],
        &[],
        "",
    );
    r.add(
        "glib",
        &[(
            "usr/lib/pkgconfig/glib-2.0.pc",
            "Name: glib-2.0\nVersion: 2.0\nRequires.private: libpcre2-8 >= 10\n",
        )],
        &[],
        &[],
        "",
    );
    let rep = pkgconf::run(&r.opts("c1")).unwrap();
    assert!(
        has(&rep, "glib", "提供者 pcre2"),
        "Requires.private 缺 deps 应报: {:?}",
        rep.findings
    );
    r.recipe("glib", &["pcre2"], &[]);
    let rep2 = pkgconf::run(&r.opts("c2")).unwrap();
    assert!(
        !has(&rep2, "glib", "提供者 pcre2"),
        "补 deps 后不应报: {:?}",
        rep2.findings
    );
    r.cleanup();
}

#[test]
fn pkg_err_reports_misplaced_static_la() {
    let mut r = Repo::new("err");
    r.add(
        "bad",
        &[
            ("usr/etc/passwd", "x"),
            ("usr/var/tmp", "x"),
            ("usr/lib/libfoo.a", "ar"),
            ("usr/lib/libfoo.la", "lt"),
            ("etc/passwd", "ok"),
        ],
        &[],
        &[],
        "",
    );
    let rep = pkg_err::run(&r.opts("c1")).unwrap();
    assert!(has(&rep, "bad", "usr/etc"), "{:?}", rep.findings);
    assert!(has(&rep, "bad", "usr/var"), "{:?}", rep.findings);
    assert!(has(&rep, "bad", ".a"), "{:?}", rep.findings);
    assert!(has(&rep, "bad", ".la"), "{:?}", rep.findings);
    assert!(
        rep.findings.get("bad").unwrap().len() >= 4,
        "{:?}",
        rep.findings
    );
    r.cleanup();
}

#[test]
fn hookchk_reports_missing_sysusers_tmpfiles_calls() {
    let mut r = Repo::new("hook");
    r.add(
        "rtkit",
        &[
            ("usr/lib/sysusers.d/rtkit.conf", "u rtkit -\n"),
            ("usr/lib/tmpfiles.d/rtkit.conf", "d /run/rtkit 0755\n"),
        ],
        &[],
        &[],
        "#!/bin/sh\nsystemd-sysusers\nsystemd-tmpfiles --create\nexit 0\n",
    );
    let rep = hook::run(&r.opts("c1")).unwrap();
    assert!(
        !rep.findings.contains_key("rtkit"),
        "都调了不应报: {:?}",
        rep.findings
    );
    r.add(
        "bad",
        &[("usr/lib/sysusers.d/x.conf", "u x -\n")],
        &[],
        &[],
        "#!/bin/sh\nexit 0\n",
    );
    let rep2 = hook::run(&r.opts("c2")).unwrap();
    assert!(
        has(&rep2, "bad", "systemd-sysusers"),
        "建了 sysusers 却未调应报: {:?}",
        rep2.findings
    );
    r.cleanup();
}

#[test]
fn cache_second_run_hits_by_lpkg_sha() {
    let mut r = Repo::new("cache");
    r.add(
        "only",
        &[("usr/lib/pkgconfig/m.pc", "Name: m\nVersion: 1\n")],
        &[],
        &[],
        "",
    );
    let o = r.opts("cc");
    let rep1 = pkgconf::run(&o).unwrap();
    assert!(rep1.cache_misses >= 1 && rep1.cache_hits == 0, "{rep1:?}");
    let rep2 = pkgconf::run(&o).unwrap();
    assert!(
        rep2.cache_hits >= 1,
        "第二遍应按 lpkg sha 命中缓存: {rep2:?}"
    );
    r.cleanup();
}
