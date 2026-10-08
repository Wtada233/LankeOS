//! gen-trackers 子命令：LLM 提示词 + 批次解析 + 编排。
//!
//! `use super::*;` 拿到父模块的共享 helper（子模块可见祖先的私有项）。

use super::*;

/// 抓取源 URL 的父目录（探测输出，供 LLM 判断真实格式）。
fn fetch_listing(source_url: &str, fetcher: &dyn Fetcher) -> (String, String) {
    let parent = match source_url.rfind('/') {
        Some(i) => source_url[..=i].to_string(),
        None => return (source_url.to_string(), "(无法确定目录)".into()),
    };
    match fetcher.get(&parent) {
        Ok(body) => {
            let truncated: String = body.chars().take(2000).collect();
            let shown = if body.len() > 2000 {
                format!("{truncated}\n...(截断)")
            } else {
                truncated
            };
            (parent, shown)
        }
        Err(e) => (parent, format!("(抓取失败: {e})")),
    }
}

/// 目标包：--packages 指定，或所有有远程 source 但无 tracker 的包。file:///空源包跳过。
fn collect_targets(
    root: &std::path::Path,
    trackers: &HashMap<String, TrackerConfig>,
    packages: Option<&str>,
) -> Vec<String> {
    if let Some(list) = packages {
        // 显式指定的也过滤：file:///空源包跳过（无需 track）
        return list
            .split(',')
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .filter(|s| {
                let has_remote = match load_build_json(&root.join(s)) {
                    Ok(b) => super::has_remote_source(&b),
                    Err(_) => false,
                };
                if !has_remote {
                    eprintln!("{}", lankefarm::tr!("track.skip_no_remote_short", s));
                }
                has_remote
            })
            .collect();
    }
    let mut result = Vec::new();
    if let Ok(rd) = std::fs::read_dir(root) {
        for e in rd.flatten() {
            let dir = e.path();
            if !dir.is_dir() {
                continue;
            }
            let name = e.file_name().to_string_lossy().into_owned();
            let Ok(b) = load_build_json(&dir) else {
                continue;
            };
            // tracker 按 **pkg-name** 匹配（**不是目录名**——目录名可能与 pkg-name 不同，
            // 同 `cmd_track_all`；用目录名查会漏判已有 tracker 而重复生成）
            if trackers.contains_key(&b.name) {
                continue;
            }
            if super::has_remote_source(&b) {
                result.push(name);
            }
        }
    }
    result.sort();
    result
}

const SYSTEM_PROMPT: &str = r#"你是 LankeOS 发行版的 tracker 配置生成器。根据给定包的源 URL 和探测输出（真实抓取），生成正确的 tracker yaml。

tracker yaml 是 sources/work_sources 的完整清单，结构：
- pkg-name（顶层必填，=包名）
- version-source：版本来源选择器（sources[i] 或 work_sources[i]，默认 sources[0]，空则 work_sources[0]）
- sources: / work_sources:：逐条 source 槽位的探测配置（**位置对应 LankeBUILD.json 的 sources/work_sources 数组**，不要 url-match、不要写 pkg-name）

source 条目可用 tracker-template 及字段：
- github: repo, mode(tags|releases), tag-prefix, template
- gitlab: host, project, mode(tags|releases), tag-prefix, template
- sourceforge: project, path, pattern, template
- gnome: template
- gcs: url(GCS/S3 桶目录), pattern, template
- html-index: url(HTML 目录列表页), pattern, template
- multi-level-html-index: levels, template —— **N 级目录逐级进**（版本藏在路径里，如 KDE frameworks
  `6.11/` 目录 → 目录内 `ki18n-6.11.0.tar.xz`）。`levels` 每级 `{name, url, pattern}`：**`name` 即占位符名**
  （`{名字}` 可在**后续级**的 url 与 template 里引用）；**必须有一级名为 `version`**（该级捕获 = 包版本，
  按名字定、不按位置）。**不要用已废弃的 `{v1}`/`{v2}`**。例：
  levels:
  - name: series
    url: https://download.kde.org/stable/frameworks/
    pattern: href="([0-9][0-9.]*)/"
  - name: version
    url: https://download.kde.org/stable/frameworks/{series}/
    pattern: ki18n-([0-9][0-9.]*)\.tar\.xz
  template: https://download.kde.org/stable/frameworks/{series}/ki18n-{version}.tar.xz
- same-version: same-version-of(锁定为指定**包**的版本，直接确定版本不经探测), tag-prefix, repo, template
- same-version-of-source: same-version-of-source(锁定为**本 tracker 中位于它之前的槽位**本轮解析出的版本，如 sources[0]), template
- script: script（内嵌 bash，stdout 每行 `<版本>|URL`；**默认恰好一行**，加 expand: true 才允许多行/多槽位）——**条目级**，与其他模板平级

条目级版本约束（只作用于本条目，探测模板适用）：major-of、major-version-lock、max-version、source-name。
template 是**完整下载 URL**（含 https:// 和主机名，占位符替换后可直接下载），不要把 URL 拆开只留文件名/相对路径。
template 占位符：{name} {version} {tag} {repo} {project} {path_version}（multi-level-html-index 另可用各级的 name）。
pattern 是提取版本的正则，必须含一个捕获组，如 (\d[\d.]*)。

**实在**无法用现成模板覆盖的（独特 API、版本在文件内容里等）才用**条目级** script：
- 一个脚本条目产**一个**槽位（放哪个列表就填哪个列表）。stdout 恰好一行 `<版本>|URL`。
- 只有上游**动态枚举**（目录里有几个文件不确定）才加 `expand: true`，此时 stdout 每行
  一个 `<版本>|URL`，逐行对应一个连续槽位。**能不用就不用**——脚本不可复用、无法统一校验。
sources:
  - tracker-template: script
    script: |
      #!/bin/bash
      # stdout 恰好一行：<版本>|URL（两侧都不得为空）
      echo "1.2.3|https://.../pkg-1.2.3.tar.gz"

规则：
- 根据探测输出的真实格式选模板，不要猜；探测失败时按源 URL 域名/结构选最合理的。
- github 用 tags/releases API，gitlab 用其 API，GCS/S3 桶用 XML listing（?delimiter=/），纯 HTML 目录列表用 html-index；**版本藏在多级目录路径里**（先有 `6.11/` 再进目录找 `pkg-6.11.0.tar.xz`）用 multi-level-html-index。
- 稳定版优先（tracker 自动过滤 rc/beta/alpha）。
- sources:/work_sources: 条目必须覆盖 LankeBUILD.json 里的全部源（探测成功时整包全量替换），顺序与 json 一致。

输出格式：直接输出 N 个 YAML 文档，每个文档前用一行 `===` 分隔。不要 JSON、不要 markdown 代码围栏、不要任何解释。示例：
===
pkg-name: acl
version-source: sources[0]
sources:
  - tracker-template: github
    repo: ...
    mode: tags
    tag-prefix: v
    template: ...
===
pkg-name: alacritty
sources:
  - tracker-template: script
    script: |
      ...
===

容错规则：
- pkg-name 必须是给定批次中的包名，不要发明、不要拼错、不要改名。
- 无法为某个包生成 tracker 时，输出一行 `none: <pkg-name>`（放在 == 分隔的块里），表示跳过该包。
- 每个包要么给有效 yaml，要么给 `none:`，不要省略。"#;

/// 把 LLM 返回的 `===` 分隔 YAML 文档拆成独立文本。
pub(super) fn parse_yaml_docs(text: &str) -> Vec<String> {
    let mut docs = Vec::new();
    let mut current = String::new();
    for line in text.lines() {
        let t = line.trim();
        if t.starts_with("```") {
            continue;
        }
        if t == "===" {
            if !current.trim().is_empty() {
                docs.push(std::mem::take(&mut current));
            }
        } else {
            current.push_str(line);
            current.push('\n');
        }
    }
    if !current.trim().is_empty() {
        docs.push(current);
    }
    docs
}

/// 批次解析结果：有效 yaml / 显式跳过（none）/ 幻觉（不在批次里的包名）。
pub(super) struct BatchResult {
    pub(super) yamls: Vec<(String, String)>, // (pkg-name, yaml 文本)
    pub(super) skipped: Vec<String>,
    pub(super) hallucinations: Vec<String>,
}

/// 校验 LLM 输出：pkg-name 必须属于批次；`none: <pkg>` 表示跳过。
pub(super) fn parse_batch_blocks(text: &str, batch: &[String]) -> BatchResult {
    let mut r = BatchResult {
        yamls: Vec::new(),
        skipped: Vec::new(),
        hallucinations: Vec::new(),
    };
    for doc in parse_yaml_docs(text) {
        let trimmed = doc.trim();
        if let Some(rest) = trimmed.strip_prefix("none:") {
            let name = rest.trim().to_string();
            if batch.contains(&name) {
                r.skipped.push(name);
            } else {
                r.hallucinations.push(name);
            }
            continue;
        }
        match serde_yaml_ng::from_str::<TrackerConfig>(&doc) {
            Ok(cfg) if batch.contains(&cfg.pkg_name) => {
                r.yamls.push((cfg.pkg_name.clone(), doc));
            }
            Ok(cfg) => r.hallucinations.push(cfg.pkg_name),
            Err(_) => {
                // 解析失败：尝试提取 pkg-name 判断是否幻觉；提取不到视为 malformed
                if let Some(name) = extract_pkg_name(&doc) {
                    r.hallucinations.push(name);
                }
            }
        }
    }
    r
}

/// 从（可能残缺的）yaml 文本中提取 `pkg-name: X`。
fn extract_pkg_name(doc: &str) -> Option<String> {
    for line in doc.lines() {
        if let Some(v) = line.trim().strip_prefix("pkg-name:") {
            return Some(v.trim().to_string());
        }
    }
    None
}

/// farm gen-trackers：batch 调 LLM 生成 tracker yaml（12 个一批）。
pub(super) fn cmd_gen_trackers(args: &GenTrackersArgs) -> ExitCode {
    let pkgs_dir = args.pkgs.to_string_lossy().into_owned();
    let data_dir = args.data.to_string_lossy().into_owned();

    // API 配置：CLI 参数优先，env 兜底
    let endpoint = if args.api_endpoint.is_empty() {
        std::env::var("LANKEFARM_LLM_BASE_URL")
            .unwrap_or_else(|_| "http://127.0.0.1:8000/v1".into())
    } else {
        args.api_endpoint.clone()
    };
    let key = if args.api_key.is_empty() {
        std::env::var("LANKEFARM_LLM_API_KEY").unwrap_or_default()
    } else {
        args.api_key.clone()
    };
    let model = match if args.model.is_empty() {
        std::env::var("LANKEFARM_LLM_MODEL").ok()
    } else {
        Some(args.model.clone())
    } {
        Some(m) => m,
        None => {
            eprintln!("{}", lankefarm::tr!("gen.no_model"));
            return ExitCode::from(2);
        }
    };
    let llm = LlmClient::new(endpoint.clone(), key, model.clone());
    // gen-trackers 无平台 token 参数：裸 RealFetcher（探测 URL 可达性用）
    let fetcher = lankefarm::net::RealFetcher::new(None, None);

    let trackers = load_trackers(&data_dir);
    let root = PathBuf::from(&pkgs_dir);
    let targets = collect_targets(&root, &trackers, args.packages.as_deref());
    if targets.is_empty() {
        println!("{}", lankefarm::tr!("gen.none"));
        return ExitCode::SUCCESS;
    }
    println!(
        "{}",
        lankefarm::tr!("gen.targets", targets.len(), endpoint, model)
    );

    std::fs::create_dir_all(&data_dir)
        .map_err(|e| {
            eprintln!("{}", lankefarm::tr!("gen.dir_fail", data_dir, e));
        })
        .ok();

    let mut written = 0;
    for (idx, batch) in targets.chunks(12).enumerate() {
        println!("{}", lankefarm::tr!("gen.batch", idx + 1, batch.len()));
        let mut sections = Vec::new();
        for name in batch {
            let build = match load_build_json(&root.join(name)) {
                Ok(b) => b,
                Err(e) => {
                    eprintln!("{}", lankefarm::tr!("gen.load_fail", name, e));
                    continue;
                }
            };
            let src =
                first_remote_source(&build.sources).unwrap_or(lankefarm::tr!("gen.no_remote_src"));
            println!("{}", lankefarm::tr!("gen.fetch", name, src));
            let (url, listing) = fetch_listing(src, &fetcher);
            sections.push(format!(
                "[包] name={}, version={}\n  源: {}\n  探测输出（{url}）:\n```\n{listing}\n```",
                build.name, build.version, src
            ));
        }
        if sections.is_empty() {
            continue;
        }
        let base_user = format!("为以下 {} 个包生成 tracker yaml（每个包前用 === 分隔的 YAML 文档）：\n\n{}\n\n直接输出 YAML。", sections.len(), sections.join("\n\n"));
        let mut user = base_user.clone();
        let mut attempts = 0;
        loop {
            attempts += 1;
            println!(
                "{}",
                lankefarm::tr!("gen.llm_calling", sections.len(), user.len())
            );
            match llm.chat(SYSTEM_PROMPT, &user) {
                Ok(resp) => {
                    let res = parse_batch_blocks(&resp, batch);
                    // 写有效 yaml（按 pkg-name，校验属于批次）
                    for (pkg, doc) in &res.yamls {
                        let path = PathBuf::from(&data_dir).join(format!("{pkg}.yaml"));
                        match std::fs::write(&path, doc) {
                            Ok(_) => {
                                println!("{}", lankefarm::tr!("gen.write", pkg));
                                written += 1;
                            }
                            Err(e) => eprintln!("{}", lankefarm::tr!("gen.write_fail", pkg, e)),
                        }
                    }
                    // 缺的：批次包既没 yaml 也没显式 none
                    let missing: Vec<String> = batch
                        .iter()
                        .filter(|n| {
                            !res.yamls.iter().any(|(p, _)| p == *n) && !res.skipped.contains(n)
                        })
                        .cloned()
                        .collect();
                    if res.hallucinations.is_empty() && missing.is_empty() {
                        break; // 批次完整（有效 or 显式跳过）
                    }
                    if attempts >= 3 {
                        eprintln!(
                            "{}",
                            lankefarm::tr!(
                                "gen.retry_exhausted",
                                attempts,
                                missing.join(", "),
                                res.hallucinations.join(", ")
                            )
                        );
                        break;
                    }
                    eprintln!(
                        "{}",
                        lankefarm::tr!(
                            "gen.retry_feedback",
                            attempts,
                            missing.join(","),
                            res.hallucinations.join(",")
                        )
                    );
                    user = format!(
                        "{} 上次输出有误：缺 {}，多 {}. 请补全；对无法生成的包输出 `none: <pkg-name>`. 重新输出。",
                        base_user, missing.join(","), res.hallucinations.join(",")
                    );
                }
                Err(e) => {
                    eprintln!("{}", lankefarm::tr!("gen.batch_fail", e));
                    break;
                }
            }
        }
    }
    println!();
    println!("{}", lankefarm::tr!("gen.done", written));
    ExitCode::SUCCESS
}
