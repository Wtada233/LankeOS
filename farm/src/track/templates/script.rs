//! script 模板：内嵌 bash 脚本逃生舱（处理模板覆盖不了的独特上游）。
//!
//! 契约：stdout 第一行 = 版本，后续行 = 具体下载 URL。

use crate::net::Fetcher;
use crate::track::{need, ProbeResult, TrackerConfig};

/// 探测：运行内嵌 bash 脚本（stdout 第一行版本，后续行 URL）。
/// `major` 非空时通过 `MAJOR` 环境变量传给脚本（约束 major-of / major-version-lock）。
/// 平台 token 通过 `GITHUB_TOKEN`/`GITLAB_TOKEN` 环境变量传给 curl，消除 script 里 GitHub/GitLab API 限流 403。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &TrackerConfig,
    major: Option<&str>,
) -> Result<ProbeResult, String> {
    let content = need(&cfg.script_content, "script-content")?;
    // 写到临时文件再跑，避免 -c 的参数转义地狱
    let tmp = std::env::temp_dir().join(format!("lankefarm-track-{}.sh", cfg.pkg_name));
    std::fs::write(&tmp, content).map_err(|e| format!("写临时脚本失败: {e}"))?;
    let mut cmd = std::process::Command::new("bash");
    cmd.arg(&tmp).env("PKG_NAME", &cfg.pkg_name);
    for (k, v) in fetcher.token_env() {
        cmd.env(k, v);
    }
    if let Some(m) = major {
        cmd.env("MAJOR", m);
    }
    let out = cmd
        .output()
        .map_err(|e| format!("运行 track 脚本失败: {e}"))?;
    let _ = std::fs::remove_file(&tmp);
    if !out.status.success() {
        return Err(format!("track 脚本退出码非零: {}", out.status));
    }
    let stdout = String::from_utf8_lossy(&out.stdout);
    let mut lines = stdout.lines();
    let version = lines.next().unwrap_or("").trim().to_string();
    if version.is_empty() {
        return Err("track 脚本未输出版本（stdout 第一行）".to_string());
    }
    let sources: Vec<String> = lines
        .map(|l| l.trim().to_string())
        .filter(|l| !l.is_empty())
        .collect();
    Ok(ProbeResult { version, sources })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn script_probe_parses_stdout() {
        let cfg = TrackerConfig {
            pkg_name: "tmux".into(),
            tracker_template: "script".into(),
            script_content: Some(
                "#!/bin/bash\necho \"3.7b\"\necho \"https://x/tmux-3.7b.tar.gz\"\n".into(),
            ),
            ..Default::default()
        };
        let r = cfg.probe(&crate::net::RealFetcher::default()).unwrap();
        assert_eq!(r.version, "3.7b");
        assert_eq!(r.sources, vec!["https://x/tmux-3.7b.tar.gz"]);
    }
}
