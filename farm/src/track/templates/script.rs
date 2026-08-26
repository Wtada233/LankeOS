//! script：**包级类型**（`type: script`），不是模板。整包走内嵌 bash 逃生舱。
//!
//! 契约：stdout 第一行 = 版本，后续行 = 具体下载 URL；出现标记行 `# work_sources` 后归 work_sources
//! （Arch noextract 对应——lpkg 只下载不解压，如 LibreOffice vendor tarball）。
//! 返回完整清单（`ProbeResult`），不参与模板的位置模型。

use std::sync::atomic::{AtomicU32, Ordering};

use crate::net::Fetcher;
use crate::track::{validate_url, ProbeResult};

/// 每个 probe 调用分配一个唯一序号，配合 PID 保证临时脚本文件名互不冲突。
static SCRIPT_SEQ: AtomicU32 = AtomicU32::new(0);

/// 运行内嵌 bash 脚本（stdout 第一行版本，后续行 URL）。
/// 平台 token 通过 `GITHUB_TOKEN`/`GITLAB_TOKEN` 环境变量传给 curl，消除 script 里 GitHub/GitLab API 限流 403。
pub fn probe(fetcher: &dyn Fetcher, content: &str, pkg_name: &str) -> Result<ProbeResult, String> {
    // 写到临时文件再跑，避免 -c 的参数转义地狱。
    // 文件名必须唯一（pkg-name + PID + 序号）：曾用固定 `lankefarm-track-{pkg}.sh`，
    // 两个并发 `farm track` 进程会互相覆盖/删除彼此的脚本——A 的 remove_file 删掉
    // B 刚写好、尚未执行的文件 → B 的 bash ENOENT → 探测失败（真实 TOCTOU，曾致
    // track_all_cycle 集成测试间歇性 flaky，与 docker/farm build 抢占无关）。
    let tmp = std::env::temp_dir().join(format!(
        "lankefarm-track-{}-{}-{}.sh",
        pkg_name,
        std::process::id(),
        SCRIPT_SEQ.fetch_add(1, Ordering::Relaxed),
    ));
    std::fs::write(&tmp, content).map_err(|e| format!("写临时脚本失败: {e}"))?;
    let mut cmd = std::process::Command::new("bash");
    cmd.arg(&tmp).env("PKG_NAME", pkg_name);
    for (k, v) in fetcher.token_env() {
        cmd.env(k, v);
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
    // 契约：首行版本，随后行为 sources URL；出现标记行 `# work_sources` 后归为 work_sources
    let mut sources = Vec::new();
    let mut work_sources = Vec::new();
    let mut in_work = false;
    for l in lines {
        let l = l.trim().to_string();
        if l.is_empty() {
            continue;
        }
        if l.starts_with("# work_sources") {
            in_work = true;
            continue;
        }
        if in_work {
            work_sources.push(l);
        } else {
            sources.push(l);
        }
    }
    for url in sources.iter().chain(&work_sources) {
        validate_url(url)?;
    }
    Ok(ProbeResult {
        version,
        sources,
        work_sources,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn script_probe_parses_stdout() {
        let r = probe(
            &crate::net::RealFetcher::default(),
            "#!/bin/bash\necho \"3.7b\"\necho \"https://x/tmux-3.7b.tar.gz\"\n",
            "tmux",
        )
        .unwrap();
        assert_eq!(r.version, "3.7b");
        assert_eq!(r.sources, vec!["https://x/tmux-3.7b.tar.gz"]);
        assert!(r.work_sources.is_empty());
    }
}
