//! `src/state.rs` 的单元测试（从生产文件搬出：那里测试比生产还长，正文被淹没）。
//! 仍是同一父模块的子模块 ⇒ `use super::*` 与 crate 内私有项照旧可用。

use super::*;

#[test]
fn set_job_persists_status_stage_and_hash() {
    let path = std::env::temp_dir().join("farm-state-test.db");
    let _ = std::fs::remove_file(&path);
    let st = State::open(&path).unwrap();
    st.set_job(
        "llvm",
        JobStatus::Blocked,
        Some("lankebuild_build"),
        Some("abc123"),
    )
    .unwrap();
    // 没有读 API（见模块文档）⇒ 直接查 SQLite 验证落库
    let (status, stage, hash): (String, Option<String>, Option<String>) = st
        .conn
        .query_row(
            "SELECT status, failure_stage, recipe_hash FROM jobs WHERE pkg='llvm'",
            [],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
        )
        .unwrap();
    assert_eq!(status, "blocked");
    assert_eq!(stage.as_deref(), Some("lankebuild_build"));
    assert_eq!(hash.as_deref(), Some("abc123"));
    st.record_build("llvm", "18.1.0", true).unwrap();
    std::fs::remove_file(&path).ok();
}

#[test]
fn delete_job_removes_entry() {
    let path = std::env::temp_dir().join("farm-state-del-test.db");
    let _ = std::fs::remove_file(&path);
    let st = State::open(&path).unwrap();
    st.set_job("alpha", JobStatus::Building, None, Some("h1"))
        .unwrap();
    st.set_job("beta", JobStatus::Blocked, Some("x"), Some("h2"))
        .unwrap();
    st.delete_job("alpha").unwrap();
    let n: i64 = st
        .conn
        .query_row("SELECT count(*) FROM jobs", [], |r| r.get(0))
        .unwrap();
    assert_eq!(n, 1, "alpha 应被删除");
    let beta: String = st
        .conn
        .query_row("SELECT status FROM jobs WHERE pkg='beta'", [], |r| r.get(0))
        .unwrap();
    assert_eq!(beta, "blocked");
    std::fs::remove_file(&path).ok();
}

#[test]
fn status_str_matches_db_vocabulary() {
    for (st, want) in [
        (JobStatus::Building, "building"),
        (JobStatus::Done, "done"),
        (JobStatus::Blocked, "blocked"),
        (JobStatus::Skipped, "skipped"),
    ] {
        assert_eq!(st.as_str(), want);
    }
}
