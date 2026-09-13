//! FarmError — 全库统一错误类型（取代历史 `Result<_, String>`）。
//!
//! 主变体是 `Msg(String)`：透明承载产生处已格式化的中文上下文文本，`Display` = 原消息
//! （既有 `eprintln!("{e}")` / `tr!(…, e)` 全部照常）。
//!
//! **另有三类底层错误的具名变体**（`Io`/`Http`/`Sqlite`），各带 `ctx` 上文字段 + `#[source]`
//! 底层错误。设计要点：`Display` 仍是 `"{ctx}: {source}"`，与旧的 `format!("{ctx}: {e}")`
//! **逐字节一致**——调用点与日志文案零变化，只是额外获得 `source()` 链与**类型可分类**
//! （`match` 可区分网络 / IO / DB，供将来重试或权限分支）。带路径的 io 失败若语境只有路径本身，
//! 仍走 `Msg`（`Io` 的 Display 不含路径）；需要时把 `Msg` 换成 `Io { ctx, source }` 即可（Display 不变）。
//!
//! `From<String>` / `From<&str>` 使既有 `Err("…".to_string())` 与 `.map_err(|e| format!(…))`
//! 产生 String 的调用点经 `?` 自动桥接——函数体几乎无需改动，只改签名 + 直出 `Err(…)` 处补 `.into()`。

#[derive(Debug, thiserror::Error)]
pub enum FarmError {
    /// 上下文化消息（保留既有全部中文描述文本）。
    #[error("{0}")]
    Msg(String),
    /// 底层 IO（fs / File / io::copy 等；zstd/tar 的失败也是 `io::Error`，同落此分支）+ 上下文。
    #[error("{ctx}: {source}")]
    Io {
        ctx: String,
        #[source]
        source: std::io::Error,
    },
    /// HTTP（ureq）+ 上下文。
    #[error("{ctx}: {source}")]
    Http {
        ctx: String,
        #[source]
        source: ureq::Error,
    },
    /// SQLite（rusqlite）+ 上下文。
    #[error("{ctx}: {source}")]
    Sqlite {
        ctx: String,
        #[source]
        source: rusqlite::Error,
    },
}

impl FarmError {
    /// IO + 上下文（`Display` = `"{ctx}: {source}"`，与旧 `format!("{ctx}: {e}")` 一致）。
    pub fn io(ctx: impl Into<String>, source: std::io::Error) -> Self {
        FarmError::Io {
            ctx: ctx.into(),
            source,
        }
    }

    /// HTTP + 上下文。
    pub fn http(ctx: impl Into<String>, source: ureq::Error) -> Self {
        FarmError::Http {
            ctx: ctx.into(),
            source,
        }
    }

    /// SQLite + 上下文。
    pub fn sqlite(ctx: impl Into<String>, source: rusqlite::Error) -> Self {
        FarmError::Sqlite {
            ctx: ctx.into(),
            source,
        }
    }
}

impl From<String> for FarmError {
    fn from(s: String) -> Self {
        FarmError::Msg(s)
    }
}

impl From<&str> for FarmError {
    fn from(s: &str) -> Self {
        FarmError::Msg(s.to_string())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::error::Error;

    #[test]
    fn msg_displays_raw_text() {
        let e = FarmError::Msg("读 x 失败: y".into());
        assert_eq!(e.to_string(), "读 x 失败: y");
    }

    #[test]
    fn from_string_and_str() {
        let e1: FarmError = "abc".to_string().into();
        let e2: FarmError = "abc".into();
        assert_eq!(e1.to_string(), e2.to_string());
    }

    #[test]
    fn io_variant_display_matches_old_format_and_keeps_source() {
        // Display 必须与旧的 `format!("{ctx}: {e}")` 逐字节一致（调用点文案零变化）
        let src = std::io::Error::new(std::io::ErrorKind::NotFound, "no such file");
        let e = FarmError::io("创建 /x 失败", src);
        assert_eq!(e.to_string(), "创建 /x 失败: no such file");
        // 但多了 source 链（旧 Msg 没有）
        assert!(e.source().is_some(), "Io 变体应保留 source 链");
        assert_eq!(e.source().unwrap().to_string(), "no such file");
    }

    #[test]
    fn msg_variant_has_no_source() {
        assert!(FarmError::Msg("x".into()).source().is_none());
    }
}
