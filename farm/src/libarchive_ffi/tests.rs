//! `src/libarchive2_compat.rs` 的验收网。
//!
//! 这些用例同时是**兼容层的合同**：换成别的实现（升级 crate / 换回 tar 系 / 换别的绑定）时，
//! 让它们全绿就是"新实现满足了 farm 需要的那几条"。fixture 与它们的出处见各用例注释。
//!
//! 仍是同一父模块的子模块 ⇒ `use super::*` 与 crate 内私有项照旧可用。

use super::*;
use std::fs;
use std::path::PathBuf;

/// 把给定 tar 字节包成 .lpkg（单帧 zstd），返回 (包路径, 解包目标目录)。
/// 目录带自增后缀，避免并行测试互相删目录（与本文件其他测试同款）。
fn wrap_lpkg(name: &str, tar: &[u8]) -> (PathBuf, PathBuf) {
    use std::io::Write;
    static COUNTER: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    let id = COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let dir =
        std::env::temp_dir().join(format!("farm-scan-lpkg-{name}-{}-{id}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();
    let lpkg = dir.join("pkg.lpkg");
    let f = fs::File::create(&lpkg).unwrap();
    let mut enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
    enc.write_all(tar).unwrap();
    enc.finish().unwrap();
    let dest = dir.join("extract");
    fs::create_dir_all(&dest).unwrap();
    (lpkg, dest)
}

/// 回归网：GNU sparse 1.0 的成员必须解到 pax `GNU.sparse.name` 给出的**真名**上。
///
/// 成员名是惯例名 `content/usr/lib/locale/GNUSparseFile.0/sparse.bin`、真名放在 pax 键里
/// （libarchive 的 `pax_restricted` 写方 = lpkg 打包器同款写法，fixture 就是它生成的）。tar crate
/// 不认这个键，会把惯例名当真路径 —— glibc 的 locale-archive 就是这么被 repack 固化进发布档、
/// 装完 locales 全丢（gettext 回落英文）的。fixture sha256
/// `ccd4912f3cb23d49b3656d164bce3d69461c8a18cabcdc6a3d98dc414c94654d`。
#[test]
fn gnu_sparse_pax10_member_lands_on_real_name() {
    let (lpkg, dest) = wrap_lpkg("sparse", include_bytes!("testdata/gnu-sparse-pax10.tar"));
    crate::scan::extract_lpkg(&lpkg, &dest).unwrap();

    let real = dest.join("content/usr/lib/locale/sparse.bin");
    let data = fs::read(&real).unwrap_or_else(|e| {
        panic!("真名路径 {real:?} 上应有文件（惯例名 GNUSparseFile.0/ 不得落地）: {e}")
    });
    assert_eq!(
        data.len(),
        1_052_672,
        "大小应为 GNU.sparse.realsize（空洞已展开）"
    );
    assert_eq!(data[0], b'A', "首块数据");
    assert_eq!(data[4096], 0, "空洞应补零");
    assert_eq!(data[1_048_576], b'Z', "尾块数据");
    assert_eq!(data[data.len() - 1], b'Z');
    assert!(
        !dest.join("content/usr/lib/locale/GNUSparseFile.0").exists(),
        "惯例名目录不得出现在解包结果里"
    );
}

/// 常规包照常解：成员名带 `./` 前缀（farm 自己的 fixture 用 `append_dir_all(".", …)` 打）也要落到
/// 正确路径上。
#[test]
fn plain_lpkg_members_extract() {
    let mut tar_bytes = Vec::new();
    {
        let mut b = tar::Builder::new(&mut tar_bytes);
        let mut h = tar::Header::new_gnu();
        h.set_size(2);
        h.set_mode(0o644);
        h.set_cksum();
        b.append_data(&mut h, "./content/usr/share/plain.txt", &b"hi"[..])
            .unwrap();
        b.finish().unwrap();
    }
    let (lpkg, dest) = wrap_lpkg("plain", &tar_bytes);
    crate::scan::extract_lpkg(&lpkg, &dest).unwrap();
    assert_eq!(
        fs::read(dest.join("content/usr/share/plain.txt")).unwrap(),
        b"hi"
    );
}

/// 成员名越界必须**当场报错**：libarchive 的写盘端默认允许绝对路径，而
/// `extract_dir.join("/etc/x")` 就是 `/etc/x` —— 不自己筛就等于把解包根开到宿主根。
#[test]
fn extract_rejects_escaping_member_paths() {
    for bad in [
        "/tmp/farm-escape-canary",
        "../escape",
        "content/../../escape",
    ] {
        // tar-rs 自己就拒写绝对路径（"paths in archives must be relative"），只能手搓头。
        let tar_bytes = raw_tar_with_name(bad, b"x");
        let (lpkg, dest) = wrap_lpkg("escape", &tar_bytes);
        let err = crate::scan::extract_lpkg(&lpkg, &dest)
            .expect_err(&format!("成员 {bad:?} 越界却解成功了"))
            .to_string();
        assert!(err.contains("越界"), "越界成员应报越界，实际: {err}");
    }
    assert!(
        !Path::new("/tmp/farm-escape-canary").exists(),
        "绝对路径成员绝不能落到宿主根上"
    );
}

/// 硬链接成员要按硬链接还原（同一 inode），不能退化成空普通文件 —— crate 的 `extract_with_flags`
/// 与 `FileType` 都不带硬链接概念，漏搬 `hardlink` 目标就会静默损坏。
#[test]
fn extract_preserves_hardlinks() {
    use std::os::unix::fs::MetadataExt;
    let mut tar_bytes = Vec::new();
    {
        let mut b = tar::Builder::new(&mut tar_bytes);
        let mut h = tar::Header::new_gnu();
        h.set_size(2);
        h.set_mode(0o644);
        h.set_cksum();
        b.append_data(&mut h, "content/a.txt", &b"hi"[..]).unwrap();
        let mut h2 = tar::Header::new_gnu();
        h2.set_size(0);
        h2.set_mode(0o644);
        h2.set_entry_type(tar::EntryType::Link);
        h2.set_link_name("content/a.txt").unwrap();
        h2.set_cksum();
        b.append_data(&mut h2, "content/b.txt", &b""[..]).unwrap();
        b.finish().unwrap();
    }
    let (lpkg, dest) = wrap_lpkg("hardlink", &tar_bytes);
    crate::scan::extract_lpkg(&lpkg, &dest).unwrap();
    let a = fs::metadata(dest.join("content/a.txt")).unwrap();
    let b = fs::metadata(dest.join("content/b.txt")).unwrap();
    assert_eq!(a.ino(), b.ino(), "硬链接成员应与目标同一 inode");
    assert_eq!(fs::read(dest.join("content/b.txt")).unwrap(), b"hi");
}

/// 总长 > 4096 字节、但每个路径分量都 ≤255 的成员名（逼"总长上限"那条判据）。
fn long_total_name() -> String {
    let mut s = String::from("content");
    for _ in 0..20 {
        s.push('/');
        s.push_str(&"a".repeat(250));
    }
    s.push_str("/f");
    s
}

/// 用 tar-rs 造一个单成员 tar（长名由它自己发 GNU 长名条目）。
fn tar_with_name(name: &str) -> Vec<u8> {
    let mut out = Vec::new();
    {
        let mut b = tar::Builder::new(&mut out);
        let mut h = tar::Header::new_gnu();
        h.set_size(1);
        h.set_mode(0o644);
        h.set_cksum();
        b.append_data(&mut h, name, &b"x"[..]).unwrap();
        b.finish().unwrap();
    }
    out
}

/// 手搓一个单成员 tar（512 字节 ustar 头 + 数据 + 两块结尾零）。
/// 用途：造 tar-rs 拒绝写出的畸形成员名（绝对路径等）来喂解包器。
fn raw_tar_with_name(name: &str, data: &[u8]) -> Vec<u8> {
    fn put(h: &mut [u8; 512], off: usize, s: &[u8]) {
        h[off..off + s.len()].copy_from_slice(s);
    }
    let mut h = [0u8; 512];
    put(&mut h, 0, name.as_bytes());
    put(&mut h, 100, b"0000644\0");
    put(&mut h, 108, b"0000000\0");
    put(&mut h, 116, b"0000000\0");
    put(&mut h, 124, format!("{:011o}\0", data.len()).as_bytes());
    put(&mut h, 136, b"00000000000\0");
    h[156] = b'0';
    put(&mut h, 257, b"ustar\0");
    put(&mut h, 263, b"00");
    put(&mut h, 148, b"        "); // 校验和字段先当空格
    let sum: u32 = h.iter().map(|b| u32::from(*b)).sum();
    put(&mut h, 148, format!("{sum:06o}\0 ").as_bytes());
    let mut out = h.to_vec();
    out.extend_from_slice(data);
    out.resize(out.len().div_ceil(512) * 512, 0);
    out.extend_from_slice(&[0u8; 1024]);
    out
}

/// ACL 往返：带 POSIX ACL 的成员解出来（ACL 落到磁盘）→ 重打成 .lpkg → 包里仍有 `SCHILY.acl.access`。
///
/// 断言直接打在**我们自己产物的字节**上（zstd 解帧后搜 pax 文本）：既证读侧把 ACL 还原到了磁盘、
/// 也证写侧（磁盘读取器）把真正的 ACL 记录发了出去 —— 手工把 ACL 当裸 xattr 写是不生效的
/// （lpkg `packer.cpp` 记过这个坑）。fixture 由 libarchive 的 `pax_restricted` 写方生成，sha256
/// `d1fb5ef2baf70490bdba485a8aa81f071d5f13ec5232f0d75357dbbb06a09173`。
#[test]
fn acl_survives_repack_roundtrip() {
    use std::io::Write;

    static COUNTER: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    let id = COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let dir = std::env::temp_dir().join(format!("farm-acl-{}-{id}", std::process::id()));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).unwrap();

    // 1) 把带 ACL 的 fixture 包成 .lpkg → 解出来（ACL 应落到磁盘）
    let lpkg = dir.join("acl.lpkg");
    {
        let f = fs::File::create(&lpkg).unwrap();
        let mut enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
        enc.write_all(include_bytes!("testdata/acl-pax.tar"))
            .unwrap();
        enc.finish().unwrap();
    }
    let extract = dir.join("extract");
    crate::scan::extract_lpkg(&lpkg, &extract).unwrap();
    let acl_file = extract.join("content/etc/acl-test.conf");
    assert!(acl_file.exists(), "fixture 成员应解到 {acl_file:?}");

    // 2) 重打（走磁盘读取器 → 带上 ACL）
    let repacked = dir.join("repacked.lpkg");
    crate::repack::pack_dir_tar(&extract, &repacked, 3).unwrap();

    // 3) 读回**原始字节**：pax 头是纯文本，直接搜
    let raw = zstd::decode_all(fs::File::open(&repacked).unwrap()).unwrap();
    let text = String::from_utf8_lossy(&raw);
    assert!(
        text.contains("SCHILY.acl.access="),
        "重打后的包里该成员应带 SCHILY.acl.access"
    );
    assert!(
        text.contains("user:1000:rwx"),
        "ACL 应原样往返（含命名用户项）；实际片段: {}",
        &text[..text.len().min(400)]
    );
    let _ = fs::remove_dir_all(&dir);
}

/// 范围守卫：**只有本模块**能直接碰 libarchive（判据 = 源码里出现 `extern "C"`，或 C API 前缀
/// `archive_read_` / `archive_write_` / `archive_entry_`）。这条一坏，"换实现只改一处"就不成立，
/// FFI 与它的契约会渗回业务代码。
#[test]
fn only_compat_touches_libarchive2() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    let mut offenders: Vec<String> = Vec::new();
    let mut stack = vec![root.clone()];
    while let Some(dir) = stack.pop() {
        for entry in fs::read_dir(&dir).unwrap() {
            let p = entry.unwrap().path();
            if p.is_dir() {
                stack.push(p);
                continue;
            }
            if p.extension().and_then(|e| e.to_str()) != Some("rs") {
                continue;
            }
            // 兼容层自身（文件与目录，含本文件的断言文案）不在范围内。
            let rel = p.strip_prefix(&root).unwrap();
            if rel == Path::new("libarchive_ffi.rs") || rel.starts_with("libarchive_ffi") {
                continue;
            }
            let text = fs::read_to_string(&p).unwrap();
            if text.contains("extern \"C\"")
                || text.contains("archive_read_")
                || text.contains("archive_write_")
                || text.contains("archive_entry_")
            {
                offenders.push(p.strip_prefix(&root).unwrap().display().to_string());
            }
        }
    }
    assert!(
        offenders.is_empty(),
        "只有 libarchive_ffi.rs 可以直接碰 libarchive，实际还有: {offenders:?}"
    );
}

/// 变异输入：定向畸变 + 字节级随机变异，断言**只许 Ok/Err** —— 不许 panic、不许把东西写到解压根之外。
///
/// ⚠️ **这不是真 fuzz**，别把它当成覆盖率制导的 fuzzing：
///   · 没有覆盖率反馈（不知道哪些分支没走到），变异是**随机撒点**，命中新路径的概率极低；
///   · 没有 sanitizer（ASAN/MSAN），**不检内存错误** —— FFI 那层的 use-after-free/越界读它看不见
///     （这条最要紧：这个文件的 bug 恰恰长在指针生命周期上）；
///   · 变异体是固定 300 轮 + 手搓样本，只能保证"已知形态不会再崩"。
/// 它唯一的作用是**每次 `cargo test` 都能跑**的那层守门员（护住已知的畸形形态与逃逸向量），
/// 换实现时当回归网用。真 fuzz 在 `fuzz/`（cargo-fuzz + libFuzzer + ASAN，要 nightly）：
///     cargo +nightly fuzz run extract_lpkg
/// 只要这台机器上有 nightly，就该跑那条而不是只依赖本测试。
#[test]
fn mutated_archives_never_panic_or_escape() {
    eprintln!("warning: mutated_archives_never_panic_or_escape 不能代替 fuzz，见fuzz/");
    // 逃逸金丝雀：绝对路径成员若能落地，写的就是这里（只钉这一条路径，足以盯住"绝对路径逃逸"这一向量）。
    const CANARY: &str = "/tmp/farm-fuzz-canary";
    let _ = fs::remove_file(CANARY);

    // ① 定向畸变：这些名字 tar-rs 自己就拒写，只能手搓头（`raw_tar_with_name`）。
    let crafted: Vec<(&str, Vec<u8>)> = vec![
        ("absolute", raw_tar_with_name("/tmp/farm-fuzz-canary", b"x")),
        ("dotdot", raw_tar_with_name("../escape", b"x")),
        ("control", raw_tar_with_name("content/bad\nname", b"x")),
        // 长名走 pax/GNU 长名条目（tar 的 100 字节头字段塞不下），交给 tar-rs 造。
        // longname：总长 > 4096 但每个分量 ≤255（逼的是总长那条判据）
        ("longname", tar_with_name(&long_total_name())),
        // longcomp：单分量 304 > NAME_MAX 255（逼的是分量那条判据）
        (
            "longcomp",
            tar_with_name(&format!("content/{}.txt", "a".repeat(300))),
        ),
        // 对照：合法名字必须能解 —— 防"守得太狠，把正常包也拒了"。
        ("ok", raw_tar_with_name("content/ok.txt", b"x")),
    ];
    for (label, bytes) in crafted {
        let (lpkg, dest) = wrap_lpkg(&format!("mut-{label}"), &bytes);
        let res = crate::scan::extract_lpkg(&lpkg, &dest);
        if label == "ok" {
            assert!(res.is_ok(), "合法成员名不该被拒: {res:?}");
        } else {
            assert!(res.is_err(), "{label} 应被拒（实际解成功了）");
        }
        assert!(
            !Path::new(CANARY).exists(),
            "{label}: 解包把文件写到了解压根之外"
        );
        let _ = fs::remove_dir_all(dest.parent().unwrap());
    }
    let _ = fs::remove_file(CANARY);

    // ② 重名成员（守卫存在的首要理由）：同名两份 → 整包拒绝 —— 不能"流式读第一份、落地最后一份"。
    let mut dup = Vec::new();
    {
        let mut b = tar::Builder::new(&mut dup);
        for body in [&b"first"[..], &b"second"[..]] {
            let mut h = tar::Header::new_gnu();
            h.set_size(body.len() as u64);
            h.set_mode(0o644);
            h.set_cksum();
            b.append_data(&mut h, "metadata.json", body).unwrap();
        }
        b.finish().unwrap();
    }
    let (lpkg, dest) = wrap_lpkg("mut-dup", &dup);
    let err = crate::scan::extract_lpkg(&lpkg, &dest)
        .expect_err("重名成员应整包拒绝")
        .to_string();
    assert!(err.contains("重名"), "错误应点名重名，实际: {err}");
    let _ = fs::remove_dir_all(dest.parent().unwrap());

    // ③ 字节级变异：位翻 / 截断 / 区间清零，确定性伪随机（xorshift，失败可原样复现）。
    let bases: [&[u8]; 2] = [
        include_bytes!("testdata/gnu-sparse-pax10.tar"),
        include_bytes!("testdata/acl-pax.tar"),
    ];
    let mut seed: u64 = 0x9E37_79B9_7F4A_7C15;
    for round in 0..300usize {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        let mut bytes = bases[round % bases.len()].to_vec();
        match round % 3 {
            0 => {
                let i = (seed as usize) % bytes.len();
                bytes[i] ^= (seed >> 32) as u8;
            }
            1 => {
                let cut = ((seed >> 8) as usize) % bytes.len();
                bytes.truncate(cut.max(1));
            }
            _ => {
                let start = (seed as usize) % bytes.len();
                let len = (((seed >> 24) as usize) % 512).max(1);
                let end = (start + len).min(bytes.len());
                for b in bytes[start..end].iter_mut() {
                    *b = 0;
                }
            }
        }
        let (lpkg, dest) = wrap_lpkg(&format!("mut-{round}"), &bytes);
        // 只要不 panic 就算过：畸变可能刚好仍是合法包（Ok），也可能被 guard/libarchive 拒（Err）。
        let _ = crate::scan::extract_lpkg(&lpkg, &dest);
        assert!(!Path::new(CANARY).exists(), "round {round}: 逃出解压根");
        let _ = fs::remove_dir_all(dest.parent().unwrap());
    }
}
