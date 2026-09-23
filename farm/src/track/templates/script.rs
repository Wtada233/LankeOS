//! script：**条目级模板**（`tracker-template: script`）。一个脚本产一个 source/work_source 槽位
//! （声明 `expand: true` 时可产多个）。
//!
//! 契约：stdout **每行** `<版本>|<URL>`（版本与 URL 都不得为空）。
//! - **默认恰好一行**；多行 → 报错。这条严格是**故意的**：它把"还留着旧的两行格式
//!   （首行版本 + 后续行 URL）"的漏迁移立刻暴露出来，而不是静默只取第一行。
//! - `expand: true` 时允许 N 行，每行 = **同一列表里的一个连续槽位**（按 stdout 顺序）；
//!   0 行 → 报错。
//! - 任一行不合法 → 整条失败（对齐"任一条目失败 → 整包不更新"的原子性）。
//! - stderr 自由（给 operator 看）。
//!
//! 槽位顺序 = 输出顺序，因此 `version-source: sources[i]` 的索引语义与其他模板完全一致
//! （它索引的是扁平槽位表，也就是 LankeBUILD.json 的 `sources` 数组）。

use crate::error::FarmError;
use std::sync::atomic::{AtomicU32, Ordering};

use crate::net::Fetcher;
use crate::track::{need, validate_url, EntryProbe, SourceConfig};

/// 每个 probe 调用分配一个唯一序号，配合 PID 保证临时脚本文件名互不冲突。
static SCRIPT_SEQ: AtomicU32 = AtomicU32::new(0);

/// 运行内嵌 bash 脚本，解析 stdout 的 `<版本>|<URL>` 行。
/// 平台 token 通过 `GITHUB_TOKEN`/`GITLAB_TOKEN` 环境变量传给 curl，消除 script 里 GitHub/GitLab API 限流 403。
/// `vars` = `version-var` 解析出的 `(变量名, 版本)`，作为环境变量注入脚本
/// （`version-var: {main: sources[0]}` → 脚本里可直接写 `$main`）。
pub fn probe(
    fetcher: &dyn Fetcher,
    cfg: &SourceConfig,
    pkg_name: &str,
    vars: &[(String, String)],
) -> Result<Vec<EntryProbe>, FarmError> {
    let content = need(&cfg.script, "script")?;
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
    // version-var 注入**先于**平台 token：token 是运维凭据，同名时以 token 为准。
    for (k, v) in vars {
        cmd.env(k, v);
    }
    for (k, v) in fetcher.token_env() {
        cmd.env(k, v);
    }
    let out = cmd
        .output()
        .map_err(|e| format!("运行 track 脚本失败: {e}"))?;
    let _ = std::fs::remove_file(&tmp);
    if !out.status.success() {
        return Err(format!(
            "track 脚本退出码非零: {}（stderr: {}）",
            out.status,
            String::from_utf8_lossy(&out.stderr).trim()
        )
        .into());
    }
    parse_stdout(&String::from_utf8_lossy(&out.stdout), cfg.expand)
}

/// 解析脚本 stdout：每行 `<版本>|<URL>`。`expand` 决定行数约束（见模块文档）。
fn parse_stdout(stdout: &str, expand: bool) -> Result<Vec<EntryProbe>, FarmError> {
    let mut probes: Vec<EntryProbe> = Vec::new();
    for (i, raw) in stdout.lines().enumerate() {
        let line = raw.trim();
        if line.is_empty() {
            continue;
        }
        let n = i + 1;
        let Some((ver, url)) = line.split_once('|') else {
            return Err(format!(
                "脚本 stdout 第 {n} 行不是 `<版本>|URL` 格式（旧的「首行版本 + 后续行 URL」两行格式已废弃）: {line}"
            )
            .into());
        };
        let (ver, url) = (ver.trim(), url.trim());
        if ver.is_empty() {
            return Err(format!("脚本 stdout 第 {n} 行版本为空: {line}").into());
        }
        if url.is_empty() {
            return Err(format!("脚本 stdout 第 {n} 行 URL 为空: {line}").into());
        }
        validate_url(url)?;
        probes.push(EntryProbe {
            version: ver.to_string(),
            url: url.to_string(),
        });
    }
    if probes.is_empty() {
        return Err("脚本 stdout 无输出（每行应为 `<版本>|URL`）"
            .to_string()
            .into());
    }
    if !expand && probes.len() > 1 {
        return Err(format!(
            "脚本 stdout 输出了 {} 行，但该条目未声明 `expand: true`（一个脚本默认只产一个槽位；\
             要产多个槽位请显式加 `expand: true`）",
            probes.len()
        )
        .into());
    }
    Ok(probes)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cfg(script: &str, expand: bool) -> SourceConfig {
        SourceConfig {
            tracker_template: "script".into(),
            script: Some(script.into()),
            expand,
            ..Default::default()
        }
    }

    #[test]
    fn single_line_probe() {
        let r = probe(
            &crate::net::RealFetcher::default(),
            &cfg(
                "#!/bin/bash\necho \"3.7b|https://x/tmux-3.7b.tar.gz\"\n",
                false,
            ),
            "tmux",
            &[],
        )
        .unwrap();
        assert_eq!(r.len(), 1);
        assert_eq!(r[0].version, "3.7b");
        assert_eq!(r[0].url, "https://x/tmux-3.7b.tar.gz");
    }

    #[test]
    fn multi_line_without_expand_is_rejected() {
        // 这条是**漏迁移守门**：旧契约（首行版本、后续行 URL）在新契约下必须硬报错，
        // 不能静默只取第一行。
        let old = "#!/bin/bash\necho \"3.7b\"\necho \"https://x/tmux-3.7b.tar.gz\"\n";
        let e = probe(
            &crate::net::RealFetcher::default(),
            &cfg(old, false),
            "tmux",
            &[],
        )
        .unwrap_err();
        assert!(
            e.to_string().contains("不是 `<版本>|URL` 格式"),
            "旧两行格式应报格式错: {e}"
        );
        // 或者格式对但行数超限 → 提示加 expand
        let two = "#!/bin/bash\necho \"1|https://x/a\"\necho \"1|https://x/b\"\n";
        let e = probe(
            &crate::net::RealFetcher::default(),
            &cfg(two, false),
            "p",
            &[],
        )
        .unwrap_err();
        assert!(e.to_string().contains("expand"), "应提示加 expand: {e}");
    }

    #[test]
    fn expand_allows_many_lines_in_order() {
        let s =
            "#!/bin/bash\nprintf '%s\\n' \"1|https://x/a\" \"1|https://x/b\" \"1|https://x/c\"\n";
        let r = probe(&crate::net::RealFetcher::default(), &cfg(s, true), "p", &[]).unwrap();
        assert_eq!(
            r.iter().map(|e| e.url.as_str()).collect::<Vec<_>>(),
            vec!["https://x/a", "https://x/b", "https://x/c"],
            "槽位顺序必须等于 stdout 顺序"
        );
        // 每行自带版本
        let s2 = "#!/bin/bash\nprintf '%s\\n' \"1|https://x/a\" \"2|https://x/b\"\n";
        let r2 = probe(
            &crate::net::RealFetcher::default(),
            &cfg(s2, true),
            "p",
            &[],
        )
        .unwrap();
        assert_eq!(r2[0].version, "1");
        assert_eq!(r2[1].version, "2");
    }

    #[test]
    fn empty_url_and_version_are_rejected() {
        for bad in ["1|", "|https://x/a", "1"] {
            let s = format!("#!/bin/bash\necho '{bad}'\n");
            assert!(
                probe(
                    &crate::net::RealFetcher::default(),
                    &cfg(&s, false),
                    "p",
                    &[]
                )
                .is_err(),
                "{bad:?} 应报错"
            );
        }
    }

    #[test]
    fn no_output_is_rejected_even_with_expand() {
        let s = "#!/bin/bash\ntrue\n";
        assert!(probe(&crate::net::RealFetcher::default(), &cfg(s, true), "p", &[]).is_err());
    }

    #[test]
    fn nonzero_exit_carries_stderr() {
        let s = "#!/bin/bash\necho 'boom' >&2\nexit 1\n";
        let e = probe(
            &crate::net::RealFetcher::default(),
            &cfg(s, false),
            "p",
            &[],
        )
        .unwrap_err();
        assert!(e.to_string().contains("boom"), "应带上 stderr: {e}");
    }

    #[test]
    fn url_split_on_first_pipe_only() {
        // URL 里含 `|` 时按**第一个** `|` 切分（版本不含 `|`）
        let s = "#!/bin/bash\necho \"1|https://x/a|b\"\n";
        let r = probe(
            &crate::net::RealFetcher::default(),
            &cfg(s, false),
            "p",
            &[],
        )
        .unwrap();
        assert_eq!(r[0].version, "1");
        assert_eq!(r[0].url, "https://x/a|b");
    }
}
