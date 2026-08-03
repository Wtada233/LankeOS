//! repack.rs — 解包 .lpkg → 改 metadata.json → 重打（§6：repack 不 rebuild）。
//!
//! 语义对标 Gentoo stage3 解压：保留 mode（含 SUID/SGID）+ numeric-owner；解包不用 /tmp
//! （NOSUID，suid 程序受影响，§6）。当前用 tar crate 保留 mode。
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
use std::io::Write;
use std::path::Path;

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
        new_needed_so.iter().map(|s| serde_json::Value::String(s.clone())).collect(),
    );
    meta["provides"] = serde_json::Value::Array(
        new_provides.iter().map(|s| serde_json::Value::String(s.clone())).collect(),
    );
    fs::write(&meta_path, serde_json::to_string_pretty(&meta).unwrap())
        .map_err(|e| format!("写 {meta_path:?} 失败: {e}"))?;
    // 2. 重打覆盖 .lpkg
    repack_lpkg(extract_dir, lpkg_path)
}

/// 把 `extract_dir` 重打成 .lpkg（zstd PAX tar，保留 mode），原子替换原文件。
///
/// **必须 `follow_symlinks(false)`**：`append_dir_all` 默认 follow 符号链接，content 里的
/// 损坏 symlink（如 dbus 的 `var/lib/dbus/machine-id` → 包内不存在的目标）会炸
/// `No such file or directory`。关闭后 symlink 按 symlink 存，不 follow。
fn repack_lpkg(extract_dir: &Path, out_path: &Path) -> Result<(), String> {
    let tmp = out_path.with_extension("lpkg.tmp");
    let f = fs::File::create(&tmp).map_err(|e| format!("创建 {tmp:?} 失败: {e}"))?;
    let enc = zstd::stream::write::Encoder::new(f, 19)
        .map_err(|e| format!("zstd 初始化失败: {e}"))?;
    let mut builder = tar::Builder::new(enc);
    builder.follow_symlinks(false);
    builder
        .append_dir_all(".", extract_dir)
        .map_err(|e| format!("tar 打包失败: {e}"))?;
    let enc = builder.into_inner().map_err(|e| format!("tar 收尾失败: {e}"))?;
    let mut f = enc.finish().map_err(|e| format!("zstd 收尾失败: {e}"))?;
    f.flush().map_err(|e| format!("flush 失败: {e}"))?;
    fs::rename(&tmp, out_path).map_err(|e| format!("替换 {out_path:?} 失败: {e}"))?;
    Ok(())
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
        let dir = std::env::temp_dir().join(format!("farm-repack-test-{}-{}", std::process::id(), id));
        let _ = fs::remove_dir_all(&dir);
        let src = dir.join("src");
        fs::create_dir_all(src.join("content")).unwrap();
        fs::write(
            src.join("metadata.json"),
            r#"{"name":"fake","version":"1.0","needed_so":["libc.so.6"],"provides":["libfoo.so","libfoo.so.1"]}"#,
        )
        .unwrap();
        fs::write(src.join("content/libfoo.so.1"), [0x7f, b'E', b'L', b'F', 2, 1, 1]).unwrap();
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
        let scan = crate::scan::scan_lpkg(&lpkg, &extract).unwrap();
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
}
