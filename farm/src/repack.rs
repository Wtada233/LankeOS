//! repack.rs — 解包 .lpkg → 改 metadata.json → 重打（§6：repack 不 rebuild）。
//!
//! 语义对标 Gentoo stage3 解压：保留 mode（含 SUID/SGID）+ numeric-owner；解包不用 /tmp
//! （NOSUID，suid 程序受影响，§6）。解包/重打包**纯 Rust**（tar + zstd crate），不再 spawn
//! `sudo`/`tar`/`zstd` CLI（操作命令以 root 运行，见 ARCH §「root 运行」）。
//!
//! **xattr 保留：做**（原先的"明确不做"决策已作废——它基于"tar 0.4.46 的 Builder 不暴露 PAX
//! xattr 写入"，而实测 `Builder::append_pax_extensions` 就在 `tar::pax` 里，且该模块**无 feature
//! 门控**）。打包时把每个条目的 xattr 逐条写成 PAX 的 `SCHILY.xattr.<name>=<value>`（GNU tar 的
//! 约定），解包侧由 `scan::extract_lpkg` 的 `set_unpack_xattrs(true)` 还原——两侧对称。
//! 丢 xattr 的代价是**功能性的**：`security.capability` 一丢，systemd native 二进制、ping 这类
//! 靠文件能力提权的程序就废了；SUID/SGID 由 mode 保留覆盖不到它（那是另一套机制）。
//!
//! 仍然不做 libarchive（ADR #13 绑定优先）：tar + xattr 两个 crate 已经够，无需引入 C 绑定。
//!
//! repack 有效性由构建不变量保证（§6：构建成功 ⇒ needed_so 的 provider 当时都在 local repo），
//! 无需额外 guard。
//!
//! repack 只改 `needed_so`/`provides_soname`（用户明确：deps 不动，由 gen_deps/deprules 规则生成；
//! **`provides`（虚拟 provider）是手写值，farm 原样保留、绝不覆盖**）。
//! **同时返回 LankeBUILD.json 需要的字段**（`update_lankebuild` 在调用方），确保仓库源定义
//! 与包内 metadata 一致（§6：把漂移 diff 落回仓库，源定义是真相）。

use crate::error::FarmError;
use std::fs;
use std::io::{self, Write};
use std::os::unix::fs::{FileTypeExt, PermissionsExt};
use std::path::{Path, PathBuf};

use crate::scan;

/// tmp 残骸清理守卫：`repack_lpkg_at` 的任何提前返回/panic 都删掉半成品 `*.lpkg.tmp`，
/// 成功 `disarm()` 后不再动它。曾失败路径把 `<ver>.lpkg.tmp` 残骸永久留在 `out/<arch>/<pkg>/`。
struct TmpGuard(Option<PathBuf>);

impl TmpGuard {
    fn disarm(&mut self) {
        self.0 = None;
    }
}

impl Drop for TmpGuard {
    fn drop(&mut self) {
        if let Some(p) = self.0.take() {
            let _ = fs::remove_file(p);
        }
    }
}

/// 解包（若未解包）→ 改 metadata.json 的 needed_so/provides_soname → 重打覆盖 .lpkg。
/// **`provides`（虚拟 provider）原样保留、绝不触碰**——它是手写值，扫描不产出。
/// `extract_dir` 复用 scan 的解包目录（单包单趟）。
pub fn repack_with_metadata(
    lpkg_path: &Path,
    extract_dir: &Path,
    new_needed_so: &[String],
    new_provides_soname: &[String],
) -> Result<(), FarmError> {
    if !extract_dir.join("metadata.json").exists() {
        scan::extract_lpkg(lpkg_path, extract_dir)?;
    }
    // 1. 改 metadata.json
    let meta_path = extract_dir.join("metadata.json");
    let mut meta = scan::read_metadata_json(&meta_path)?;
    meta["needed_so"] = serde_json::Value::Array(
        new_needed_so
            .iter()
            .map(|s| serde_json::Value::String(s.clone()))
            .collect(),
    );
    meta["provides_soname"] = serde_json::Value::Array(
        new_provides_soname
            .iter()
            .map(|s| serde_json::Value::String(s.clone()))
            .collect(),
    );
    fs::write(&meta_path, serde_json::to_string_pretty(&meta).unwrap())
        .map_err(|e| format!("写 {meta_path:?} 失败: {e}"))?;
    // 2. 重打覆盖 .lpkg
    repack_lpkg(extract_dir, lpkg_path)
}

/// 把 `extract_dir` 重打成 .lpkg（zstd PAX tar，保留 mode/owner），原子替换原文件。
///
/// 遍历用 `symlink_metadata`（不 follow）——content 里的损坏 symlink（如 dbus 的
/// `var/lib/dbus/machine-id` → 包内不存在的目标）按 symlink 存，不炸 `No such file`。
/// build 每次成功都经此把容器产物归一化为发行档（level 22 + mtime 1970，字节可复现）。
fn repack_lpkg(extract_dir: &Path, out_path: &Path) -> Result<(), FarmError> {
    repack_lpkg_at(extract_dir, out_path, 22)
}

fn repack_lpkg_at(extract_dir: &Path, out_path: &Path, level: i32) -> Result<(), FarmError> {
    let tmp = out_path.with_extension("lpkg.tmp");
    // 先登记守卫（连 File::create 失败也清掉可能存在的旧残骸）
    let mut guard = TmpGuard(Some(tmp.clone()));
    let f = fs::File::create(&tmp).map_err(|e| format!("创建 {tmp:?} 失败: {e}"))?;
    let mut enc =
        zstd::stream::write::Encoder::new(f, level).map_err(|e| format!("zstd 初始化失败: {e}"))?;

    // 打包必须 root（操作命令已强制）：普通用户 `fs::metadata` 读不到完整 mode（SUID/SGID 被
    // 内核剥掉），也无法读 root-only 文件（如 /etc/shadow 0600）。tar::Builder 流式写进 zstd
    // Encoder；`pack_dir_tar` 对齐旧 `sudo tar --numeric-owner --mtime=@0`（uid/gid 0、mtime 0、
    // mode 完整含 SUID、symlink 不 follow、排序遍历字节确定）。
    pack_dir_tar(extract_dir, &mut enc)?;

    let mut f = enc.finish().map_err(|e| format!("zstd 收尾失败: {e}"))?;
    f.flush().map_err(|e| format!("flush 失败: {e}"))?;
    fs::rename(&tmp, out_path).map_err(|e| format!("替换 {out_path:?} 失败: {e}"))?;
    guard.disarm(); // 成功：tmp 已 rename 到目标，无需再删
    Ok(())
}

/// 把 `root` 目录递归打成 tar 流（写进 `out`），纯 tar crate。
///
/// 对齐旧 `tar --numeric-owner --mtime=@0 -cf` 语义：
/// - 每个成员 header 的 uid/gid **固定 0**、mtime **固定 0**（可复现——不泄漏打包时间，
///   同一内容两次打包字节一致）；mode 取 `symlink_metadata` 的权限位（root 下完整含 SUID/SGID）。
/// - symlink 按 symlink 存（`read_link`，**不 stat 目标**，容忍损坏 symlink）。
/// - `read_dir` 结果按路径排序 → 遍历顺序确定（配合 mtime=0 得到字节可复现的归档）。
/// - 遇到 socket/设备/fifo（LFS 包 content 不该有）→ 明确报错，不留静默。
pub fn pack_dir_tar(root: &Path, out: &mut dyn Write) -> Result<(), FarmError> {
    let mut b = tar::Builder::new(out);
    append_tree(&mut b, root, root)?;
    b.finish().map_err(|e| format!("tar 收尾失败: {e}").into())
}

/// 递归把 `abs_dir` 下的成员写进 builder；`abs_root` = 包根（tar 路径的相对基准）。
fn append_tree(
    b: &mut tar::Builder<&mut dyn Write>,
    abs_root: &Path,
    abs_dir: &Path,
) -> Result<(), FarmError> {
    let mut entries: Vec<(PathBuf, PathBuf)> = fs::read_dir(abs_dir)
        .map_err(|e| format!("读取 {abs_dir:?} 失败: {e}"))?
        .filter_map(|e| e.ok().map(|e| e.path()))
        .map(|abs| {
            let rel = abs.strip_prefix(abs_root).unwrap_or(&abs).to_path_buf();
            (abs, rel)
        })
        .collect();
    entries.sort_by(|a, b| a.1.cmp(&b.1)); // 按 tar 路径排序 → 确定序
    for (abs, rel) in entries {
        let md = fs::symlink_metadata(&abs).map_err(|e| format!("stat {abs:?} 失败: {e}"))?;
        let ft = md.file_type();
        // 先写 xattr、再写条目本体：PAX 扩展头是**紧随其后**那个条目的元数据，顺序不能反。
        // 目录/文件/符号链接都适用（三种都可能有 xattr）。
        let xattrs = read_pax_xattrs(&abs)?;
        if !xattrs.is_empty() {
            b.append_pax_extensions(xattrs.iter().map(|(k, v)| (k.as_str(), v.as_slice())))
                .map_err(|e| format!("tar 写 xattr（{abs:?}）失败: {e}"))?;
        }
        if ft.is_dir() {
            let mode = md.permissions().mode() & 0o7777;
            let mut h = new_header(mode, 0, tar::EntryType::Directory);
            b.append_data(&mut h, &rel, io::empty())
                .map_err(|e| format!("tar 写目录 {rel:?} 失败: {e}"))?;
            append_tree(b, abs_root, &abs)?;
        } else if ft.is_file() {
            let mode = md.permissions().mode() & 0o7777;
            let mut h = new_header(mode, md.len(), tar::EntryType::Regular);
            let f = fs::File::open(&abs).map_err(|e| format!("打开 {abs:?} 失败: {e}"))?;
            b.append_data(&mut h, &rel, f)
                .map_err(|e| format!("tar 写文件 {rel:?} 失败: {e}"))?;
        } else if ft.is_symlink() {
            let target = fs::read_link(&abs).map_err(|e| format!("read_link {abs:?} 失败: {e}"))?;
            // size 必须显式置 0 并写进 header（否则读侧 size 字段为空报错），symlink 无数据体。
            let mut h = new_header(0o777, 0, tar::EntryType::Symlink);
            h.set_link_name(&target)
                .map_err(|e| format!("tar 写 symlink 目标 {target:?} 失败: {e}"))?;
            b.append_data(&mut h, &rel, io::empty())
                .map_err(|e| format!("tar 写 symlink {rel:?} 失败: {e}"))?;
        } else if ft.is_socket() || ft.is_block_device() || ft.is_char_device() || ft.is_fifo() {
            return Err(format!(
                "content 含不支持的特殊文件类型 {abs:?}（socket/设备/fifo）——不应出现在 .lpkg"
            )
            .into());
        }
    }
    Ok(())
}

/// PAX 键前缀（GNU tar 的 xattr 约定）。与**读取端**对称：tar crate 的 `entry.rs` 用
/// `pax::PAX_SCHILYXATTR`（值即此串）剥掉前缀后 `xattr::set` 还原——那个常量在 tar 的**私有**
/// 模块 `pax` 里取不到，故这里字面写下同一份。两侧任一变都意味着格式漂移，改动时要一起看。
const PAX_SCHILYXATTR: &str = "SCHILY.xattr.";

/// 读条目**自身**的全部 xattr，转成 PAX 的 `SCHILY.xattr.<名>` → 值字节对。
///
/// - **不跟随符号链接**（`list_deref`/`get_deref`）：符号链接的 xattr 属于链接本身，不属于目标。
/// - 值是**原始字节**：`security.capability` 是二进制结构，不能当字符串处理（PAX 值本就是字节串）。
/// - **按 PAX 键排序**：枚举顺序由文件系统决定，不排序会破坏"同一内容两次打包字节一致"的可复现性
///   契约——mtime/uid/gid 都已归一到 0，xattr 顺序是同一个契约的一部分。
/// - 读失败分两类，不能一刀切（实测教训）：
///   - **`ENOENT`（悬空符号链接）/ `EOPNOTSUPP`（文件系统或该文件类型不支持 xattr）** → 当"没有
///     xattr"：这两种表示**这里本来就没东西可保**，不是读失败。悬空符号链接是**合法包内容**
///     （dbus 的 `var/lib/dbus/machine-id`、ncurses 等都有；`repack_survives_broken_symlink_in_content`
///     就是守它的），在它上面 `llistxattr` 会返回 ENOENT——最初按"任何错误都致命"写，直接把那三个
///     既有测试打挂了。
///   - **其余错误**（EACCES 等）→ **报错**：静默丢一个 `security.capability` 等于发个残包
///     （与"repack 失败必须 BLOCK、绝不静默降级"的既有立场一致）。
fn read_pax_xattrs(path: &Path) -> Result<Vec<(String, Vec<u8>)>, FarmError> {
    let names = match xattr::list_deref(path) {
        Ok(n) => n,
        Err(e)
            if matches!(
                e.kind(),
                std::io::ErrorKind::NotFound | std::io::ErrorKind::Unsupported
            ) =>
        {
            return Ok(Vec::new());
        }
        Err(e) => return Err(format!("列 xattr {path:?} 失败: {e}").into()),
    };
    let mut out: Vec<(String, Vec<u8>)> = Vec::new();
    for name in names {
        let value = xattr::get_deref(path, name.as_os_str())
            .map_err(|e| format!("读 xattr {name:?}（{path:?}）失败: {e}"))?
            .unwrap_or_default();
        out.push((
            format!("{PAX_SCHILYXATTR}{}", name.to_string_lossy()),
            value,
        ));
    }
    out.sort_by(|a, b| a.0.cmp(&b.0));
    Ok(out)
}

/// 构造一个 uid/gid/mtime 固定为 0 的 tar header（path 由 `append_data` 写入 header）。
fn new_header(mode: u32, size: u64, ty: tar::EntryType) -> tar::Header {
    let mut h = tar::Header::new_gnu();
    h.set_uid(0);
    h.set_gid(0);
    h.set_mtime(0);
    h.set_mode(mode);
    h.set_size(size);
    h.set_entry_type(ty);
    h
}

#[cfg(test)]
mod tests;
