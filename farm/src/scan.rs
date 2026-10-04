//! scan.rs — 原生解包 .lpkg + ELF needed_so/provides_soname 扫描（§6，Tier-0 输入）。
//!
//! 替代 gen_deps.py 的 needed_so/provides_soname 生成（**只这两部分**；deps 由 gen_deps/deprules
//! 规则生成，farm 不扫）。语义对齐 gen_deps.py `scan_package`：
//!
//! - `needed_so` = 包内所有 ELF 的 DT_NEEDED（去路径取 basename）− 包自身 SONAME（自提供跳过，
//!   如 firefox 捆绑 libnss3.so 不得依赖系统 nss 包）；
//! - `provides_soname` = 系统标准库路径（`usr/lib`、`lib`、`usr/lib64`、`lib64`）下的 SONAME + `.so` 文件名回退（老库不设 SONAME 但文件名就是 DT_NEEDED 目标）；纯虚拟 provider 不在此列（它由人手写在 LankeBUILD.json，扫描不产出）。
//!
//! 扫描与 repack 共用一次解包（§6：单包单趟，避免二次解压）。扫描只读，不落库。

use crate::error::FarmError;
use std::collections::HashSet;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};

/// 扫描结果类型 = `verify::ScanResult` 的**单一来源**（曾在本模块另有一份同名异构定义，字段与
/// verify 版不同 → 漂移风险。现由 verify 拥有、scan 复用；scan 依赖 verify 是上层依赖下层，
/// 分层不变）。`scan_lpkg` 额外用 `with_name` 填 name/version 溯源信息。
pub use crate::verify::ScanResult;

/// 解包 .lpkg（zstd 压缩 PAX tar）到 `extract_dir`，然后扫描 content/。
/// `extract_dir` 由调用方给出（确定性路径，非 /tmp——NOSUID，见 §6）。
/// `repo_provides` = 仓库全部 **SONAME**（`provides_soname`）：needed_so 条目不在其中 → 无 provider
/// → 判 not found → 不进 needed_so（如 perl 不提供 libperl.so，postgresql 的 plperl.so 链接它
/// 但标准搜索无提供者，运行期靠 RPATH → 扫描无完整系统状态，不猜，直接 not-found 忽略）。
pub fn scan_lpkg(
    lpkg_path: &Path,
    extract_dir: &Path,
    repo_provides: &HashSet<String>,
) -> Result<ScanResult, FarmError> {
    extract_lpkg(lpkg_path, extract_dir)?;
    let meta = read_metadata_json(&extract_dir.join("metadata.json"))?;
    let name = meta["name"].as_str().unwrap_or("").to_string();
    let version = meta["version"].as_str().unwrap_or("").to_string();
    let deps = meta["deps"]
        .as_array()
        .map(|a| {
            a.iter()
                .filter_map(|v| v.as_str().map(String::from))
                .collect()
        })
        .unwrap_or_default();
    let (needed_so, provides_soname) = scan_content(&extract_dir.join("content"), repo_provides);
    Ok(ScanResult::from_parts(needed_so, provides_soname, deps).with_name(name, version))
}

/// 进程是否以 root 运行（farm 解包/重打包需读写 root 属主文件与 SUID，操作命令强制 root）。
pub fn running_as_root() -> bool {
    // SAFETY: geteuid 无失败路径。
    let euid = unsafe { libc::geteuid() };
    euid == 0
}

/// 删除目录树。解包/重打包以 root 运行后，`fs::remove_dir_all` 能删 root 属主树（含 content/etc、
/// content/var 等只读 root 目录——export 的 `.export-extract` 曾因删不动残留 14G）。
/// 曾用 `sudo -n rm -rf` 兜底（已去 sudo，见 ARCH §「root 运行」）。
pub(crate) fn remove_dir_tree(path: &Path) -> Result<(), FarmError> {
    fs::remove_dir_all(path).map_err(|e| format!("删除目录树 {path:?} 失败: {e}").into())
}

/// 解包 .lpkg（zstd 压缩 tar），保留 mode/uid/gid。纯 Rust（tar + zstd crate，无 sudo/tar/zstd CLI）。
///
/// 语义对齐旧 `sudo tar --numeric-owner -xf`：mode 完整（含 SUID）、按 header 数字 uid/gid chown、
/// 保留 mtime。以 root 运行时保留所有权（`/etc/shadow` 0600、SUID 等需要特权）；非 root（单测跑
/// 用户属主 fixture）只保留 mode、不 chown。
pub fn extract_lpkg(lpkg_path: &Path, extract_dir: &Path) -> Result<(), FarmError> {
    if extract_dir.exists() {
        remove_dir_tree(extract_dir)?;
    }
    fs::create_dir_all(extract_dir).map_err(|e| format!("创建 {extract_dir:?} 失败: {e}"))?;
    let f = fs::File::open(lpkg_path).map_err(|e| format!("打开 {lpkg_path:?} 失败: {e}"))?;
    // 单帧 zstd 解压（farm/lpkg 打包都是单帧；read_lpkg_metadata 同假设）。
    let dec = zstd::stream::read::Decoder::new(f)
        .map_err(|e| format!("zstd 解压 {lpkg_path:?} 失败: {e}"))?;
    let mut ar = tar::Archive::new(dec);
    ar.set_preserve_permissions(true);
    ar.set_preserve_ownerships(running_as_root());
    ar.set_overwrite(true);
    // xattr：tar 默认**不还原**（`unpack_xattrs: false`），必须显式打开——否则
    // `security.capability`（文件能力，如 systemd native 二进制、ping）在解包/重打这条路上直接丢。
    // tar 从 PAX 的 `SCHILY.xattr.<name>` 记录还原（`xattr::set`，**跟随符号链接**；符号链接自身的
    // xattr 极少见，此处不特殊处理）。设 `security.*` 需要特权——真实路径都要求 root（CLI 的
    // `ensure_root` 强制），非 root 单测的 fixture 不带 xattr，故不会误伤。
    ar.set_unpack_xattrs(true);
    ar.unpack(extract_dir)
        .map_err(|e| format!("tar 解包 {lpkg_path:?} 失败: {e}").into())
}

/// 读 metadata.json（返回 serde Value 供 name/version 与后续 repack 复用）。
pub(crate) fn read_metadata_json(path: &Path) -> Result<serde_json::Value, FarmError> {
    let content = fs::read_to_string(path).map_err(|e| format!("读 {path:?} 失败: {e}"))?;
    serde_json::from_str(&content).map_err(|e| format!("解析 {path:?} 失败: {e}").into())
}

/// 流式读 .lpkg 的 metadata.json（不落盘 content）——seed 判断"是否已剥 needed_so"用。
/// 若 metadata.json 是 tar 首项，则只解压到它为止，开销小。
pub fn read_lpkg_metadata(lpkg_path: &Path) -> Result<serde_json::Value, FarmError> {
    let f = fs::File::open(lpkg_path).map_err(|e| format!("打开 {lpkg_path:?} 失败: {e}"))?;
    let dec = zstd::stream::read::Decoder::new(f)
        .map_err(|e| format!("zstd 解压 {lpkg_path:?} 失败: {e}"))?;
    let mut ar = tar::Archive::new(dec);
    for entry in ar
        .entries()
        .map_err(|e| format!("tar 读 {lpkg_path:?} 失败: {e}"))?
    {
        let mut e = entry.map_err(|e| format!("tar 项读失败: {e}"))?;
        let name = e
            .path()
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default();
        if name == "metadata.json" || name.ends_with("/metadata.json") {
            let mut s = String::new();
            use std::io::Read;
            e.read_to_string(&mut s)
                .map_err(|e| format!("读 metadata.json 失败: {e}"))?;
            return serde_json::from_str(&s)
                .map_err(|e| format!("解析 metadata.json 失败: {e}").into());
        }
    }
    Err(format!("{lpkg_path:?} 内无 metadata.json").into())
}

/// 遍历 content/，扫 ELF → (needed_so, provides_soname)。
fn scan_content(content_dir: &Path, repo_provides: &HashSet<String>) -> (Vec<String>, Vec<String>) {
    let mut files: Vec<PathBuf> = Vec::new();
    collect_files(content_dir, &mut files);

    let mut all_sonames: HashSet<String> = HashSet::new();
    let mut needs: HashSet<String> = HashSet::new();
    // 包内所有 .so* 文件 basename（任何路径）——not-found 判定用：包内同名 .so → 二进制经
    // RPATH 用自带库（标准搜索视为 not found）→ 该 NEEDED 不进 needed_so。
    let mut all_so_basenames: HashSet<String> = HashSet::new();
    // HashSet 去重：同一 SONAME 常被符号链接分支（文件名）和 ELF 分支（SONAME）各贡献一次，
    // 如 libmagic 的 usr/lib/libmagic.so.1 符号链接 + libmagic.so.1.0.0 的 SONAME。
    let mut provides_soname: HashSet<String> = HashSet::new();

    for fpath in &files {
        if let Some(n) = fpath.file_name().and_then(|n| n.to_str()) {
            if n.contains(".so") {
                all_so_basenames.insert(n.to_string());
            }
        }
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
                        provides_soname.insert(n.to_string());
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
            provides_soname.extend(sonames);
        } else if in_lib {
            // 无 SONAME 回退：文件名本身是其他包的 DT_NEEDED 目标
            if let Some(n) = fpath.file_name().and_then(|n| n.to_str()) {
                if n.contains(".so") {
                    provides_soname.insert(n.to_string());
                }
            }
        }
        for n in needed {
            needs.insert(basename(&n));
        }
    }

    // 不进 needed_so 的三类（语义不同，效果都是忽略）：
    //  1) SONAME 自提供（all_sonames）→ 包自身提供，自引用 → 忽略；
    //  2) 包内同名 .so 文件（all_so_basenames，任何路径）→ 二进制经 RPATH 用自带库，
    //     标准搜索视为 not found → 忽略（perl 的 CORE/libperl.so 即此例）；
    //  3) 仓库无 provider（repo_provides 不含该 SONAME）→ not found → 忽略
    //     （postgresql 的 plperl.so 链接 libperl.so，perl 不提供 → 无 provider → 不进 needed）。
    // 不做 RPATH/RUNPATH 解析——扫描器没有完整运行时系统状态，RPATH 指向他包库会误判，本质无解。
    let mut needed_so: Vec<String> = needs
        .difference(&all_sonames)
        .filter(|&s| !all_so_basenames.contains(s))
        .filter(|&s| repo_provides.contains(s))
        .cloned()
        .collect();
    needed_so.sort();
    let mut provides_soname: Vec<String> = provides_soname.into_iter().collect();
    provides_soname.sort();
    (needed_so, provides_soname)
}

/// 递归收集叶子成员（含符号链接）到 `out`；只对**目录**递归。
fn collect_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    for entry in rd.flatten() {
        let p = entry.path();
        let Ok(ft) = entry.file_type() else { continue };
        if ft.is_dir() {
            collect_files(&p, out);
        } else {
            out.push(p);
        }
    }
}

fn basename(s: &str) -> String {
    s.rsplit('/').next().unwrap_or(s).to_string()
}

fn is_elf(path: &Path) -> bool {
    let Ok(f) = fs::File::open(path) else {
        return false;
    };
    let mut magic = [0u8; 4];
    let Ok(n) = (&f).take(4).read(&mut magic) else {
        return false;
    };
    n == 4 && magic == [0x7f, b'E', b'L', b'F']
}

/// 是否系统标准库路径（gen_deps `_in_system_lib_dir`）：
/// 直接子级为 usr/lib、lib、usr/lib64、lib64（排除 usr/lib/chromium/ 等捆绑路径）。
fn in_system_lib_dir(fpath: &Path, content_dir: &Path) -> bool {
    let Ok(rel) = fpath.strip_prefix(content_dir) else {
        return false;
    };
    under_lib_dir(rel)
}

/// 系统库目录（**唯一清单**）：`.lpkg` 内容里只有这些目录下的文件参与 SONAME/NEEDED 扫描与 ABI 备份。
///
/// 加一种库目录**只改这里**——历史上这份清单散在 4 处（`scan` 里 1 处、`build/repo.rs` 里 3 处），
/// 两处带尾 `/` 两处不带，加目录时极易漏（漏了表现为：新库目录里的 .so 不进 ABI 反图）。
pub(crate) const LIB_DIRS: &[&str] = &["usr/lib", "lib", "usr/lib64", "lib64"];

/// 相对路径是否位于某个系统库目录**之下**（`usr/lib/foo.so` → true；`usr/lib` 自身 → false）。
pub(crate) fn under_lib_dir(rel: &Path) -> bool {
    rel.parent()
        .is_some_and(|p| LIB_DIRS.contains(&p.to_string_lossy().as_ref()))
}

/// 剥掉开头的系统库目录前缀（`usr/lib/foo.so` → `foo.so`）；没命中返回 `None`。
/// 备份符号链接复刻相对路径时用（相对备份树根 `/usr/lib`）。**带尾 `/` 故与列表顺序无关**。
pub(crate) fn strip_lib_dir(abs: &Path) -> Option<PathBuf> {
    let s = abs.to_string_lossy();
    LIB_DIRS
        .iter()
        .find_map(|d| s.strip_prefix(&format!("{d}/")).map(PathBuf::from))
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
mod tests;
