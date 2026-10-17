//! libarchive_ffi.rs — farm 对**系统 libarchive** 的手写绑定：`.lpkg` 的读与写。
//!
//! # 为什么是手写 FFI
//!
//! `.lpkg` 的写方是 lpkg（libarchive 的 `pax_restricted` 打包器）。farm 要读它写的东西，最稳的做法
//! 是**链同一份实现**：`#[link(name = "archive")]` → 系统的 `libarchive.so.13`，**与 lpkg 同库同版本**
//! （发行版升级 libarchive，两边一起走）。不用 binding crate 的原因（试过 `libarchive2` 0.2.1 并记账）：
//! 它自带一份**静态 libarchive 3.8.1**（与系统的 3.8.9 分叉 = 同一事故的弱化版），构建期要
//! cmake + bindgen(libclang) + 5 个开发库（把 C 工具链塞进**发行版自举工具**的构建路径），
//! 而且有几条能力它表达不了：非 UTF-8 成员名（`archive_entry_pathname_utf8` 会转换）、
//! 目录的 default ACL、`archive_entry_clone()`（于是字段要手抄一份、crate 加字段就会静默漏搬）。
//!
//! # 这里就是一堆兼容垃圾 —— 我们要自己守的 C API 契约
//!
//! 每条都对应下面一段绕法或一次显式检查。**改这个文件前先读这里**：
//!
//! 1. **指针有效期**：`archive_entry_pathname` / `archive_entry_hardlink` / `archive_entry_symlink`
//!    返回的指针只活到"下一条目被读入"为止；`archive_read_data_block` 的 `buff` 只活到下次调用。
//!    要留就当场拷成 `String`/`Vec`（下面 `cstr_to_string`）。
//! 2. **`archive_write_finish_entry` 的返回值不能丢**：它可能因磁盘满/EIO 失败 —— 吞掉就是
//!    "解压报成功、盘上是截断内容"（lpkg `archive.cpp` 特意为此改过）。
//! 3. **`archive_write_close` 的返回值 = 整包是否真写完**（lpkg `packer.cpp` 同款判据）：
//!    `< ARCHIVE_WARN` 必须失败（否则截断的 `.lpkg` 会被当成功、还算出哈希）。
//! 4. **释放顺序**：句柄一律 RAII（`Reader`/`Disk`/`Writer` 的 `Drop` = close + free）；
//!    条目 `archive_entry_new()` 后必须 `archive_entry_free()`（`Entry` 守卫）。
//! 5. **不调 `archive_write_disk_set_standard_lookup()`**：静态链接/chroot 下的 NSS 会让它段错误
//!    （lpkg 已移除）。代价是属主按 header 数字走 —— 正是我们要的 numeric-owner 语义。
//! 6. **不加 `ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS`**：我们是先把成员名重写成"解压根 + 相对名"
//!    的绝对路径再交给写盘端（同 lpkg），加了它每个成员都会被拒。防逃逸靠下面的 `member_path`。
//! 7. **符号链接要 `set_perm(0)`**：Linux 没有 lchmod，libarchive 会对链接本身 chmod → 跟随链接
//!    改掉**目标**的权限（包发 `<root>/usr/bin/sudo -> /usr/bin/sudo.real` 就把真的 sudo 变 777）。
//! 8. **数据要块式搬**（`archive_read_data_block` → `archive_write_data_block`）：稀疏成员的洞按
//!    offset 落位，既不整份进内存也不把洞摊平（lpkg 同款）。
//! 9. **磁盘读取器填条目之前，条目上得先有磁盘路径**（`archive_read_disk_entry_from_file` 按它读
//!    元数据），填完再改回归档内名字 —— 顺序反了报 "Couldn't determine path"。
//! 10. **`entry_from_file` 带 xattr 与真正的 ACL**（`SCHILY.xattr.*` / `SCHILY.acl.*`）。手工把 ACL
//!     当裸 xattr 写（`SCHILY.xattr.system.posix_acl_access`）**不生效**（lpkg `packer.cpp` 记录过）。
//!
//! # 范围与验收
//!
//! **全 farm 只有这个文件声明 `extern "C"`**（`tests.rs` 的 `only_compat_touches_libarchive2` 守着），
//! 对外只有三个函数：`extract` / `read_member` / `write_tree`。
//!
//! - `tests.rs`：fixture 网 —— 稀疏 1.0 真名还原、越界拒绝、硬链接、ACL/xattr 往返、SUID、`./` 根成员、
//!   坏符号链接、可复现（同树两次打包逐字节一致）。换实现（升级 crate / 换回 tar 系）时这套就是验收。
//! - `tests.rs`：**变异输入** —— 畸形头、重名成员、控制字符、超长名、截断、随机位翻转，只许
//!   `Ok`/`Err`，不许 panic、不许逃出解压根。
//! - `fuzz/`：`cargo-fuzz`（libFuzzer）harness，需要 nightly，按需跑：
//!   `cargo +nightly fuzz run extract_lpkg`。Rust 侧没有 stable 的覆盖率制导 fuzz（`cargo-fuzz`/`afl.rs`
//!   都要 nightly），所以**日常那道网是上面的变异测试**，fuzz 是想深挖时的手段。

use crate::error::FarmError;
use std::collections::HashSet;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::fs;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::FileTypeExt;
use std::path::{Component, Path, PathBuf};
use std::ptr;

// ─────────────────────────── 常量（对照 /usr/include/archive.h、archive_entry.h）───────────────────────────

/// 返回码：低于 `ARCHIVE_WARN`（= FAILED/FATAL）才是失败，`ARCHIVE_WARN` 只告警 —— lpkg 两侧同一阈值。
const ARCHIVE_EOF: c_int = 1;
const ARCHIVE_OK: c_int = 0;
const ARCHIVE_WARN: c_int = -20;

/// `archive_write_disk` 的解包选项。与 lpkg `archive.cpp` 的同一条（含**不加**哪个的理由见文件头 5/6）。
const EXTRACT_OWNER: c_int = 0x0001;
const EXTRACT_PERM: c_int = 0x0002;
const EXTRACT_TIME: c_int = 0x0004;
const EXTRACT_UNLINK: c_int = 0x0010;
const EXTRACT_ACL: c_int = 0x0020;
const EXTRACT_FFLAGS: c_int = 0x0040;
const EXTRACT_XATTR: c_int = 0x0080;
const EXTRACT_SECURE_SYMLINKS: c_int = 0x0100;
const EXTRACT_SECURE_NODOTDOT: c_int = 0x0200;

/// 文件类型位（`archive_entry_filetype` 返回 `mode_t`）。
const AE_IFIFO: u32 = 0o010000;
const AE_IFCHR: u32 = 0o020000;
const AE_IFBLK: u32 = 0o060000;
const AE_IFLNK: u32 = 0o120000;
const AE_IFSOCK: u32 = 0o140000;

/// 成员名总长上限（字节）。取 PATH_MAX 量级：超长路径是 libarchive 已知会绕过自己符号链接检查的
/// 那一格（issue #744/#745），必须自己封。属**结构性**判据，不是资源上界（别据此加成员数/解压比限额）。
const MAX_MEMBER_NAME_BYTES: usize = 4096;
/// 单个路径分量上限 = POSIX NAME_MAX：超过它任何文件系统都建不出来。
const MAX_COMPONENT_BYTES: usize = 255;

#[repr(C)]
struct archive {
    _private: [u8; 0],
}
#[repr(C)]
struct archive_entry {
    _private: [u8; 0],
}

#[link(name = "archive")]
extern "C" {
    // ── 读归档 ──
    fn archive_read_new() -> *mut archive;
    fn archive_read_support_filter_all(a: *mut archive) -> c_int;
    fn archive_read_support_format_all(a: *mut archive) -> c_int;
    fn archive_read_open_filename(a: *mut archive, path: *const c_char, block_size: usize)
        -> c_int;
    fn archive_read_next_header(a: *mut archive, entry: *mut *mut archive_entry) -> c_int;
    fn archive_read_data(a: *mut archive, buf: *mut c_void, len: usize) -> isize;
    fn archive_read_data_block(
        a: *mut archive,
        buf: *mut *const c_void,
        size: *mut usize,
        offset: *mut i64,
    ) -> c_int;
    fn archive_read_data_skip(a: *mut archive) -> c_int;
    fn archive_read_close(a: *mut archive) -> c_int;
    fn archive_read_free(a: *mut archive) -> c_int;
    fn archive_error_string(a: *mut archive) -> *const c_char;

    // ── 写盘（解包）──
    fn archive_write_disk_new() -> *mut archive;
    fn archive_write_disk_set_options(a: *mut archive, flags: c_int) -> c_int;
    fn archive_write_header(a: *mut archive, entry: *mut archive_entry) -> c_int;
    fn archive_write_data_block(
        a: *mut archive,
        buf: *const c_void,
        size: usize,
        offset: i64,
    ) -> isize;
    fn archive_write_finish_entry(a: *mut archive) -> c_int;
    fn archive_write_close(a: *mut archive) -> c_int;
    fn archive_write_free(a: *mut archive) -> c_int;

    // ── 打包 ──
    fn archive_write_new() -> *mut archive;
    fn archive_write_add_filter_zstd(a: *mut archive) -> c_int;
    fn archive_write_set_filter_option(
        a: *mut archive,
        module: *const c_char,
        option: *const c_char,
        value: *const c_char,
    ) -> c_int;
    fn archive_write_set_format_pax_restricted(a: *mut archive) -> c_int;
    fn archive_write_open_filename(a: *mut archive, path: *const c_char) -> c_int;
    fn archive_write_data(a: *mut archive, buf: *const c_void, size: usize) -> isize;

    // ── 磁盘走查（打包时按磁盘填条目）──
    fn archive_read_disk_new() -> *mut archive;
    fn archive_read_disk_set_symlink_physical(a: *mut archive) -> c_int;
    fn archive_read_disk_entry_from_file(
        a: *mut archive,
        entry: *mut archive_entry,
        fd: c_int,
        st: *const libc::stat,
    ) -> c_int;

    // ── 条目 ──
    fn archive_entry_new() -> *mut archive_entry;
    fn archive_entry_free(entry: *mut archive_entry);
    fn archive_entry_pathname(entry: *mut archive_entry) -> *const c_char;
    fn archive_entry_set_pathname(entry: *mut archive_entry, path: *const c_char);
    fn archive_entry_filetype(entry: *mut archive_entry) -> u32;
    fn archive_entry_symlink(entry: *mut archive_entry) -> *const c_char;
    fn archive_entry_set_symlink(entry: *mut archive_entry, target: *const c_char);
    fn archive_entry_hardlink(entry: *mut archive_entry) -> *const c_char;
    fn archive_entry_set_hardlink(entry: *mut archive_entry, target: *const c_char);
    fn archive_entry_set_perm(entry: *mut archive_entry, perm: u32);
    fn archive_entry_set_size(entry: *mut archive_entry, size: i64);
    fn archive_entry_set_uid(entry: *mut archive_entry, uid: i64);
    fn archive_entry_set_gid(entry: *mut archive_entry, gid: i64);
    fn archive_entry_set_mtime(entry: *mut archive_entry, sec: i64, nsec: i64);
    fn archive_entry_unset_atime(entry: *mut archive_entry);
    fn archive_entry_unset_ctime(entry: *mut archive_entry);
    fn archive_entry_unset_birthtime(entry: *mut archive_entry);
}

// ─────────────────────────── RAII 守卫 ───────────────────────────

/// 读端句柄（`Drop` = close + free）。
struct Reader(*mut archive);
/// 写盘端句柄（`Drop` = close + free）。
struct Disk(*mut archive);
/// 打包端句柄（`Drop` = close + free）。
struct Writer(*mut archive);
/// 条目句柄（`Drop` = free）。C 侧要显式 `archive_entry_free`，Rust 的 `Drop` 只负责别漏。
struct Entry(*mut archive_entry);

impl Drop for Reader {
    fn drop(&mut self) {
        // SAFETY: 句柄由 archive_read_new/archive_read_disk_new 产出，只在此处释放一次。
        if !self.0.is_null() {
            unsafe {
                archive_read_close(self.0);
                archive_read_free(self.0);
            }
        }
    }
}

impl Drop for Disk {
    fn drop(&mut self) {
        // SAFETY: 同上，写盘端。
        if !self.0.is_null() {
            unsafe {
                archive_write_close(self.0);
                archive_write_free(self.0);
            }
        }
    }
}

impl Drop for Writer {
    fn drop(&mut self) {
        // SAFETY: 同上，打包端（正常情况下已显式 close 并置空）。
        if !self.0.is_null() {
            unsafe {
                archive_write_close(self.0);
                archive_write_free(self.0);
            }
        }
    }
}

impl Drop for Entry {
    fn drop(&mut self) {
        // SAFETY: 由 archive_entry_new 产出、只在这里释放。
        if !self.0.is_null() {
            unsafe { archive_entry_free(self.0) };
        }
    }
}

/// libarchive 的错误串（可能为 NULL —— 直接喂给 CStr 会 UB，故统一兜底）。
fn err_str(a: *mut archive) -> String {
    // SAFETY: 任意非空句柄可调；返回内部静态串或 NULL。
    let p = unsafe { archive_error_string(a) };
    if p.is_null() {
        return "（libarchive 未给出原因）".to_string();
    }
    // SAFETY: 非空即 NUL 结尾的 C 串。
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

/// 判决：`< ARCHIVE_WARN` 报错（FAILED/FATAL），`ARCHIVE_WARN` 告警后继续 —— lpkg 两侧同一判据。
fn judge(status: c_int, a: *mut archive, what: &str, lpkg: &Path) -> Result<(), FarmError> {
    if status < ARCHIVE_WARN {
        return Err(format!("libarchive {what} 失败（{lpkg:?}）: {}", err_str(a)).into());
    }
    if status < ARCHIVE_OK {
        eprintln!(
            "warning: libarchive {what} 有警告（{lpkg:?}）: {}",
            err_str(a)
        );
    }
    Ok(())
}

/// C 串 → `String`（成员名/链接目标是**字节**，这里按 UTF-8 无损取；非法则报错而不是静默改名）。
fn cstr_to_string(p: *const c_char, what: &str, lpkg: &Path) -> Result<String, FarmError> {
    if p.is_null() {
        return Ok(String::new());
    }
    // SAFETY: 非空即 NUL 结尾的 C 串。
    let bytes = unsafe { CStr::from_ptr(p) }.to_bytes();
    match std::str::from_utf8(bytes) {
        Ok(s) => Ok(s.to_string()),
        Err(_) => Err(format!("{lpkg:?} 的{what}不是合法 UTF-8，拒绝静默改名").into()),
    }
}

fn cstring(path: &Path) -> Result<CString, FarmError> {
    CString::new(path.as_os_str().as_bytes())
        .map_err(|_| FarmError::from(format!("路径含 NUL 字节: {path:?}")))
}

// ─────────────────────────── 成员名判据（读写两侧共用）───────────────────────────

/// 词法归一化：消掉 `.`、用栈消 `..`（**不碰文件系统** —— 判的是"这个名字打算去哪儿"）。
/// 上溯出根的 `..` 作为前导分量保留，调用方据此判越界。手写而不是用 `Path::lexically_normal`
/// （后者至今是 nightly-only）。
fn lexical_components(path: &Path) -> Vec<std::ffi::OsString> {
    let mut out: Vec<std::ffi::OsString> = Vec::new();
    for comp in path.components() {
        match comp {
            Component::CurDir => {}
            Component::ParentDir => {
                if matches!(out.last(), Some(last) if last != "..") {
                    out.pop();
                } else {
                    out.push("..".into());
                }
            }
            Component::Normal(part) => out.push(part.to_os_string()),
            // 绝对路径 / 盘符由调用方各自判（符号链接允许绝对目标，硬链接不允许）。
            Component::RootDir | Component::Prefix(_) => {}
        }
    }
    out
}

/// 相对路径归一化后是否**逃出根**（首实义分量是 `..`）。判据与 lpkg `tar_guard.cpp` 同源：
/// `usr/bin/../../etc/x` 归一化成 `etc/x` 仍在根内（合法），只有前导 `..` 才算穿出去。
fn escapes_root_lexically(relative: &Path) -> bool {
    matches!(lexical_components(relative).first(), Some(first) if first == "..")
}

/// 成员名 → 归档内**相对路径**（写侧用），或解压根内路径（读侧把根拼上）。
///
/// - 只收 `Normal`，`./` 前缀丢弃（farm 自己的 fixture 就是 `append_dir_all(".")` 打出来的）。
/// - 绝对路径 / `..` / 盘符前缀 → 报错。**比 lpkg 严**：lpkg 放行"归一化后仍在根内"的 `..`
///   （它还得服务上游源码 tarball）；farm 的输入域只有自家/lpkg 产出的 `.lpkg`，严一点不误伤。
/// - 返回空路径 = 该成员指的就是归档根（名字是 `.`/`./`），调用方跳过。
/// - 长度/分量上限与 lpkg `tar_guard.cpp` 一致（超长路径会绕过 libarchive 自己的符号链接检查）。
fn member_path(raw: &str, lpkg: &Path) -> Result<PathBuf, FarmError> {
    if raw.len() > MAX_MEMBER_NAME_BYTES {
        return Err(format!(
            "{lpkg:?} 成员名过长（{} 字节 > {MAX_MEMBER_NAME_BYTES}）",
            raw.len()
        )
        .into());
    }
    let mut run = 0usize;
    for b in raw.bytes() {
        if b == b'/' {
            run = 0;
            continue;
        }
        run += 1;
        if run > MAX_COMPONENT_BYTES {
            return Err(
                format!("{lpkg:?} 成员名的路径分量过长（{run} > {MAX_COMPONENT_BYTES}）").into(),
            );
        }
    }
    let mut rel = PathBuf::new();
    for comp in Path::new(raw).components() {
        match comp {
            Component::Normal(part) => rel.push(part),
            Component::CurDir => {}
            Component::RootDir | Component::ParentDir | Component::Prefix(_) => {
                return Err(format!("{lpkg:?} 成员路径越界（绝对路径或含 ..）: {raw:?}").into());
            }
        }
    }
    Ok(rel)
}

/// 畸形/敌意 tar 的**跨成员**守卫（一个归档一个实例）。判据照 lpkg `tar_guard.cpp`，含它**故意不做**的几条。
struct MemberGuard {
    seen: HashSet<String>,
}

impl MemberGuard {
    fn new() -> Self {
        Self {
            seen: HashSet::new(),
        }
    }

    /// `rel` 是 `member_path` 归一化后的相对路径。
    fn check(
        &mut self,
        entry: *mut archive_entry,
        raw: &str,
        rel: &Path,
        lpkg: &Path,
    ) -> Result<(), FarmError> {
        // ① 控制字符（含 ESC）：ESC 能把名字变成终端 ANSI 指令，`\n` 会把异常消息切成两行。
        //    反斜杠**不拒** —— systemd 的 `system-systemd\x2dmute\x2dconsole.slice` 字面就带它，
        //    且是 base 包（lpkg tar_guard.cpp 记过这个教训）。
        for b in raw.bytes() {
            if b < 0x20 || b == 0x7f {
                return Err(format!("{lpkg:?} 成员名含控制字符: {raw:?}").into());
            }
        }
        // ② 逃出根（`..` 或绝对路径，已在 member_path 里判过，这里判归一化后的语义）。
        if escapes_root_lexically(rel) {
            return Err(format!("{lpkg:?} 成员路径逃出解压根: {raw:?}").into());
        }
        // ③ 符号链接目标：绝对目标放行（`<root>/usr/bin/foo -> /etc/foo` 合法且必要）；
        //    相对目标按"链接所在目录 + 目标"归一化，逃出根才拒 —— 不能按"含不含 `..`"判，
        //    那会误杀 `usr/bin/foo -> ../lib/foo`（合法且常见）。
        //    尾斜杠与类型不一致、声明尺寸说谎两条**不判**：libarchive 自己归一化/直接 FATAL
        //    （lpkg 把这两条作为死代码删掉了，别加回来）。
        // SAFETY: 条目有效，返回指针活到下一条目读入为止。
        let ftype = unsafe { archive_entry_filetype(entry) };
        if ftype == AE_IFLNK {
            // SAFETY: 同上。
            let target = unsafe { archive_entry_symlink(entry) };
            let target = cstr_to_string(target, "符号链接目标", lpkg)?;
            if !target.is_empty() {
                let t = Path::new(&target);
                if !t.is_absolute() {
                    let joined = rel.parent().unwrap_or(Path::new("")).join(t);
                    if escapes_root_lexically(&joined) {
                        return Err(format!(
                            "{lpkg:?} 的符号链接 {raw:?} 目标逃出解压根: {target:?}"
                        )
                        .into());
                    }
                }
            }
        }
        // ④ 重名成员：tar 是纯顺序格式，后一份静默覆盖前一份 ⇒ "流式读到的"与"落盘的是"**不是同一个对象**
        //    （lpkg 真实踩过 metadata.json 放两份）。放在最后判，让更具体的原因先报出来。
        if !self.seen.insert(rel.to_string_lossy().into_owned()) {
            return Err(format!("{lpkg:?} 有重名成员: {raw:?}").into());
        }
        Ok(())
    }
}

// ─────────────────────────── 解包 ───────────────────────────

/// 解包 `.lpkg`（zstd 帧 + pax tar）到 `extract_dir`（**须已建好且为空**）。
///
/// `restore_owner` = 以 root 运行时按 header 数字 uid/gid chown（**不**查用户名表，numeric-owner；
/// 非 root 时不设该选项，否则每个成员都会 EPERM）。调用顺序照 lpkg `extract_tar_zst`。
pub fn extract(lpkg_path: &Path, extract_dir: &Path, restore_owner: bool) -> Result<(), FarmError> {
    let c_lpkg = cstring(lpkg_path)?;
    let root = extract_dir.to_path_buf();
    let mut guard = MemberGuard::new();

    // SAFETY: 全程只用上面的 FFI；句柄由 Reader/Disk/Entry 的 Drop 释放；条目指针只在
    // archive_read_next_header 之后、下一次调用之前使用（需要留存的字符串当场拷成 String）。
    unsafe {
        let reader = Reader(archive_read_new());
        if reader.0.is_null() {
            return Err(format!("libarchive 初始化失败（{lpkg_path:?}）").into());
        }
        judge(
            archive_read_support_filter_all(reader.0),
            reader.0,
            "启用过滤器",
            lpkg_path,
        )?;
        judge(
            archive_read_support_format_all(reader.0),
            reader.0,
            "启用归档格式",
            lpkg_path,
        )?;

        let disk = Disk(archive_write_disk_new());
        if disk.0.is_null() {
            return Err(format!("libarchive 写盘端初始化失败（{lpkg_path:?}）").into());
        }
        let mut flags = EXTRACT_TIME
            | EXTRACT_PERM
            | EXTRACT_ACL
            | EXTRACT_XATTR
            | EXTRACT_FFLAGS
            | EXTRACT_SECURE_SYMLINKS
            | EXTRACT_SECURE_NODOTDOT
            | EXTRACT_UNLINK;
        if restore_owner {
            flags |= EXTRACT_OWNER;
        }
        judge(
            archive_write_disk_set_options(disk.0, flags),
            disk.0,
            "设置解包选项",
            lpkg_path,
        )?;
        // 注意：**不**调 archive_write_disk_set_standard_lookup（文件头第 5 条）。

        judge(
            archive_read_open_filename(reader.0, c_lpkg.as_ptr(), 64 * 1024),
            reader.0,
            "打开包",
            lpkg_path,
        )?;

        loop {
            let mut entry_ptr: *mut archive_entry = ptr::null_mut();
            let status = archive_read_next_header(reader.0, &mut entry_ptr);
            if status == ARCHIVE_EOF {
                break;
            }
            judge(status, reader.0, "读条目头", lpkg_path)?;
            if entry_ptr.is_null() {
                continue;
            }
            // 注意：这个条目**归 libarchive 所有**（随 archive_read_free 释放，库还会跨条目复用它），
            // 所以这里**不能**用会 archive_entry_free 的 `Entry` 守卫 —— 那是 free 掉库的对象。
            // `Entry` 守卫只用在写侧的 archive_entry_new() 上。
            let entry = entry_ptr;

            let raw_name = archive_entry_pathname(entry);
            if raw_name.is_null() {
                continue; // 无名成员：不落盘（lpkg 同款）
            }
            let raw_name = cstr_to_string(raw_name, "成员名", lpkg_path)?;
            let rel = member_path(&raw_name, lpkg_path)?;
            if rel.as_os_str().is_empty() {
                continue; // `.` / `./` 这类归档根条目：不产生文件（lpkg 同款）
            }
            guard.check(entry, &raw_name, &rel, lpkg_path)?;

            let dest = root.join(&rel);
            let c_dest = cstring(&dest)?;
            archive_entry_set_pathname(entry, c_dest.as_ptr());

            // 符号链接：perm 置 0，免得 libarchive 对链接 chmod 而改掉**目标**的权限（文件头第 7 条）。
            let ftype = archive_entry_filetype(entry);
            if ftype == AE_IFLNK {
                archive_entry_set_perm(entry, 0);
            }
            // 特殊文件成员：整包拒绝（lpkg 给出的后果：FIFO 会让后续读哈希永久阻塞、设备会让复制无限读）。
            if matches!(ftype, AE_IFIFO | AE_IFCHR | AE_IFBLK | AE_IFSOCK) {
                return Err(format!(
                    "{lpkg_path:?} 含不支持的特殊文件成员 {raw_name:?}（fifo/设备/socket）——不应出现在 .lpkg"
                )
                .into());
            }

            // 硬链接：目标必须在解压根内，否则一个成员就能给任意已有文件起别名。判据用**原始目标**
            // （绝对路径，或相对路径归一化后逃出根）→ 整包拒绝（lpkg 同款粒度：畸形是结构性的，
            // "跳过该成员"只会让人拿到一个装了一半的包）。
            let hardlink = archive_entry_hardlink(entry);
            if !hardlink.is_null() {
                let target = cstr_to_string(hardlink, "硬链接目标", lpkg_path)?;
                // 判据用**原始目标**：绝对路径或 `..` 上溯出根 → 整包拒绝（lpkg 同款 —— 否则一个
                // 成员就能给任意已有文件（如 /etc/shadow）起别名）。`member_path` 已覆盖这两条，
                // 这里只负责"归一化后重写成解压根下的绝对路径"（libarchive 需要绝对路径）。
                let target_rel = member_path(&target, lpkg_path)?;
                let c_target = cstring(&root.join(&target_rel))?;
                archive_entry_set_hardlink(entry, c_target.as_ptr());
            }

            judge(
                archive_write_header(disk.0, entry),
                disk.0,
                "解包条目",
                lpkg_path,
            )?;

            // 块式搬数据：读取端只给数据块（洞不读），写盘端按 offset 落位 → 洞保持为洞、不整份进内存。
            loop {
                let mut buff: *const c_void = ptr::null();
                let mut size: usize = 0;
                let mut offset: i64 = 0;
                let status = archive_read_data_block(reader.0, &mut buff, &mut size, &mut offset);
                if status == ARCHIVE_EOF {
                    break;
                }
                if status < ARCHIVE_WARN {
                    return Err(format!(
                        "libarchive 读条目数据失败（{lpkg_path:?} / {raw_name:?}）: {}",
                        err_str(reader.0)
                    )
                    .into());
                }
                if archive_write_data_block(disk.0, buff, size, offset) < 0 {
                    return Err(format!(
                        "libarchive 写条目数据失败（{lpkg_path:?} / {raw_name:?}）: {}",
                        err_str(disk.0)
                    )
                    .into());
                }
            }
            // finish_entry 的返回值必须查：磁盘满/EIO 时会失败，吞掉就是"报成功、盘上截断"。
            judge(
                archive_write_finish_entry(disk.0),
                disk.0,
                "收尾条目",
                lpkg_path,
            )?;
        }
        // 显式收尾：目录 mtime/权限等延迟动作在这里落盘，close 的失败要报出来。
        judge(archive_write_close(disk.0), disk.0, "收尾写盘", lpkg_path)?;
        Ok(())
    }
}

// ─────────────────────────── 流式取单个成员 ───────────────────────────

/// 流式取某个成员的原始内容（不落盘其余 content），没这个成员返回 `None`。
///
/// 成员名按**归一化后的相对路径**比较（`./` 前缀丢弃），不做松匹配。
///
/// 这里**不跑** `MemberGuard`：lpkg 在同样的"取 metadata.json"路径上权衡过 —— `packer.cpp` 把
/// metadata.json 放在第一个成员，该函数正常读一个头就返回；改成扫到 EOF 会让每次元数据读取都把
/// 整个包解压一遍（每个包多一遍 I/O），而**重名成员在 `extract` 里已经整包拒绝**，
/// "读第一份、落地最后一份"那条链在解包那一步就断了（farm 的解包同样跑 guard）。
pub fn read_member(lpkg_path: &Path, wanted: &str) -> Result<Option<Vec<u8>>, FarmError> {
    let c_lpkg = cstring(lpkg_path)?;
    // SAFETY: 同 extract。
    unsafe {
        let reader = Reader(archive_read_new());
        if reader.0.is_null() {
            return Err(format!("libarchive 初始化失败（{lpkg_path:?}）").into());
        }
        judge(
            archive_read_support_filter_all(reader.0),
            reader.0,
            "启用过滤器",
            lpkg_path,
        )?;
        judge(
            archive_read_support_format_all(reader.0),
            reader.0,
            "启用归档格式",
            lpkg_path,
        )?;
        judge(
            archive_read_open_filename(reader.0, c_lpkg.as_ptr(), 64 * 1024),
            reader.0,
            "打开包",
            lpkg_path,
        )?;

        loop {
            let mut entry_ptr: *mut archive_entry = ptr::null_mut();
            let status = archive_read_next_header(reader.0, &mut entry_ptr);
            if status == ARCHIVE_EOF {
                return Ok(None);
            }
            judge(status, reader.0, "读条目头", lpkg_path)?;
            if entry_ptr.is_null() {
                continue;
            }
            // 同 extract：条目归 libarchive 所有，不包 Entry 守卫。
            let entry = entry_ptr;
            let raw_name = archive_entry_pathname(entry);
            let matched = if raw_name.is_null() {
                false
            } else {
                let raw = cstr_to_string(raw_name, "成员名", lpkg_path)?;
                member_path(&raw, lpkg_path)?.as_os_str().as_bytes() == wanted.as_bytes()
            };
            if matched {
                // `archive_read_data`（而非 block）会把稀疏成员的洞补零，得到的是**完整逻辑内容**。
                let mut out = Vec::new();
                let mut buf = vec![0u8; 64 * 1024];
                loop {
                    let n = archive_read_data(reader.0, buf.as_mut_ptr().cast(), buf.len());
                    if n < 0 {
                        return Err(format!(
                            "读 {wanted} 失败（{lpkg_path:?}）: {}",
                            err_str(reader.0)
                        )
                        .into());
                    }
                    if n == 0 {
                        break;
                    }
                    out.extend_from_slice(&buf[..n as usize]);
                }
                return Ok(Some(out));
            }
            judge(
                archive_read_data_skip(reader.0),
                reader.0,
                "跳过成员数据",
                lpkg_path,
            )?;
        }
    }
}

// ─────────────────────────── 打包 ───────────────────────────

/// 把 `root` 树里 `rel_paths` 列出的成员（调用方给**已排序**的路径表）写成 `.lpkg`。
///
/// 路径表由调用方排序：遍历顺序确定是 farm 的"字节可复现"策略，不属于这一层。
/// 归一化对齐旧 `tar --numeric-owner --mtime=@0 -cf`：uid/gid 固定 0、mtime 固定 0、
/// atime/ctime/birthtime **不设**（不设才不会被 pax 写成"打包时刻"）。
/// `level` = zstd 压缩级别（发行档 22）—— lpkg 的打包器用默认级别，farm 显式钉住以便产出稳定。
pub fn write_tree(
    root: &Path,
    out_path: &Path,
    level: u8,
    rel_paths: &[PathBuf],
) -> Result<(), FarmError> {
    let c_out = cstring(out_path)?;

    // SAFETY: 同 extract；条目由 Entry 守卫释放，句柄由 Writer/Disk 守卫释放。
    unsafe {
        let mut writer = Writer(archive_write_new());
        if writer.0.is_null() {
            return Err(format!("libarchive 初始化失败（{out_path:?}）").into());
        }
        // 与 lpkg packer.cpp 同款：zstd 过滤器 + pax_restricted 格式。
        judge(
            archive_write_add_filter_zstd(writer.0),
            writer.0,
            "启用 zstd 过滤器",
            out_path,
        )?;
        if level != 0 {
            let module = CString::new("zstd").unwrap();
            let option = CString::new("compression-level").unwrap();
            let value = CString::new(level.to_string()).unwrap();
            let rc = archive_write_set_filter_option(
                writer.0,
                module.as_ptr(),
                option.as_ptr(),
                value.as_ptr(),
            );
            if rc < ARCHIVE_OK {
                // 不是致命：打包内容与可复现性不受影响，只是压缩级别回到 libarchive 默认值。
                eprintln!(
                    "warning: libarchive 不接受 zstd compression-level={level}（{out_path:?}）: {}",
                    err_str(writer.0)
                );
            }
        }
        judge(
            archive_write_set_format_pax_restricted(writer.0),
            writer.0,
            "设置 pax_restricted 格式",
            out_path,
        )?;
        judge(
            archive_write_open_filename(writer.0, c_out.as_ptr()),
            writer.0,
            "打开输出包",
            out_path,
        )?;

        for rel in rel_paths {
            let abs = root.join(rel);
            // 磁盘侧类型判据（symlink_metadata：符号链接按链接本身，损坏链接也算合法内容）。
            let md = fs::symlink_metadata(&abs).map_err(|e| format!("stat {abs:?} 失败: {e}"))?;
            let ft = md.file_type();
            if ft.is_socket() || ft.is_block_device() || ft.is_char_device() || ft.is_fifo() {
                return Err(format!(
                    "content 含不支持的特殊文件类型 {abs:?}（socket/设备/fifo）——不应出现在 .lpkg"
                )
                .into());
            }
            // 写侧同样过名字判据（与解包侧共用 member_path）：名字进入归档的**唯一漏斗**，
            // 不判的话 farm 能产出一个自己的（以及 lpkg 的）解包器会整包拒收的包。
            let rel_str = rel.to_string_lossy();
            // 写侧同样过名字判据（与解包侧共用 member_path）：名字进入归档的**唯一漏斗** ——
            // 不判的话 farm 能产出一个自己的（以及 lpkg 的）解包器会整包拒收的包。
            member_path(&rel_str, out_path)?;

            // SAFETY: 条目与磁盘走查器都是新建的，各自有守卫。
            // 顺序照 lpkg `add_to_archive`：条目上先放**磁盘路径**（disk reader 按它读元数据），
            // 填完再改成归档内名字 —— 反了会报 "Couldn't determine path"。
            let entry = Entry(archive_entry_new());
            if entry.0.is_null() {
                return Err(format!("libarchive 条目分配失败（{abs:?}）").into());
            }
            let c_abs = cstring(&abs)?;
            archive_entry_set_pathname(entry.0, c_abs.as_ptr());

            let mut st: libc::stat = std::mem::zeroed();
            if libc::lstat(c_abs.as_ptr(), &mut st) != 0 {
                return Err(
                    format!("lstat {abs:?} 失败: {}", std::io::Error::last_os_error()).into(),
                );
            }
            let disk = Reader(archive_read_disk_new());
            if disk.0.is_null() {
                return Err(format!("libarchive 磁盘读取器初始化失败（{abs:?}）").into());
            }
            // 不跟随符号链接（与 lstat 语义一致）。
            judge(
                archive_read_disk_set_symlink_physical(disk.0),
                disk.0,
                "设置符号链接模式",
                out_path,
            )?;
            // 这一句带 xattr **和真正的 ACL**（文件头第 10 条）。读不到元数据不能静默跳过该文件
            // （那会打出一个少内容的包），直接报错。
            let rc = archive_read_disk_entry_from_file(disk.0, entry.0, -1, &st);
            if rc < ARCHIVE_WARN {
                return Err(format!(
                    "读 {abs:?} 的元数据失败（{out_path:?}）: {}",
                    err_str(disk.0)
                )
                .into());
            }

            // 归档内名字 + farm 的归一化（可复现：不泄漏打包机身份与打包时刻）。
            let c_rel = cstring(Path::new(rel_str.as_ref()))?;
            archive_entry_set_pathname(entry.0, c_rel.as_ptr());
            archive_entry_set_size(entry.0, if ft.is_file() { md.len() as i64 } else { 0 });
            archive_entry_set_uid(entry.0, 0);
            archive_entry_set_gid(entry.0, 0);
            archive_entry_set_mtime(entry.0, 0, 0);
            archive_entry_unset_atime(entry.0);
            archive_entry_unset_ctime(entry.0);
            archive_entry_unset_birthtime(entry.0);

            // 符号链接目标显式读取：`readlink` 不保证 NUL 结尾，长度顶到缓冲上限 = 被截断，
            // 静默打包一个**错误**的链接目标不可接受（lpkg `add_to_archive` 同判据）。
            if ft.is_symlink() {
                let target =
                    fs::read_link(&abs).map_err(|e| format!("read_link {abs:?} 失败: {e}"))?;
                let c_target = cstring(&target)?;
                archive_entry_set_symlink(entry.0, c_target.as_ptr());
            }

            judge(
                archive_write_header(writer.0, entry.0),
                writer.0,
                "写条目头",
                out_path,
            )?;
            if ft.is_file() {
                use std::io::Read;
                let mut f = fs::File::open(&abs).map_err(|e| format!("打开 {abs:?} 失败: {e}"))?;
                let mut buf = vec![0u8; 64 * 1024];
                loop {
                    let n = f
                        .read(&mut buf)
                        .map_err(|e| format!("读 {abs:?} 失败: {e}"))?;
                    if n == 0 {
                        break;
                    }
                    if archive_write_data(writer.0, buf.as_ptr().cast(), n) < 0 {
                        return Err(format!(
                            "写 {rel_str:?} 的数据失败（{out_path:?}）: {}",
                            err_str(writer.0)
                        )
                        .into());
                    }
                }
            }
        }

        // close 的返回值 = "整包是否真的写完落盘"（lpkg packer.cpp 同判据）：写失败时 libarchive
        // 返回 FAILED/FATAL，不查就会把截断的 `.lpkg` 当成功。
        let close_rc = archive_write_close(writer.0);
        archive_write_free(writer.0);
        writer.0 = ptr::null_mut(); // 已释放：别让 Drop 再 free 一次
        if close_rc < ARCHIVE_WARN {
            return Err(format!("libarchive 收尾 {out_path:?} 失败（包被截断）").into());
        }
        if close_rc < ARCHIVE_OK {
            eprintln!(
                "warning: libarchive 收尾 {out_path:?} 有警告: {close_rc}（包内容可能不完整）"
            );
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests;
