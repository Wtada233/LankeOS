#![no_main]
//! fuzz/read_member.rs — 流式取单个成员那条路（`archive_read_data` + 名字匹配）的 libFuzzer harness。
//!
//! 与 `extract_lpkg` 同构：输入按裸 tar 解释、自己包 zstd 帧；额外把输入的前若干字节当"要找的成员名"，
//! 让名字匹配那一段也被变异覆盖。`read_lpkg_metadata` 走的就是这条（seed/verify 的热路径）。
//!
//! 跑：`cargo +nightly fuzz run read_member`。

use libfuzzer_sys::fuzz_target;
use std::io::Write;

fuzz_target!(|data: &[u8]| {
    // 前 64 字节当"要找的成员名"，其余当 tar 字节（这样名字匹配与 tar 解析两条都被覆盖）。
    let (wanted, tar_bytes) = data.split_at(data.len().min(64));
    let wanted = String::from_utf8_lossy(wanted).into_owned();

    let mut framed = Vec::new();
    {
        let Ok(mut enc) = zstd::stream::write::Encoder::new(&mut framed, 3) else {
            return;
        };
        if enc.write_all(tar_bytes).is_err() || enc.finish().is_err() {
            return;
        }
    }

    let dir = std::env::temp_dir().join(format!("farm-fuzz-readmember-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&dir);
    let pkg = dir.join("input.lpkg");
    if std::fs::write(&pkg, &framed).is_err() {
        return;
    }

    let _ = lankefarm::libarchive_ffi::read_member(&pkg, &wanted);

    let _ = std::fs::remove_file(&pkg);
});
