//! i18n.rs — 用户可见消息的本地化。
//!
//! 中文为默认；生效 locale 按 gettext/POSIX 优先级 `LC_ALL` > `LC_MESSAGES` > `LANG`（空值视为未设），
//! 其值以 `en` 开头时切英文。键缺失回退到键名（便于发现漏译）。
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

/// 纯函数：从生效 locale 值判定语言。`en` 开头 → 英文，否则中文。
pub fn detect_lang(lang_env: &str) -> Lang {
    if lang_env.to_ascii_lowercase().starts_with("en") {
        Lang::En
    } else {
        Lang::Zh
    }
}

/// 纯函数：按 gettext/POSIX 优先级取生效 locale —— `LC_ALL` > `LC_MESSAGES` > `LANG`，空值视为未设。
/// POSIX 里 `LC_ALL` 覆盖一切，所以颠倒成 `LANG.or_else(LC_ALL)` 会在
/// `LC_ALL=en_US.UTF-8` + `LANG=zh_CN.UTF-8` 时错选中文。
pub fn pick_locale<'a>(
    lc_all: Option<&'a str>,
    lc_messages: Option<&'a str>,
    lang: Option<&'a str>,
) -> Option<&'a str> {
    [lc_all, lc_messages, lang]
        .into_iter()
        .flatten()
        .find(|s| !s.is_empty())
}

fn lang() -> Lang {
    *LANG.get_or_init(|| {
        // 先绑定 String 再借（临时值会在 let 语句末 drop，不能直接 as_deref 传给 pick_locale）
        let lc_all = std::env::var("LC_ALL").ok();
        let lc_messages = std::env::var("LC_MESSAGES").ok();
        let lang = std::env::var("LANG").ok();
        let l =
            pick_locale(lc_all.as_deref(), lc_messages.as_deref(), lang.as_deref()).unwrap_or("");
        detect_lang(l)
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
        ("build.repack", "  [repack] {} metadata.json needed_so/provides_soname 已修正并重打包（不 rebuild）"),
        ("build.blocked", "  [BLOCKED] {}：{}"),
        ("build.cycle", "  [cycle] 循环依赖 {} → {}，已切断该边（若后续构建失败请人工检查这对依赖）"),
        ("build.source_missing", "  [source-missing] {} 源预下载失败：{}"),
        ("build.source_missing_fatal", "  [source-missing] {} 源预下载失败（http/https 源必须下载，非交互无 operator 介入）：{}"),
        ("build.repo_fail", "  [!] {} 上传本地仓库失败: {}"),
        ("build.index_fail", "  [!] {} 更新 repo index 失败: {}"),
        ("build.serve_fail", "内嵌 repo 服务器绑定 127.0.0.1:{} 失败: {}（默认端口 80 需要 root）"),
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
        ("export.summary", "[export 汇总] {}，{}（扁平化复制，不重打包）"),
        ("export.exported", "导出 {}"),
        ("export.failed", "失败 {}"),
        ("export.failed_item", "{}"),
        // ── repack ──
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
        ("abifix.title", "[abifix] {} 个包引用无 provider 的 SONAME，bump release 并重建"),
        ("abifix.target", "  {}: 引用无 provider {}"),
        ("abifix.target_ver", "  {}: {} 的提供者不覆盖要求的版本 {}（符号版本缺口）"),
        ("abifix.none", "[abifix] 无引用无 provider SONAME 的包（仓库一致）"),
        ("build.meta_sync", "  [元数据] {} needed_so/provides_soname 已同步到 LankeBUILD.json"),
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
        ("track.probe_source_fail", "  [源探测失败] {}: {}"),
        ("track.probe_fail_skip", "  [跳过写入] {}: 源探测失败，需 --probe-fail-continue 才会在 --run 下写入"),
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
        ("cli.need_root", "farm 操作命令（build/validate/abifix/export/seed）必须以 root 运行——.lpkg 解包/重打包要读写 root 属主文件与 SUID/SGID。请用 root 用户或 `sudo farm …` 执行"),
        ("abi_fullchk.start", "[abi-fullchk] 审计 input={} arch={} 缓存={}（范围: {}）"),
        ("abi_fullchk.all", "全部包"),
        ("abi_fullchk.summary", "[abi-fullchk] 完成：{} 个 .lpkg / {} 个 ELF；缓存命中 {} / 重扫 {}；provider SONAME {}"),
        ("abi_fullchk.none", "[abi-fullchk] 无未解析符号@版本（consumer 全部可解析）"),
        ("abi_fullchk.missing_pkg", "\n### {}: {} 个未解析版本符号"),
        ("abi_fullchk.missing_item", "    {}\n        {} @ {}   (候选提供: {})"),
        ("abi_fullchk.more", "    ... 共 {}"),
        ("abi_fullchk.total", "\n[TOTAL] {} 个 not-found，分布在 {} 个包"),
        ("abi_fullchk.failed", "  [!] {}"),
        ("chk.summary", "[{}] 检查 {} 包；缓存命中 {} / 重扫 {}"),
        ("chk.none", "[{}] 无发现"),
        ("chk.header", "\n### {}: {} 项"),
        ("chk.item", "    {}\n        {}"),
        ("chk.total", "[{}] 共 {} 项"),
        ("chk.failed", "  [!] {}"),
        // ── chk 各检則的 finding 文案（provider/consumer 判定 + 打包铁律护栏）──
        ("chk.abi.orphan_soname", "{}（仓库无任何包提供：提供者已换新 SONAME 或尚未构建 → 本包需重建/重链）"),
        ("chk.abi.unresolved_symbol", "{} @ {}"),
        ("chk.abi.unresolved_symbol_cands", "{} @ {}（候选提供: {}）"),
        ("chk.module.no_provider", "{}：仓库内无任何包提供（{} 缺失）"),
        ("chk.module.missing_dep", "依赖树不正确：{} 少依赖（提供者 {}，请加入 deps 或由 needed_so 覆盖）"),
        ("chk.builddep.missing_tool", "{}，但配方 build_deps 缺 {}（构建期需要该工具，请补进 build_deps）"),
        ("chk.builddep.files_more", " 等 {} 个"),
        ("chk.build-deps.provider_not_declared", "needed_so 的 {} 由 {} 提供，但不在本包 build_deps（构建容器只装 build_deps；被传递满足也不算——needed_so 是直接链接依赖）"),
        ("chk.pkg-err.misplaced_root", "错位：应为 /etc 或 /var（无 usr 前缀），不能有 usr/etc|usr/var"),
        ("chk.pkg-err.libtool_archive", "包含 .la（libtool 存档，已废弃，应删）"),
        ("chk.pkg-err.static_lib", "包含 .a 静态库（一般应删；若 .cmake 引用则用 IGNORE_CHK_PKGERR 豁免）"),
        ("chk.hook.sysusers", "建了 sysusers 档案但 postinst 未调 systemd-sysusers"),
        ("chk.hook.tmpfiles", "建了 tmpfiles 档案但 postinst 未调 systemd-tmpfiles --create"),
        ("chk.pycache.dir", "包含 __pycache__ 字节码缓存目录（{} 项；构建期生成、含绝对路径且不可复现，应在 package 阶段删除）"),
        ("chk.pycache.loose", "散落的 Python 字节码（.pyc/.pyo，不在 __pycache__ 里；构建期生成、不可复现，应在 package 阶段删除）"),
        ("chk.qml.rel_include", "相对路径 include {} 的提供者在 {}，不在本包 deps/needed_so"),
        ("chk.vapi.label", "包含 Vala 绑定 .vapi"),
        ("chk.introspection.label", "包含 g-ir-scanner 生成的 .gir（GObject Introspection 元数据）"),
        ("chk.unknown_kind", "未知检則: {}"),
        ("farm_flags.unknown", "  未知 farm flag: {}"),
        ("build.image_fallback", "  [warn] 从 {} 创建失败，回退原始 base 并重置滚动计数"),
        (
            "build.container_kept",
            "  [容器已保留] 构建现场在里面，进去看：docker exec -it {} bash（用完请 docker rm -f {}）",
        ),
        ("track.parse_fail_ignored", "  [warn] 忽略无法解析的 tracker {}: {}"),
        // ── repo 上传 / ABI 过渡备份 / 索引 ──
        ("repo.read_metadata_json", "读 {} 的 metadata.json 失败: {}"),
        ("repo.repack_failed", "repack {} 失败: {}"),
        ("repo.no_artifact", "无构建产物"),
        ("repo.no_valid_version", "配方无有效版本"),
        ("repo.create_pkg_dir", "创建 {} 失败: {}"),
        ("repo.move_artifact", "移动 {} → {} 失败: {}"),
        ("repo.read_pkg_dir", "读取仓库目录 {} 失败: {}"),
        ("repo.read_pkg_dir_entry", "读取仓库目录 {} 的一项失败: {}"),
        ("repo.backup_old_sonames", "备份 {} 的旧 SONAME 失败（{}），保留旧版本不删除"),
        ("repo.remove_old_version", "删除旧版本 {} 失败: {}"),
        ("repo.read_old_metadata", "读旧包 {} 的 metadata 失败: {}"),
        ("repo.create_backup_dir", "创建备份目录失败: {}"),
        ("repo.read_lib_dir", "读取库目录 {} 失败: {}"),
        ("repo.read_lib_dir_entry", "读取库目录 {} 的一项失败: {}"),
        ("repo.read_symlink", "读取符号链接 {} 失败: {}"),
        ("repo.create_backup_subdir", "创建备份子目录 {} 失败: {}"),
        ("repo.backup_file", "备份 {} → {} 失败: {}"),
        ("repo.create_backup_symlink", "创建备份符号链接 {} 失败: {}"),
        ("repo.remove_old_backup", "删除旧备份 {} 失败: {}"),
        ("repo.read_file", "读 {} 失败: {}"),
        ("repo.write_file", "写 {} 失败: {}"),
        ("repo.index_missing", "缺少本地 repo 索引 {}（{}）——请先 `farm seed` 播种，禁止无基线构建"),
        ("repo.index_empty", "本地 repo 索引 {} 为空——请先 `farm seed` 播种，禁止无基线构建"),
        // ── libarchive（.lpkg 读写）──
        ("archive.no_reason", "（libarchive 未给出原因）"),
        ("archive.op_failed", "libarchive {} 失败（{}）: {}"),
        ("archive.op_warning", "warning: libarchive {} 有警告（{}）: {}"),
        ("archive.not_utf8", "{} 的{}不是合法 UTF-8，拒绝静默改名"),
        ("archive.nul_byte", "路径含 NUL 字节: {}"),
        ("archive.member_name_too_long", "{} 成员名过长（{} 字节 > {}）"),
        ("archive.component_too_long", "{} 成员名的路径分量过长（{} > {}）"),
        ("archive.member_path_escape", "{} 成员路径越界（绝对路径或含 ..）: {}"),
        ("archive.member_control_char", "{} 成员名含控制字符: {}"),
        ("archive.member_escapes_root", "{} 成员路径逃出解压根: {}"),
        ("archive.step_symlink_target", "符号链接目标"),
        ("archive.symlink_escapes_root", "{} 的符号链接 {} 目标逃出解压根: {}"),
        ("archive.duplicate_member", "{} 有重名成员: {}"),
        ("archive.init_failed", "libarchive 初始化失败（{}）"),
        ("archive.step_filter", "启用过滤器"),
        ("archive.step_format", "启用归档格式"),
        ("archive.disk_init_failed", "libarchive 写盘端初始化失败（{}）"),
        ("archive.step_extract_options", "设置解包选项"),
        ("archive.step_open", "打开包"),
        ("archive.step_read_header", "读条目头"),
        ("archive.step_member_name", "成员名"),
        ("archive.special_file_member", "{} 含不支持的特殊文件成员 {}（fifo/设备/socket）——不应出现在 .lpkg"),
        ("archive.step_hardlink_target", "硬链接目标"),
        ("archive.step_extract_entry", "解包条目"),
        ("archive.read_data_failed", "libarchive 读条目数据失败（{} / {}）: {}"),
        ("archive.write_data_failed", "libarchive 写条目数据失败（{} / {}）: {}"),
        ("archive.step_finish_entry", "收尾条目"),
        ("archive.step_close_disk", "收尾写盘"),
        ("archive.read_member_failed", "读 {} 失败（{}）: {}"),
        ("archive.step_skip_data", "跳过成员数据"),
        ("archive.step_zstd_filter", "启用 zstd 过滤器"),
        ("archive.zstd_level_ignored", "warning: libarchive 不接受 zstd compression-level={}（{}）: {}"),
        ("archive.step_pax_format", "设置 pax_restricted 格式"),
        ("archive.step_open_output", "打开输出包"),
        ("archive.stat_failed", "stat {} 失败: {}"),
        ("archive.special_file_walk", "content 含不支持的特殊文件类型 {}（socket/设备/fifo）——不应出现在 .lpkg"),
        ("archive.entry_alloc_failed", "libarchive 条目分配失败（{}）"),
        ("archive.lstat_failed", "lstat {} 失败: {}"),
        ("archive.disk_reader_init_failed", "libarchive 磁盘读取器初始化失败（{}）"),
        ("archive.step_symlink_mode", "设置符号链接模式"),
        ("archive.read_metadata_failed", "读 {} 的元数据失败（{}）: {}"),
        ("archive.readlink_failed", "read_link {} 失败: {}"),
        ("archive.step_write_header", "写条目头"),
        ("archive.open_failed", "打开 {} 失败: {}"),
        ("archive.read_failed", "读 {} 失败: {}"),
        ("archive.write_file_data_failed", "写 {} 的数据失败（{}）: {}"),
        ("archive.close_output_failed", "libarchive 收尾 {} 失败（包被截断）"),
        ("archive.close_output_warning", "warning: libarchive 收尾 {} 有警告: {}（包内容可能不完整）"),
        // ── docker 编排 ──
        ("docker.export_failed", "docker export 失败: {}"),
        ("docker.import_failed", "docker import 失败: {}"),
        ("docker.create_finalize_failed", "docker create（finalize）失败: {}"),
        ("docker.create_failed", "docker create 失败: {}"),
        ("docker.start_failed", "docker start 失败（{}）"),
        ("docker.write_mirror_conf_failed", "容器内写入 mirror.conf 失败（{}）"),
        ("docker.exec_failed", "docker exec 失败: {}"),
        ("docker.upgrade_failed", "容器内 lpkg upgrade 失败（{}）"),
        ("docker.commit_failed", "docker commit {} 失败（滚动快照未保存）"),
        ("docker.restore_exec_failed", "docker exec 恢复旧 .so 失败: {}"),
        ("docker.restore_backup_failed", "容器内恢复备份旧 .so 失败（{}）"),
        ("docker.cp_recipe_failed", "docker cp {} 配方进容器失败"),
        ("docker.build_failed", "容器内 lpkg build 失败（{}）"),
        ("docker.list_artifacts_failed", "docker exec 取产物名失败: {}"),
        ("docker.no_artifact", "容器内未产出 .lpkg（{}）"),
        ("docker.cp_artifact_failed", "docker cp {} .lpkg 回宿主失败"),
        ("docker.build_image_failed", "docker build 失败: {}"),
        // ── net / repack / state / seed / serve / export / scan / llm ──
        ("net.create_remote", "创建 remote {} 失败: {}"),
        ("net.connect_failed", "连接 {} 失败: {}"),
        ("net.list_refs_failed", "列出 {} 的 refs 失败: {}"),
        ("net.list_tags_unimplemented", "list_tags 未实现: {}"),
        ("net.read_url", "读 {}"),
        ("net.create_file_failed", "创建 {} 失败"),
        ("net.write_file_failed", "写 {} 失败"),
        ("net.request_failed", "{} 请求失败"),
        ("net.read_response_failed", "读 {} 响应失败"),
        ("repack.write_meta_failed", "写 {} 失败: {}"),
        ("repack.replace_failed", "替换 {} 失败: {}"),
        ("repack.read_dir_failed", "读取 {} 失败: {}"),
        ("repack.read_dir_entry_failed", "读取 {} 的目录项失败: {}"),
        ("repack.stat_failed", "stat {} 失败: {}"),
        ("state.create_dir_failed", "创建状态目录 {} 失败"),
        ("state.open_sqlite_failed", "打开 SQLite {} 失败"),
        ("state.init_schema_failed", "初始化 SQLite schema 失败"),
        ("state.update_job_failed", "更新 job 状态失败"),
        ("state.delete_job_failed", "删除 job 条目失败"),
        ("state.record_history_failed", "记录构建历史失败"),
        ("seed.create_dir_failed", "创建 {} 失败: {}"),
        ("seed.write_index_failed", "写本地 index.txt 失败: {}"),
        ("seed.create_dir_short", "创建目录失败"),
        ("seed.sha256_mismatch", "SHA256 不匹配"),
        ("serve.root_inaccessible", "root {} 不可访问: {}"),
        ("serve.bind_failed", "绑定 {}:{} 失败: {}"),
        ("export.create_output_failed", "创建输出目录 {} 失败: {}"),
        ("export.read_repo_failed", "读取 {} 失败: {}"),
        ("export.bad_pkg_name", "{}: 包名含非法字符，跳过"),
        ("export.no_lpkg", "{}: 无 .lpkg"),
        ("export.unparsable_version", "{}: 无法解析 .lpkg 文件名版本"),
        ("export.bad_version_name", "{}: 版本名 {} 含非法字符，跳过"),
        ("scan.remove_tree_failed", "删除目录树 {} 失败: {}"),
        ("scan.create_dir_failed", "创建 {} 失败: {}"),
        ("scan.parse_metadata_failed", "解析 metadata.json 失败: {}"),
        ("scan.no_metadata", "{} 内无 metadata.json"),
        ("scan.read_failed", "读 {} 失败: {}"),
        ("scan.parse_file_failed", "解析 {} 失败: {}"),
        ("llm.api_failed", "LLM API 调用失败: {}"),
        ("llm.read_response_failed", "LLM 响应读取失败: {}"),
        ("llm.non_json_response", "LLM 响应非 JSON: {}"),
        ("llm.missing_content", "LLM 响应无 choices[0].message.content: {}"),
        // ── track tracker 解析 / 探测 ──
        ("track.parse_yaml_failed", "解析 tracker yaml 失败: {}"),
        ("track.no_entries", "tracker {} 无 sources/work_sources 条目"),
        ("track.version_source_oob", "version-source 越界: {}[{}]（共 {} 条）"),
        ("track.probe_failed", "{}[{}] 探测失败: {}"),
        ("track.bad_version_var_name", "version-var 名 `{}` 不是合法 shell 变量名（应为 [A-Za-z_][A-Za-z0-9_]*）"),
        ("track.reserved_version_var", "version-var 名不得占用保留变量 `PKG_NAME`"),
        ("track.major_of_no_version", "major-of 依赖 {} 无版本（读 LankeBUILD.json 失败）"),
        ("track.unknown_template", "未知 tracker_template: {}"),
        ("track.field_validation_failed", "tracker 字段校验失败（{}）: {}"),
        ("track.unsupported_field", "tracker-template {} 不支持字段: {}（需要版本封顶/稳定分支等约束时改用支持它的模板，或用条目级 script 模板 `tracker-template: script`）"),
        ("track.invalid_version_source", "version-source 无效 '{}'（应为 sources[i] 或 work_sources[i]）"),
        ("track.slot_not_available", "{} 尚不可用——只能引用本 tracker 中**位于它之前**的槽位（sources 先于 work_sources 探测，列表内从左到右；当前 {} 已探测 {} 条）"),
        ("track.missing_field", "tracker 配置缺 {}"),
        ("track.unresolved_placeholder", "探测生成的 URL 残留未替换占位符: {}"),
        ("track.invalid_regex", "正则无效 {}: {}"),
        ("track.invalid_regex_short", "正则无效: {}"),
        ("track.no_version_match", "{} 中未匹配到版本"),
        ("track.no_matching_release", "releases 中无匹配版本/主版本"),
        ("track.no_matching_tag", "release tag 无匹配版本/主版本"),
        ("track.no_matching_tags", "tags 中无匹配版本/主版本"),
        ("track.gcs_page_limit", "GCS 分页超出安全上限"),
        ("track.gcs_no_match", "{} GCS 列表未匹配到版本"),
        ("track.gnome_no_version_dir", "gnome 目录列表无版本子目录"),
        ("track.gnome_no_constraint_dir", "gnome 目录无匹配主版本/封顶的子目录"),
        ("track.gnome_no_stable_file", "gnome 目录无匹配稳定版本文件"),
        ("track.invalid_exclude_regex", "exclude 正则无效 `{}`: {}"),
        ("track.api_parse_failed", "{} API 响应解析失败: {}"),
        ("track.api_not_array", "{} API 响应非数组"),
        ("track.ctx_list", "列表"),
        ("track.release_api_parse_failed", "release API 响应解析失败: {}"),
        ("track.release_no_tag", "release API 响应无 tag_name"),
        ("track.multi_level_requires_levels", "multi-level-html-index 需 levels 列表（每级 {name, url, pattern}）"),
        ("track.multi_level_missing_version", "levels 缺 name: version（该级捕获 = 包版本）"),
        ("track.multi_level_no_match", "{} 中未匹配到版本（pattern: {}）"),
        ("track.level_name_empty", "levels[{}].name 不得为空"),
        ("track.level_name_reserved", "levels[{}].name 不得取保留名 `{}`（上游名占位符已被占用）"),
        ("track.level_name_duplicate", "levels[{}].name `{}` 与前面的级重名"),
        ("track.multi_level_missing_version_level", "levels 必须有一级 name: {}（该级捕获 = 包版本，不按位置）"),
        ("track.unknown_placeholder", "{} 引用了未知占位符 {}（可用: {}；位置隐式的 {v1}..{vN} 已废弃——每级须用 name 显式命名）"),
        ("track.pypi_json_failed", "PyPI JSON 解析失败（{}）: {}"),
        ("track.pypi_no_releases", "PyPI {} 无 releases"),
        ("track.pypi_no_matching_version", "PyPI {} 无满足约束的版本（major={}）"),
        ("track.pypi_no_sdist_url", "PyPI {} {} 无 sdist URL"),
        ("track.pypi_no_info_version", "PyPI {} 无 info.version"),
        ("track.pypi_latest_no_sdist", "PyPI {} 最新版无 sdist URL"),
        ("track.same_version_no_version", "same-version-of 依赖 {} 无版本（读 LankeBUILD.json/已解析版本失败）"),
        ("track.script_write_failed", "写临时脚本失败: {}"),
        ("track.script_run_failed", "运行 track 脚本失败: {}"),
        ("track.script_nonzero_exit", "track 脚本退出码非零: {}（stderr: {}）"),
        ("track.script_bad_line", "脚本 stdout 第 {} 行不是 `<版本>|URL` 格式（旧的「首行版本 + 后续行 URL」两行格式已废弃）: {}"),
        ("track.script_empty_version", "脚本 stdout 第 {} 行版本为空: {}"),
        ("track.script_empty_url", "脚本 stdout 第 {} 行 URL 为空: {}"),
        ("track.script_no_output", "脚本 stdout 无输出（每行应为 `<版本>|URL`）"),
        ("track.script_needs_expand", "脚本 stdout 输出了 {} 行，但该条目未声明 `expand: true`（一个脚本默认只产一个槽位；动态枚举请显式加 expand: true）"),
        ("track.sourceforge_no_match", "RSS 中未匹配到版本"),
        // ── custom_checks / build / cli ──
        ("chk.create_cache_dir_failed", "创建缓存目录 {} 失败: {}"),
        ("chk.serialize_cache_failed", "序列化缓存失败: {}"),
        ("chk.write_cache_failed", "写缓存 {} 失败: {}"),
        ("chk.create_cache_dir_short", "创建缓存目录失败: {}"),
        ("chk.read_repo_failed", "读取 {} 失败: {}"),
        ("chk.create_scratch_failed", "创建临时工作目录失败: {}"),
        ("chk.unpack_failed", "{}: 解包失败 {}"),
        ("chk.elf_parse_failed", "ELF 解析失败: {}"),
        ("build.no_lankebuild", "无 LankeBUILD.json"),
        ("build.source_no_filename", "{}: 源 URL 无文件名（裸目录 URL 不可作为源）"),
        ("build.source_duplicate_filename", "{}: 与另一源解析出相同文件名 {}（query/fragment 已剥离），无法预下载"),
        ("build.unknown_stage", "未知阶段"),
        ("build.version_script_write_failed", "写 version-change 脚本失败: {}"),
        ("build.version_script_run_failed", "运行 version-change 脚本失败: {}"),
        ("cli.serve_ready_send_failed", "serve 就绪信号发送失败: {}"),
        ("cli.serve_not_ready_5s", "5s 内未就绪"),
        ("cli.open_log_failed", "打开日志文件失败 {}: {}"),
        ("cli.read_failed", "读取 {} 失败: {}"),
        ("cli.parse_failed", "解析 {} 失败: {}"),
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
        ("build.repack", "  [repack] {} metadata.json needed_so/provides_soname corrected and repacked (no rebuild)"),
        ("build.blocked", "  [BLOCKED] {}: {}"),
        ("build.cycle", "  [cycle] circular dependency {} -> {} cut (if build fails, inspect this pair)"),
        ("build.source_missing", "  [source-missing] {} source pre-download failed: {}"),
        ("build.source_missing_fatal", "  [source-missing] {} source pre-download failed (http/https sources must be downloaded; no interactive operator): {}"),
        ("build.repo_fail", "  [!] {} failed to publish to local repo: {}"),
        ("build.index_fail", "  [!] {} failed to update repo index: {}"),
        ("build.serve_fail", "embedded repo server bind 127.0.0.1:{} failed: {} (default port 80 needs root)"),
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
        ("export.summary", "[export summary] {}, {} (flatten-copied, not repacked)"),
        ("export.exported", "exported {}"),
        ("export.failed", "failed {}"),
        ("export.failed_item", "{}"),
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
        ("abifix.title", "[abifix] {} package(s) reference a provider-less SONAME - bumping release and rebuilding"),
        ("abifix.target", "  {}: references unprovided {}"),
        ("abifix.target_ver", "  {}: providers of {} do not cover the required version(s) {} (symbol-version gap)"),
        ("abifix.none", "[abifix] no package references an unprovided SONAME (repo consistent)"),
        ("build.meta_sync", "  [metadata] {} needed_so/provides_soname synced to LankeBUILD.json"),
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
        ("track.probe_source_fail", "  [source probe failed] {}: {}"),
        (
            "track.probe_fail_skip",
            "  [skipped write] {}: source probe failed; need --probe-fail-continue to write with --run",
        ),
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
        ("cli.need_root", "farm operation commands (build/validate/abifix/export/seed) must run as root — .lpkg unpack/repack needs root-owned files and SUID/SGID. Run as root user or via `sudo farm …`"),
        ("abi_fullchk.start", "[abi-fullchk] auditing input={} arch={} cache={} (scope: {})"),
        ("abi_fullchk.all", "all packages"),
        ("abi_fullchk.summary", "[abi-fullchk] done: {} .lpkg / {} ELF; cache hits {} / rescans {}; provider SONAMEs {}"),
        ("abi_fullchk.none", "[abi-fullchk] no unresolved symbol@version (all consumers resolve)"),
        ("abi_fullchk.missing_pkg", "\n### {}: {} unresolved versioned symbols"),
        ("abi_fullchk.missing_item", "    {}\n        {} @ {}   (candidates: {})"),
        ("abi_fullchk.more", "    ... {} more"),
        ("abi_fullchk.total", "\n[TOTAL] {} not-found across {} packages"),
        ("abi_fullchk.failed", "  [!] {}"),
        ("chk.summary", "[{}] checked {} packages; cache hits {} / rescans {}"),
        ("chk.none", "[{}] no findings"),
        ("chk.header", "\n### {}: {} item(s)"),
        ("chk.item", "    {}\n        {}"),
        ("chk.total", "[{}] {} total"),
        ("chk.failed", "  [!] {}"),
        // ── chk finding texts (provider/consumer decisions + packaging guardrails) ──
        ("chk.abi.orphan_soname", "{} (no package in the repo provides it: the provider changed its SONAME or was never built -> rebuild/relink this package)"),
        ("chk.abi.unresolved_symbol", "{} @ {}"),
        ("chk.abi.unresolved_symbol_cands", "{} @ {} (candidates: {})"),
        ("chk.module.no_provider", "{}: no package in the repo provides it ({} missing)"),
        ("chk.module.missing_dep", "dependency tree incorrect: {} is missing a dependency (provider {}; add it to deps or let needed_so cover it)"),
        ("chk.builddep.missing_tool", "{}, but the recipe's build_deps lacks {} (needed at build time; add it to build_deps)"),
        ("chk.builddep.files_more", " and {} more"),
        ("chk.build-deps.provider_not_declared", "needed_so {} is provided by {}, but is not in this package's build_deps (the build container installs only build_deps; transitive satisfaction does not count - needed_so entries are direct link dependencies)"),
        ("chk.pkg-err.misplaced_root", "misplaced: should be /etc or /var (no usr prefix); usr/etc or usr/var are wrong"),
        ("chk.pkg-err.libtool_archive", "contains .la (libtool archive, obsolete, remove it)"),
        ("chk.pkg-err.static_lib", "contains a static .a library (usually remove it; if a .cmake file references it, exempt with IGNORE_CHK_PKGERR)"),
        ("chk.hook.sysusers", "ships sysusers.d config but postinst does not call systemd-sysusers"),
        ("chk.hook.tmpfiles", "ships tmpfiles.d config but postinst does not call systemd-tmpfiles --create"),
        ("chk.pycache.dir", "contains a __pycache__ bytecode cache directory ({} entries; build-time generated, embeds absolute paths, not reproducible - remove it in the package phase)"),
        ("chk.pycache.loose", "stray Python bytecode (.pyc/.pyo outside __pycache__; build-time generated, not reproducible - remove it in the package phase)"),
        ("chk.qml.rel_include", "relative-path include {} is provided by {}, which is not in this package's deps/needed_so"),
        ("chk.vapi.label", "ships Vala bindings (.vapi)"),
        ("chk.introspection.label", "ships g-ir-scanner generated .gir (GObject Introspection metadata)"),
        ("chk.unknown_kind", "unknown check kind: {}"),
        ("farm_flags.unknown", "  unknown farm flag: {}"),
        ("build.image_fallback", "  [warn] failed to create from {}, falling back to the original base and resetting the roll counter"),
        (
            "build.container_kept",
            "  [container kept] the build tree is inside it: docker exec -it {} bash  (clean up later with docker rm -f {})",
        ),
        ("track.parse_fail_ignored", "  [warn] ignoring unparsable tracker {}: {}"),
        // ── repo upload / ABI transition backups / index ──
        ("repo.read_metadata_json", "failed to read metadata.json from {}: {}"),
        ("repo.repack_failed", "repack {} failed: {}"),
        ("repo.no_artifact", "no build artifact"),
        ("repo.no_valid_version", "recipe has no valid version"),
        ("repo.create_pkg_dir", "failed to create {}: {}"),
        ("repo.move_artifact", "failed to move {} → {}: {}"),
        ("repo.read_pkg_dir", "failed to read repository directory {}: {}"),
        ("repo.read_pkg_dir_entry", "failed to read an entry of repository directory {}: {}"),
        ("repo.backup_old_sonames", "failed to back up old SONAMEs of {} ({}); keeping the old version"),
        ("repo.remove_old_version", "failed to remove old version {}: {}"),
        ("repo.read_old_metadata", "failed to read metadata of old package {}: {}"),
        ("repo.create_backup_dir", "failed to create backup directory: {}"),
        ("repo.read_lib_dir", "failed to read library directory {}: {}"),
        ("repo.read_lib_dir_entry", "failed to read an entry of library directory {}: {}"),
        ("repo.read_symlink", "failed to read symlink {}: {}"),
        ("repo.create_backup_subdir", "failed to create backup subdirectory {}: {}"),
        ("repo.backup_file", "failed to back up {} → {}: {}"),
        ("repo.create_backup_symlink", "failed to create backup symlink {}: {}"),
        ("repo.remove_old_backup", "failed to remove old backup {}: {}"),
        ("repo.read_file", "failed to read {}: {}"),
        ("repo.write_file", "failed to write {}: {}"),
        ("repo.index_missing", "local repo index {} is missing ({}); run `farm seed` first — building without a baseline is forbidden"),
        ("repo.index_empty", "local repo index {} is empty; run `farm seed` first — building without a baseline is forbidden"),
        // ── libarchive (.lpkg read/write) ──
        ("archive.no_reason", "(libarchive gave no reason)"),
        ("archive.op_failed", "libarchive {} failed ({}): {}"),
        ("archive.op_warning", "warning: libarchive {} completed with warnings ({}): {}"),
        ("archive.not_utf8", "{} {} is not valid UTF-8; refusing to silently rename"),
        ("archive.nul_byte", "path contains a NUL byte: {}"),
        ("archive.member_name_too_long", "{} member name too long ({} bytes > {})"),
        ("archive.component_too_long", "{} member name has an over-long path component ({} > {})"),
        ("archive.member_path_escape", "{} member path escapes the root (absolute or contains ..): {}"),
        ("archive.member_control_char", "{} member name contains a control character: {}"),
        ("archive.member_escapes_root", "{} member path escapes the extraction root: {}"),
        ("archive.step_symlink_target", "symlink target"),
        ("archive.symlink_escapes_root", "{} symlink {} target escapes the extraction root: {}"),
        ("archive.duplicate_member", "{} has a duplicate member: {}"),
        ("archive.init_failed", "libarchive initialization failed ({})"),
        ("archive.step_filter", "enable filters"),
        ("archive.step_format", "enable archive formats"),
        ("archive.disk_init_failed", "libarchive disk writer initialization failed ({})"),
        ("archive.step_extract_options", "set extraction options"),
        ("archive.step_open", "open package"),
        ("archive.step_read_header", "read entry header"),
        ("archive.step_member_name", "member name"),
        ("archive.special_file_member", "{} contains an unsupported special-file member {} (fifo/device/socket) — this should not appear in a .lpkg"),
        ("archive.step_hardlink_target", "hardlink target"),
        ("archive.step_extract_entry", "extract entry"),
        ("archive.read_data_failed", "libarchive failed to read entry data ({} / {}): {}"),
        ("archive.write_data_failed", "libarchive failed to write entry data ({} / {}): {}"),
        ("archive.step_finish_entry", "finish entry"),
        ("archive.step_close_disk", "close disk writer"),
        ("archive.read_member_failed", "failed to read {} ({}): {}"),
        ("archive.step_skip_data", "skip member data"),
        ("archive.step_zstd_filter", "enable zstd filter"),
        ("archive.zstd_level_ignored", "warning: libarchive rejected zstd compression-level={} ({}): {}"),
        ("archive.step_pax_format", "set pax_restricted format"),
        ("archive.step_open_output", "open output package"),
        ("archive.stat_failed", "stat {} failed: {}"),
        ("archive.special_file_walk", "content contains an unsupported special-file type {} (socket/device/fifo) — this should not appear in a .lpkg"),
        ("archive.entry_alloc_failed", "libarchive entry allocation failed ({})"),
        ("archive.lstat_failed", "lstat {} failed: {}"),
        ("archive.disk_reader_init_failed", "libarchive disk reader initialization failed ({})"),
        ("archive.step_symlink_mode", "set symlink mode"),
        ("archive.read_metadata_failed", "failed to read metadata of {} ({}): {}"),
        ("archive.readlink_failed", "read_link {} failed: {}"),
        ("archive.step_write_header", "write entry header"),
        ("archive.open_failed", "failed to open {}: {}"),
        ("archive.read_failed", "failed to read {}: {}"),
        ("archive.write_file_data_failed", "failed to write data for {} ({}): {}"),
        ("archive.close_output_failed", "libarchive failed to finalize {} (package truncated)"),
        ("archive.close_output_warning", "warning: libarchive finalized {} with warnings: {} (package may be incomplete)"),
        // ── docker orchestration ──
        ("docker.export_failed", "docker export failed: {}"),
        ("docker.import_failed", "docker import failed: {}"),
        ("docker.create_finalize_failed", "docker create (finalize) failed: {}"),
        ("docker.create_failed", "docker create failed: {}"),
        ("docker.start_failed", "docker start failed ({})"),
        ("docker.write_mirror_conf_failed", "failed to write mirror.conf inside the container ({})"),
        ("docker.exec_failed", "docker exec failed: {}"),
        ("docker.upgrade_failed", "lpkg upgrade failed inside the container ({})"),
        ("docker.commit_failed", "docker commit {} failed (roll snapshot not saved)"),
        ("docker.restore_exec_failed", "docker exec failed while restoring old .so files: {}"),
        ("docker.restore_backup_failed", "failed to restore backup old .so files inside the container ({})"),
        ("docker.cp_recipe_failed", "failed to copy the recipe for {} into the container"),
        ("docker.build_failed", "lpkg build failed inside the container ({})"),
        ("docker.list_artifacts_failed", "docker exec failed to list artifact names: {}"),
        ("docker.no_artifact", "no .lpkg was produced inside the container ({})"),
        ("docker.cp_artifact_failed", "failed to copy the .lpkg for {} back to the host"),
        ("docker.build_image_failed", "docker build failed: {}"),
        // ── net / repack / state / seed / serve / export / scan / llm ──
        ("net.create_remote", "failed to create remote {}: {}"),
        ("net.connect_failed", "failed to connect to {}: {}"),
        ("net.list_refs_failed", "failed to list refs for {}: {}"),
        ("net.list_tags_unimplemented", "list_tags not implemented: {}"),
        ("net.read_url", "read {}"),
        ("net.create_file_failed", "failed to create {}"),
        ("net.write_file_failed", "failed to write {}"),
        ("net.request_failed", "request to {} failed"),
        ("net.read_response_failed", "failed to read response from {}"),
        ("repack.write_meta_failed", "failed to write {}: {}"),
        ("repack.replace_failed", "failed to replace {}: {}"),
        ("repack.read_dir_failed", "failed to read {}: {}"),
        ("repack.read_dir_entry_failed", "failed to read an entry of {}: {}"),
        ("repack.stat_failed", "stat {} failed: {}"),
        ("state.create_dir_failed", "failed to create state directory {}"),
        ("state.open_sqlite_failed", "failed to open SQLite {}"),
        ("state.init_schema_failed", "failed to initialize SQLite schema"),
        ("state.update_job_failed", "failed to update job status"),
        ("state.delete_job_failed", "failed to delete job entry"),
        ("state.record_history_failed", "failed to record build history"),
        ("seed.create_dir_failed", "failed to create {}: {}"),
        ("seed.write_index_failed", "failed to write local index.txt: {}"),
        ("seed.create_dir_short", "failed to create directory"),
        ("seed.sha256_mismatch", "SHA256 mismatch"),
        ("serve.root_inaccessible", "root {} is inaccessible: {}"),
        ("serve.bind_failed", "failed to bind {}:{}: {}"),
        ("export.create_output_failed", "failed to create output directory {}: {}"),
        ("export.read_repo_failed", "failed to read {}: {}"),
        ("export.bad_pkg_name", "{}: package name contains illegal characters; skipped"),
        ("export.no_lpkg", "{}: no .lpkg"),
        ("export.unparsable_version", "{}: cannot parse the version from the .lpkg filename"),
        ("export.bad_version_name", "{}: version name {} contains illegal characters; skipped"),
        ("scan.remove_tree_failed", "failed to remove directory tree {}: {}"),
        ("scan.create_dir_failed", "failed to create {}: {}"),
        ("scan.parse_metadata_failed", "failed to parse metadata.json: {}"),
        ("scan.no_metadata", "{} contains no metadata.json"),
        ("scan.read_failed", "failed to read {}: {}"),
        ("scan.parse_file_failed", "failed to parse {}: {}"),
        ("llm.api_failed", "LLM API call failed: {}"),
        ("llm.read_response_failed", "failed to read LLM response: {}"),
        ("llm.non_json_response", "LLM response is not JSON: {}"),
        ("llm.missing_content", "LLM response has no choices[0].message.content: {}"),
        // ── track tracker parsing / probing ──
        ("track.parse_yaml_failed", "failed to parse tracker yaml: {}"),
        ("track.no_entries", "tracker {} has no sources/work_sources entries"),
        ("track.version_source_oob", "version-source out of range: {}[{}] ({} entries)"),
        ("track.probe_failed", "{}[{}] probe failed: {}"),
        ("track.bad_version_var_name", "version-var name `{}` is not a valid shell variable name (expected [A-Za-z_][A-Za-z0-9_]*)"),
        ("track.reserved_version_var", "version-var name must not shadow the reserved variable `PKG_NAME`"),
        ("track.major_of_no_version", "major-of dependency {} has no version (failed to read LankeBUILD.json)"),
        ("track.unknown_template", "unknown tracker_template: {}"),
        ("track.field_validation_failed", "tracker field validation failed ({}): {}"),
        ("track.unsupported_field", "tracker-template {} does not support field: {} (use a template that supports version caps/stable branches, or the entry-level script template `tracker-template: script`)"),
        ("track.invalid_version_source", "invalid version-source '{}' (expected sources[i] or work_sources[i])"),
        ("track.slot_not_available", "{} is not available yet - only slots located before it in this tracker can be referenced (sources are probed before work_sources, left to right; {} currently has {} slots probed)"),
        ("track.missing_field", "tracker config is missing {}"),
        ("track.unresolved_placeholder", "probed URL still contains an unreplaced placeholder: {}"),
        ("track.invalid_regex", "invalid regex {}: {}"),
        ("track.invalid_regex_short", "invalid regex: {}"),
        ("track.no_version_match", "no version matched in {}"),
        ("track.no_matching_release", "no matching version/major in releases"),
        ("track.no_matching_tag", "no matching version/major in release tags"),
        ("track.no_matching_tags", "no matching version/major in tags"),
        ("track.gcs_page_limit", "GCS pagination exceeded the safety limit"),
        ("track.gcs_no_match", "no version matched in GCS listing {}"),
        ("track.gnome_no_version_dir", "gnome directory listing has no version subdirectories"),
        ("track.gnome_no_constraint_dir", "gnome directory has no subdirectory matching the major/cap"),
        ("track.gnome_no_stable_file", "gnome directory has no file matching a stable version"),
        ("track.invalid_exclude_regex", "invalid exclude regex `{}`: {}"),
        ("track.api_parse_failed", "failed to parse {} API response: {}"),
        ("track.api_not_array", "{} API response is not an array"),
        ("track.ctx_list", "list"),
        ("track.release_api_parse_failed", "failed to parse release API response: {}"),
        ("track.release_no_tag", "release API response has no tag_name"),
        ("track.multi_level_requires_levels", "multi-level-html-index requires a levels list (each level: {name, url, pattern})"),
        ("track.multi_level_missing_version", "levels is missing name: version (that level's capture is the package version)"),
        ("track.multi_level_no_match", "no version matched in {} (pattern: {})"),
        ("track.level_name_empty", "levels[{}].name must not be empty"),
        ("track.level_name_reserved", "levels[{}].name must not use the reserved name `{}` (the upstream-name placeholder is already taken)"),
        ("track.level_name_duplicate", "levels[{}].name `{}` duplicates an earlier level"),
        ("track.multi_level_missing_version_level", "levels must contain a level named {} (that level's capture is the package version, not positional)"),
        ("track.unknown_placeholder", "{} references an unknown placeholder {} (available: {}; positional {v1}..{vN} is deprecated - each level must be named explicitly)"),
        ("track.pypi_json_failed", "failed to parse PyPI JSON ({}): {}"),
        ("track.pypi_no_releases", "PyPI {} has no releases"),
        ("track.pypi_no_matching_version", "PyPI {} has no version satisfying the constraints (major={})"),
        ("track.pypi_no_sdist_url", "PyPI {} {} has no sdist URL"),
        ("track.pypi_no_info_version", "PyPI {} has no info.version"),
        ("track.pypi_latest_no_sdist", "PyPI {} latest version has no sdist URL"),
        ("track.same_version_no_version", "same-version-of dependency {} has no version (failed to read LankeBUILD.json/resolved version)"),
        ("track.script_write_failed", "failed to write temporary script: {}"),
        ("track.script_run_failed", "failed to run track script: {}"),
        ("track.script_nonzero_exit", "track script exited non-zero: {} (stderr: {})"),
        ("track.script_bad_line", "script stdout line {} is not in `<version>|URL` format (the old two-line format is deprecated): {}"),
        ("track.script_empty_version", "script stdout line {} has an empty version: {}"),
        ("track.script_empty_url", "script stdout line {} has an empty URL: {}"),
        ("track.script_no_output", "script stdout is empty (each line should be `<version>|URL`)"),
        ("track.script_needs_expand", "script stdout produced {} lines, but the entry does not declare `expand: true` (a script produces one slot by default; add expand: true for dynamic enumeration)"),
        ("track.sourceforge_no_match", "no version matched in RSS"),
        // ── custom_checks / build / cli ──
        ("chk.create_cache_dir_failed", "failed to create cache directory {}: {}"),
        ("chk.serialize_cache_failed", "failed to serialize cache: {}"),
        ("chk.write_cache_failed", "failed to write cache {}: {}"),
        ("chk.create_cache_dir_short", "failed to create cache directory: {}"),
        ("chk.read_repo_failed", "failed to read {}: {}"),
        ("chk.create_scratch_failed", "failed to create temporary work directory: {}"),
        ("chk.unpack_failed", "{}: failed to unpack {}"),
        ("chk.elf_parse_failed", "failed to parse ELF: {}"),
        ("build.no_lankebuild", "no LankeBUILD.json"),
        ("build.source_no_filename", "{}: source URL has no filename (a bare directory URL cannot be used as a source)"),
        ("build.source_duplicate_filename", "{}: resolves to the same filename {} as another source (query/fragment stripped); cannot pre-download"),
        ("build.unknown_stage", "unknown stage"),
        ("build.version_script_write_failed", "failed to write version-change script: {}"),
        ("build.version_script_run_failed", "failed to run version-change script: {}"),
        ("cli.serve_ready_send_failed", "failed to send serve-ready signal: {}"),
        ("cli.serve_not_ready_5s", "not ready within 5s"),
        ("cli.open_log_failed", "failed to open log file {}: {}"),
        ("cli.read_failed", "failed to read {}: {}"),
        ("cli.parse_failed", "failed to parse {}: {}"),
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
    fn pick_locale_follows_gettext_precedence() {
        // LC_ALL > LC_MESSAGES > LANG（POSIX：LC_ALL 覆盖一切）
        assert_eq!(
            pick_locale(
                Some("en_US.UTF-8"),
                Some("zh_CN.UTF-8"),
                Some("zh_CN.UTF-8")
            ),
            Some("en_US.UTF-8")
        );
        // 无 LC_ALL：LC_MESSAGES 胜 LANG
        assert_eq!(
            pick_locale(None, Some("en_GB.UTF-8"), Some("zh_CN.UTF-8")),
            Some("en_GB.UTF-8")
        );
        // 只有 LANG
        assert_eq!(pick_locale(None, None, Some("en_US")), Some("en_US"));
        // 空值视为未设（`LC_ALL=` 不应压掉 LANG）
        assert_eq!(pick_locale(Some(""), None, Some("en_US")), Some("en_US"));
        assert_eq!(pick_locale(None, None, None), None);
        assert_eq!(pick_locale(Some(""), Some(""), Some("")), None);
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
    fn zh_and_en_placeholder_counts_match() {
        // 每个键的 `{}` 占位符数中英必须一致：否则 tr!("key", a, b) 在一种语言里会漏参/多参
        // 而静默拼错（fmt 是手写位置替换，不抛错）。这是"目录键一致"之外的第二层防漂移。
        for (k, zh) in ZH.iter() {
            let zh_n = zh.matches("{}").count();
            let en_n = EN.get(k).expect("键缺失").matches("{}").count();
            assert_eq!(
                zh_n, en_n,
                "键 {k} 的占位符数中英不一致（zh={zh_n} en={en_n}）：tr! 带参会静默拼错"
            );
        }
    }

    #[test]
    fn zh_and_en_catalogs_have_same_keys() {
        let zh_keys: std::collections::HashSet<_> = ZH.keys().copied().collect();
        let en_keys: std::collections::HashSet<_> = EN.keys().copied().collect();
        assert_eq!(zh_keys, en_keys, "中英目录键必须一致");
    }
}
