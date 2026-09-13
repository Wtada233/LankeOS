//! OpenAI 兼容 chat/completions 客户端（batch tracker yaml 生成用）。
//!
//! 配置：CLI 参数优先，环境变量兜底（`LANKEFARM_LLM_BASE_URL` / `LANKEFARM_LLM_API_KEY` / `LANKEFARM_LLM_MODEL`）。
//! 输出为**纯文本**（yaml 文档），不强制 `response_format: json_object`——JSON 对 LLM 不可靠。

use crate::error::FarmError;
use serde_json::{json, Value};

/// LLM 生成 yaml 可能数十秒；给整体请求设 300s 上限防挂死（响应无大 body，用整体超时即可，
/// 不像下载那样需要区分"总时长"与"读写空闲"）。
const LLM_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(300);

pub struct LlmClient {
    base_url: String,
    api_key: String,
    model: String,
}

impl LlmClient {
    pub fn new(
        base_url: impl Into<String>,
        api_key: impl Into<String>,
        model: impl Into<String>,
    ) -> Self {
        LlmClient {
            base_url: base_url.into(),
            api_key: api_key.into(),
            model: model.into(),
        }
    }

    /// 调 chat/completions，返回 `choices[0].message.content` 原文。
    pub fn chat(&self, system: &str, user: &str) -> Result<String, FarmError> {
        let url = format!("{}/chat/completions", self.base_url);
        let body = json!({
            "model": self.model,
            "temperature": 0,
            "messages": [
                {"role": "system", "content": system},
                {"role": "user", "content": user},
            ],
        });
        let mut req = ureq::post(&url)
            .set("Content-Type", "application/json")
            .timeout(LLM_TIMEOUT);
        if !self.api_key.is_empty() {
            req = req.set("Authorization", &format!("Bearer {}", self.api_key));
        }
        let resp = req
            .send_string(&body.to_string())
            .map_err(|e| format!("LLM API 调用失败: {e}"))?;
        let text = resp
            .into_string()
            .map_err(|e| format!("LLM 响应读取失败: {e}"))?;
        let data: Value =
            serde_json::from_str(&text).map_err(|e| format!("LLM 响应非 JSON: {e}"))?;
        data["choices"][0]["message"]["content"]
            .as_str()
            .map(|s| s.to_string())
            .ok_or_else(|| {
                format!("LLM 响应无 choices[0].message.content: {}", excerpt(&text)).into()
            })
    }
}

/// 取响应开头最多 200 个**字符**作为错误提示（不是字节）。
/// `&text[..200]` 是字节索引，落在多字节 UTF-8（中文/emoji）边界会 panic——LLM 响应含中文时
/// 这条错误路径必崩；`chars().take()` 按字符切，任何输入都安全。
fn excerpt(s: &str) -> String {
    s.chars().take(200).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn excerpt_truncates_by_chars_not_bytes() {
        // 300 个汉字 = 900 字节：按字节切 200 会 panic 或切坏多字节字符
        let s = "中".repeat(300);
        let e = excerpt(&s);
        assert_eq!(e.chars().count(), 200);
        assert!(e.chars().all(|c| c == '中'));
        // 边界：短串/空串原样
        assert_eq!(excerpt("abc"), "abc");
        assert_eq!(excerpt(""), "");
        // 恰好 200 字符不多不少
        assert_eq!(excerpt(&"a".repeat(200)), "a".repeat(200));
        assert_eq!(excerpt(&"a".repeat(201)).chars().count(), 200);
    }
}
