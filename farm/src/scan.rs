//! scan.rs — 原生解包 .lpkg + ELF needed_so/provides 扫描（§6，Tier-0 输入）。
//!
//! 替代 gen_deps.py 的 needed_so/provides 生成（**只这两部分**；deps 由 gen_deps/deprules
//! 规则生成，farm 不扫）。语义对齐 gen_deps.py `scan_package`：
//!
//! - `needed_so` = 包内所有 ELF 的 DT_NEEDED（去路径取 basename）− 包自身 SONAME（自提供跳过，
//!   如 firefox 捆绑 libnss3.so 不得依赖系统 nss 包）；
//! - `provides`  = 系统标准库路径（`usr/lib`、`lib`、`usr/lib64`、`lib64`）下的 SONAME
//!   + `.so` 文件名回退（老库不设 SONAME 但文件名就是 DT_NEEDED 目标）。
//!
//! 扫描与 repack 共用一次解包（§6：单包单趟，避免二次解压）。扫描只读，不落库。

use std::collections::HashSet;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

/// 扫描结果：needed_so / provides / deps（needed_so/provides 由扫描得出；deps 由 metadata.json 转述）。
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ScanResult {
    pub name: String,
    pub version: String,
    pub needed_so: Vec<String>,
    pub provides: Vec<String>,
    pub deps: Vec<String>,
}

impl ScanResult {
    pub fn new(name: &str, version: &str, needed_so: &[&str], provides: &[&str], deps: &[&str]) -> Self {
        ScanResult {
            name: name.to_string(),
            version: version.to_string(),
            needed_so: needed_so.iter().map(|s| s.to_string()).collect(),
            provides: provides.iter().map(|s| s.to_string()).collect(),
            deps: deps.iter().map(|s| s.to_string()).collect(),
        }
    }
}

/// 解包 .lpkg（zstd 压缩 PAX tar）到 `extract_dir`，然后扫描 content/。
/// `extract_dir` 由调用方给出（确定性路径，非 /tmp——NOSUID，见 §6）。
pub fn scan_lpkg(lpkg_path: &Path, extract_dir: &Path) -> Result<ScanResult, String> {
    extract_lpkg(lpkg_path, extract_dir)?;
    let meta = read_metadata_json(&extract_dir.join("metadata.json"))?;
    let name = meta["name"].as_str().unwrap_or("").to_string();
    let version = meta["version"].as_str().unwrap_or("").to_string();
    let deps = meta["deps"]
        .as_array()
        .map(|a| a.iter().filter_map(|v| v.as_str().map(String::from)).collect())
        .unwrap_or_default();
    let (needed_so, provides) = scan_content(&extract_dir.join("content"));
    Ok(ScanResult {
        name,
        version,
        needed_so,
        provides,
        deps,
    })
}

/// 解包 .lpkg（zstd 压缩 tar），保留 mode。
pub fn extract_lpkg(lpkg_path: &Path, extract_dir: &Path) -> Result<(), String> {
    if extract_dir.exists() {
        fs::remove_dir_all(extract_dir).map_err(|e| format!("清空 {extract_dir:?} 失败: {e}"))?;
    }
    fs::create_dir_all(extract_dir).map_err(|e| format!("创建 {extract_dir:?} 失败: {e}"))?;
    let f = fs::File::open(lpkg_path).map_err(|e| format!("打开 {lpkg_path:?} 失败: {e}"))?;
    let dec = zstd::stream::read::Decoder::new(f)
        .map_err(|e| format!("zstd 解压 {lpkg_path:?} 失败: {e}"))?;
    let mut ar = tar::Archive::new(dec);
    ar.set_preserve_permissions(true);
    ar.unpack(extract_dir).map_err(|e| format!("tar 解包 {lpkg_path:?} 失败: {e}"))?;
    Ok(())
}

/// 读 metadata.json（返回 serde Value 供 name/version 与后续 repack 复用）。
pub(crate) fn read_metadata_json(path: &Path) -> Result<serde_json::Value, String> {
    let content =
        fs::read_to_string(path).map_err(|e| format!("读 {path:?} 失败: {e}"))?;
    serde_json::from_str(&content).map_err(|e| format!("解析 {path:?} 失败: {e}"))
}

/// 流式读 .lpkg 的 metadata.json（不落盘 content）——seed 判断"是否已剥 needed_so"用。
/// 若 metadata.json 是 tar 首项，则只解压到它为止，开销小。
pub fn read_lpkg_metadata(lpkg_path: &Path) -> Result<serde_json::Value, String> {
    let f = fs::File::open(lpkg_path).map_err(|e| format!("打开 {lpkg_path:?} 失败: {e}"))?;
    let dec = zstd::stream::read::Decoder::new(f)
        .map_err(|e| format!("zstd 解压 {lpkg_path:?} 失败: {e}"))?;
    let mut ar = tar::Archive::new(dec);
    for entry in ar.entries().map_err(|e| format!("tar 读 {lpkg_path:?} 失败: {e}"))? {
        let mut e = entry.map_err(|e| format!("tar 项读失败: {e}"))?;
        let name = e
            .path()
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default();
        if name == "metadata.json" || name.ends_with("/metadata.json") {
            let mut s = String::new();
            use std::io::Read;
            e.read_to_string(&mut s).map_err(|e| format!("读 metadata.json 失败: {e}"))?;
            return serde_json::from_str(&s).map_err(|e| format!("解析 metadata.json 失败: {e}"));
        }
    }
    Err(format!("{lpkg_path:?} 内无 metadata.json"))
}

/// 遍历 content/，扫 ELF → (needed_so, provides)。
fn scan_content(content_dir: &Path) -> (Vec<String>, Vec<String>) {
    let mut files: Vec<PathBuf> = Vec::new();
    collect_files(content_dir, content_dir, &mut files);

    let mut all_sonames: HashSet<String> = HashSet::new();
    let mut needs: HashSet<String> = HashSet::new();
    // HashSet 去重：同一 SONAME 常被符号链接分支（文件名）和 ELF 分支（SONAME）各贡献一次，
    // 如 libmagic 的 usr/lib/libmagic.so.1 符号链接 + libmagic.so.1.0.0 的 SONAME。
    let mut provides: HashSet<String> = HashSet::new();

    for fpath in &files {
        // 符号链接：系统库路径下 `.so` 且指向 ELF → 注册文件名作提供者
        if let Ok(target) = fs::read_link(fpath) {
            let is_so = fpath
                .file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|n| n.contains(".so"));
            if is_so && in_system_lib_dir(fpath, content_dir) {
                let resolved = if target.is_absolute() {
                    target
                } else {
                    fpath.parent().unwrap_or(Path::new("")).join(target)
                };
                let resolved = resolved.canonicalize().unwrap_or(resolved);
                if is_elf(&resolved) {
                    if let Some(n) = fpath.file_name().and_then(|n| n.to_str()) {
                        provides.insert(n.to_string());
                    }
                }
            }
            continue;
        }
        if !is_elf(fpath) {
            continue;
        }
        let Ok(bytes) = fs::read(fpath) else { continue };
        let (sonames, needed) = parse_elf_dynamic(&bytes);
        for sn in &sonames {
            all_sonames.insert(sn.clone());
        }
        let in_lib = in_system_lib_dir(fpath, content_dir);
        if !sonames.is_empty() && in_lib {
            provides.extend(sonames);
        } else if in_lib {
            // 无 SONAME 回退：文件名本身是其他包的 DT_NEEDED 目标
            if let Some(n) = fpath.file_name().and_then(|n| n.to_str()) {
                if n.contains(".so") {
                    provides.insert(n.to_string());
                }
            }
        }
        for n in needed {
            needs.insert(basename(&n));
        }
    }

    // 自提供跳过：needed_so = DT_NEEDED − 包内 SONAME。输出排序保证确定性
    // （HashSet 迭代序随机，乱序写回 LankeBUILD.json 会造成无谓 diff）。
    let mut needed_so: Vec<String> = needs.difference(&all_sonames).cloned().collect();
    needed_so.sort();
    let mut provides: Vec<String> = provides.into_iter().collect();
    provides.sort();
    (needed_so, provides)
}

/// 递归收集文件路径（含符号链接）。
fn collect_files(dir: &Path, content_dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    for entry in rd.flatten() {
        let p = entry.path();
        let Ok(ft) = entry.file_type() else { continue };
        if ft.is_dir() {
            collect_files(&p, content_dir, out);
        } else {
            // 排除 metadata.json（在 content 外，不会走到；保险）
            out.push(p);
        }
    }
    let _ = content_dir;
}

fn basename(s: &str) -> String {
    s.rsplit('/').next().unwrap_or(s).to_string()
}

fn is_elf(path: &Path) -> bool {
    let Ok(f) = fs::File::open(path) else { return false };
    let mut magic = [0u8; 4];
    let Ok(n) = (&f).take(4).read(&mut magic) else { return false };
    n == 4 && magic == [0x7f, b'E', b'L', b'F']
}

/// 是否系统标准库路径（gen_deps `_in_system_lib_dir`）：
/// 直接子级为 usr/lib、lib、usr/lib64、lib64（排除 usr/lib/chromium/ 等捆绑路径）。
fn in_system_lib_dir(fpath: &Path, content_dir: &Path) -> bool {
    let Ok(rel) = fpath.strip_prefix(content_dir) else {
        return false;
    };
    let Some(parent) = rel.parent() else { return false };
    matches!(
        parent.to_string_lossy().as_ref(),
        "usr/lib" | "lib" | "usr/lib64" | "lib64"
    )
}

/// 解析 ELF .dynamic：返回 (sonames, needed)。解析失败按空处理（对齐 gen_deps 的 try/except）。
fn parse_elf_dynamic(bytes: &[u8]) -> (Vec<String>, Vec<String>) {
    use goblin::elf::dynamic::{DT_NEEDED, DT_SONAME};
    let Ok(elf) = goblin::elf::Elf::parse(bytes) else {
        return (Vec::new(), Vec::new());
    };
    let mut sonames = Vec::new();
    let mut needed = Vec::new();
    if let Some(dynsec) = &elf.dynamic {
        for d in &dynsec.dyns {
            match d.d_tag {
                DT_NEEDED => {
                    if let Some(s) = elf.dynstrtab.get_at(d.d_val as usize) {
                        needed.push(s.to_string());
                    }
                }
                DT_SONAME => {
                    if let Some(s) = elf.dynstrtab.get_at(d.d_val as usize) {
                        sonames.push(s.to_string());
                    }
                }
                _ => {}
            }
        }
    }
    (sonames, needed)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn basename_strips_path() {
        assert_eq!(basename("libc.so.6"), "libc.so.6");
        assert_eq!(basename("/usr/lib/libm.so.6"), "libm.so.6");
    }

    #[test]
    fn in_system_lib_dir_matches_standard_paths() {
        let c = Path::new("/x/content");
        assert!(in_system_lib_dir(Path::new("/x/content/usr/lib/libc.so.6"), c));
        assert!(in_system_lib_dir(Path::new("/x/content/lib/libz.so.1"), c));
        assert!(in_system_lib_dir(Path::new("/x/content/usr/lib64/libm.so"), c));
        // 捆绑路径排除
        assert!(!in_system_lib_dir(Path::new("/x/content/usr/lib/chromium/libnss3.so"), c));
        assert!(!in_system_lib_dir(Path::new("/x/content/usr/bin/lpkg"), c));
    }

    #[test]
    fn is_elf_magic_check() {
        let f = std::env::temp_dir().join("farm-scan-elf-test");
        std::fs::write(&f, [0x7f, b'E', b'L', b'F', 2, 1, 1]).unwrap();
        assert!(is_elf(&f));
        std::fs::write(&f, b"#!/bin/sh\n").unwrap();
        assert!(!is_elf(&f));
        std::fs::remove_file(&f).ok();
    }

    /// 复现 libmagic 重复 provides：同一 SONAME 被符号链接分支（文件名）和 ELF 分支（SONAME）
    /// 各贡献一次。构造 content/usr/lib/libc.so.6（真实 ELF）+ lib/libc.so.6 与
    /// usr/lib/libc.so 两个符号链接 → 无去重时 libc.so.6 出现两次。
    #[test]
    fn scan_content_dedups_provides_from_symlink_and_soname() {
        let host_lib = [
            "/usr/lib/libc.so.6",
            "/lib/x86_64-linux-gnu/libc.so.6",
            "/usr/lib/x86_64-linux-gnu/libc.so.6",
            "/lib/libc.so.6",
        ]
        .iter()
        .find_map(|p| std::fs::canonicalize(p).ok());
        let Some(src) = host_lib else {
            eprintln!("{}", crate::tr!("test.skip_host_libc"));
            return;
        };
        let tmp = std::env::temp_dir().join(format!("farm-scan-dedup-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&tmp);
        let content = tmp.join("content");
        std::fs::create_dir_all(content.join("usr/lib")).unwrap();
        std::fs::create_dir_all(content.join("lib")).unwrap();
        std::fs::copy(&src, content.join("usr/lib/libc.so.6")).unwrap();
        // 符号链接 basename=libc.so.6，与真实 ELF 的 SONAME（或回退文件名）相同 → 撞车
        std::os::unix::fs::symlink("../usr/lib/libc.so.6", content.join("lib/libc.so.6")).unwrap();
        std::os::unix::fs::symlink("libc.so.6", content.join("usr/lib/libc.so")).unwrap();

        let (_, provides) = scan_content(&content);
        assert_eq!(provides, vec!["libc.so", "libc.so.6"]); // libc.so.6 只出现一次

        let _ = std::fs::remove_dir_all(&tmp);
    }
}
