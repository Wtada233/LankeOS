//! state.rs — SQLite 持久状态（§11）。
//!
//! 心智模型：**容器易失，repo 持久，SQLite 记账**。
//! - job 队列 / 构建历史 / 配方 hash：跨运行持久，供 operator 用 `--state` 查看与排查；
//! - ⚠️ 当前只有写端（`set_job`/`record_build`），`job_recipe_hash`/`list_by_status` 等读端
//!   尚无调用方：**"配方 hash 变化自动 requeue"尚未实现**。BLOCKED 包的续跑目前靠 operator
//!   手动 `farm build <pkg>` 重跑。若将来实现差分 requeue，读端已就绪。
//!   注意失败路径（source 缺失 / repack / repo / index 失败）也会 `set_job(Blocked)` 落库，
//!   避免 job 永久停在 Building。

use std::path::Path;

/// farm 私有状态（SQLite 文件）。
pub struct State {
    conn: rusqlite::Connection,
}

/// job 状态机（§11：queued → building → verifying → done | blocked | skipped）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum JobStatus {
    Queued,
    Building,
    Verifying,
    Done,
    Blocked,
    Skipped,
}

impl JobStatus {
    pub fn as_str(&self) -> &'static str {
        match self {
            JobStatus::Queued => "queued",
            JobStatus::Building => "building",
            JobStatus::Verifying => "verifying",
            JobStatus::Done => "done",
            JobStatus::Blocked => "blocked",
            JobStatus::Skipped => "skipped",
        }
    }
    fn from_str(s: &str) -> Option<JobStatus> {
        match s {
            "queued" => Some(JobStatus::Queued),
            "building" => Some(JobStatus::Building),
            "verifying" => Some(JobStatus::Verifying),
            "done" => Some(JobStatus::Done),
            "blocked" => Some(JobStatus::Blocked),
            "skipped" => Some(JobStatus::Skipped),
            _ => None,
        }
    }
}

impl State {
    /// 打开（或创建）状态库。
    pub fn open(path: &Path) -> Result<State, String> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)
                .map_err(|e| format!("创建状态目录 {parent:?} 失败: {e}"))?;
        }
        let conn = rusqlite::Connection::open(path)
            .map_err(|e| format!("打开 SQLite {path:?} 失败: {e}"))?;
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
        .map_err(|e| format!("初始化 SQLite schema 失败: {e}"))?;
        Ok(State { conn })
    }

    /// upsert job 状态（含失败阶段与配方 hash）。
    pub fn set_job(
        &self,
        pkg: &str,
        status: JobStatus,
        failure_stage: Option<&str>,
        recipe_hash: Option<&str>,
    ) -> Result<(), String> {
        self.conn
            .execute(
                "INSERT INTO jobs (pkg, status, failure_stage, recipe_hash, updated_at)
                 VALUES (?1, ?2, ?3, ?4, datetime('now'))
                 ON CONFLICT(pkg) DO UPDATE SET
                    status = ?2, failure_stage = ?3, recipe_hash = ?4,
                    updated_at = datetime('now')",
                rusqlite::params![pkg, status.as_str(), failure_stage, recipe_hash],
            )
            .map_err(|e| format!("更新 job 状态失败: {e}"))?;
        Ok(())
    }

    pub fn job_status(&self, pkg: &str) -> Option<JobStatus> {
        let mut stmt = self.conn.prepare("SELECT status FROM jobs WHERE pkg=?1").ok()?;
        let s: String = stmt.query_row(rusqlite::params![pkg], |r| r.get(0)).ok()?;
        JobStatus::from_str(&s)
    }

    pub fn job_failure_stage(&self, pkg: &str) -> Option<String> {
        let mut stmt = self
            .conn
            .prepare("SELECT failure_stage FROM jobs WHERE pkg=?1")
            .ok()?;
        stmt.query_row(rusqlite::params![pkg], |r| r.get(0)).ok()
    }

    pub fn job_recipe_hash(&self, pkg: &str) -> Option<String> {
        let mut stmt = self
            .conn
            .prepare("SELECT recipe_hash FROM jobs WHERE pkg=?1")
            .ok()?;
        stmt.query_row(rusqlite::params![pkg], |r| r.get(0)).ok()
    }

    /// 删除某包的 job 条目（Ctrl+C 中断时清理当前在途条目）。
    pub fn delete_job(&self, pkg: &str) -> Result<(), String> {
        self.conn
            .execute("DELETE FROM jobs WHERE pkg=?1", rusqlite::params![pkg])
            .map(|_| ())
            .map_err(|e| format!("删除 job 条目失败: {e}"))
    }

    /// 指定状态的包列表。
    pub fn list_by_status(&self, status: JobStatus) -> Vec<String> {
        let mut stmt = match self.conn.prepare("SELECT pkg FROM jobs WHERE status=?1") {
            Ok(s) => s,
            Err(_) => return Vec::new(),
        };
        let Ok(rows) = stmt.query_map(rusqlite::params![status.as_str()], |r| r.get(0)) else {
            return Vec::new();
        };
        rows.filter_map(|r| r.ok()).collect()
    }

    pub fn record_build(&self, pkg: &str, version: &str, ok: bool) -> Result<(), String> {
        self.conn
            .execute(
                "INSERT INTO build_history (pkg, version, outcome) VALUES (?1, ?2, ?3)",
                rusqlite::params![pkg, version, if ok { "ok" } else { "failed" }],
            )
            .map_err(|e| format!("记录构建历史失败: {e}"))?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn job_status_roundtrip() {
        let path = std::env::temp_dir().join("farm-state-test.db");
        let _ = std::fs::remove_file(&path);
        let st = State::open(&path).unwrap();
        st.set_job("llvm", JobStatus::Blocked, Some("lankebuild_build"), Some("abc123"))
            .unwrap();
        assert_eq!(st.job_status("llvm"), Some(JobStatus::Blocked));
        assert_eq!(st.job_failure_stage("llvm").as_deref(), Some("lankebuild_build"));
        assert_eq!(st.job_recipe_hash("llvm").as_deref(), Some("abc123"));
        assert_eq!(st.list_by_status(JobStatus::Blocked), vec!["llvm".to_string()]);
        assert!(st.list_by_status(JobStatus::Done).is_empty());
        st.record_build("llvm", "18.1.0", true).unwrap();
        std::fs::remove_file(&path).ok();
    }

    #[test]
    fn delete_job_removes_entry() {
        let path = std::env::temp_dir().join("farm-state-del-test.db");
        let _ = std::fs::remove_file(&path);
        let st = State::open(&path).unwrap();
        st.set_job("alpha", JobStatus::Building, None, Some("h1")).unwrap();
        st.set_job("beta", JobStatus::Blocked, Some("x"), Some("h2")).unwrap();
        st.delete_job("alpha").unwrap();
        assert_eq!(st.job_status("alpha"), None);
        assert_eq!(st.job_status("beta"), Some(JobStatus::Blocked));
        std::fs::remove_file(&path).ok();
    }

    #[test]
    fn status_str_roundtrip() {
        for s in [
            JobStatus::Queued,
            JobStatus::Building,
            JobStatus::Verifying,
            JobStatus::Done,
            JobStatus::Blocked,
            JobStatus::Skipped,
        ] {
            assert_eq!(JobStatus::from_str(s.as_str()), Some(s));
        }
        assert_eq!(JobStatus::from_str("nope"), None);
    }
}
