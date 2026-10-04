//! `src/repack.rs` 的单元测试（从生产文件搬出：那里测试比生产还长，正文被淹没）。
//! 仍是同一父模块的子模块 ⇒ `use super::*` 与 crate 内私有项照旧可用。

use super::*;
use std::path::PathBuf;

/// 合成一个最小 .lpkg：metadata.json + content/libfoo.so（假 ELF）。
/// 目录带自增后缀，避免并行测试互相删目录（竞态）。
fn make_fake_lpkg() -> (PathBuf, PathBuf) {
    static COUNTER: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    let id = COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let dir = std::env::temp_dir().join(format!("farm-repack-test-{}-{}", std::process::id(), id));
    let _ = fs::remove_dir_all(&dir);
    let src = dir.join("src");
    fs::create_dir_all(src.join("content")).unwrap();
    fs::write(
            src.join("metadata.json"),
            r#"{"name":"fake","version":"1.0","needed_so":["libc.so.6"],"provides":["virtualcap"],"provides_soname":["libfoo.so","libfoo.so.1"]}"#,
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
    assert_eq!(meta["provides_soname"][1], "libfoo.so.1");
    // **关键行为**：虚拟 provides 是手写值，repack 绝不覆盖（这就是"虚拟 provider 存不下来"的修复）
    assert_eq!(
        meta["provides"],
        serde_json::json!(["virtualcap"]),
        "repack 不得触碰虚拟 provides"
    );

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
fn repack_failure_removes_tmp_leftover() {
    // 失败路径不得留 `.lpkg.tmp` 残骸：造一个含 FIFO 的打包树 → pack_dir_tar 明确报错。
    let base = std::env::temp_dir().join(format!("farm-repack-tmp-{}", std::process::id()));
    let _ = fs::remove_dir_all(&base);
    let root = base.join("root");
    fs::create_dir_all(root.join("content")).unwrap();
    let fifo = root.join("content/apipe");
    let cpath = std::ffi::CString::new(fifo.to_str().unwrap()).unwrap();
    // SAFETY: mkfifo 仅创建命名管道，参数为合法 C 路径；返回 0 表示成功。
    let rc = unsafe { libc::mkfifo(cpath.as_ptr(), 0o644) };
    assert_eq!(rc, 0, "mkfifo 应成功");

    let out = base.join("out.lpkg");
    let res = repack_lpkg_at(&root, &out, 3);
    assert!(res.is_err(), "含 FIFO 的树必须报错");
    assert!(
        !out.with_extension("lpkg.tmp").exists(),
        "失败路径不得留下 .lpkg.tmp 残骸"
    );
    assert!(!out.exists(), "失败不得产出目标 .lpkg");
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

/// xattr 打包/解包往返：**保真**（含二进制值）+ **字节可复现**（枚举顺序不确定，必须排序）。
/// 文件系统不支持 xattr 时跳过——与 `pack_preserves_suid_when_root` 的非 root 跳过同款。
#[test]
fn pack_roundtrip_preserves_xattrs_and_stays_reproducible() {
    let base = std::env::temp_dir().join(format!("farm-xattr-{}", std::process::id()));
    let _ = fs::remove_dir_all(&base);
    let root = base.join("root");
    fs::create_dir_all(root.join("content")).unwrap();
    let f = root.join("content/hascap");
    fs::write(&f, b"x").unwrap();

    // 值刻意取**二进制**（`security.capability` 就是二进制结构，当字符串处理会坏）
    if xattr::set(&f, "user.lankefarm_a", b"cap\x00\x01\xff").is_err()
        || xattr::set(&f, "user.lankefarm_b", b"second").is_err()
    {
        eprintln!("文件系统不支持 user.* xattr（或权限不足），跳过");
        fs::remove_dir_all(&base).ok();
        return;
    }
    // 特权命名空间单独测一条（root 下）：真正要保住的 `security.capability` 就在这一档
    // （需要 CAP_SETFCAP）；这里用 `trusted.*` 走**同一条特权 syscall 路径**，避免手搓
    // `vfs_cap_data` 的二进制格式（格式错会被内核拒，测的就成了格式而不是保留行为）。
    let priv_ns = crate::scan::running_as_root()
        && xattr::set(&f, "trusted.lankefarm_priv", b"priv\x00\xff").is_ok();

    let lpkg1 = base.join("a.lpkg");
    let lpkg2 = base.join("b.lpkg");
    repack_lpkg_at(&root, &lpkg1, 3).unwrap();
    repack_lpkg_at(&root, &lpkg2, 3).unwrap();
    assert_eq!(
        fs::read(&lpkg1).unwrap(),
        fs::read(&lpkg2).unwrap(),
        "含 xattr 也必须字节可复现（xattr 按名字排序）"
    );

    // 解包还原：两个 xattr 都在，且二进制值原样
    let extract = base.join("extract");
    crate::scan::extract_lpkg(&lpkg1, &extract).unwrap();
    let got = extract.join("content/hascap");
    assert_eq!(
        xattr::get(&got, "user.lankefarm_a").unwrap().as_deref(),
        Some(&b"cap\x00\x01\xff"[..]),
        "二进制 xattr 值必须原样还原"
    );
    assert_eq!(
        xattr::get(&got, "user.lankefarm_b").unwrap().as_deref(),
        Some(&b"second"[..]),
        "多个 xattr 都要还原"
    );
    if priv_ns {
        assert_eq!(
            xattr::get(&got, "trusted.lankefarm_priv")
                .unwrap()
                .as_deref(),
            Some(&b"priv\x00\xff"[..]),
            "特权命名空间（security.capability 所在档）的 xattr 也必须原样还原"
        );
    }
    fs::remove_dir_all(&base).ok();
}
