//! 集成测试：stub binding 驱动完整流水线（§3.5）。
//!
//! 用 fixture + StubBinding 跑通 graph → detect → propagate → verify，
//! 绕开真实 `lpkg build`。同时用真实 lankerepo index.txt 做冒烟。

use std::collections::HashMap;

use lankefarm::abi;
use lankefarm::graph::{self, Index, RevMap};
use lankefarm::lpkg_binding::{BuildOutcome, StubBinding};
use lankefarm::verify;

fn fixture(name: &str) -> Index {
    let path = format!("{}/tests/fixtures/{}", env!("CARGO_MANIFEST_DIR"), name);
    Index::parse(&std::fs::read_to_string(path).unwrap())
}

fn to_refs(v: &[String]) -> Vec<&str> {
    v.iter().map(String::as_str).collect()
}

/// stub：所有包重建后 provides 与旧索引一致（ABI 保持 → 级联停）。
fn abi_preserving_stub(index: &Index, pkgs: &[String]) -> StubBinding {
    let mut outcomes = HashMap::new();
    for p in pkgs {
        if let Some(info) = index.packages.get(p) {
            outcomes.insert(
                p.clone(),
                BuildOutcome::success(
                    &to_refs(&info.needed_so),
                    &to_refs(&info.provides),
                    &to_refs(&info.deps),
                ),
            );
        }
    }
    StubBinding::new(outcomes)
}

fn bump_soname(old: &Index, pkg: &str) -> Vec<String> {
    let info = &old.packages[pkg];
    let mut new_provides = info.provides.clone();
    for p in &mut new_provides {
        if graph::is_soname_versioned(p) {
            if let Some(pos) = p.rfind(".so.") {
                let n: u32 = p[pos + 4..].parse().unwrap();
                *p = format!("{}.so.{}", &p[..pos], n + 1);
                break;
            }
        }
    }
    new_provides
}

fn fake_new_index(old: &Index, pkg: &str, new_provides: &[String]) -> Index {
    let mut packages = old.packages.clone();
    if let Some(info) = packages.get_mut(pkg) {
        info.provides = new_provides.to_vec();
    }
    Index::from_packages(packages)
}

fn scan_of(provides: &[String], needed_so: &[String]) -> verify::ScanResult {
    verify::ScanResult {
        needed_so: needed_so.to_vec(),
        provides: provides.to_vec(),
        deps: vec![],
    }
}

#[test]
fn full_pipeline_libxml2_break_rebuilds_only_direct() {
    let old = fixture("chain-index.txt");
    let rev = RevMap::build(&old);
    let new_provides = bump_soname(&old, "libxml2");

    let breaks = abi::detect_abi_breaks(&old, &fake_new_index(&old, "libxml2", &new_provides));
    assert_eq!(breaks, vec!["libxml2"]);

    let direct = graph::reverse_dependents(&old, &rev, "libxml2");
    assert_eq!(direct, vec!["llvm"]);
    let mut binding = abi_preserving_stub(&old, &direct);
    let res = abi::propagate(&old, &rev, "libxml2", &new_provides, &mut binding);

    assert_eq!(res.rebuilt, vec!["libxml2", "llvm"]);
    assert!(!res.rebuilt.contains(&"rust".to_string()));
    assert!(res.blocked.is_empty());

    let root_action = verify::decide(
        &scan_of(&new_provides, &[]),
        &scan_of(&old.packages["libxml2"].provides, &[]),
    );
    assert_eq!(root_action, verify::VerifyAction::AbiBreak);

    let llvm_action = verify::decide(
        &scan_of(
            &old.packages["llvm"].provides,
            &old.packages["llvm"].needed_so,
        ),
        &scan_of(
            &old.packages["llvm"].provides,
            &old.packages["llvm"].needed_so,
        ),
    );
    assert_eq!(llvm_action, verify::VerifyAction::Unchanged);
}

#[test]
fn cascade_when_llvm_abi_changes_includes_rust() {
    let old = fixture("chain-index.txt");
    let rev = RevMap::build(&old);
    let new_provides = bump_soname(&old, "libxml2");

    let mut outcomes = HashMap::new();
    outcomes.insert(
        "llvm".to_string(),
        BuildOutcome {
            ok: true,
            needed_so: vec!["libxml2.so.3".into(), "libc.so.6".into()],
            provides: vec!["libLLVM.so".into(), "libLLVM.so.19".into()],
            deps: vec![],
            failure_stage: None,
            lpkg_path: None,
        },
    );
    let mut binding = StubBinding::new(outcomes);
    let res = abi::propagate(&old, &rev, "libxml2", &new_provides, &mut binding);
    assert_eq!(res.rebuilt, vec!["libxml2", "llvm", "rust"]);
}

#[test]
fn blocked_on_build_failure() {
    let old = fixture("chain-index.txt");
    let rev = RevMap::build(&old);
    let new_provides = bump_soname(&old, "libxml2");
    let mut outcomes = HashMap::new();
    outcomes.insert(
        "llvm".to_string(),
        BuildOutcome::failure("lankebuild_build"),
    );
    let mut binding = StubBinding::new(outcomes);
    let res = abi::propagate(&old, &rev, "libxml2", &new_provides, &mut binding);
    assert_eq!(res.rebuilt, vec!["libxml2"]);
    assert_eq!(res.blocked, vec!["llvm"]);
}

#[test]
fn real_index_smoke() {
    let old = fixture("real-index.txt");
    assert!(
        old.len() > 100,
        "真实 index 应有 300+ 包，得到 {}",
        old.len()
    );
    let rev = RevMap::build(&old);
    let rdeps = graph::reverse_dependents(&old, &rev, "systemd");
    assert!(!rdeps.is_empty(), "systemd 的真实直接依赖者不应为空");
    assert!(graph::link_deps(&old, "systemd").contains(&"glibc".to_string()));
}

/// 端到端：`farm track --all -j N` 并行探测必须保序——
/// `beta`（same-version: alpha + order: after(alpha)）必须读到 alpha **本轮解析出的新版本** 2.0，
/// 而非 LankeBUILD.json 的旧版本 1.0（保序 + resolved 传播）。
/// 用 `script` 模板，bash 直接 echo 版本，无需网络。
#[test]
fn track_all_parallel_respects_after_ordering() {
    let tmp = std::env::temp_dir().join(format!("lankefarm-track-j-{}", std::process::id()));
    let pkgs = tmp.join("pkgs");
    let data = tmp.join("data/trackers");
    std::fs::create_dir_all(pkgs.join("alpha")).unwrap();
    std::fs::create_dir_all(pkgs.join("beta")).unwrap();
    std::fs::create_dir_all(&data).unwrap();
    let write = |p: &std::path::Path, c: &str| std::fs::write(p, c).unwrap();
    write(
        &pkgs.join("alpha/LankeBUILD.json"),
        r#"{"name":"alpha","version":"1.0","sources":["https://example.com/a-1.0.tar.gz"]}"#,
    );
    write(
        &pkgs.join("beta/LankeBUILD.json"),
        r#"{"name":"beta","version":"1.0","sources":["https://example.com/b-1.0.tar.gz"]}"#,
    );
    // alpha：script 模板，echo 出 2.0
    write(
        &data.join("alpha.yaml"),
        "pkg-name: alpha\ntracker-template: script\nscript-content: |\n  #!/bin/bash\n  echo \"2.0\"\n  echo \"https://example.com/a-2.0.tar.gz\"\n",
    );
    // beta：same-version 锁定 alpha，忽略 script 匹配（echo 9.9 也不生效）
    write(
        &data.join("beta.yaml"),
        "pkg-name: beta\ntracker-template: script\nsame-version: alpha\norder: after(alpha)\ntemplate: https://example.com/b-{version}.tar.gz\nscript-content: |\n  #!/bin/bash\n  echo \"9.9\"\n  echo \"https://example.com/b-9.9.tar.gz\"\n",
    );

    let bin = env!("CARGO_BIN_EXE_lankefarm");
    let out = std::process::Command::new(bin)
        .args(["track", "--all", "--pkgs"])
        .arg(&pkgs)
        .args(["--data"])
        .arg(&data)
        .args(["-j", "4"])
        .output()
        .unwrap();
    let stdout = String::from_utf8_lossy(&out.stdout);
    let _ = std::fs::remove_dir_all(&tmp);
    assert!(out.status.success(), "退出码非零，stdout: {stdout}");
    assert!(stdout.contains("alpha: 1.0 → 2.0"), "stdout: {stdout}");
    assert!(
        stdout.contains("beta: 1.0 → 2.0"),
        "beta 未读到 alpha 解析出的新版本（顺序被破坏？），stdout: {stdout}"
    );
    assert!(stdout.contains("提案 2"), "stdout: {stdout}");
}

/// 多源包按 `url-match` 正则匹配实际 URL（非索引）：sources[1] 与 work_sources[0] 都被正确升级。
#[test]
fn track_run_regex_matches_sources_and_work_sources() {
    let tmp = std::env::temp_dir().join(format!("lankefarm-multi-{}", std::process::id()));
    let pkgs = tmp.join("pkgs");
    let data = tmp.join("data/trackers");
    std::fs::create_dir_all(pkgs.join("multi")).unwrap();
    std::fs::create_dir_all(&data).unwrap();
    let write = |p: &std::path::Path, c: &str| std::fs::write(p, c).unwrap();
    write(
        &pkgs.join("multi/LankeBUILD.json"),
        r#"{"name":"multi","version":"1.0","sources":["https://example.com/multi-1.0.tar.gz","https://github.com/foo/bar/archive/refs/tags/v2.0.tar.gz"],"work_sources":["https://patches.example.com/multi-work.patch"]}"#,
    );
    write(
        &data.join("multi.yaml"),
        r#"pkg-name: multi
tracker-template: script
script-content: |
  #!/bin/bash
  echo "2.0"
  echo "https://example.com/multi-2.0.tar.gz"
sources:
  - pkg-name: vendored
    url-match: "github.com/foo/bar"
    tracker-template: script
    script-content: |
      #!/bin/bash
      echo "2.1"
      echo "https://github.com/foo/bar/archive/refs/tags/v2.1.tar.gz"
work_sources:
  - pkg-name: workpatch
    url-match: "multi-work"
    tracker-template: script
    script-content: |
      #!/bin/bash
      echo "1.1"
      echo "https://patches.example.com/multi-work-1.1.patch"
"#,
    );

    let bin = env!("CARGO_BIN_EXE_lankefarm");
    let out = std::process::Command::new(bin)
        .args(["track", "multi", "--run", "--pkgs"])
        .arg(&pkgs)
        .args(["--data"])
        .arg(&data)
        .output()
        .unwrap();
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(out.status.success(), "退出码非零，stdout: {stdout}");
    assert!(
        stdout.contains("sources[1] → https://github.com/foo/bar/archive/refs/tags/v2.1.tar.gz"),
        "sources[1] 未被正则匹配升级，stdout: {stdout}"
    );
    assert!(
        stdout.contains("work_sources[0] → https://patches.example.com/multi-work-1.1.patch"),
        "work_sources[0] 未被正则匹配升级，stdout: {stdout}"
    );
    let json = std::fs::read_to_string(pkgs.join("multi/LankeBUILD.json")).unwrap();
    assert!(json.contains("https://github.com/foo/bar/archive/refs/tags/v2.1.tar.gz"));
    assert!(json.contains("https://patches.example.com/multi-work-1.1.patch"));
    let _ = std::fs::remove_dir_all(&tmp);
}
