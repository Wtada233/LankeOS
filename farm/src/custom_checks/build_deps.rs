//! build-deps chk — 配方的**构建依赖完整性**：每个包 `needed_so` 的 SONAME 提供者，必须出现在
//! 它自己的 `build_deps` 里。
//!
//! 语义（用户规则，**勿"优化"成闭包判定**）：
//! - `needed_so` 里写的是**直接**链接依赖 → 提供者必须**直接**写在 `build_deps`。
//!   **即使该依赖能被别的 build_dep 传递满足，语义也不对**（CLAUDE.md 铁律：build_deps 要写全）。
//!   所以本检則**不做**闭包/传递可达判定——这是刻意的。
//! - `base` / `base-devel` 直接 `deps` 并集覆盖的 provider 视为满足（铁律：这些包本就不该写进
//!   build_deps）。判定读 `pkgs/base/LankeBUILD.json` 与 `pkgs/base-devel/LankeBUILD.json` 的
//!   `deps`（**直接依赖**，不展开闭包、不硬编码）。
//! - 自提供（provider == 本包）跳过；仓库内无任何 provider 的 SONAME 跳过（那是 `farm abifix`
//!   的领域，不是 build_deps 漏写）。
//!
//! **只读配方 `LankeBUILD.json`，不扫 `.lpkg`**——无需解包，因此不用缓存、不占 `--source`。
//! SONAME→provider 关系完全来自各包配方的 `provides` 字段（不是扫包）。

use super::{ChkOpts, Finding, Report, Severity};
use crate::error::FarmError;
use crate::tr;
use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::path::Path;

/// `IGNORE_CHK_<KIND>` 的 KIND。
const KIND: &str = "BUILDDEPS";

/// SONAME → provider 包名集合（从全部配方的 `provides` 建立；确定性排序）。
fn provider_map(pkgs_dir: &Path, pkgs: &[String]) -> BTreeMap<String, BTreeSet<String>> {
    let mut map: BTreeMap<String, BTreeSet<String>> = BTreeMap::new();
    for pkg in pkgs {
        let Some(b) = crate::build::read_lankebuild(pkgs_dir, pkg) else {
            continue;
        };
        for cap in &b.provides {
            map.entry(cap.clone()).or_default().insert(pkg.clone());
        }
    }
    map
}

/// `base` / `base-devel` 的**直接** `deps` 并集 + 这两个包自身——这些提供者视为已满足
/// （CLAUDE.md 铁律：不该写进 build_deps）。读 json 直接依赖，不展开闭包。
fn base_covered(pkgs_dir: &Path) -> HashSet<String> {
    let mut set: HashSet<String> = HashSet::new();
    for p in ["base", "base-devel"] {
        set.insert(p.to_string());
        if let Some(b) = crate::build::read_lankebuild(pkgs_dir, p) {
            set.extend(b.deps);
        }
    }
    set
}

/// 跑 build-deps 检則。
pub fn run(opts: &ChkOpts) -> Result<Report, FarmError> {
    let pkgs = crate::build::sorted_pkg_names(&opts.pkgs_dir);
    let providers = provider_map(&opts.pkgs_dir, &pkgs);
    let covered = base_covered(&opts.pkgs_dir);

    let audit_all = opts.subset.is_empty();
    let audit: HashSet<&str> = opts.subset.iter().map(String::as_str).collect();
    let mut report = Report::default();

    for pkg in &pkgs {
        if !(audit_all || audit.contains(pkg.as_str())) {
            continue;
        }
        if super::is_ignored(&opts.pkgs_dir, pkg, KIND) {
            continue;
        }
        let Some(b) = crate::build::read_lankebuild(&opts.pkgs_dir, pkg) else {
            continue;
        };
        report.checked += 1;
        let bd: HashSet<&str> = b.build_deps.iter().map(String::as_str).collect();
        let own: HashSet<&str> = b.provides.iter().map(String::as_str).collect();
        let mut items: Vec<Finding> = Vec::new();
        for soname in &b.needed_so {
            if own.contains(soname.as_str()) {
                continue; // 自提供（farm 扫描本就会把自提供从 needed_so 扣掉）
            }
            let Some(owners) = providers.get(soname) else {
                continue; // 仓库内无 provider → 不是 build_deps 漏写（归 abifix）
            };
            // 满足：任一 provider 直接写在 build_deps，或被 base/base-devel 直接 deps 覆盖
            if owners
                .iter()
                .any(|o| bd.contains(o.as_str()) || covered.contains(o))
            {
                continue;
            }
            let o: Vec<&str> = owners.iter().map(String::as_str).collect();
            items.push(Finding {
                file: soname.clone(),
                what: tr!("chk.build-deps.provider_not_declared", soname, o.join("|")),
                severity: Severity::Critical,
            });
        }
        if !items.is_empty() {
            report.findings.insert(pkg.clone(), items);
        }
    }
    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 造一个只有配方的假 pkgs 目录（本检則不需要 .lpkg）。
    fn write_pkg(dir: &Path, name: &str, provides: &[&str], needed: &[&str], bd: &[&str]) {
        let p = dir.join(name);
        std::fs::create_dir_all(&p).unwrap();
        std::fs::write(
            p.join("LankeBUILD.json"),
            serde_json::to_string(&serde_json::json!({
                "name": name, "version": "1.0",
                "provides": provides, "needed_so": needed, "build_deps": bd
            }))
            .unwrap(),
        )
        .unwrap();
    }

    fn opts(pkgs_dir: &Path) -> ChkOpts {
        ChkOpts {
            source: pkgs_dir.to_path_buf(),
            arch: "x86_64".into(),
            cache: pkgs_dir.join(".cache"),
            pkgs_dir: pkgs_dir.to_path_buf(),
            subset: vec![],
            full_rescan: false,
        }
    }

    fn tmp(name: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("farm-bd-{}-{}", name, std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn flags_provider_missing_from_build_deps() {
        let d = tmp("miss");
        write_pkg(&d, "libfoo", &["libfoo.so.1"], &[], &["base-devel"]);
        // 直接依赖满足 → 不报
        write_pkg(
            &d,
            "ok",
            &["libok.so.1"],
            &["libfoo.so.1"],
            &["base-devel", "libfoo"],
        );
        // 漏写 → Critical
        write_pkg(
            &d,
            "bad",
            &["libbad.so.1"],
            &["libfoo.so.1"],
            &["base-devel"],
        );
        let rep = run(&opts(&d)).unwrap();
        assert!(!rep.findings.contains_key("ok"), "{:?}", rep.findings);
        assert!(!rep.findings.contains_key("libfoo"), "{:?}", rep.findings);
        let f = &rep.findings["bad"];
        assert_eq!(f.len(), 1);
        assert_eq!(f[0].severity, Severity::Critical);
        assert_eq!(f[0].file, "libfoo.so.1");
        assert!(f[0].what.contains("libfoo"));
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn transitive_availability_does_not_satisfy() {
        // 用户规则：needed_so 是**直接**依赖，被别的 build_dep 传递满足也**不算**
        let d = tmp("trans");
        write_pkg(&d, "libfoo", &["libfoo.so.1"], &[], &["base-devel"]);
        write_pkg(
            &d,
            "libbar",
            &["libbar.so.1"],
            &["libfoo.so.1"],
            &["base-devel", "libfoo"],
        );
        // consumer 只写 libbar，靠 libbar 的 deps 传递能拿到 libfoo —— 仍必须报
        write_pkg(
            &d,
            "consumer",
            &["consumer.so.1"],
            &["libfoo.so.1", "libbar.so.1"],
            &["base-devel", "libbar"],
        );
        let rep = run(&opts(&d)).unwrap();
        let f = &rep.findings["consumer"];
        assert_eq!(f.len(), 1, "只该报 libfoo.so.1: {f:?}");
        assert_eq!(f[0].file, "libfoo.so.1");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn base_and_base_devel_covered_providers_are_satisfied() {
        let d = tmp("base");
        write_pkg(&d, "glibc", &["libc.so.6"], &[], &["base-devel"]);
        write_pkg(&d, "gcc", &["libgcc_s.so.1"], &[], &["base-devel"]);
        // base-devel 的**直接 deps** 覆盖 glibc → libc.so.6 视为满足
        let bd_dir = d.join("base-devel");
        std::fs::create_dir_all(&bd_dir).unwrap();
        std::fs::write(
            bd_dir.join("LankeBUILD.json"),
            serde_json::to_string(&serde_json::json!({
                "name": "base-devel", "version": "1.0",
                "provides": [], "needed_so": [], "build_deps": [], "deps": ["glibc"]
            }))
            .unwrap(),
        )
        .unwrap();

        write_pkg(
            &d,
            "app",
            &["app.so.1"],
            &["libc.so.6", "libgcc_s.so.1"],
            &["base-devel"],
        );
        let rep = run(&opts(&d)).unwrap();
        // libc.so.6 → glibc（被 base-devel 直接 deps 覆盖）满足；
        // libgcc_s.so.1 → gcc 未被覆盖 → 报
        let f = &rep.findings["app"];
        assert_eq!(f.len(), 1, "只该报 libgcc_s.so.1: {f:?}");
        assert_eq!(f[0].file, "libgcc_s.so.1");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn self_provided_and_providerless_sonames_are_skipped() {
        let d = tmp("skip");
        write_pkg(
            &d,
            "selfish",
            &["libself.so.1"],
            &["libself.so.1"],
            &["base-devel"],
        );
        // 仓库内无 provider（如 perl 不提供 libperl.so）→ 不报（归 abifix）
        write_pkg(
            &d,
            "orphan",
            &["orphan.so.1"],
            &["libperl.so"],
            &["base-devel"],
        );
        let rep = run(&opts(&d)).unwrap();
        assert!(rep.findings.is_empty(), "{:?}", rep.findings);
        assert_eq!(rep.checked, 2);
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn ignore_flag_and_subset_are_honored() {
        let d = tmp("filter");
        write_pkg(&d, "libfoo", &["libfoo.so.1"], &[], &["base-devel"]);
        write_pkg(
            &d,
            "bad",
            &["libbad.so.1"],
            &["libfoo.so.1"],
            &["base-devel"],
        );
        std::fs::create_dir_all(d.join("bad2")).unwrap();
        std::fs::write(
            d.join("bad2/LankeBUILD.json"),
            serde_json::to_string(&serde_json::json!({
                "name": "bad2", "version": "1.0", "provides": [], "needed_so": ["libfoo.so.1"],
                "build_deps": ["base-devel"], "farm_flags": ["IGNORE_CHK_BUILDDEPS"]
            }))
            .unwrap(),
        )
        .unwrap();
        // 豁免生效（豁免包在 is_ignored 处 continue，不计入 checked——与其它检則同构）
        let rep = run(&opts(&d)).unwrap();
        assert!(!rep.findings.contains_key("bad2"), "{:?}", rep.findings);
        assert!(rep.findings.contains_key("bad"), "{:?}", rep.findings);
        // --pkg 只审计子集
        let mut o = opts(&d);
        o.subset = vec!["bad".to_string()];
        let rep2 = run(&o).unwrap();
        assert_eq!(rep2.checked, 1);
        assert!(rep2.findings.contains_key("bad"), "{:?}", rep2.findings);
        let mut o2 = opts(&d);
        o2.subset = vec!["bad2".to_string()];
        let rep3 = run(&o2).unwrap();
        assert!(rep3.findings.is_empty(), "{:?}", rep3.findings);
        assert_eq!(rep3.checked, 0, "豁免包不计数");
        let _ = std::fs::remove_dir_all(&d);
    }
}
