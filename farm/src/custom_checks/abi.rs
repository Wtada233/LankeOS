//! custom_checks/abi — 全仓库 ABI 符号/版本审计（`farm chk abi`；原顶层 `abi_fullchk.rs`/`manual-abi-fullchk`）。
//!
//! 与 qml/pkgconf/pkg_err/hook 同一个缓存框架：**整包 cache**（`super::walk_all`，key = 当前
//! `.lpkg` 文件 sha256 + schema）——每包一个 `<cache>/<pkg>.json`，里面存**该包每个 ELF 文件**的
//! 扫描信息（soname / needed / 导出版本符号 defined / 未定义引用 undef）。`.lpkg` 未变 → 跳过
//! 解包与重扫（其它 chk 同款）；不再维护 per-SONAME 单文件缓存。
//!
//! 算法（两段式，镜像 python `/tmp/scan_elf_ver.py`）：
//! 1. provider 目录：遍历全部包的 cached analysis，收集带 SONAME 的 ELF 导出的 `sym@ver`；
//! 2. consumer：可执行/库的 `undef`（symbol@version）引用 → 若其 DT_NEEDED 候选库都不提供 → 报缺失。
//!
//! 原生 ELF 扫描用 goblin 读 `.gnu.version_d/.gnu.version_r/.gnu.version`（verdef/verneed/versym）与
//! dynsym/dynstr，不 shell readelf/objdump。语义注记：同 SONAME 多份（打包 bug）→ 排序遍历最后胜。

use super::{walk_all, ChkOpts, Finding, Report, Severity};
use crate::error::FarmError;
use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};

use goblin::elf::Elf;

/// 单个 ELF 的动态信息（轻解析，不做符号 join）。
struct Probe {
    soname: Option<String>,
    needed: Vec<String>,
}

fn probe(bytes: &[u8]) -> Result<Probe, FarmError> {
    let elf = Elf::parse(bytes).map_err(|e| format!("ELF 解析失败: {e}"))?;
    Ok(Probe {
        soname: elf.soname.map(String::from),
        needed: elf.libraries.iter().map(|s| s.to_string()).collect(),
    })
}

/// 完整符号版本 join：defined（版本 → 符号集，provider 导出）与 undef（(符号, 版本)，
/// consumer 引用）。索引 0/1（local/global=无版本）跳过（对齐 objdump -T 只收带 @ 的项）。
fn symbols(
    bytes: &[u8],
) -> Result<(BTreeMap<String, BTreeSet<String>>, Vec<(String, String)>), FarmError> {
    let elf = goblin::elf::Elf::parse(bytes).map_err(|e| format!("ELF 解析失败: {e}"))?;
    let strtab = &elf.dynstrtab;
    // verdef：vd_ndx → 版本名（本 .so 导出的版本）
    let mut defver: std::collections::HashMap<u16, String> = std::collections::HashMap::new();
    if let Some(vd) = &elf.verdef {
        for d in vd.iter() {
            if let Some(name) = d.iter().next().and_then(|a| strtab.get_at(a.vda_name)) {
                defver.insert(d.vd_ndx, name.to_string());
            }
        }
    }
    // verneed：vna_other → 版本名（对外部库的版本引用）
    let mut needver: std::collections::HashMap<u16, String> = std::collections::HashMap::new();
    if let Some(vn) = &elf.verneed {
        for n in vn.iter() {
            for a in n.iter() {
                if let Some(name) = strtab.get_at(a.vna_name) {
                    needver.insert(a.vna_other, name.to_string());
                }
            }
        }
    }
    let mut defined: BTreeMap<String, BTreeSet<String>> = BTreeMap::new();
    let mut undef: Vec<(String, String)> = Vec::new();
    if let Some(vs) = &elf.versym {
        for (i, sym) in elf.dynsyms.iter().enumerate() {
            let Some(vsym) = vs.get_at(i) else { continue };
            let idx = vsym.version();
            if idx == 0 || idx == 1 {
                continue; // VER_NDX_LOCAL / VER_NDX_GLOBAL：无版本
            }
            let Some(name) = strtab.get_at(sym.st_name) else {
                continue;
            };
            if sym.st_shndx == 0 {
                // SHN_UNDEF：import，需外部库提供
                if let Some(ver) = needver.get(&idx) {
                    undef.push((name.to_string(), ver.clone()));
                }
            } else if let Some(ver) = defver.get(&idx) {
                defined
                    .entry(ver.clone())
                    .or_default()
                    .insert(name.to_string());
            }
        }
    }
    Ok((defined, undef))
}

/// 收集目录下所有常规文件（DFS，排序 → 确定序；不含符号链接成员——.so 真身是常规文件）。
fn collect_files(root: &Path) -> Vec<PathBuf> {
    fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
        let Ok(rd) = fs::read_dir(dir) else { return };
        let mut subs: Vec<PathBuf> = Vec::new();
        for e in rd.flatten() {
            let p = e.path();
            let Ok(ft) = e.file_type() else { continue };
            if ft.is_dir() {
                subs.push(p);
            } else if ft.is_file() {
                out.push(p);
            }
        }
        subs.sort();
        for s in subs {
            walk(&s, out);
        }
    }
    let mut v = Vec::new();
    walk(root, &mut v);
    v.sort();
    v
}

fn dedup_sorted_undef(mut undef: Vec<(String, String)>) -> Vec<(String, String)> {
    undef.sort();
    undef.dedup();
    undef
}

/// 包的内容分析：遍历 content 下每个 ELF，存 soname/needed/defined/undef。供 `walk_all` 整包缓存。
fn analyze(extract: &Path) -> Result<serde_json::Value, FarmError> {
    let content = extract.join("content");
    let mut files: Vec<serde_json::Value> = Vec::new();
    for f in collect_files(&content) {
        let Ok(bytes) = fs::read(&f) else {
            continue;
        };
        let Ok(p) = probe(&bytes) else {
            continue; // 非 ELF / 不可解析
        };
        let Ok((defined, undef)) = symbols(&bytes) else {
            continue;
        };
        let rel = f
            .strip_prefix(&content)
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default();
        files.push(serde_json::json!({
            "file": rel,
            "soname": p.soname,
            "needed": p.needed,
            "defined": defined,      // {ver: [sym…]}，provider 导出
            "undef": dedup_sorted_undef(undef), // [[sym, ver]…]，consumer 引用
        }));
    }
    Ok(serde_json::json!({ "files": files }))
}

/// 与 qml/pkgconf/pkg_err/hook 同级的检則入口：审计 `ChkOpts.source` 全部包 → 统一 `Report`。
/// provider = 全仓库带 SONAME ELF 的 `defined`；consumer = 各包 ELF 的 `undef` 引用——其 DT_NEEDED
/// 候选库都不提供该 `sym@ver` → `Finding`（`what` = `sym @ ver（候选提供: …）`）。整包缓存，
/// `.lpkg` 未变不重扫（`farm chk full` 二遍起与其它 chk 一样全命中）。
pub fn run(o: &ChkOpts) -> Result<Report, FarmError> {
    let (analyses, hits, misses, failed) = walk_all(o, |ext, _pkg| analyze(ext))?;

    // 1) provider 目录：soname → sym → {ver}
    let mut catalog: BTreeMap<String, BTreeMap<String, BTreeSet<String>>> = BTreeMap::new();
    for a in analyses.values() {
        let Some(files) = a["files"].as_array() else {
            continue;
        };
        for f in files {
            let Some(soname) = f["soname"].as_str() else {
                continue;
            };
            let Some(def) = f["defined"].as_object() else {
                continue;
            };
            let entry = catalog.entry(soname.to_string()).or_default();
            for (ver, syms) in def {
                let Some(syms) = syms.as_array() else {
                    continue;
                };
                for s in syms.iter().filter_map(|s| s.as_str()) {
                    entry.entry(s.to_string()).or_default().insert(ver.clone());
                }
            }
        }
    }

    // 2) consumer 判定（全仓库 provider 已建全后统一做，确定性）
    let audit_all = o.subset.is_empty();
    let audit: BTreeSet<&str> = o.subset.iter().map(String::as_str).collect();
    let mut report = Report {
        cache_hits: hits,
        cache_misses: misses,
        failed,
        ..Default::default()
    };
    for (pkg, a) in &analyses {
        if !(audit_all || audit.contains(pkg.as_str())) {
            continue;
        }
        if super::is_ignored(&o.pkgs_dir, pkg, "ABI") {
            continue;
        }
        report.checked += 1;
        let mut items: Vec<Finding> = Vec::new();
        let Some(files) = a["files"].as_array() else {
            continue;
        };
        for f in files {
            let Some(undef) = f["undef"].as_array() else {
                continue;
            };
            if undef.is_empty() {
                continue;
            }
            let rel = f["file"].as_str().unwrap_or("?");
            // 候选 = 本 ELF 链接、且全仓库确有 provider 的 SONAME（都未提供才报）
            let cands: Vec<String> = f["needed"]
                .as_array()
                .map(|a| {
                    a.iter()
                        .filter_map(|v| v.as_str())
                        .filter(|s| catalog.contains_key(*s))
                        .map(str::to_string)
                        .collect()
                })
                .unwrap_or_default();
            for u in undef {
                let (Some(sym), Some(ver)) = (u[0].as_str(), u[1].as_str()) else {
                    continue;
                };
                let resolved = cands.iter().any(|son| {
                    catalog
                        .get(son)
                        .and_then(|m| m.get(sym))
                        .is_some_and(|vs| vs.contains(ver))
                });
                if resolved {
                    continue;
                }
                // 全仓库有别的库提供该 sym@ver → Warning（缺依赖）；仓库也没有 → Critical（真断裂）
                let provided_anywhere = catalog
                    .values()
                    .any(|m| m.get(sym).is_some_and(|vs| vs.contains(ver)));
                let severity = if provided_anywhere {
                    Severity::Warning
                } else {
                    Severity::Critical
                };
                items.push(Finding {
                    file: rel.to_string(),
                    what: format!(
                        "{} @ {}{}",
                        sym,
                        ver,
                        if cands.is_empty() {
                            String::new()
                        } else {
                            format!("（候选提供: {}）", cands.join(", "))
                        }
                    ),
                    severity,
                });
            }
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

    /// 找宿主可用的带符号版本的共享库（无则 None，测试跳过——与 i18n test.skip_host_libc 同思路）。
    fn host_versioned_lib() -> Option<PathBuf> {
        for cand in [
            "/usr/lib/libc.so.6",
            "/usr/lib64/libc.so.6",
            "/lib/libc.so.6",
            "/lib64/libc.so.6",
        ] {
            let p = Path::new(cand);
            if p.is_file() {
                return Some(p.to_path_buf());
            }
        }
        None
    }

    #[test]
    fn native_parses_host_libc_exports_and_imports() {
        let Some(lib) = host_versioned_lib() else {
            eprintln!("跳过：宿主无 libc.so.6");
            return;
        };
        let bytes = fs::read(&lib).unwrap();
        let pr = probe(&bytes).unwrap();
        assert_eq!(pr.soname.as_deref(), Some("libc.so.6"));
        assert!(pr.needed.iter().any(|n| n.starts_with("ld-linux")));
        let (defined, undef) = symbols(&bytes).unwrap();
        assert!(!defined.is_empty(), "libc 应导出带版本符号");
        assert!(
            defined.keys().any(|v| v.starts_with("GLIBC_")),
            "libc 导出应有 GLIBC_* 版本: {:?}",
            defined.keys().take(5).collect::<Vec<_>>()
        );
        assert!(!undef.is_empty(), "libc 也应引用外部符号@版本");
    }

    /// 造一个假 .lpkg（宿主 libc 当内容），端到端跑 run：整包缓存落盘 / 第二遍命中，且不再有
    /// per-SONAME 单文件缓存。
    #[test]
    fn whole_package_cache_hit_and_no_soname_files() {
        let Some(lib) = host_versioned_lib() else {
            eprintln!("跳过：宿主无 libc.so.6");
            return;
        };
        let base = std::env::temp_dir().join(format!("farm-abicache-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        // 构造假仓库 out/x86_64/mylib/1.0.lpkg，content/usr/lib 放宿主 libc 副本
        let src = base.join("src");
        fs::create_dir_all(src.join("content/usr/lib")).unwrap();
        fs::write(
            src.join("metadata.json"),
            r#"{"name":"mylib","version":"1.0"}"#,
        )
        .unwrap();
        let libname = lib.file_name().unwrap().to_string_lossy().into_owned();
        fs::copy(&lib, src.join("content/usr/lib").join(&libname)).unwrap();
        let lpkg = base.join("repo/x86_64/mylib/1.0.lpkg");
        fs::create_dir_all(lpkg.parent().unwrap()).unwrap();
        let f = fs::File::create(&lpkg).unwrap();
        let enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
        let mut b = tar::Builder::new(enc);
        b.append_dir_all(".", &src).unwrap();
        let enc = b.into_inner().unwrap();
        enc.finish().unwrap();
        fs::remove_dir_all(&src).unwrap();

        let cache = base.join("cache");
        let opts = ChkOpts {
            source: base.join("repo"),
            arch: "x86_64".into(),
            cache: cache.clone(),
            pkgs_dir: base.join("pkgs"),
            subset: vec!["mylib".into()],
            full_rescan: false,
        };
        // 第一遍：整包缓存落盘（mylib.json），无 per-SONAME 文件
        let r1 = run(&opts).unwrap();
        assert!(r1.cache_misses >= 1, "首扫应写整包缓存: {r1:?}");
        assert!(r1.failed.is_empty(), "不应有失败: {:?}", r1.failed);
        assert!(cache.join("mylib.json").exists(), "应有整包缓存 mylib.json");
        assert!(
            !cache.join("libc.so.6.json").exists(),
            "不应再写 per-SONAME 单文件缓存"
        );
        // 第二遍：整包缓存命中，不重扫
        let r2 = run(&opts).unwrap();
        assert!(r2.cache_hits >= 1, "第二遍应命中整包缓存: {r2:?}");
        fs::remove_dir_all(&base).ok();
    }

    /// 预构建 + strip 的受控 fixture 端到端冒烟（用户场景）。fixture 二进制随仓库提交于
    /// `tests/fixtures/abi/`（strip 过，模拟 LankeOS 打包），测试只把它们装进 .lpkg 跑——**不依赖
    /// 宿主编译器**：
    /// - 包 libx：libx.so.1 导出 sym1@V1、sym2@V2（真 .gnu.version_d）；
    /// - 包 foo：可执行（无 SONAME），引用 sym1@V2、sym2@V2（真 .gnu.version_r）；
    /// - 包 nover：无版本节、无导入的可执行（空路径不炸）。
    /// provider 目录来自 source 全量；断言只报 foo 的 sym1@V2 缺失（sym2@V2 有 V2，不报）。
    #[test]
    fn smoke_reports_missing_versioned_symbol() {
        let fx = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/abi");
        for need in ["libx.so.1", "foo", "nover"] {
            assert!(fx.join(need).is_file(), "缺 fixture: {:?}", fx.join(need));
        }
        let base = std::env::temp_dir().join(format!("farm-abifchk-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        fs::create_dir_all(&base).unwrap();
        // 组装 .lpkg：content 里放真 ELF fixture
        let repo = base.join("repo/x86_64");
        let mk_pkg = |name: &str, rel: &str, fixture: &str| {
            let root = base.join(format!("pkg-{name}"));
            fs::create_dir_all(root.join("content").join(rel)).unwrap();
            fs::write(
                root.join("metadata.json"),
                format!(r#"{{"name":"{name}","version":"1.0"}}"#),
            )
            .unwrap();
            fs::copy(
                fx.join(fixture),
                root.join("content").join(rel).join(fixture),
            )
            .unwrap();
            let pkgdir = repo.join(name);
            fs::create_dir_all(&pkgdir).unwrap();
            let f = fs::File::create(pkgdir.join("1.0.lpkg")).unwrap();
            let enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
            let mut b = tar::Builder::new(enc);
            b.append_dir_all(".", &root).unwrap();
            let enc = b.into_inner().unwrap();
            enc.finish().unwrap();
            fs::remove_dir_all(&root).unwrap();
        };
        mk_pkg("libx", "usr/lib", "libx.so.1");
        mk_pkg("foo", "usr/bin", "foo");
        mk_pkg("nover", "usr/bin", "nover");

        let cache = base.join("cache");
        let mk_opts = |pkgs: Vec<String>| ChkOpts {
            source: base.join("repo"),
            arch: "x86_64".into(),
            cache: cache.clone(),
            pkgs_dir: base.join("pkgs"),
            subset: pkgs,
            full_rescan: false,
        };
        let r = run(&mk_opts(vec![])).unwrap();
        let foo_items = r.findings.get("foo").expect("foo 应有缺失符号");
        let what: Vec<String> = foo_items.iter().map(|f| f.what.clone()).collect();
        assert!(
            what.iter().any(|w| w.contains("sym1 @ V2")),
            "foo 引用 sym1@V2，libx 只提供 V1，应报缺失: {what:?}"
        );
        assert!(
            !what.iter().any(|w| w.contains("sym2 @ V2")),
            "sym2@V2 由 libx 提供，不应报缺失: {what:?}"
        );
        assert!(
            !r.findings.contains_key("libx") && !r.findings.contains_key("nover"),
            "libx(provider)/nover(无版本) 不应误报: {:?}",
            r.findings
        );
        // subset foo：provider 仍来自 source 全量，缺照样报
        let r2 = run(&mk_opts(vec!["foo".into()])).unwrap();
        assert!(
            r2.findings
                .get("foo")
                .is_some_and(|v| v.iter().any(|f| f.what.contains("sym1 @ V2"))),
            "只审计 foo 也应报 sym1@V2（provider 用全量 source）: {:?}",
            r2.findings
        );
        fs::remove_dir_all(&base).ok();
    }
}
