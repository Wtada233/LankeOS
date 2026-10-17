#![no_main]
//! fuzz/extract_lpkg.rs — 解包路径（libarchive + `libarchive_ffi` 那层 glue）的 libFuzzer harness。
//!
//! 输入按**裸 tar** 解释：harness 自己包一层 zstd 帧再喂给 `extract_lpkg` —— 这样 libFuzzer 的变异
//! 直接作用在 **tar 结构**上，而不是被 zstd 解压器吃掉（解压器自己的健壮性不是这里要测的）。
//!
//! 跑：`cargo +nightly fuzz run extract_lpkg`（要 nightly；stable 侧的同类网是
//! `cargo test mutated_archives_never_panic_or_escape`）。

use libfuzzer_sys::fuzz_target;
use std::io::Write;

fuzz_target!(|data: &[u8]| {
    // 单帧 zstd 包装（与 lpkg/farm 的 `.lpkg` 同形）。
    let mut framed = Vec::new();
    {
        let Ok(mut enc) = zstd::stream::write::Encoder::new(&mut framed, 3) else {
            return;
        };
        if enc.write_all(data).is_err() || enc.finish().is_err() {
            return;
        }
    }

    let dir = std::env::temp_dir().join(format!("farm-fuzz-extract-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&dir);
    let pkg = dir.join("input.lpkg");
    if std::fs::write(&pkg, &framed).is_err() {
        return;
    }
    let dest = dir.join("out");
    let _ = std::fs::remove_dir_all(&dest);
    let _ = std::fs::create_dir_all(&dest);

    // 断言只有一条：**不 panic、不 UB**（返回值 Ok/Err 都合法 —— 变异可能刚好仍是合法包）。
    // 逃出解压根由 `libarchive_ffi` 的成员名判据负责，那边有单独的用例钉着。
    let _ = lankefarm::scan::extract_lpkg(&pkg, &dest);

    let _ = std::fs::remove_dir_all(&dest);
    let _ = std::fs::remove_file(&pkg);
});
