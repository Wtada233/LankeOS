//! OpenAI 兼容 chat/completions 客户端（batch tracker yaml 生成用）。
//!
//! 配置：CLI 参数优先，环境变量兜底（`LANKEFARM_LLM_BASE_URL` / `LANKEFARM_LLM_API_KEY` / `LANKEFARM_LLM_MODEL`）。
//! 输出为**纯文本**（yaml 文档），不强制 `response_format: json_object`——JSON 对 LLM 不可靠。

use serde_json::{json, Value};

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
    pub fn chat(&self, system: &str, user: &str) -> Result<String, String> {
        let url = format!("{}/chat/completions", self.base_url);
        let body = json!({
            "model": self.model,
            "temperature": 0,
            "messages": [
                {"role": "system", "content": system},
                {"role": "user", "content": user},
            ],
        });
        let mut req = ureq::post(&url).set("Content-Type", "application/json");
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
                format!(
                    "LLM 响应无 choices[0].message.content: {}",
                    &text[..text.len().min(200)]
                )
            })
    }
}
