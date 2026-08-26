//! i18n.rs — 用户可见消息的本地化。
//!
//! 中文为默认；`LANG`/`LC_ALL` 以 `en` 开头时切英文。键缺失回退到键名（便于发现漏译）。
//! 用法：
//!   `tr!("build.start")` → 取本地化字符串
//!   `tr!("build.start", pkg, ver)` → `format!` 语义（目录串里用 `{}` 占位）
//!
//! 视觉层（ANSI 颜色）在 ux.rs；本模块只管文案。

use std::collections::HashMap;
use std::sync::{LazyLock, OnceLock};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Lang {
    Zh,
    En,
}

static LANG: OnceLock<Lang> = OnceLock::new();

/// 纯函数：从 `LANG`/`LC_ALL` 环境值判定语言。`en` 开头 → 英文，否则中文。
pub fn detect_lang(lang_env: &str) -> Lang {
    if lang_env.to_ascii_lowercase().starts_with("en") {
        Lang::En
    } else {
        Lang::Zh
    }
}

fn lang() -> Lang {
    *LANG.get_or_init(|| {
        let l = std::env::var("LANG")
            .or_else(|_| std::env::var("LC_ALL"))
            .unwrap_or_default();
        detect_lang(&l)
    })
}

/// 从指定语言目录取键；缺失返回 None（供 tr 回退与测试）。
fn catalog_get(lang: Lang, key: &str) -> Option<&'static str> {
    match lang {
        Lang::Zh => ZH.get(key).copied(),
        Lang::En => EN.get(key).copied(),
    }
}

/// 当前语言是否为英文（clap 帮助文本据此覆盖 doc comment）。
pub fn is_en() -> bool {
    lang() == Lang::En
}

/// 取本地化字符串；键缺失回退到键名。字面量键（`&'static str`）返回 `&'static str`。
pub fn tr(key: &str) -> &str {
    catalog_get(lang(), key).unwrap_or(key)
}

/// 运行时格式串：把 `{}` 逐位置换为参数（farm 消息只用位置占位，不解析宽度/命名）。
/// Rust 的 `format!` 要求字面量格式串，运行时串需手动替换。
pub fn fmt(template: &str, args: &[&dyn std::fmt::Display]) -> String {
    let mut out = String::new();
    let mut rest = template;
    for a in args {
        match rest.find("{}") {
            Some(i) => {
                out.push_str(&rest[..i]);
                out.push_str(&a.to_string());
                rest = &rest[i + 2..];
            }
            None => {
                out.push_str(&a.to_string());
            }
        }
    }
    out.push_str(rest);
    out
}

/// 本地化宏：`tr!("key")` 取串；`tr!("key", args..)` 按 `{}` 顺序填充。
#[macro_export]
macro_rules! tr {
    ($key:expr) => {
        $crate::i18n::tr($key)
    };
    ($key:expr, $($arg:expr),+ $(,)?) => {
        $crate::i18n::fmt($crate::i18n::tr($key), &[ $( &$arg as &dyn std::fmt::Display ),+ ])
    };
}

/// 中文目录（默认）。键名 = 语义标识，避免散落的裸字符串。
static ZH: LazyLock<HashMap<&'static str, &'static str>> = LazyLock::new(|| {
    HashMap::from([
        // ── build 主流程 ──
        ("build.start", "[build] {} {}{}"),
        ("build.victim", "（ABI 传播重建）"),
        ("build.repo", "  [repo] {} {} 已进本地仓库，index 已更新"),
        ("build.abi", "  [abi] {} SONAME 变化（{}），{} 需重建"),
        ("build.group_rebuild", "  [rebuild-group] {} 运行时变化，{} 需重建"),
        ("build.version_rebuild", "  [version-change] {} 版本变化，{} 需重建"),
        ("build.repack", "  [repack] {} metadata.json needed_so/provides 已修正并重打包（不 rebuild）"),
        ("build.blocked", "  [BLOCKED] {}：{}"),
        ("build.cycle", "  [cycle] 循环依赖 {} → {}，已切断该边（若后续构建失败请人工检查这对依赖）"),
        ("build.source_missing", "  [source-missing] {} 源预下载失败：{}"),
        ("build.source_missing_fatal", "  [source-missing] {} 源预下载失败（http/https 源必须下载，非交互无 operator 介入）：{}"),
        ("build.repo_fail", "  [!] {} 上传本地仓库失败: {}"),
        ("build.index_fail", "  [!] {} 更新 repo index 失败: {}"),
        ("build.serve_fail", "内嵌 repo 服务器绑定 127.0.0.1:{} 失败: {}（默认端口 80 需要 root）"),
        ("build.index_no_soname", "  [warn] {} 的 needed_so 全空——像是剥离时代遗留的旧索引，请 `farm seed` 重新播种（否则 ABI 传播失明）"),
        ("build.group_parse_fail", "  [warn] 解析重建组 {} 失败，已忽略"),
        ("build.prompt", "  选择：1) 打开 shell 修复（退出后继续构建该包） 2) 跳过此包 3) 结束构建"),
        ("build.prompt_invalid", "  无效输入，请输入 1/2/3"),
        ("build.fix_shell", "  [fix] 宿主 shell：{}（修改配方/源后 exit，自动继续构建该包）"),
        ("build.fix_shell_exit", "  [fix] {} shell 已退出，继续构建"),
        ("build.skipped", "（operator 跳过）"),
        // ── 汇总 ──
        ("summary.title", "[build 汇总] {}，{}，{}，{}，{}，{}（不检测版本——更新由 farm track 生成）"),
        ("summary.source_missing", "source-missing: {}"),
        ("summary.blocked", "BLOCKED（需手动 `farm build <pkg>` 重跑）: {}"),
        ("summary.skipped", "跳过: {}"),
        ("summary.built", "构建 {}"),
        ("summary.repacked", "repack {}"),
        ("summary.abi_broken", "ABI 断裂 {}"),
        ("summary.skipped_cnt", "跳过 {}"),
        ("summary.blocked_cnt", "BLOCKED {}"),
        // ── seed ──
        ("seed.progress", "  [seed] {} {}"),
        ("seed.summary", "[seed 汇总] 包 {}：成功 {}，失败 {}"),
        // ── export ──
        ("export.summary", "[export 汇总] {}，{}（zstd level 22 ultra 重打包）"),
        ("export.exported", "导出 {}"),
        ("export.failed", "失败 {}"),
        ("export.failed_item", "{}"),
        // ── repack ──
        ("repack.done", "[repack] {} 已重打包（zstd -22 --ultra），sha256 {}"),
        ("repack.ok", "重打包 {} 个"),
        ("repack.summary", "[repack 汇总] 包 {}：{}，index 已更新（{}）"),
        ("repack.no_pkg", "用法：farm repack <pkg> [--input <dir>] [--arch <arch>]"),
        // ── state / track / gen-trackers ──
        ("state.open", "[state] 状态库 {}"),
        ("state.open_fail", "  [warn] 打开状态库失败（本次不记录）: {}"),
        ("build.need_image", "farm build 必须指定 --image <镜像>：仅容器构建，禁止主机直接构建（会污染宿主环境）"),
        ("track.applied", "  [已生成新版] {}.json 更新为 {}"),
        ("track.latest", "[track] {}: {}（已最新）"),
        ("track.parallel", "[track] 并行 {} jobs（依赖顺序由 dep_edges 门控，after/last 保持）"),
        ("track.no_tracker", "  无 tracker: {}"),
        ("gen.no_model", "缺少模型：--model <name> 或 LANKEFARM_LLM_MODEL"),
        ("gen.none", "[gen-trackers] 无待生成包"),
        ("gen.dir_fail", "创建 {} 失败: {}"),
        ("gen.batch", "[gen-trackers] 批次 {}（{} 个包）"),
        ("gen.fetch", "  [抓取] {}: {}"),
        ("gen.write_fail", "  [!] 写 {}.yaml 失败: {}"),
        ("gen.batch_fail", "  [!] 批次失败（LLM 错误）: {}"),
        ("pkgs.not_dir", "{} 不是目录"),
        ("pkgs.read_fail", "读取 {} 失败: {}"),
        // ── net / lpkg_binding / build 细节 ──
        ("net.download_fail", "  [!] 下载 {} 失败（{}/{}）: {}"),
        ("build.scan_fail", "  [!] 扫描 {} 产物失败: {}"),
        ("build.incremental_skip", "[增量] 跳过 {} 个版本与本地 repo 一致的包"),
        ("build.blocked_ni", "  [BLOCKED] {} 构建失败：{}（非交互，标记阻塞继续）"),
        ("build.repack_fail", "  [!] repack {} 失败: {}"),
        ("build.release_bump", "  [release] {} release → {}（传播重建）"),
        ("build.meta_sync", "  [元数据] {} needed_so/provides 已同步到 LankeBUILD.json"),
        ("build.source_prefetched", "  [源] {} 已预取 {}"),
        ("build.plan", "[build 计划] 构建顺序（{} 包，仅最开始能确认的；ABI 受害者随后动态入队）："),
        ("build.plan_confirm", "确认开始构建？回车继续，n 取消："),
        ("build.plan_cancel", "[build] 已取消构建"),
        ("build.backup_clean", "  [backup] {} 已清理（旧 SONAME 不再被任何包 needed_so 引用）"),
        ("gen.write", "  [写] {}.yaml"),
        ("gen.done", "[gen-trackers] 完成，写入 {} 个 yaml"),
        // ── track / gen-trackers 补充 ──
        ("track.usage", "farm track <pkg> --run --pkgs <dir> --data <dir>"),
        ("track.skip_no_remote", "  [skip] {}: 仅 file:// 或无远程源，无需 track"),
        ("track.no_tracker_pkg", "{} 无对应 tracker（data/trackers 中无 pkg-name 匹配的 yaml，用 farm gen-trackers 生成或手动写）"),
        ("track.proposal", "[track] {}: {} → {}（{}）"),
        ("track.work_sources", "  [work_sources] {}"),
        ("track.regress", "  [!] {}: 探测到倒退版本 {} → {}（tracker 配置/模板疑似错误，忽略）"),
        ("track.probe_fail", "  [probe 失败] {}: {}"),
        ("track.probe_fail_all", "  [!] {}: {}"),
        ("track.summary", "[汇总] 包 {} 个：{} {}，探测失败 {}，无 tracker {}，孤儿 yaml {}"),
        ("track.summary_applied", "已应用"),
        ("track.summary_proposals", "提案"),
        ("track.orphans", "  [忽略] 孤儿 tracker yaml（无对应 LankeBUILD.json）: {}"),
        ("track.apply_fail", "  [!] 写入 LankeBUILD.json 失败 {}"),
        ("track.skip_no_remote_short", "  [skip] {}: 仅 file:// 或无远程源"),
        ("gen.targets", "[gen-trackers] 目标 {} 个包，API {}，模型 {}，每批 12 个"),
        ("gen.load_fail", "  [!] {}: {}"),
        ("gen.llm_calling", "  [LLM] 抓取完毕（{} 个包，prompt ~{} 字符），调用 API..."),
        ("gen.retry_exhausted", "  [!] 批次重试 {} 次仍不完整——缺 {}，多 {}"),
        ("gen.retry_feedback", "  [重试 {}] 缺 {}, 多 {}，带反馈重新调用..."),
        ("gen.no_remote_src", "(无远程源)"),
        // ── build / serve / seed CLI ──
        ("build.usage", "farm build <pkg>... | --all"),
        ("build.ctrl_c_clean", "\n[ctrl-c] 已清理：容器 / DB 条目 / 滚动镜像，baseline 已用最新 commit 覆盖"),
        ("build.ctrl_c_install_fail", "  [warn] 安装 Ctrl+C 处理器失败（中断将不自动清理）: {}"),
        ("build.serve_start", "[serve] 内嵌本地 repo 服务器 http://127.0.0.1:{}（docker 模式）"),
        ("build.finalize_fail", "  [warn] build 收尾 finalize 失败（滚动镜像残留，下次构建会重试）: {}"),
        ("build.version_change_stderr", "  [version-change] {}→{} 判定脚本 stderr: {}"),
        ("build.version_change_fail", "[!] {} version-change 脚本失败: {}"),
        ("serve.started", "[serve] 本地 repo 服务器 http://{}:{}（root={}）"),
        ("seed.usage", "farm seed --remote <url> [--arch x86_64] [--out <dir>]"),
        ("seed.failed_item", "  [!] {}: {}"),
        ("test.skip_host_libc", "跳过：宿主机无 libc.so.6 可用作 ELF fixture"),
    ])
});

/// 英文目录。
static EN: LazyLock<HashMap<&'static str, &'static str>> = LazyLock::new(|| {
    HashMap::from([
        ("build.start", "[build] {} {}{}"),
        ("build.victim", " (ABI propagation rebuild)"),
        ("build.repo", "  [repo] {} {} placed in local repo, index updated"),
        ("build.abi", "  [abi] {} SONAME changed ({}), {} needs rebuild"),
        ("build.group_rebuild", "  [rebuild-group] {} runtime changed, {} needs rebuild"),
        ("build.version_rebuild", "  [version-change] {} version changed, {} needs rebuild"),
        ("build.repack", "  [repack] {} metadata.json needed_so/provides corrected and repacked (no rebuild)"),
        ("build.blocked", "  [BLOCKED] {}: {}"),
        ("build.cycle", "  [cycle] circular dependency {} -> {} cut (if build fails, inspect this pair)"),
        ("build.source_missing", "  [source-missing] {} source pre-download failed: {}"),
        ("build.source_missing_fatal", "  [source-missing] {} source pre-download failed (http/https sources must be downloaded; no interactive operator): {}"),
        ("build.repo_fail", "  [!] {} failed to publish to local repo: {}"),
        ("build.index_fail", "  [!] {} failed to update repo index: {}"),
        ("build.serve_fail", "embedded repo server bind 127.0.0.1:{} failed: {} (default port 80 needs root)"),
        ("build.index_no_soname", "  [warn] {} has zero needed_so - looks like a pre-stripping legacy index; re-run `farm seed` or ABI propagation goes blind"),
        ("build.group_parse_fail", "  [warn] failed to parse rebuild group {}, ignored"),
        ("build.prompt", "  Choose: 1) open shell to fix (continues this package on exit) 2) skip 3) end"),
        ("build.prompt_invalid", "  Invalid input, enter 1/2/3"),
        ("build.fix_shell", "  [fix] host shell: {} (exit after fixing recipe/source, continues this package)"),
        ("build.fix_shell_exit", "  [fix] {} shell exited, continuing"),
        ("build.skipped", " (operator skip)"),
        ("summary.title", "[build summary] {}, {}, {}, {}, {}, {} (no version check - updates come from farm track)"),
        ("summary.source_missing", "source-missing: {}"),
        ("summary.blocked", "BLOCKED (rerun `farm build <pkg>` manually): {}"),
        ("summary.skipped", "skipped: {}"),
        ("summary.built", "built {}"),
        ("summary.repacked", "repacked {}"),
        ("summary.abi_broken", "ABI breaks {}"),
        ("summary.skipped_cnt", "skipped {}"),
        ("summary.blocked_cnt", "BLOCKED {}"),
        ("seed.progress", "  [seed] {} {}"),
        ("seed.summary", "[seed summary] packages {}: ok {}, failed {}"),
        ("export.summary", "[export summary] {}, {} (zstd level 22 ultra repack)"),
        ("export.exported", "exported {}"),
        ("export.failed", "failed {}"),
        ("export.failed_item", "{}"),
        ("repack.done", "[repack] {} repacked (zstd -22 --ultra), sha256 {}"),
        ("repack.ok", "repacked {}"),
        ("repack.summary", "[repack summary] package {}: {}, index updated ({})"),
        ("repack.no_pkg", "Usage: farm repack <pkg> [--input <dir>] [--arch <arch>]"),
        ("state.open", "[state] state DB {}"),
        ("state.open_fail", "  [warn] failed to open state DB (not recording this run): {}"),
        ("build.need_image", "farm build requires --image <image>: container builds only, host builds would pollute the environment"),
        ("track.applied", "  [applied] {}.json updated to {}"),
        ("track.latest", "[track] {}: {} (already latest)"),
        ("track.parallel", "[track] {} parallel jobs (dependency order gated by dep_edges, after/last kept)"),
        ("track.no_tracker", "  no tracker: {}"),
        ("gen.no_model", "Missing model: --model <name> or LANKEFARM_LLM_MODEL"),
        ("gen.none", "[gen-trackers] nothing to generate"),
        ("gen.dir_fail", "failed to create {}: {}"),
        ("gen.batch", "[gen-trackers] batch {} ({} packages)"),
        ("gen.fetch", "  [fetch] {}: {}"),
        ("gen.write_fail", "  [!] failed to write {}.yaml: {}"),
        ("gen.batch_fail", "  [!] batch failed (LLM error): {}"),
        ("pkgs.not_dir", "{} is not a directory"),
        ("pkgs.read_fail", "failed to read {}: {}"),
        ("net.download_fail", "  [!] download {} failed ({}/{}): {}"),
        ("build.scan_fail", "  [!] scan {} artifact failed: {}"),
        ("build.incremental_skip", "[incremental] skipped {} packages already matching local repo"),
        ("build.blocked_ni", "  [BLOCKED] {} build failed: {} (non-interactive, marking blocked)"),
        ("build.repack_fail", "  [!] repack {} failed: {}"),
        ("build.release_bump", "  [release] {} release -> {} (propagation rebuild)"),
        ("build.meta_sync", "  [metadata] {} needed_so/provides synced to LankeBUILD.json"),
        ("build.source_prefetched", "  [source] {} prefetched {}"),
        ("build.plan", "[build plan] build order ({} packages, initially-confirmed only; ABI victims join dynamically):"),
        ("build.plan_confirm", "Start build? Enter to continue, n to cancel: "),
        ("build.plan_cancel", "[build] cancelled"),
        ("build.backup_clean", "  [backup] {} cleaned (old SONAME no longer referenced by any package needed_so)"),
        ("gen.write", "  [write] {}.yaml"),
        ("gen.done", "[gen-trackers] done, wrote {} yaml"),
        // ── track / gen-trackers additions ──
        ("track.usage", "farm track <pkg> --run --pkgs <dir> --data <dir>"),
        ("track.skip_no_remote", "  [skip] {}: file:// only or no remote source, nothing to track"),
        ("track.no_tracker_pkg", "{} has no tracker (no yaml with matching pkg-name in data/trackers; use farm gen-trackers or write manually)"),
        ("track.proposal", "[track] {}: {} -> {} ({})"),
        ("track.work_sources", "  [work_sources] {}"),
        ("track.regress", "  [!] {}: probed regression {} -> {} (tracker config/template likely wrong, ignored)"),
        ("track.probe_fail", "  [probe failed] {}: {}"),
        ("track.probe_fail_all", "  [!] {}: {}"),
        ("track.summary", "[summary] {} packages: {} {}, probe failures {}, no tracker {}, orphan yaml {}"),
        ("track.summary_applied", "applied"),
        ("track.summary_proposals", "proposals"),
        ("track.orphans", "  [ignored] orphan tracker yaml (no matching LankeBUILD.json): {}"),
        ("track.apply_fail", "  [!] failed to write LankeBUILD.json: {}"),
        ("track.skip_no_remote_short", "  [skip] {}: file:// only or no remote source"),
        ("gen.targets", "[gen-trackers] {} target packages, API {}, model {}, 12 per batch"),
        ("gen.load_fail", "  [!] {}: {}"),
        ("gen.llm_calling", "  [LLM] fetched ({} packages, prompt ~{} chars), calling API..."),
        ("gen.retry_exhausted", "  [!] batch retried {} times still incomplete -- missing {}, extra {}"),
        ("gen.retry_feedback", "  [retry {}] missing {}, extra {}, calling again with feedback..."),
        ("gen.no_remote_src", "(no remote source)"),
        // ── build / serve / seed CLI ──
        ("build.usage", "farm build <pkg>... | --all"),
        ("build.ctrl_c_clean", "\n[ctrl-c] cleaned up: containers / DB entries / rolling images, baseline overwritten with latest commit"),
        ("build.ctrl_c_install_fail", "  [warn] failed to install Ctrl+C handler (interrupts will not auto-clean): {}"),
        ("build.serve_start", "[serve] embedded local repo server http://127.0.0.1:{} (docker mode)"),
        ("build.finalize_fail", "  [warn] build finalize failed (rolling images left behind, retried next build): {}"),
        ("build.version_change_stderr", "  [version-change] {}->{} decision script stderr: {}"),
        ("build.version_change_fail", "[!] {} version-change script failed: {}"),
        ("serve.started", "[serve] local repo server http://{}:{} (root={})"),
        ("seed.usage", "farm seed --remote <url> [--arch x86_64] [--out <dir>]"),
        ("seed.failed_item", "  [!] {}: {}"),
        ("test.skip_host_libc", "skipping: no host libc.so.6 available for ELF fixture"),
    ])
});

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detect_lang_matches_env_variants() {
        assert_eq!(detect_lang("en_US.UTF-8"), Lang::En);
        assert_eq!(detect_lang("EN"), Lang::En); // 大写
        assert_eq!(detect_lang("en"), Lang::En);
        assert_eq!(detect_lang("zh_CN.UTF-8"), Lang::Zh);
        assert_eq!(detect_lang("C"), Lang::Zh); // POSIX C locale 回退中文
        assert_eq!(detect_lang(""), Lang::Zh);
        assert_eq!(detect_lang("fr_FR"), Lang::Zh); // 非 en 一律中文
    }

    #[test]
    fn catalog_get_returns_both_languages() {
        assert!(
            catalog_get(Lang::Zh, "build.start").is_some(),
            "zh 应有 build.start"
        );
        assert!(
            catalog_get(Lang::En, "build.start").is_some(),
            "en 应有 build.start"
        );
        assert_eq!(catalog_get(Lang::Zh, "no.such"), None);
    }

    #[test]
    fn zh_and_en_are_not_identical() {
        // 至少有一条消息中英不同（证明目录真在切换，而非同一份）
        assert_ne!(
            catalog_get(Lang::Zh, "build.repo"),
            catalog_get(Lang::En, "build.repo")
        );
    }

    #[test]
    fn fmt_substitutes_positional() {
        assert_eq!(fmt("hello {} {}", &[&"a", &"b"]), "hello a b");
        assert_eq!(fmt("plain", &[] as &[&dyn std::fmt::Display]), "plain");
        // 参数多于占位：多出的拼在末尾
        assert_eq!(fmt("{}", &[&"a", &"b"]), "ab");
        // 占位多于参数：剩余 {} 原样保留
        assert_eq!(fmt("{} and {}", &[&"a"]), "a and {}");
        // 参数含数字/路径
        assert_eq!(fmt("{} {}", &[&3usize, &"/tmp/x"]), "3 /tmp/x");
    }

    #[test]
    fn tr_missing_key_falls_back_to_key() {
        assert_eq!(tr("no.such.key"), "no.such.key");
        assert!(!tr("build.start").is_empty(), "目录应含 build.start");
    }

    #[test]
    fn tr_macro_expands_with_args() {
        // 宏带参：位置填充
        let s = tr!("build.repo", "libfoo", "/out/x86_64/libfoo/1.0.lpkg");
        assert!(
            s.contains("libfoo") && s.contains("1.0.lpkg"),
            "宏应填充参数: {s}"
        );
    }

    #[test]
    fn zh_and_en_catalogs_have_same_keys() {
        let zh_keys: std::collections::HashSet<_> = ZH.keys().copied().collect();
        let en_keys: std::collections::HashSet<_> = EN.keys().copied().collect();
        assert_eq!(zh_keys, en_keys, "中英目录键必须一致");
    }
}
