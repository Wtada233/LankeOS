//! HTTP 获取抽象（§9 track 的探测通道）。
//!
//! `Fetcher` trait 让 track 探测可注入：测试用 `MockFetcher`（无网络），
//! 真实运行用 `RealFetcher`（ureq，进程内 HTTP，符合绑定优先）。
//! `RealFetcher` 按 URL 自动附加平台 token（GitHub/GitLab），消除 API 限流 403 噪音。

use crate::error::FarmError;
use std::collections::HashMap;
use std::sync::LazyLock;
use std::time::Duration;

/// 连接超时（TCP + TLS 握手）。ureq 2 默认已是 30s，这里显式化。
const CONNECT_TIMEOUT: Duration = Duration::from_secs(30);
/// **单次读写空闲**超时（不是请求总时长）：卡死/半开连接在此报错；慢速大文件下载不受总时长限制
/// （ureq 2 默认 read/write **无超时**，可永久 hang；connect 默认 30s 已存在）。
const READ_IDLE_TIMEOUT: Duration = Duration::from_secs(60);
const WRITE_IDLE_TIMEOUT: Duration = Duration::from_secs(60);

static AGENT: LazyLock<ureq::Agent> =
    LazyLock::new(|| build_agent(CONNECT_TIMEOUT, READ_IDLE_TIMEOUT, WRITE_IDLE_TIMEOUT));

/// 构造 agent（测试注入短超时用，避免为超时写一个真等 60s 的测试）。
pub fn build_agent(connect: Duration, read: Duration, write: Duration) -> ureq::Agent {
    ureq::builder()
        .timeout_connect(connect)
        .timeout_read(read)
        .timeout_write(write)
        .build()
}

/// 进程内共享 HTTP agent（连接池 + 读写空闲超时）。所有出站 HTTP（下载/探测/track）都走它。
pub fn http_agent() -> &'static ureq::Agent {
    &AGENT
}

/// 用 libgit2（git 协议 ref advertisement）列出远端**全部** tag——`git ls-remote --tags` 的进程内等价。
///
/// 为什么不用平台 REST 的 tags 端点：那是**分页窗口**（GitHub 默认 30 / 最大 100，GitLab 上限 100，
/// 都靠翻页），而且**顺序与版本无关**——tracker 要的是"最新版本"，只看首页会取到老 tag
/// （hdf5 事故：`/repos/HDFGroup/hdf5/tags` 首页 30 个全是老 tag，`max` 够不到 2.2.0）。
/// ref advertisement 一次给全，与 tag 数量无关（不需要猜 per_page 要多大）。
///
/// 凭据策略（用户规则）：**有 token 就用，没有就走匿名**——不用凭据助手、不做其它 fallback：
/// - `token = Some(_)`：设凭据回调，服务端要认证时用 PAT（`USER_PASS_PLAINTEXT`，用户名任意：
///   GitHub 惯用 `x-access-token`、GitLab 用 `oauth2`）；**服务端允许匿名时 libgit2 不会调它**，
///   所以 public 仓库带不带 token 都正常走。
/// - `token = None`：不设回调 → libgit2 匿名连接（private 仓库会失败，属预期）。
/// - 回调内若服务端只接受我们不支持的凭据类型（如 SSH key）→ 交回空凭据（退回匿名）而非硬失败。
pub fn list_remote_tags(repo_url: &str, token: Option<&str>) -> Result<Vec<String>, FarmError> {
    let mut remote = git2::Remote::create_detached(repo_url)
        .map_err(|e| format!("创建 remote {repo_url} 失败: {e}"))?;
    let mut callbacks = git2::RemoteCallbacks::new();
    if let Some(tok) = token {
        let tok = tok.to_string();
        callbacks.credentials(move |_url, _user_from_url, allowed| {
            if allowed.contains(git2::CredentialType::USER_PASS_PLAINTEXT) {
                git2::Cred::userpass_plaintext("x-access-token", &tok)
            } else {
                // 只认我们不支持的凭据类型 → 退回"无凭据"（匿名），不在这里硬失败
                git2::Cred::default()
            }
        });
    }
    remote
        .connect_auth(git2::Direction::Fetch, Some(callbacks), None)
        .map_err(|e| format!("连接 {repo_url} 失败: {e}"))?;
    let heads = remote
        .list()
        .map_err(|e| format!("列出 {repo_url} 的 refs 失败: {e}"))?;
    let mut tags: Vec<String> = heads
        .iter()
        .filter_map(|h| h.name().strip_prefix("refs/tags/"))
        // annotated tag 会同时出现 `refs/tags/v1` 与 peeled 的 `refs/tags/v1^{}` → 归一
        .map(|t| t.trim_end_matches("^{}").to_string())
        .collect();
    tags.sort();
    tags.dedup();
    Ok(tags)
}

/// 裸 git URL 的平台 token 选择（按 host，纯函数可单测）：
/// `github.com` / `*.github.com` → GitHub token；`gitlab.com` / `*.gitlab.com` / host 段含 `gitlab`
/// （自托管）→ GitLab token；其余（含自托管 GitHub/其它）→ 无（匿名）。
fn git_token_for<'a>(
    url: &str,
    github: &'a Option<String>,
    gitlab: &'a Option<String>,
) -> Option<&'a str> {
    let host = url_host(url);
    if host == "github.com" || host.ends_with(".github.com") {
        github.as_deref()
    } else if host == "gitlab.com"
        || host.ends_with(".gitlab.com")
        || host.split('.').any(|l| l == "gitlab")
    {
        gitlab.as_deref()
    } else {
        None
    }
}

/// curl UA——镜像站/托管站对 curl 放行，对自定义或浏览器 UA 反而限流/挑战。
/// 版本与系统 curl 一致，保证与 script 模板里 curl 发出的 UA 相同。
pub const CURL_UA: &str = "curl/8.21.0";

pub trait Fetcher {
    fn get(&self, url: &str) -> Result<String, FarmError>;

    /// 列出远端仓库的**全部 tag**（`git ls-remote --tags` 语义：一次拿全，无分页窗口）。
    ///
    /// `repo_url` = 裸 git URL（如 `https://github.com/owner/repo.git`）。
    /// **不要**退回平台 REST 的 tags 端点：那是**分页**的（GitHub 默认 30 / 最大 100，
    /// GitLab `per_page` 上限 100，都靠翻页），且**顺序与版本无关**——tracker 要的是"最新版本"，
    /// 只看首页会取到老 tag（hdf5 事故：首页 30 个全是老 tag，`max` 够不到 2.2.0）。
    fn list_tags(&self, repo_url: &str) -> Result<Vec<String>, FarmError> {
        Err(format!("list_tags 未实现: {repo_url}").into())
    }

    /// 平台 token 环境变量（供 script 模板内嵌 curl 继承），无则空。
    fn token_env(&self) -> Vec<(String, String)> {
        Vec::new()
    }
}

/// 取 URL 的 host（手写解析，无新依赖）：剥 scheme → 截到首个 `/`|`?`|`#` → 去 userinfo 与端口 → 小写。
fn url_host(url: &str) -> String {
    let after_scheme = url.split_once("://").map(|(_, r)| r).unwrap_or(url);
    let authority = after_scheme.split(['/', '?', '#']).next().unwrap_or("");
    let host_port = authority.rsplit('@').next().unwrap_or(authority); // 去 userinfo
    let host = host_port.split(':').next().unwrap_or(host_port); // 去端口
    host.to_ascii_lowercase()
}

/// 取 URL 的 path（含前导 `/`，不含 query/fragment）。
fn url_path(url: &str) -> &str {
    let after_scheme = url.split_once("://").map(|(_, r)| r).unwrap_or(url);
    match after_scheme.find('/') {
        Some(i) => {
            let p = &after_scheme[i..];
            p.split(['?', '#']).next().unwrap_or(p)
        }
        None => "",
    }
}

/// 平台 token 选择（纯函数，可单测）：
/// - host `api.github.com` → GitHub token；
/// - host `gitlab.com` / `*.gitlab.com` / 任一段为 `gitlab`（自托管 `gitlab.example.org`），
///   或 path 以 `/api/v4/` 开头（自托管实例 API，如 invent.kde.org）→ GitLab token；
/// - 其余无。
///
/// 历史：曾 `url.contains("gitlab")` 过宽——镜像站 URL 里出现 "gitlab" 字样
/// （如 `https://mirror.x/gitlab/foo.tar.gz`）会被误加 GitLab token（把 token 泄露给第三方）。
pub fn bearer_token_for<'a>(
    url: &str,
    github: &'a Option<String>,
    gitlab: &'a Option<String>,
) -> Option<&'a str> {
    let host = url_host(url);
    if host == "api.github.com" {
        return github.as_deref();
    }
    let is_gitlab_host = host == "gitlab.com"
        || host.ends_with(".gitlab.com")
        || host.split('.').any(|l| l == "gitlab");
    if is_gitlab_host || url_path(url).starts_with("/api/v4/") {
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
    fn get(&self, url: &str) -> Result<String, FarmError> {
        let mut req = http_agent().get(url).set("User-Agent", CURL_UA);
        if let Some(tok) = bearer_token_for(url, &self.github_token, &self.gitlab_token) {
            req = req.set("Authorization", &format!("Bearer {tok}"));
        }
        let resp = req
            .call()
            .map_err(|e| FarmError::http(format!("GET {url}"), e))?;
        resp.into_string()
            .map_err(|e| FarmError::io(format!("GET {url}"), e))
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

    fn list_tags(&self, repo_url: &str) -> Result<Vec<String>, FarmError> {
        // 有 token 就用（private 仓库/私有实例需要），没有就走匿名
        let tok = git_token_for(repo_url, &self.github_token, &self.gitlab_token);
        list_remote_tags(repo_url, tok)
    }
}

/// 抓取文本（如 index.txt）。失败返回错误信息。
pub fn fetch_text(url: &str) -> Result<String, FarmError> {
    let body = http_agent()
        .get(url)
        .set("User-Agent", CURL_UA)
        .call()
        .map_err(|e| FarmError::http(format!("GET {url}"), e))?;
    body.into_string()
        .map_err(|e| FarmError::io(format!("读 {url}"), e))
}

/// 下载重试退避：第 `failed` 次失败后等待 `2^failed` 秒（封顶 30s）。固定 2s 在长时间故障下会
/// 疯狂重试（镜像站限流时越试越糟），指数退避给对端恢复窗口。
fn retry_backoff(failed: u32) -> Duration {
    Duration::from_secs((1u64 << failed.min(5)).min(30))
}

/// 下载到文件（§8.6 源预下载），带可配置重试。瞬时网络错误可自愈；耗尽后返回错误。
/// **失败必删半文件**：`download_once` 先 `File::create` 再 copy，网络中断会留下截断文件；不清理
/// 则下次把它当"已就绪"（同类漏洞在 seed.rs 已修过，此处对齐）。调用方须保证 `dest` 此刻不是
/// 有效文件（`sources.rs` 在 `dest.exists()` 时已 `continue`）。
pub fn download_to_file(url: &str, dest: &std::path::Path, retries: u32) -> Result<(), FarmError> {
    download_with_backoff(url, dest, retries.max(1), std::thread::sleep)
}

/// `download_to_file` 主体：`sleep` 注入以便单测不真等。
fn download_with_backoff(
    url: &str,
    dest: &std::path::Path,
    attempts: u32,
    sleep: impl Fn(Duration),
) -> Result<(), FarmError> {
    for i in 1..=attempts {
        match download_once(url, dest) {
            Ok(()) => return Ok(()),
            Err(e) if i < attempts => {
                eprintln!("{}", crate::tr!("net.download_fail", url, i, attempts, e));
                sleep(retry_backoff(i));
            }
            Err(e) => {
                let _ = std::fs::remove_file(dest); // 清半文件，别让下次当"已下载"
                return Err(e);
            }
        }
    }
    unreachable!("重试循环已穷尽")
}

fn download_once(url: &str, dest: &std::path::Path) -> Result<(), FarmError> {
    let resp = http_agent()
        .get(url)
        .set("User-Agent", CURL_UA)
        .call()
        .map_err(|e| FarmError::http(format!("GET {url}"), e))?;
    let mut f =
        std::fs::File::create(dest).map_err(|e| FarmError::io(format!("创建 {dest:?} 失败"), e))?;
    std::io::copy(&mut resp.into_reader(), &mut f)
        .map_err(|e| FarmError::io(format!("写 {dest:?} 失败"), e))?;
    Ok(())
}

/// 探测 source URL 是否可达：GET 并读第一个字节，确认响应正常。
/// 状态非 2xx/3xx → Err（如 404/403/5xx；redirect 由 ureq 自动跟随，最终状态为准）。
/// track 写入前用它校验新源 URL，失败时打印警告并跳过 --run（除非 --probe-fail-continue）。
/// `git+`/`file://` 源由 lpkg（libgit2）处理，非 HTTP 下载，跳过探测（不误报）。
pub fn probe_source(url: &str) -> Result<(), FarmError> {
    if url.starts_with("git+") || url.starts_with("file://") {
        return Ok(());
    }
    let resp = match http_agent().get(url).set("User-Agent", CURL_UA).call() {
        Ok(r) => r,
        Err(ureq::Error::Status(code, _)) => return Err(format!("{url} HTTP {code}").into()),
        Err(e) => return Err(FarmError::http(format!("{url} 请求失败"), e)),
    };
    let status = resp.status();
    if !(200..400).contains(&status) {
        return Err(format!("{url} HTTP {status}").into());
    }
    // 读第一个字节确认 body 可流式读取（不只是 header 响应）
    let mut reader = resp.into_reader();
    let mut buf = [0u8; 1];
    let _ = std::io::Read::read(&mut reader, &mut buf)
        .map_err(|e| FarmError::io(format!("读 {url} 响应失败"), e))?;
    Ok(())
}

/// Mock：预设响应，测试用（无网络）。
#[derive(Debug, Default)]
pub struct MockFetcher {
    pub responses: HashMap<String, String>,
    /// `list_tags` 的预设：repo URL → 全部 tag（模拟 git 协议的全量结果）。
    pub tags: HashMap<String, Vec<String>>,
}

impl MockFetcher {
    pub fn new(responses: HashMap<String, String>) -> Self {
        MockFetcher {
            responses,
            tags: HashMap::new(),
        }
    }

    pub fn entry(mut self, url: impl Into<String>, body: impl Into<String>) -> Self {
        self.responses.insert(url.into(), body.into());
        self
    }

    /// 预设某 repo 的**全量** tag 列表（`list_tags` 用）。
    pub fn tags(mut self, repo_url: impl Into<String>, tags: &[&str]) -> Self {
        self.tags.insert(
            repo_url.into(),
            tags.iter().map(|t| t.to_string()).collect(),
        );
        self
    }
}

impl Fetcher for MockFetcher {
    fn get(&self, url: &str) -> Result<String, FarmError> {
        self.responses
            .get(url)
            .cloned()
            .ok_or_else(|| format!("MockFetcher: 无预设响应 {url}").into())
    }

    fn list_tags(&self, repo_url: &str) -> Result<Vec<String>, FarmError> {
        self.tags
            .get(repo_url)
            .cloned()
            .ok_or_else(|| format!("MockFetcher: 无预设 tags {repo_url}").into())
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
    fn retry_backoff_is_exponential_and_capped() {
        assert_eq!(retry_backoff(1), Duration::from_secs(2));
        assert_eq!(retry_backoff(2), Duration::from_secs(4));
        assert_eq!(retry_backoff(3), Duration::from_secs(8));
        assert_eq!(retry_backoff(4), Duration::from_secs(16));
        assert_eq!(retry_backoff(5), Duration::from_secs(30));
        assert_eq!(retry_backoff(9), Duration::from_secs(30), "封顶 30s");
    }

    #[test]
    fn download_failure_removes_partial_file_and_backs_off() {
        // 对不存在路径下载（404，ureq 视为 Err）→ 重试耗尽后必须删掉残留文件（含预置垃圾）
        let root = std::env::temp_dir().join(format!("farm-net-partial-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let port: u16 = 18082;
        let r = root.clone();
        let h = std::thread::spawn(move || {
            let _ = crate::serve::serve("127.0.0.1", &r, port);
        });
        std::thread::sleep(Duration::from_millis(300));

        let dest = root.join("dl.bin");
        std::fs::write(&dest, b"stale-partial").unwrap(); // 预置垃圾，证明确实被清
        let sleeps = std::cell::Cell::new(0u32);
        let res = download_with_backoff(
            &format!("http://127.0.0.1:{port}/missing"),
            &dest,
            3,
            |_| sleeps.set(sleeps.get() + 1),
        );
        assert!(res.is_err(), "404 应报错");
        assert!(!dest.exists(), "失败后不得留下半文件/旧垃圾");
        assert_eq!(sleeps.get(), 2, "3 次尝试之间退避 2 次");
        std::fs::remove_dir_all(&root).ok();
        drop(h);
    }

    #[test]
    fn bearer_token_not_added_for_mirror_paths() {
        let gl = Some("gl-token".to_string());
        // 回归：URL 里出现 "gitlab" 字样但 host 不是 gitlab → 不得加 token（旧实现会加）
        assert_eq!(
            bearer_token_for("https://mirror.example.com/gitlab/foo.tar.gz", &None, &gl),
            None
        );
        assert_eq!(
            bearer_token_for("https://mirror.example.com/d?ref=gitlab/x", &None, &gl),
            None
        );
        // 自托管 gitlab（host 段含 gitlab）与 /api/v4/ 前缀仍应加
        assert_eq!(
            bearer_token_for("https://gitlab.example.org/api/v4/p/x", &None, &gl),
            Some("gl-token")
        );
        assert_eq!(
            bearer_token_for("https://invent.kde.org/api/v4/projects/x", &None, &gl),
            Some("gl-token")
        );
    }

    #[test]
    fn agent_read_timeout_aborts_stalled_body() {
        // 服务端接受连接但永不应答 → 读超时必须让调用在秒级失败，而非无限挂起
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        std::thread::spawn(move || {
            let _held = listener.accept();
            std::thread::sleep(Duration::from_secs(10));
        });
        let agent = build_agent(
            Duration::from_millis(200),
            Duration::from_millis(200),
            Duration::from_millis(200),
        );
        let start = std::time::Instant::now();
        let res = agent.get(&format!("http://127.0.0.1:{port}/x")).call();
        assert!(res.is_err(), "无应答连接应因读超时报错");
        assert!(
            start.elapsed() < Duration::from_secs(5),
            "应在秒级超时而非挂起（实测 {:?}）",
            start.elapsed()
        );
    }

    #[test]
    fn git_token_for_picks_by_host() {
        let gh = Some("gh-token".to_string());
        let gl = Some("gl-token".to_string());
        // github.com → GitHub token；没有就匿名
        assert_eq!(
            git_token_for("https://github.com/o/r.git", &gh, &gl),
            Some("gh-token")
        );
        assert_eq!(
            git_token_for("https://github.com/o/r.git", &None, &gl),
            None
        );
        // gitlab.com 与自托管 gitlab（host 段含 gitlab）→ GitLab token
        assert_eq!(
            git_token_for("https://gitlab.com/a/b.git", &gh, &gl),
            Some("gl-token")
        );
        assert_eq!(
            git_token_for("https://gitlab.example.org/a/b.git", &gh, &gl),
            Some("gl-token")
        );
        // 其它 forge（自托管 gitea 等）→ 无凭据（匿名）
        assert_eq!(
            git_token_for("https://git.example.com/a/b.git", &gh, &gl),
            None
        );
        assert_eq!(
            git_token_for("https://gitea.example.org/a/b.git", &gh, &gl),
            None
        );
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
