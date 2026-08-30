//! HTTP 获取抽象（§9 track 的探测通道）。
//!
//! `Fetcher` trait 让 track 探测可注入：测试用 `MockFetcher`（无网络），
//! 真实运行用 `RealFetcher`（ureq，进程内 HTTP，符合绑定优先）。
//! `RealFetcher` 按 URL 自动附加平台 token（GitHub/GitLab），消除 API 限流 403 噪音。

use std::collections::HashMap;

/// curl UA——镜像站/托管站对 curl 放行，对自定义或浏览器 UA 反而限流/挑战。
/// 版本与系统 curl 一致，保证与 script 模板里 curl 发出的 UA 相同。
pub const CURL_UA: &str = "curl/8.21.0";

pub trait Fetcher {
    fn get(&self, url: &str) -> Result<String, String>;

    /// 平台 token 环境变量（供 script 模板内嵌 curl 继承），无则空。
    fn token_env(&self) -> Vec<(String, String)> {
        Vec::new()
    }
}

/// 平台 token 选择（纯函数，可单测）：
/// `api.github.com` → GitHub token；gitlab API（含 `/api/v4/` 的自托管实例）→ GitLab token；其余无。
pub fn bearer_token_for<'a>(
    url: &str,
    github: &'a Option<String>,
    gitlab: &'a Option<String>,
) -> Option<&'a str> {
    if url.contains("api.github.com") {
        github.as_deref()
    } else if url.contains("gitlab") || url.contains("/api/v4/") {
        gitlab.as_deref()
    } else {
        None
    }
}

/// 真实 HTTP 获取（ureq + rustls，进程内）。
pub struct RealFetcher {
    pub github_token: Option<String>,
    pub gitlab_token: Option<String>,
}

impl RealFetcher {
    pub fn new(github_token: Option<String>, gitlab_token: Option<String>) -> Self {
        RealFetcher {
            github_token,
            gitlab_token,
        }
    }

    /// 从环境变量构造：`GITHUB_TOKEN` / `GITLAB_TOKEN`。
    pub fn from_env() -> Self {
        RealFetcher {
            github_token: std::env::var("GITHUB_TOKEN").ok(),
            gitlab_token: std::env::var("GITLAB_TOKEN").ok(),
        }
    }
}

impl Default for RealFetcher {
    fn default() -> Self {
        RealFetcher::new(None, None)
    }
}

impl Fetcher for RealFetcher {
    fn get(&self, url: &str) -> Result<String, String> {
        let mut req = ureq::get(url).set("User-Agent", CURL_UA);
        if let Some(tok) = bearer_token_for(url, &self.github_token, &self.gitlab_token) {
            req = req.set("Authorization", &format!("Bearer {tok}"));
        }
        let resp = req.call().map_err(|e| format!("GET {url}: {e}"))?;
        resp.into_string().map_err(|e| format!("GET {url}: {e}"))
    }

    fn token_env(&self) -> Vec<(String, String)> {
        let mut v = Vec::new();
        if let Some(t) = &self.github_token {
            v.push(("GITHUB_TOKEN".to_string(), t.clone()));
        }
        if let Some(t) = &self.gitlab_token {
            v.push(("GITLAB_TOKEN".to_string(), t.clone()));
        }
        v
    }
}

/// 抓取文本（如 index.txt）。失败返回错误信息。
pub fn fetch_text(url: &str) -> Result<String, String> {
    let body = ureq::get(url)
        .set("User-Agent", CURL_UA)
        .call()
        .map_err(|e| format!("GET {url}: {e}"))?;
    body.into_string().map_err(|e| format!("读 {url}: {e}"))
}

/// 下载到文件（§8.6 源预下载），带可配置重试。瞬时网络错误可自愈；耗尽后返回错误。
pub fn download_to_file(url: &str, dest: &std::path::Path, retries: u32) -> Result<(), String> {
    let attempts = retries.max(1);
    for i in 1..=attempts {
        match download_once(url, dest) {
            Ok(()) => return Ok(()),
            Err(e) if i < attempts => {
                eprintln!("{}", crate::tr!("net.download_fail", url, i, attempts, e));
                std::thread::sleep(std::time::Duration::from_secs(2));
            }
            Err(e) => return Err(e),
        }
    }
    unreachable!("重试循环已穷尽")
}

fn download_once(url: &str, dest: &std::path::Path) -> Result<(), String> {
    let resp = ureq::get(url)
        .set("User-Agent", CURL_UA)
        .call()
        .map_err(|e| format!("GET {url}: {e}"))?;
    let mut f = std::fs::File::create(dest).map_err(|e| format!("创建 {dest:?} 失败: {e}"))?;
    std::io::copy(&mut resp.into_reader(), &mut f).map_err(|e| format!("写 {dest:?} 失败: {e}"))?;
    Ok(())
}

/// 探测 source URL 是否可达：GET 并读第一个字节，确认响应正常。
/// 状态非 2xx/3xx → Err（如 404/403/5xx；redirect 由 ureq 自动跟随，最终状态为准）。
/// track 写入前用它校验新源 URL，失败时打印警告并跳过 --run（除非 --probe-fail-continue）。
/// `git+`/`file://` 源由 lpkg（libgit2）处理，非 HTTP 下载，跳过探测（不误报）。
pub fn probe_source(url: &str) -> Result<(), String> {
    if url.starts_with("git+") || url.starts_with("file://") {
        return Ok(());
    }
    let resp = match ureq::get(url).set("User-Agent", CURL_UA).call() {
        Ok(r) => r,
        Err(ureq::Error::Status(code, _)) => return Err(format!("{url} HTTP {code}")),
        Err(e) => return Err(format!("{url} 请求失败: {e}")),
    };
    let status = resp.status();
    if !(200..400).contains(&status) {
        return Err(format!("{url} HTTP {status}"));
    }
    // 读第一个字节确认 body 可流式读取（不只是 header 响应）
    let mut reader = resp.into_reader();
    let mut buf = [0u8; 1];
    let _ = std::io::Read::read(&mut reader, &mut buf)
        .map_err(|e| format!("读 {url} 响应失败: {e}"))?;
    Ok(())
}

/// Mock：预设响应，测试用（无网络）。
#[derive(Debug, Default)]
pub struct MockFetcher {
    pub responses: HashMap<String, String>,
}

impl MockFetcher {
    pub fn new(responses: HashMap<String, String>) -> Self {
        MockFetcher { responses }
    }

    pub fn entry(mut self, url: impl Into<String>, body: impl Into<String>) -> Self {
        self.responses.insert(url.into(), body.into());
        self
    }
}

impl Fetcher for MockFetcher {
    fn get(&self, url: &str) -> Result<String, String> {
        self.responses
            .get(url)
            .cloned()
            .ok_or_else(|| format!("MockFetcher: 无预设响应 {url}"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mock_fetcher_returns_preset() {
        let f = MockFetcher::new(HashMap::new()).entry("https://x/", "hello");
        assert_eq!(f.get("https://x/").unwrap(), "hello");
        assert!(f.get("https://nope/").is_err());
    }

    #[test]
    fn download_to_file_fetches_and_404() {
        // 用本地 serve.rs 起 HTTP 服务器：成功下载 + 404 报错
        let root = std::env::temp_dir().join(format!("farm-net-serve-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        std::fs::write(root.join("ok.bin"), b"data").unwrap();
        let port: u16 = 18081;
        let r = root.clone();
        let h = std::thread::spawn(move || {
            let _ = crate::serve::serve("127.0.0.1", &r, port);
        });
        std::thread::sleep(std::time::Duration::from_millis(300));

        let dest = root.join("dl.bin");
        download_to_file(&format!("http://127.0.0.1:{port}/ok.bin"), &dest, 1).unwrap();
        assert_eq!(std::fs::read(&dest).unwrap(), b"data");

        // 404 → Err（不产生文件）
        assert!(download_to_file(&format!("http://127.0.0.1:{port}/missing"), &dest, 1).is_err());

        std::fs::remove_dir_all(&root).ok();
        drop(h);
    }

    #[test]
    fn bearer_token_matches_platform_urls() {
        let gh = Some("gh-token".to_string());
        let gl = Some("gl-token".to_string());
        assert_eq!(
            bearer_token_for("https://api.github.com/repos/x/y/tags", &gh, &gl),
            Some("gh-token")
        );
        assert_eq!(
            bearer_token_for("https://gitlab.com/api/v4/projects/x", &gh, &gl),
            Some("gl-token")
        );
        // 自托管 gitlab（invent.kde.org 走 /api/v4/）
        assert_eq!(
            bearer_token_for("https://invent.kde.org/api/v4/projects/x", &gh, &gl),
            Some("gl-token")
        );
        // 普通网页/镜像：不加 token
        assert_eq!(
            bearer_token_for("https://ftp.gnu.org/gnu/x/", &gh, &gl),
            None
        );
        // 平台有 token 但 URL 不匹配 → None
        assert_eq!(
            bearer_token_for("https://api.github.com/repos/x/y/tags", &gh, &None),
            Some("gh-token")
        );
        assert_eq!(
            bearer_token_for("https://api.github.com/repos/x/y/tags", &None, &gl),
            None
        );
    }
}
