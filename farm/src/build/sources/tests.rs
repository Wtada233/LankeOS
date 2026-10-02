//! `src/build/sources.rs` 的单元测试。
//! 从 `build/tests.rs` 与 `sources.rs` 迁来：`sources_ready` 用到 `is_skip_source` /
//! `source_filename` 这两个**本模块私有**的纯函数，只有住在本模块里才不必为测试放宽可见性
//! （它此前是 `sources.rs` 里 `#[cfg(test)] pub fn` —— 不进二进制，但按约定生产文件不放测试代码）。

use super::*;

/// 所有网络源的文件是否已就绪（source-missing 后 operator 放置文件 → 差分应重建）。
fn sources_ready(pkgs_dir: &Path, pkg: &str) -> bool {
    let Some(b) = read_lankebuild(pkgs_dir, pkg) else {
        return false;
    };
    b.sources.iter().chain(b.work_sources.iter()).all(|url| {
        if is_skip_source(url) {
            return true;
        }
        let filename = source_filename(url);
        !filename.is_empty() && pkgs_dir.join(pkg).join(filename).exists()
    })
}

/// 只写 `sources`/`work_sources` 的最小配方（本模块测试自用）。
fn write_pkg_sources(pkgs: &Path, name: &str, sources: &[&str], work_sources: &[&str]) {
    let dir = pkgs.join(name);
    std::fs::create_dir_all(&dir).unwrap();
    let json = serde_json::json!({
        "name": name,
        "version": "1.0",
        "sources": sources,
        "work_sources": work_sources,
    });
    std::fs::write(
        dir.join("LankeBUILD.json"),
        serde_json::to_string_pretty(&json).unwrap(),
    )
    .unwrap();
}

#[test]
fn sources_ready_tracks_network_files() {
    let pkgs = std::env::temp_dir().join(format!("farm-src-ready-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&pkgs);
    write_pkg_sources(
        &pkgs,
        "p",
        &[
            "file:///x",
            "git+https://github.com/a/b@v1",
            "http://127.0.0.1:1/a.tar.gz",
        ],
        &[],
    );
    // a.tar.gz 不存在 → 未就绪；file:// 与 git+ 源不参与"文件就绪"判定
    assert!(!sources_ready(&pkgs, "p"));
    std::fs::write(pkgs.join("p/a.tar.gz"), b"").unwrap();
    assert!(sources_ready(&pkgs, "p"));
    let _ = std::fs::remove_dir_all(&pkgs);
}
