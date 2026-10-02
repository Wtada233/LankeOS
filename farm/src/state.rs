//! state.rs — SQLite 持久状态（§11）。
//!
//! 心智模型：**容器易失，repo 持久，SQLite 记账**。
//! - **只写**：job 状态（`building` / `done` / `blocked` / `skipped`）与构建历史（`build_history`）。
//!   `--state <db>` 落库后，operator 直接用 sqlite3 查——批式 CLI 不需要读 API。
//! - **没有读端，这是有意的**：farm 是批式 CLI 而非常驻 daemon，所以"配方 hash 变了就重建"由
//!   `recipe_hash` + `.build_ok` + `farm validate` 在**每次运行时**评估（`build::has_build_ok`），
//!   无需按 job 状态在后台 requeue。BLOCKED 包的续跑靠 operator 手动 `farm build <pkg>`。
//!   （曾按"将来可能常驻"预留了读端与 `queued`/`verifying` 两态；已明确**暂不做 daemon** ⇒ 删除，
//!   将来真要常驻时再按那时的形态补。）
//! - 失败路径（source 缺失 / repack / repo / index 失败）也会 `set_job(Blocked)` 落库，
//!   避免 job 永久停在 Building。

use crate::error::FarmError;
use std::path::Path;

/// farm 私有状态（SQLite 文件）。
pub struct State {
    conn: rusqlite::Connection,
}

/// job 状态机（§11：building → done | blocked | skipped）。
/// 只有这四个：`queued`/`verifying` 原是给"常驻 daemon"预留的中途态，已随"暂不做 daemon"删除
/// （farm 是批式 CLI，一个包要么在建、要么终态）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum JobStatus {
    Building,
    Done,
    Blocked,
    Skipped,
}

impl JobStatus {
    pub fn as_str(&self) -> &'static str {
        match self {
            JobStatus::Building => "building",
            JobStatus::Done => "done",
            JobStatus::Blocked => "blocked",
            JobStatus::Skipped => "skipped",
        }
    }
}

impl State {
    /// 打开（或创建）状态库。
    pub fn open(path: &Path) -> Result<State, FarmError> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)
                .map_err(|e| FarmError::io(format!("创建状态目录 {parent:?} 失败"), e))?;
        }
        let conn = rusqlite::Connection::open(path)
            .map_err(|e| FarmError::sqlite(format!("打开 SQLite {path:?} 失败"), e))?;
        conn.execute_batch(
            "CREATE TABLE IF NOT EXISTS jobs (
                pkg TEXT PRIMARY KEY,
                status TEXT NOT NULL,
                failure_stage TEXT,
                recipe_hash TEXT,
                updated_at TEXT NOT NULL DEFAULT (datetime('now'))
            );
            CREATE TABLE IF NOT EXISTS build_history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                pkg TEXT NOT NULL,
                version TEXT,
                outcome TEXT NOT NULL,
                at TEXT NOT NULL DEFAULT (datetime('now'))
            );
            ",
        )
        .map_err(|e| FarmError::sqlite("初始化 SQLite schema 失败", e))?;
        Ok(State { conn })
    }

    /// upsert job 状态（含失败阶段与配方 hash）。
    pub fn set_job(
        &self,
        pkg: &str,
        status: JobStatus,
        failure_stage: Option<&str>,
        recipe_hash: Option<&str>,
    ) -> Result<(), FarmError> {
        self.conn
            .execute(
                "INSERT INTO jobs (pkg, status, failure_stage, recipe_hash, updated_at)
                 VALUES (?1, ?2, ?3, ?4, datetime('now'))
                 ON CONFLICT(pkg) DO UPDATE SET
                    status = ?2, failure_stage = ?3, recipe_hash = ?4,
                    updated_at = datetime('now')",
                rusqlite::params![pkg, status.as_str(), failure_stage, recipe_hash],
            )
            .map_err(|e| FarmError::sqlite("更新 job 状态失败", e))?;
        Ok(())
    }

    /// 删除某包的 job 条目（Ctrl+C 中断时清理当前在途条目）。
    pub fn delete_job(&self, pkg: &str) -> Result<(), FarmError> {
        self.conn
            .execute("DELETE FROM jobs WHERE pkg=?1", rusqlite::params![pkg])
            .map(|_| ())
            .map_err(|e| FarmError::sqlite("删除 job 条目失败", e))
    }

    pub fn record_build(&self, pkg: &str, version: &str, ok: bool) -> Result<(), FarmError> {
        self.conn
            .execute(
                "INSERT INTO build_history (pkg, version, outcome) VALUES (?1, ?2, ?3)",
                rusqlite::params![pkg, version, if ok { "ok" } else { "failed" }],
            )
            .map_err(|e| FarmError::sqlite("记录构建历史失败", e))?;
        Ok(())
    }
}

#[cfg(test)]
mod tests;
