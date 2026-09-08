//! abi_fullchk.rs — `manual-abi-fullchk`：全仓库 ABI 符号/版本审计。
//!
//! 参考 `/tmp/scan_elf_ver.py`（运行时符号@版本 not-found 审计），但：
//! - **原生 ELF 扫描**：goblin 0.9 直接给 `.gnu.version_d/.gnu.version_r/.gnu.version`（verdef/verneed/
//!   versym）与 dynsym/dynstr——不 shell readelf/objdump、不加新依赖。
//! - **缓存**：每个 SONAME 一个 json（默认 `~/.cache/lankefarm-abi/<soname>.json`），内容含该 SONAME
//!   内容的 sha256 + 它提供的全部符号版本 → 符号列表。真实 .so 的 sha 与缓存一致 → **不重扫**该文件
//!   （只登记 provider 导出）；不一致 → 原生重扫并覆写缓存。
//! - **tmpdir 工作**：每个 .lpkg 只解包一次到临时工作目录、直接扫盘上文件（修参考脚本"每成员解一次"）。
//!
//! 两段式（镜像 python）：provider 目录（SONAME → 导出版本符号）→ consumer（可执行 + 未命中缓存的
//! 库）引用 `name@ver` 无任何 DT_NEEDED 候选库提供 → 按包报告。
//!
//! 语义注记：同 SONAME 出现多份不同内容的文件（打包 bug）→ 最后一次（确定性排序）胜；缓存命中时该
//! 库不做 consumer 再审计（首次全扫已覆盖，之后以可执行/变更库为主）。

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fs;
use std::path::{Path, PathBuf};

use goblin::elf::header::ET_DYN;
use sha2::{Digest, Sha256};

use crate::custom_checks::Severity;
use crate::scan;

/// 一个缺失的 consumer 符号@版本。
#[derive(Debug, Clone)]
pub struct MissingItem {
    /// 相对 content 的 ELF 路径（如 `usr/bin/gjs`）
    pub elf: String,
    pub name: String,
    pub ver: String,
    /// 候选 DT_NEEDED 库（provider 目录里有记录的），都未提供该 name@ver
    pub candidates: Vec<String>,
    /// 三段判定：闭包内找不到；全仓库有其它的提供库 → Warning；仓库也无 → Critical。
    pub severity: Severity,
}

/// fullchk 报告。
#[derive(Debug, Default)]
pub struct FullchkReport {
    /// 处理的 .lpkg 数（含失败）
    pub lpkg_files: usize,
    /// 成功扫描到的 ELF 数
    pub elf_files: usize,
    pub cache_hits: u64,
    pub cache_misses: u64,
    /// provider 目录：SONAME 数
    pub provider_sonames: usize,
    /// pkg → 缺失符号（排序、确定性）
    pub missing: BTreeMap<String, Vec<MissingItem>>,
    /// 解包/读取失败的 .lpkg
    pub failed: Vec<String>,
}

pub struct FullchkOpts {
    /// **provider/cache 来源**：构建仓库根（含 `<arch>/`），**所有包**都用来建 provider 目录
    /// （consumer 引用必须对照全仓库谁提供什么符号@版本）。默认 `out`。
    pub source: PathBuf,
    pub arch: String,
    /// 缓存目录（每个 SONAME 一个 json）
    pub cache: PathBuf,
    /// 空 = 审计全部包；否则只审计/报告这些包（provider 仍来自 `source` 全量）
    pub pkgs: Vec<String>,
    /// 配方根（读 farm_flags 的 IGNORE_CHK_ABI 豁免；只读 flags，deps 不管）
    pub pkgs_dir: PathBuf,
    /// 忽略缓存强制全量重扫
    pub full_rescan: bool,
}

/// 默认缓存目录：`$HOME/.cache/lankefarm-abi`（无 HOME 时回落 out/ 旁）。
pub fn default_cache_dir(source: &Path) -> PathBuf {
    match std::env::var("HOME") {
        Ok(h) if !h.is_empty() => PathBuf::from(h).join(".cache/lankefarm-abi"),
        _ => source.join(".abi-cache"),
    }
}

/// 单个 ELF 的动态信息（轻解析，不做符号 join）。
struct Probe {
    is_dyn: bool,
    soname: Option<String>,
    needed: Vec<String>,
}

fn probe(bytes: &[u8]) -> Result<Probe, String> {
    let elf = goblin::elf::Elf::parse(bytes).map_err(|e| format!("ELF 解析失败: {e}"))?;
    Ok(Probe {
        is_dyn: elf.header.e_type == ET_DYN,
        soname: elf.soname.map(String::from),
        needed: elf.libraries.iter().map(|s| s.to_string()).collect(),
    })
}

/// 完整符号版本 join：defined（版本 → 符号集，provider 导出）与 undef（(符号, 版本)，
/// consumer 引用）。索引 0/1（local/global=无版本）跳过（对齐 objdump -T 只收带 @ 的项）。
fn symbols(
    bytes: &[u8],
) -> Result<(BTreeMap<String, BTreeSet<String>>, Vec<(String, String)>), String> {
    let elf = goblin::elf::Elf::parse(bytes).map_err(|e| format!("ELF 解析失败: {e}"))?;
    let strtab = &elf.dynstrtab;
    // verdef：vd_ndx → 版本名（本 .so 导出的版本）
    let mut defver: HashMap<u16, String> = HashMap::new();
    if let Some(vd) = &elf.verdef {
        for d in vd.iter() {
            if let Some(name) = d.iter().next().and_then(|a| strtab.get_at(a.vda_name)) {
                defver.insert(d.vd_ndx, name.to_string());
            }
        }
    }
    // verneed：vna_other → 版本名（对外部库的版本引用）
    let mut needver: HashMap<u16, String> = HashMap::new();
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

/// cache json：SONAME → { sha256, versions: {ver: [sym…]} }。
#[derive(serde::Serialize, serde::Deserialize)]
struct SonameCache {
    soname: String,
    sha256: String,
    #[serde(default)]
    versions: BTreeMap<String, Vec<String>>,
}

fn cache_path(cache: &Path, soname: &str) -> PathBuf {
    let safe: String = soname
        .chars()
        .map(|c| match c {
            'a'..='z' | 'A'..='Z' | '0'..='9' | '.' | '_' | '-' | '+' => c,
            _ => '_',
        })
        .collect();
    cache.join(format!("{safe}.json"))
}

/// 缓存命中：文件存在且 `sha256` 字段 == 目标文件 sha → 返回缓存版本表；否则 None（需重扫）。
fn load_cache(cache: &Path, soname: &str, sha: &str) -> Option<BTreeMap<String, Vec<String>>> {
    let path = cache_path(cache, soname);
    let c: SonameCache = serde_json::from_str(&fs::read_to_string(path).ok()?).ok()?;
    if c.soname != soname || c.sha256 != sha {
        return None;
    }
    Some(c.versions)
}

fn write_cache(
    cache: &Path,
    soname: &str,
    sha: &str,
    versions: &BTreeMap<String, Vec<String>>,
) -> Result<(), String> {
    let c = SonameCache {
        soname: soname.to_string(),
        sha256: sha.to_string(),
        versions: versions.clone(),
    };
    let json = serde_json::to_string_pretty(&c).map_err(|e| format!("序列化缓存失败: {e}"))?;
    fs::write(cache_path(cache, soname), json).map_err(|e| format!("写缓存失败: {e}"))
}

fn sha256_hex(bytes: &[u8]) -> String {
    let mut h = Sha256::new();
    h.update(bytes);
    format!("{:x}", h.finalize())
}

fn is_elf_file(p: &Path) -> bool {
    let Ok(f) = fs::File::open(p) else {
        return false;
    };
    use std::io::Read;
    let mut magic = [0u8; 4];
    let mut r = f;
    if r.read_exact(&mut magic).is_err() {
        return false;
    }
    magic == *b"\x7fELF"
}

/// 收集目录下所有文件（DFS，排序 → 确定序）。
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

/// consumer 审计上下文（provider 目录建完后统一判缺失）。
struct Consumer {
    pkg: String,
    elf: String,
    needed: Vec<String>,
    undef: Vec<(String, String)>,
}

/// 两段式全 ABI 审计。见模块头。
pub fn run_fullchk(o: &FullchkOpts) -> Result<FullchkReport, String> {
    fs::create_dir_all(&o.cache).map_err(|e| format!("创建缓存目录 {:?} 失败: {e}", o.cache))?;
    let repo_root = o.source.join(&o.arch);
    // **provider/cache 来源 = source 下所有包**（consumer 引用要对照全仓库）；`--pkgs` 只收窄审计范围。
    let mut pkgdirs: Vec<PathBuf> = fs::read_dir(&repo_root)
        .map_err(|e| format!("读取 {repo_root:?} 失败: {e}"))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.is_dir())
        .collect();
    pkgdirs.sort();
    // 审计范围：pkgs 为空 = 全部；否则只对列出的包收集 consumer 并报告（provider 仍全量建）
    let audit_all = o.pkgs.is_empty();
    let audit: std::collections::BTreeSet<&str> = o.pkgs.iter().map(String::as_str).collect();

    // provider 目录：soname -> name -> {ver}
    let mut catalog: BTreeMap<String, BTreeMap<String, BTreeSet<String>>> = BTreeMap::new();
    let mut consumers: Vec<Consumer> = Vec::new();
    let mut report = FullchkReport::default();

    // 临时工作目录（每个 .lpkg 解一次，扫完清理）
    let scratch = std::env::temp_dir().join(format!(
        "lankefarm-abi-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.subsec_nanos())
            .unwrap_or(0)
    ));
    fs::create_dir_all(&scratch).map_err(|e| format!("创建临时工作目录失败: {e}"))?;
    let result: Result<(), String> = (|| {
        let is_audited = |pkg: &str| audit_all || audit.contains(pkg);
        for pkgdir in &pkgdirs {
            let pkg = pkgdir
                .file_name()
                .and_then(|n| n.to_str())
                .unwrap_or("?")
                .to_string();
            let mut lpkg_files: Vec<PathBuf> = fs::read_dir(pkgdir)
                .map(|rd| {
                    rd.filter_map(|e| e.ok().map(|e| e.path()))
                        .filter(|p| p.extension().is_some_and(|x| x == "lpkg"))
                        .collect()
                })
                .unwrap_or_default();
            lpkg_files.sort();
            for lpkg in lpkg_files {
                report.lpkg_files += 1;
                // 解一次到临时目录
                let extract_dir = scratch.join(&pkg);
                if let Err(e) = scan::extract_lpkg(&lpkg, &extract_dir) {
                    report.failed.push(format!("{pkg}: {e}"));
                    continue;
                }
                let content = extract_dir.join("content");
                for f in collect_files(&content) {
                    if !is_elf_file(&f) {
                        continue;
                    }
                    let Ok(bytes) = fs::read(&f) else { continue };
                    report.elf_files += 1;
                    let rel = f
                        .strip_prefix(&content)
                        .map(|p| p.to_string_lossy().into_owned())
                        .unwrap_or_else(|_| f.display().to_string());
                    // 轻解析：类型 / SONAME / DT_NEEDED
                    let Ok(pr) = probe(&bytes) else { continue };
                    let sha = sha256_hex(&bytes);
                    if pr.is_dyn && pr.soname.is_some() {
                        let soname = pr.soname.as_deref().unwrap_or_default().to_string();
                        if !o.full_rescan {
                            if let Some(versions) = load_cache(&o.cache, &soname, &sha) {
                                report.cache_hits += 1;
                                register_provider(&mut catalog, &soname, &versions);
                                continue; // 缓存命中：不重扫该文件
                            }
                        }
                        report.cache_misses += 1;
                        let (defined, undef) = symbols(&bytes)?;
                        let versions = defined
                            .into_iter()
                            .map(|(v, s)| {
                                let mut v2: Vec<String> = s.into_iter().collect();
                                v2.sort();
                                (v, v2)
                            })
                            .collect();
                        write_cache(&o.cache, &soname, &sha, &versions)?;
                        register_provider(&mut catalog, &soname, &versions);
                        // provider 库自身也可作 consumer（引用其它库）；仅审计范围内的包收集
                        if is_audited(&pkg) && !undef.is_empty() {
                            consumers.push(Consumer {
                                pkg: pkg.clone(),
                                elf: rel,
                                needed: pr.needed.clone(),
                                undef: dedup_sorted_undef(undef),
                            });
                        }
                    } else {
                        // 非库 ELF（可执行/无 SONAME 的库）：只做 consumer 审计
                        if !is_audited(&pkg) || pr.needed.is_empty() {
                            continue;
                        }
                        let (_defined, undef) = symbols(&bytes)?;
                        if undef.is_empty() {
                            continue;
                        }
                        consumers.push(Consumer {
                            pkg: pkg.clone(),
                            elf: rel,
                            needed: pr.needed,
                            undef: dedup_sorted_undef(undef),
                        });
                    }
                }
                // 每包解包目录用完即清：全仓库顺序解到同一 scratch 会累积到磁盘满
                // （zip/zlib 等后序包曾 ENOSPC 解包失败）。
                let _ = scan::remove_dir_tree(&extract_dir);
            }
        }
        // consumer 判缺失：候选 = DT_NEEDED ∩ provider 目录；候选为空跳过（无法判断，同 python）
        consumers.sort_by(|a, b| (&a.pkg, &a.elf).cmp(&(&b.pkg, &b.elf)));
        for con in consumers {
            // farm_flags 豁免：IGNORE_CHK_ABI 的包（LankeBUILD.json 只读 flags）
            if crate::custom_checks::is_ignored(&o.pkgs_dir, &con.pkg, "ABI") {
                continue;
            }
            let cands: Vec<String> = con
                .needed
                .iter()
                .filter(|n| catalog.contains_key(*n))
                .cloned()
                .collect();
            if cands.is_empty() {
                continue;
            }
            for (name, ver) in &con.undef {
                // 三段判定：闭包内(DT_NEEDED 候选)找到 → ok；否则全仓库还有别的库提供 → Warning；
                // 全仓库也无 → Critical（该 name@ver 无人提供 = 真缺口）。
                let found = cands.iter().any(|s| {
                    catalog
                        .get(s)
                        .and_then(|nv| nv.get(name))
                        .is_some_and(|vers| vers.contains(ver))
                });
                if !found {
                    let whole_repo = catalog
                        .values()
                        .any(|nv| nv.get(name).is_some_and(|vs| vs.contains(ver)));
                    let severity = if whole_repo {
                        Severity::Warning
                    } else {
                        Severity::Critical
                    };
                    report
                        .missing
                        .entry(con.pkg.clone())
                        .or_default()
                        .push(MissingItem {
                            elf: con.elf.clone(),
                            name: name.clone(),
                            ver: ver.clone(),
                            candidates: cands.clone(),
                            severity,
                        });
                }
            }
        }
        report.provider_sonames = catalog.len();
        Ok(())
    })();
    let _ = fs::remove_dir_all(&scratch);
    result?;
    Ok(report)
}

fn register_provider(
    catalog: &mut BTreeMap<String, BTreeMap<String, BTreeSet<String>>>,
    soname: &str,
    versions: &BTreeMap<String, Vec<String>>,
) {
    let entry = catalog.entry(soname.to_string()).or_default();
    for (ver, syms) in versions {
        for s in syms {
            entry.entry(s.clone()).or_default().insert(ver.clone());
        }
    }
}

fn dedup_sorted_undef(mut undef: Vec<(String, String)>) -> Vec<(String, String)> {
    undef.sort();
    undef.dedup();
    undef
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cache_hit_miss_and_write_roundtrip() {
        let dir = std::env::temp_dir().join(format!("farm-abicache-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let versions: BTreeMap<String, Vec<String>> =
            BTreeMap::from([("GLIBC_2.2".into(), vec!["a".into(), "b".into()])]);
        write_cache(&dir, "libx.so.1", "sha1", &versions).unwrap();
        assert_eq!(
            load_cache(&dir, "libx.so.1", "sha1"),
            Some(versions.clone()),
            "sha 匹配应命中缓存"
        );
        assert_eq!(
            load_cache(&dir, "libx.so.1", "sha2"),
            None,
            "sha 不匹配应判 miss（触发重扫）"
        );
        assert_eq!(
            load_cache(&dir, "liby.so.1", "sha1"),
            None,
            "soname 不符不算命中"
        );
        fs::remove_dir_all(&dir).ok();
    }

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
        assert!(pr.is_dyn, "{lib:?} 应为共享库");
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

    /// 把宿主 libc 包装成一个假 .lpkg，端到端跑 run_fullchk：provider 目录 + 缓存落盘/命中。
    #[test]
    fn fullchk_extract_once_and_cache_hit() {
        let Some(lib) = host_versioned_lib() else {
            eprintln!("跳过：宿主无 libc.so.6");
            return;
        };
        let base = std::env::temp_dir().join(format!("farm-fullchk-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        // 构造假仓库 out/x86_64/mylib/1.0.lpkg，content/usr/lib 放宿主 libc 的副本
        let src = base.join("src");
        fs::create_dir_all(src.join("content/usr/lib")).unwrap();
        fs::write(
            src.join("metadata.json"),
            r#"{"name":"mylib","version":"1.0"}"#,
        )
        .unwrap();
        let real_bytes = fs::read(&lib).unwrap();
        let libname = lib.file_name().unwrap().to_string_lossy().into_owned();
        fs::write(src.join("content/usr/lib").join(&libname), &real_bytes).unwrap();
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
        let opts = FullchkOpts {
            source: base.join("repo"),
            arch: "x86_64".into(),
            cache: cache.clone(),
            pkgs: vec!["mylib".into()],
            pkgs_dir: base.join("pkgs"),
            full_rescan: false,
        };
        let sha = sha256_hex(&real_bytes);
        // 第一遍：provider 目录含 libc、缓存落盘
        let r1 = run_fullchk(&opts).unwrap();
        assert!(r1.cache_misses >= 1, "首扫应写缓存: {r1:?}");
        assert!(r1.failed.is_empty(), "不应有失败: {:?}", r1.failed);
        assert!(r1.provider_sonames >= 1, "provider 目录应含 libc: {r1:?}");
        let cf = cache.join("libc.so.6.json");
        assert!(cf.exists(), "应写 libc.so.6.json");
        let c: SonameCache = serde_json::from_str(&fs::read_to_string(&cf).unwrap()).unwrap();
        assert_eq!(c.sha256, sha, "缓存 sha 应等于真实 .so");
        assert!(!c.versions.is_empty(), "缓存应有符号版本");
        // 第二遍：缓存命中
        let r2 = run_fullchk(&opts).unwrap();
        assert!(r2.cache_hits >= 1, "第二遍应命中缓存: {r2:?}");
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
        let mk_opts = |pkgs: Vec<String>| FullchkOpts {
            source: base.join("repo"),
            arch: "x86_64".into(),
            cache: cache.clone(),
            pkgs,
            pkgs_dir: base.join("pkgs"),
            full_rescan: false,
        };
        let r = run_fullchk(&mk_opts(vec![])).unwrap();
        let foo_items = r.missing.get("foo").expect("foo 应有缺失符号");
        let got: Vec<(String, String)> = foo_items
            .iter()
            .map(|m| (m.name.clone(), m.ver.clone()))
            .collect();
        assert!(
            got.contains(&("sym1".into(), "V2".into())),
            "foo 引用 sym1@V2，libx 只提供 V1，应报缺失: {got:?}"
        );
        assert!(
            !got.contains(&("sym2".into(), "V2".into())),
            "sym2@V2 由 libx 提供，不应报缺失: {got:?}"
        );
        assert!(
            !r.missing.contains_key("libx") && !r.missing.contains_key("nover"),
            "libx(provider)/nover(无版本) 不应误报: {:?}",
            r.missing
        );
        // --pkgs foo：provider 仍来自 source 全量，缺照样报
        let r2 = run_fullchk(&mk_opts(vec!["foo".into()])).unwrap();
        assert!(
            r2.missing
                .get("foo")
                .is_some_and(|v| v.iter().any(|m| m.name == "sym1" && m.ver == "V2")),
            "只审计 foo 也应报 sym1@V2（provider 用全量 source）: {:?}",
            r2.missing
        );
        fs::remove_dir_all(&base).ok();
    }
}
