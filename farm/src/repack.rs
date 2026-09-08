//! repack.rs — 解包 .lpkg → 改 metadata.json → 重打（§6：repack 不 rebuild）。
//!
//! 语义对标 Gentoo stage3 解压：保留 mode（含 SUID/SGID）+ numeric-owner；解包不用 /tmp
//! （NOSUID，suid 程序受影响，§6）。解包/重打包**纯 Rust**（tar + zstd crate），不再 spawn
//! `sudo`/`tar`/`zstd` CLI（操作命令以 root 运行，见 ARCH §「root 运行」）。
//!
//! **xattr 保留：明确不做（已决策，原 TODO）**。tar 0.4.46 的 Builder 不暴露 PAX xattr 写入
//! （`SCHILY.xattr.*`），解包侧虽有 `xattr` feature 也补不上 repack 的写入侧；libarchive 绑定
//! （ADR #13 绑定优先）成本高。LankeOS 是 LFS 系系统，默认无 SELinux xattr；SUID/SGID 已由
//! mode 保留覆盖。若将来需要 `security.capability`（如 systemd native 构建），才需引入 libarchive。
//!
//! repack 有效性由构建不变量保证（§6：构建成功 ⇒ needed_so 的 provider 当时都在 local repo），
//! 无需额外 guard。
//!
//! repack 只改 `needed_so`/`provides`（用户明确：deps 不动，由 gen_deps/deprules 规则生成）。
//! **同时返回 LankeBUILD.json 需要的字段**（`update_lankebuild` 在调用方），确保仓库源定义
//! 与包内 metadata 一致（§6：把漂移 diff 落回仓库，源定义是真相）。

use std::fs;
use std::io::{self, Write};
use std::os::unix::fs::{FileTypeExt, PermissionsExt};
use std::path::{Path, PathBuf};

use crate::scan;

/// 解包（若未解包）→ 改 metadata.json 的 needed_so/provides → 重打覆盖 .lpkg。
/// `extract_dir` 复用 scan 的解包目录（单包单趟）。
pub fn repack_with_metadata(
    lpkg_path: &Path,
    extract_dir: &Path,
    new_needed_so: &[String],
    new_provides: &[String],
) -> Result<(), String> {
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
    meta["provides"] = serde_json::Value::Array(
        new_provides
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
fn repack_lpkg(extract_dir: &Path, out_path: &Path) -> Result<(), String> {
    repack_lpkg_at(extract_dir, out_path, 22)
}

fn repack_lpkg_at(extract_dir: &Path, out_path: &Path, level: i32) -> Result<(), String> {
    let tmp = out_path.with_extension("lpkg.tmp");
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
pub fn pack_dir_tar(root: &Path, out: &mut dyn Write) -> Result<(), String> {
    let mut b = tar::Builder::new(out);
    append_tree(&mut b, root, root)?;
    b.finish().map_err(|e| format!("tar 收尾失败: {e}"))
}

/// 递归把 `abs_dir` 下的成员写进 builder；`abs_root` = 包根（tar 路径的相对基准）。
fn append_tree(
    b: &mut tar::Builder<&mut dyn Write>,
    abs_root: &Path,
    abs_dir: &Path,
) -> Result<(), String> {
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
            ));
        }
    }
    Ok(())
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
mod tests {
    use super::*;
    use std::path::PathBuf;

    /// 合成一个最小 .lpkg：metadata.json + content/libfoo.so（假 ELF）。
    /// 目录带自增后缀，避免并行测试互相删目录（竞态）。
    fn make_fake_lpkg() -> (PathBuf, PathBuf) {
        static COUNTER: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let id = COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let dir =
            std::env::temp_dir().join(format!("farm-repack-test-{}-{}", std::process::id(), id));
        let _ = fs::remove_dir_all(&dir);
        let src = dir.join("src");
        fs::create_dir_all(src.join("content")).unwrap();
        fs::write(
            src.join("metadata.json"),
            r#"{"name":"fake","version":"1.0","needed_so":["libc.so.6"],"provides":["libfoo.so","libfoo.so.1"]}"#,
        )
        .unwrap();
        fs::write(
            src.join("content/libfoo.so.1"),
            [0x7f, b'E', b'L', b'F', 2, 1, 1],
        )
        .unwrap();
        // 损坏 symlink（指向包内不存在的目标，如 dbus 的 var/lib/dbus/machine-id）——
        // append_dir_all 默认 follow 会炸 NotFound；必须 follow_symlinks(false) 按 symlink 存。
        std::os::unix::fs::symlink("nonexistent-target", src.join("content/broken-link")).unwrap();

        let out = dir.join("fake-1.0.lpkg");
        let f = fs::File::create(&out).unwrap();
        let enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
        let mut b = tar::Builder::new(enc);
        b.follow_symlinks(false);
        b.append_dir_all(".", &src).unwrap();
        let enc = b.into_inner().unwrap();
        enc.finish().unwrap();
        (out, src)
    }

    #[test]
    fn repack_updates_metadata_and_roundtrips() {
        let (lpkg, src) = make_fake_lpkg();
        let extract = std::env::temp_dir().join("farm-repack-extract");
        let new_needed = vec!["libc.so.6".to_string(), "libm.so.6".to_string()];
        let new_prov = vec!["libfoo.so".to_string(), "libfoo.so.1".to_string()];

        repack_with_metadata(&lpkg, &extract, &new_needed, &new_prov).unwrap();

        // metadata.json 已更新（repack 只改元数据；content 扫描是另一回事）
        let meta = crate::scan::read_metadata_json(&extract.join("metadata.json")).unwrap();
        assert_eq!(meta["name"], "fake");
        assert_eq!(meta["needed_so"][1], "libm.so.6");
        assert_eq!(meta["provides"][1], "libfoo.so.1");

        // 重打的 .lpkg 仍可解包（round-trip 完整），扫描出的 name/version 正确
        let scan = crate::scan::scan_lpkg(&lpkg, &extract, &Default::default()).unwrap();
        assert_eq!(scan.name, "fake");
        assert_eq!(scan.version, "1.0");

        fs::remove_dir_all(&src).ok();
        fs::remove_dir_all(&extract).ok();
        fs::remove_file(&lpkg).ok();
        let dir = std::env::temp_dir().join("farm-repack-test");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn repack_survives_broken_symlink_in_content() {
        // 复现 dbus/ncurses 等 seed 失败：content 含损坏 symlink，repack 不能炸。
        let (lpkg, src) = make_fake_lpkg();
        let extract = std::env::temp_dir().join("farm-repack-brokenlink");
        repack_with_metadata(&lpkg, &extract, &[], &["libfoo.so".to_string()]).unwrap();
        // 重打后解包，损坏 symlink 仍在（按 symlink 存了，没被 follow 吞掉）
        crate::scan::extract_lpkg(&lpkg, &extract).unwrap();
        let l = extract.join("content/broken-link");
        assert!(std::fs::symlink_metadata(&l).is_ok(), "symlink 应保留");
        fs::remove_dir_all(&src).ok();
        fs::remove_dir_all(&extract).ok();
        fs::remove_file(&lpkg).ok();
        let dir = std::env::temp_dir().join("farm-repack-test");
        fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn repack_normalizes_mtime_to_epoch() {
        // 可复现构建：repack 产出的 .lpkg 所有成员 mtime = 1970-01-01（--mtime=@0），
        // tar 不泄漏源文件/打包时间；同一内容两次 repack 字节一致。
        let root = std::env::temp_dir().join(format!("farm-repack-mtime-{}", std::process::id()));
        let dir = root.join("src");
        fs::create_dir_all(dir.join("content")).unwrap();
        let f = dir.join("content/libfoo.so");
        fs::write(&f, [0x7f, b'E', b'L', b'F', 2, 1, 1]).unwrap();
        // 源文件 mtime 故意设成现在 → 若不被归一，tar 里会带出当前时间戳
        {
            let file = fs::File::open(&f).unwrap();
            let _ = file.set_times(fs::FileTimes::new().set_modified(std::time::SystemTime::now()));
        }
        let out = root.join("out.lpkg");
        repack_lpkg_at(&dir, &out, 3).unwrap();

        let fp = fs::File::open(&out).unwrap();
        let dec = zstd::stream::read::Decoder::new(fp).unwrap();
        let mut ar = tar::Archive::new(dec);
        let mut entries = 0usize;
        for e in ar.entries().unwrap() {
            let e = e.unwrap();
            assert_eq!(
                e.header().mtime().unwrap(),
                0,
                "{} mtime 应归一 0（1970-01-01）",
                e.path().unwrap().display()
            );
            entries += 1;
        }
        assert!(entries >= 2, "至少含目录与文件两个成员");
        fs::remove_dir_all(&root).ok();
    }

    /// 造一棵带 mode 差异文件 + 完好/损坏 symlink 的包树（root 无关）。
    fn build_tree(root: &Path) {
        use std::os::unix::fs::PermissionsExt;
        fs::create_dir_all(root.join("content")).unwrap();
        fs::write(
            root.join("metadata.json"),
            r#"{"name":"fake","version":"1.0"}"#,
        )
        .unwrap();
        fs::write(root.join("content/plain"), b"abc").unwrap();
        fs::set_permissions(
            root.join("content/plain"),
            fs::Permissions::from_mode(0o640),
        )
        .unwrap();
        std::os::unix::fs::symlink("nonexistent-target", root.join("content/broken")).unwrap();
        std::os::unix::fs::symlink("plain", root.join("content/ok-link")).unwrap();
        fs::set_permissions(root.join("content"), fs::Permissions::from_mode(0o750)).unwrap();
    }

    #[test]
    fn pack_roundtrip_preserves_mode_symlink_mtime_reproducible() {
        use std::os::unix::fs::MetadataExt;
        use std::os::unix::fs::PermissionsExt;
        let base = std::env::temp_dir().join(format!("farm-packrt-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        let root = base.join("root");
        fs::create_dir_all(&root).unwrap();
        build_tree(&root);

        // 同一内容打两次 → 字节一致（mtime 归一 0 后可复现，取代对 sudo tar --mtime=@0 的依赖）
        let lpkg1 = base.join("a.lpkg");
        let lpkg2 = base.join("b.lpkg");
        repack_lpkg_at(&root, &lpkg1, 3).unwrap();
        repack_lpkg_at(&root, &lpkg2, 3).unwrap();
        assert_eq!(
            fs::read(&lpkg1).unwrap(),
            fs::read(&lpkg2).unwrap(),
            "mtime 归一 + 排序遍历应字节可复现"
        );

        // header 属性：uid/gid/mtime==0，mode 保留
        let f = fs::File::open(&lpkg1).unwrap();
        let dec = zstd::stream::read::Decoder::new(f).unwrap();
        let mut ar = tar::Archive::new(dec);
        let mut saw_plain = false;
        for e in ar.entries().unwrap() {
            let e = e.unwrap();
            let h = e.header();
            assert_eq!(h.uid().unwrap(), 0, "uid 应为 0: {:?}", e.path().unwrap());
            assert_eq!(h.gid().unwrap(), 0, "gid 应为 0");
            assert_eq!(h.mtime().unwrap(), 0, "mtime 应为 0");
            let p = e.path().unwrap().to_string_lossy().into_owned();
            if p == "content/plain" {
                saw_plain = true;
                assert_eq!(h.mode().unwrap() & 0o7777, 0o640, "mode 应保留");
            }
        }
        assert!(saw_plain, "应含 content/plain");

        // 解包：mode（含目录 0750）、完好/损坏 symlink 都保留，mtime 落盘为 1970（epoch 0）
        let extract = base.join("extract");
        crate::scan::extract_lpkg(&lpkg1, &extract).unwrap();
        let pm = fs::symlink_metadata(extract.join("content/plain")).unwrap();
        assert_eq!(pm.permissions().mode() & 0o7777, 0o640, "文件 mode 保留");
        // 落盘 mtime 归一 ~epoch（部分 FS 把 0 圆到 1），绝非构建时刻——header mtime==0 已在上方断言。
        assert!(
            pm.mtime() < 3600,
            "解包后文件 mtime 应归一 ~1970-01-01: {}",
            pm.mtime()
        );
        let dm = fs::symlink_metadata(extract.join("content")).unwrap();
        assert_eq!(dm.permissions().mode() & 0o7777, 0o750, "目录 mode 保留");
        assert!(
            fs::symlink_metadata(extract.join("content/broken"))
                .unwrap()
                .file_type()
                .is_symlink(),
            "损坏 symlink 应按 symlink 保留"
        );
        assert_eq!(
            fs::read_link(extract.join("content/ok-link"))
                .unwrap()
                .to_string_lossy()
                .into_owned(),
            "plain"
        );
        fs::remove_dir_all(&base).ok();
    }

    #[test]
    fn pack_preserves_suid_when_root() {
        use std::os::unix::fs::PermissionsExt;
        if !crate::scan::running_as_root() {
            eprintln!("非 root：跳过 SUID 断言（CI 非 root runner 不跑）");
            return;
        }
        let base = std::env::temp_dir().join(format!("farm-packsuid-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        let root = base.join("root");
        fs::create_dir_all(&root).unwrap();
        let f = root.join("suid");
        fs::write(&f, b"x").unwrap();
        fs::set_permissions(&f, fs::Permissions::from_mode(0o4755)).unwrap();

        let lpkg = base.join("suid.lpkg");
        repack_lpkg_at(&root, &lpkg, 3).unwrap();
        let extract = base.join("extract");
        crate::scan::extract_lpkg(&lpkg, &extract).unwrap();
        let m = fs::symlink_metadata(extract.join("suid")).unwrap();
        assert_eq!(
            m.permissions().mode() & 0o7777,
            0o4755,
            "SUID 位应随打包/解包保留（root 下）"
        );
        fs::remove_dir_all(&base).ok();
    }
}
