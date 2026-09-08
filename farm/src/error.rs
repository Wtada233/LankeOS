//! FarmError — 全库统一错误类型（取代历史 `Result<_, String>`）。
//!
//! 现有消息几乎全部在产生处就已格式化为带上下文的中文文本，因此本枚举的主变体是
//! `Msg(String)`：透明承载原文本，`Display` = 原消息（既有 `eprintln!("{e}")` /
//! `tr!(…, e)` 全部照常）。代码库目前几乎从不按错误类型分支——真正需要分类时，
//! 在对应调用点新增具名变体（含 `#[from]`/From 构造）即可，这是预留的可扩展方向，
//! 不预先造假枚举。
//!
//! `From<String>` / `From<&str>` 使既有 `Err("…".to_string())` 与 `.map_err(|e| format!(…))`
//! 产生 String 的调用点经 `?` 自动桥接——函数体几乎无需改动，只改签名 + 直出
//! `Err(…)` 处补 `.into()`。

#[derive(Debug, Clone, thiserror::Error)]
pub enum FarmError {
    /// 上下文化消息（保留既有全部中文描述文本）。
    #[error("{0}")]
    Msg(String),
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
}
