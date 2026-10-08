#include "install_common.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <random>
#include <sstream>
#include <string>

#include "base/exception.hpp"
#include "base/so_spec.hpp"
#include "base/utils.hpp"
#include "db/test_breakpoints.hpp"
#include "i18n/localization.hpp"
#include "solver.hpp"
#include "ui/term.hpp"
#include "vercmp/dep_parser.hpp"  // detail::dependency_name_of（`deps/` 行 → 包名，唯一实现）

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace detail
{

fs::path staged_tmp_path(const fs::path& dst)
{
    fs::path p = dst;
    p += constants::SUFFIX_LPKG_TMP;
    return p;
}

void drop_staged_tmp(const fs::path& tmp)
{
    // 不抛重载 + 独立 ec：收尾删除失败绝不能顶替正在传播的原异常（与 builder 侧同一纪律）。
    std::error_code ec;
    fs::remove(tmp, ec);
    if (ec) log_warning(string_format("warning.cleanup_failed", tmp.string()));
}

fs::path confine_target_path(const fs::path& rel)
{
    const fs::path root = Config::instance().root_dir();
    const fs::path physical = root / rel;
    // `root_dir() == "/"`（常规安装，不带 --root）：任何绝对路径都在其内 —— 不付检索代价
    // （与 `reverse_execute` 的 confinement 同一条"只在真可能出现越界时才检查"的取舍）。
    if (root.empty() || root == fs::path("/")) return physical;
    // 判据只解析**父目录**（末段是要落位的那个名字本身，且包里合法的绝对目标链接必须放行）；
    // 解不开就放行。两点的完整理由见 `install_common.hpp` 的声明注释。
    if (path_within_resolved(physical, root)) return physical;
    throw LpkgException(string_format("error.install_escape_root", rel.string(), root.string()));
}

// ============================================================================
// 每文件系统 sidecar stash + 目录元数据化删除
// ============================================================================

fs::path stash_parent_dir(const fs::path& phys)
{
    const fs::path root = Config::instance().root_dir();
    const fs::path root_n = root.lexically_normal();
    auto dev_of = [](const fs::path& p) -> dev_t {
        struct stat st{};
        return ::lstat(p.c_str(), &st) == 0 ? st.st_dev : static_cast<dev_t>(-1);
    };
    // 边界判定统一走 path_within：这里曾手写 `rs + "/"`，root_dir=="/" 时拼成 "//" →
    // within_root 对除 "/" 外的所有路径恒 false → cur 被 clamp 成 "/" → 循环第一轮
    // par == best 直接 break → 无论目标文件在哪个文件系统，stash 恒落在 "/"（宿主
    // rootfs），跨设备 rename 立刻 EXDEV（vfat ESP 上的 /boot/vmlinuz 升级必炸）。
    auto within_root = [&](const fs::path& p) -> bool { return path_within(p, root_n); };

    fs::path cur = phys.has_parent_path() ? phys.parent_path() : phys;

    // ── 先解析**父链上的符号链接** ────────────────────────────────────────────
    // 下面那段上溯是**词法**的（`parent_path()` 逐级 + `is_mount_point()` 拿 mountinfo
    // **精确匹配**），而 mountinfo 记的是**内核解析后**的路径。父链上只要有一级是符号链接、
    // 且指向**别的挂载**，词法父链与真实父链就不是同一条 → 上溯停在**错误的**文件系统顶层
    // → stash 与 phys 不同挂载 → `safe_rename` EXDEV → **该包永久无法 remove/upgrade**。
    //
    // 宿主机（真实 mountinfo）：`/var/run -> ../run`（tmpfs）而 mountinfo 里只有
    // `/run` → `/var/run/foo` 的词法上溯停在 `/`（overlay），phys 真身在 `/run` → 不同设备。
    // stock 布局里只有 `/var/run` 这一个跨挂载链接（`/bin` `/lib` `/sbin` 都指向同一文件系统
    // 内），所以当前包集没炸（cups/qemu-x86-64/samba 在配方层 `rm -rf var/run` 绕过）——
    // 但**管理员自建的跨盘链接**（`/opt -> /mnt/opt` 这类）同样会踩，判据是"链接是否跨挂载"，
    // 与路径名无关。
    //
    // 只解析**父目录**：`phys` 自身（末段）正是我们要搬走的对象，它的符号链接语义不能动
    // （搬的是那个链接本身，不是它的目标）。
    //
    // 解析失败（父目录已被删、路径不可达）→ 退回原来的词法路径 + clamp 行为。
    // 解析成功但落在 root_dir **之外**时**不** clamp：那是"沙箱内有个符号链接指向外面"，
    // 此时 phys 本身也在 root 之外，root 早已不是有效边界；而"stash 与 phys 同挂载"是
    // rename 能否成立的硬前提 —— 同挂载优先于 root 包含（stash 是 lpkg 自己的记账目录，
    // 名字带 pid 与包名，不覆盖任何东西）。生产环境 root=="/"，这条永远不触发。
    {
        std::error_code cec;
        const fs::path resolved = fs::canonical(cur, cec);
        if (!cec && !resolved.empty()) {
            cur = resolved;
        } else if (cur.empty() || !within_root(cur)) {
            cur = root;
        }
    }
    if (cur.empty()) cur = root;
    const dev_t d = dev_of(phys);
    if (d == static_cast<dev_t>(-1)) return cur;  // phys 已消失 → 用它所在目录位置

    // 上溯边界 = 挂载点（vfsmount 才是 rename 的 EXDEV 边界），**不是 st_dev**：
    // overlay 上目录报 overlay 的 dev、upper 层文件报底层 fs 的 dev，同一条路径链上
    // 两者不等 → 用 st_dev 判"到边界了"会在第一个父目录就误停，stash 落进文件自己
    // 所在目录（包子树内），破坏"stash 挂文件系统顶层、不在被删子树里"的约定。
    const bool have_mounts = !mount_points().empty();
    fs::path best = cur;
    while (true) {
        if (have_mounts && is_mount_point(best)) break;  // best 已是本文件系统顶层
        const fs::path par = best.parent_path();
        if (par.empty() || par == best) break;
        if (!within_root(par)) break;                 // 越过 root_dir 边界 → best 是根内最上层
        if (!have_mounts && dev_of(par) != d) break;  // /proc 不可用 → 退回 st_dev 近似
        best = par;
    }
    return best;
}

fs::path ensure_stash_dir(const fs::path& phys, std::string_view pkg)
{
    const fs::path parent = stash_parent_dir(phys);
    fs::path dir = parent / (std::string(".lpkg_bak_") + std::string(pkg) + "_" +
                             std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    const auto before = fs::symlink_status(dir, ec);
    const bool pre_existing = !ec && before.type() != fs::file_type::not_found;
    // 该名字被**符号链接**占着：create_directories 对"已存在的 symlink→目录"静默成功，
    // 随后的 chmod 与备份 rename 全会**穿过链接**落进目标目录 —— 备份跑到外面去了，
    // 而清理侧（remove_stash_dir / cleanup_orphan_stashes）根本看不见它们。绝不接受。
    if (pre_existing && before.type() == fs::file_type::symlink) {
        throw LpkgException(string_format("error.bak_collision", phys.string(), std::string(pkg)));
    }
    fs::create_directories(dir, ec);
    if (ec) {
        // 创建失败必须**当场失败**：丢弃 `ec` 的话函数会照样返回这个目录 ——
        // 后续的 `sink.backup` rename 会报一个与真实原因无关的错（ENOENT/误导性路径），
        // 而 `0700` 加固静默没生效（目录压根不存在）。对照 `base/utils.cpp` 的
        // `ensure_dir_exists`：那边一直是 fail-closed，这里漏了。
        throw LpkgException(string_format("error.create_dir_failed", dir.string()) + ": " +
                            ec.message());
    }
    if (!pre_existing) (void)::chmod(dir.c_str(), 0700);  // root-only：备份残留隔离
    return dir;
}

fs::path stash_bak_target(const fs::path& phys, std::string_view pkg)
{
    fs::path dir = ensure_stash_dir(phys, pkg);
    const std::string base = phys.filename().string();
    for (int i = 0; i < constants::UNIQUE_BAK_MAX_ATTEMPTS; ++i) {
        fs::path bak = dir / (base + std::string(constants::SUFFIX_LPKG_BAK) + std::string(pkg) +
                              "_" + random_suffix());
        // 判"这个名字空不空"必须用 **lstat 语义**（exists_no_follow）：`fs::exists` 在符号
        // 链接环上会抛（见 base/path_predicates.hpp 的谓词说明），而这里被占的名字可以是任何形态的
        // 残留物 —— 悬空/自环链接同样占着这个名字，rename 上去会把它顶掉。
        if (!exists_no_follow(bak)) return bak;
    }
    throw LpkgException(string_format("error.bak_collision", phys.string(), pkg));
}

void remove_stash_dir(const fs::path& stash)
{
    std::error_code ec;
    fs::remove_all(stash, ec);
    // 失败**不抛**：调用点在"批次已提交"之后的收尾路径上，抛了会把"安装已成功"报成命令失败。
    // 但也不能**静默** —— 残留的 stash 会一直占着磁盘（大包能占几百 MB），而唯一会再回头收它的
    // 是 `cleanup_orphan_stashes`。判据与措辞同 recover.cpp / wal_op.cpp 里同形的 remove_all 失败。
    if (ec) log_warning(string_format("warning.cleanup_failed", stash.string()));
}

/**
 * 撤销"本包对该目录 xattr 键的声明"，并在**没有别的属主**时把键从盘上删掉（改前值写 WAL）。
 */
bool revoke_xattr_key_if_unowned(Cache& cache, const std::string& pkg, const std::string& logical,
                                 const std::string& key, OpSink& sink, const fs::path& root)
{
    // 归属集合里先摘掉本包：**无论盘上撤不撤**，本包都不再声明这个键了（第 ② 步的判据）。
    auto owners = cache.get_xattr_key_owners(logical, key);
    owners.erase(pkg);

    bool removed = false;
    if (owners.empty()) {
        // 无人持有才动盘。路径由**逻辑目录键**拼出：`fs::path("/usr/share/x/").relative_path()`
        // 给出 `usr/share/x/`，最后 `strip_trailing_slash` —— `lremovexattr` 对带尾斜杠的路径
        // 会**穿透**到末段符号链接的目标上（与元数据块逐字同源的那条边界）。
        const fs::path phys = strip_trailing_slash(root / fs::path(logical).relative_path());
        // 撤旧值先写 WAL（`unset_xattr` 内部做），整批回滚能把键逐字节还原。
        removed = sink.unset_xattr(phys, key, "xattrrm_after_wal_" + pkg);
    }
    cache.remove_xattr_key_owner(logical, key, pkg);
    return removed;
}

/**
 * 删除"本包独占、且此刻为空"的 owned 目录（`remove` 与 `upgrade` **共用**，ARCH.md §3.6）。
 *
 * 两个调用点的语义完全一致，原先各写了一遍（移除侧 `do_remove_package()` 的阶段 B、
 * 升级侧 `remove_obsolete_files()` 的阶段 2）：任何一侧改了守卫，另一侧不会跟着变。
 * 候选集的筛选与**时序守卫**留在调用点 —— 那是两侧真正的差异：
 *   · 移除侧：候选 = 本包**全部** owned 目录键（包要整个消失）；
 *   · 升级侧：候选 = 仅"新版本不再提供"的目录，且要**先排除**"新版本在该目录下还有条目"
 *     的（那一趟跑在写入**之前**，判据必须钉回与原来"写入之后"同一口径，见调用点说明）。
 *
 * 五条判据（与两处原实现的定稿逐条一致）：
 *   1. **目录键剥尾斜杠**：键是 DB 键（带尾斜杠），物理路径必须先规范化 ——
 *      尾斜杠会让判定类调用解引用末段链接（见 base/utils.hpp 的 strip_trailing_slash）。
 *   2. **最深优先**：子目录先于父目录（父目录只有在子目录被 rmdir 之后才可能为空）。
 *      按路径字面长度降序排序即可 —— 子孙路径恒长于祖先。
 *   3. **只处理"本包是最后持有者"的**：先摘本包归属，还有别的持有者 → 整树保留
 *      （引用计数；别的包的内容绝不能连带删掉）。
 *   4. **真目录**（非 symlink）：`symlink -> 目录` 不是"我们建的那个目录"，
 *      判定必须走 lstat 语义（`is_real_directory`，等价于 is_directory && !is_symlink）。
 *   5. **此刻为空**：目录里只要有无主内容（用户塞的文件、lpkg 自身状态目录、保留的
 *      conffile）就**整树保留** —— 绝不误删不属于本包的东西（旧版"子树无其他 owner 即可删"
 *      的递归规则曾把共享祖先下 `usr/share/lpkg/docs` 删掉，已否决）。
 * 满足全部五条才 `sink.remove_empty_dir()`（WAL `DIR_RM` + 元数据 + rmdir）；
 * **目录型挂载点由它在内部挡下**（rmdir 恒 EBUSY），这里只负责告警。
 *
 * @param candidate_dir_keys 候选目录键（**DB 键的原始形态**，带尾斜杠；筛选由调用点做）
 * @param report             逐目录报告回调（可空）。**只在两处调用**：判"含无主内容 → 整树
 *                           保留"的那一支（`removed == false`）与真正要 rmdir 之前
 *                           （`removed == true`，**在 sink.remove_empty_dir 之前**，与调用点
 *                           原先自己打日志的时刻逐字一致）。它不参与任何判据。
 */
void remove_empty_owned_dirs(Cache& cache, const std::string& pkg,
                             const std::vector<std::string>& candidate_dir_keys, OpSink& sink,
                             DirReporter report)
{
    std::vector<fs::path> keys;
    keys.reserve(candidate_dir_keys.size());
    for (const auto& k : candidate_dir_keys) keys.emplace_back(k);
    // 最深优先（见上面第 2 条）。判据用**字符串长度**而不是字典序：子孙恒长于祖先，
    // 且长度序对"互不为祖先"的目录之间的先后不作任何承诺（两者顺序不影响结果）。
    std::ranges::sort(keys, [](const fs::path& a, const fs::path& b) {
        return a.string().size() > b.string().size();
    });

    const fs::path root = Config::instance().root_dir();
    for (const auto& key : keys) {
        // ① 摘本包归属（DB 键**原样**参与：目录键就是带尾斜杠的形态）
        cache.remove_file_owner(key.string(), pkg);
        // ② 还有别的持有者 → 整树保留
        if (!cache.get_file_owners(key.string()).empty()) continue;

        // ③ 物理路径：键剥尾斜杠后按 root 重定位（与归档条目同一套落点规则）
        const fs::path bare = strip_trailing_slash(key);
        const fs::path phys = bare.is_absolute() ? root / bare.relative_path() : root / bare;
        // ④ 真目录（lstat 语义、不抛 —— 环/不可达一律判否，与"这里没有我们要删的目录"同路）
        if (!is_real_directory(phys)) continue;
        // ⑤ 此刻为空：含无主内容 → 整树保留
        std::error_code ec;
        if (!fs::is_empty(phys, ec)) {
            if (report) report(phys, false);
            continue;
        }
        if (report) report(phys, true);
        // WAL: DIR_RM（含 mode/uid/gid） + rmdir（一次调用）。
        // 目录型**挂载点**由 remove_empty_dir 自己挡下（rmdir 恒 EBUSY，写行就成了"行说
        // 删了、盘面还在"）→ 跳过 + 告警，目录保留。
        if (sink.remove_empty_dir(phys) == DirRemoval::SkippedMountPoint)
            log_warning(string_format("warning.remove_mount_point", phys.string()));
    }
}

/** 从 lpkg 归档文件中读取 metadata.json 并解析为 JSON 对象 */
json read_archive_metadata(const fs::path& archive_path)
{
    std::string meta_json =
        extract_file_from_archive(archive_path, std::string(constants::PKG_METADATA_FILE));
    if (meta_json.empty())
        throw LpkgException(
            string_format("error.local_pkg_missing_metadata", archive_path.string()));
    return json::parse(meta_json);
}

namespace
{
/**
 * 钩子文件名的**人话**：`postinst.sh` → "post-install hook"（`hook.name.*` 键）。
 * 未知名字回退成文件名本身（新增钩子类型时不必先补 l10n 才不崩）。
 */
std::string hook_display_name(std::string_view hook_name)
{
    if (hook_name == constants::POSTINST_SH) return get_string("hook.name.postinst");
    if (hook_name == constants::PRERM_SH) return get_string("hook.name.prerm");
    return std::string(hook_name);
}
}  // namespace

/// 支持 chroot 环境下运行，使用 mount namespace 隔离。
void run_hook(std::string_view pkg_name, std::string_view hook_name)
{
    if (Config::instance().no_hooks_mode()) return;

    const fs::path hook_path = Config::instance().hooks_dir() / pkg_name / hook_name;
    // 判定不抛（ELOOP 会让 fs::exists 抛 → 一个含符号链接环的 hook 路径会把安装/卸载打断，
    // 见 base/path_predicates.hpp 的谓词说明）。exists_follow 为假时短路，后面的 is_regular_file
    // 不会跑到（它同样是"跟随"的判定，对解不开的路径会抛）。
    if (!exists_follow(hook_path) || !fs::is_regular_file(hook_path)) return;

    // 断点：**hook 执行点**（hooks 已启用、脚本确实存在、只剩 exec）。测试据此取证
    // "这个 hook 到底跑了没有"——沙盒 root 里没有 /bin/bash，真让脚本跑起来需要一整套
    // rootfs，取证只能落在"执行决策已作出"这一刻；本行是 postinst/prerm 唯一执行入口
    // 里唯一这样的位置（test_hook_transaction.cpp 用它钉住钩子的执行时机）。
    BreakpointManager::instance().hit("hook_run_" + std::string(hook_name));

    // 人性化名字 + 包名（systemd/pacman 风格的**单行状态**）：`==> Running post-install hook
    // of package foo ... [OK]`，[OK] 靠终端最右边。挂载时机统一到批次提交后（见
    // finish_committed_batch），一次多包批次会连续跑多个钩子，只写 `postinst.sh`
    // 在日志里没有上下文（哪个包？在人跑什么？）。
    const std::string human = hook_display_name(hook_name);
    ui::Line line(string_format("ui.running_hook", human, std::string(pkg_name)));

    const bool use_chroot =
        (Config::instance().root_dir() != "/" && Config::instance().root_dir().string() != "/");

    if (use_chroot) {
        // 钩子由 BIN_BASH 执行，chroot 后按 /bin/bash 解析——必须检查 bash 而非 sh
        const fs::path bash_rel = std::string(constants::BIN_BASH).substr(1);  // "bin/bash"
        // 判定不抛（ELOOP 会让 fs::exists 抛，见 base/path_predicates.hpp 的谓词说明）
        if (!exists_follow(Config::instance().root_dir() / bash_rel)) {
            line.finish(ui::skipped());  // 目标 root 里没有 bash：明确告诉用户"跳过了"
            log_warning(string_format("warning.hook_failed_setup", std::string(hook_name),
                                      get_string("error.bash_not_found")));
            return;
        }
    }

    // chroot 内按**目标 root 的绝对路径**解析脚本，否则用宿主绝对路径。
    // 执行统一交给 run_shell_in_root：chroot/fork/exec/waitpid 只有那一份实现，
    // 与外部触发器（trigger.cpp）走同一条路径（此前这里有一份等价但独立的代码）。
    const std::string script =
        // `lexically_relative`：chroot 内的路径必须是**这个名字**，
        // 而在宿主上 `fs::relative` 会解析符号链接（chroot 内解析结果可能完全不同）。
        use_chroot ? "/" + hook_path.lexically_relative(Config::instance().root_dir()).string()
                   : fs::absolute(hook_path).string();

    const int ret = run_shell_in_root(shell_quote(script));
    line.finish(ui::ok(ret == 0));
    if (ret != 0) {
        log_warning(
            string_format("warning.hook_failed_exec", std::string(hook_name), std::to_string(ret)));
    }
}

/**
 * `deps` / `provides` / `needed_so` 的每一条都必须"单行、无控制字符"。
 *
 * 非校验不可的原因：归档**成员名**早就有消毒（`archive.cpp` 的
 * `member_name_rejection_message`），但这三个字段没有 —— 它们同样会被写进**行式 / 制表符
 * 分帧**的状态文件：
 *   · `deps/<pkg>`、`needed_so/<pkg>`：**一行一条**（`\n` 注入 ⇒ 凭空多出依赖 / 多出
 *     一条 SONAME 记录）；
 *   · `provides.db`：`<capability>\t<pkgs>`（`\t` 注入 ⇒ 键在重载时被截断、提供者串错位）。
 * 后果：`provides = ["a\ncapX\tE"]` 读回会变成幽灵提供者，而 `dep_satisfied_on_disk`
 * 只看"这个 capability 有没有提供者" ⇒ **假满足**依赖（装出一个坏系统）；`deps`/`needed_so`
 * 里的 `\n` 还会污染反向依赖图，**阻止**正常卸载（DoS）。仓库来源的包另有一道"与索引逐字段
 * 比对"，但本地 `.lpkg` 走不到那里 —— 校验放在这里（metadata 解析的唯一出口）才覆盖全。
 *
 * **只拒控制字符，不拒 `,`**：依赖串允许带约束（`"cmake >= 3.20, < 4.0"`，见
 * `vercmp/dep_parser.cpp` 对逗号的处理），拒了会误伤合法包。空串不在这里拦（读者本来就会
 * 跳过空行/空取值，拦下来反而是行为变化）。
 */
void reject_unsafe_metadata_tokens(const std::vector<std::string>& values, std::string_view field,
                                   const fs::path& meta_path)
{
    for (const auto& v : values) {
        // 注意用**带长度**的 string_view：`"\0..."` 这种字面量走 C 串构造会在第一个字节就断。
        const std::size_t bad = v.find_first_of(std::string_view("\0\n\r\t", 4));
        if (bad == std::string::npos) continue;
        // 实参顺序与 l10n 文案的占位符一致：`{} {} {}` = 元数据文件、字段名、偏移。
        throw LpkgException(string_format("error.unsafe_metadata_field", meta_path.string(),
                                          std::string(field), std::to_string(bad)));
    }
}

/**
 * 校验两个 SONAME 字段的**规格语法**（`base/so_spec.hpp`）。
 *
 * 与它上面的 `reject_unsafe_metadata_tokens` 是**两件事**：那个管"分帧字符会不会伪造记录"，
 * 这个管"这个 SONAME 规格写不写得通"。都放在**读入处**：规格一旦写歪（少个花括号、冒号后
 * 空着、花括号里带空格），宽容解析会把它整串当裸名，于是它**永远匹配不上任何提供者** ——
 * 静默的错误答案，比报错难查得多。
 */
void reject_bad_soname_specs(const std::vector<std::string>& values, std::string_view field,
                             const fs::path& meta_path)
{
    for (const auto& v : values) {
        if (so_spec_wellformed(v)) continue;
        // 实参顺序与 l10n 文案一致：`{} {} {}` = 元数据文件、字段名、原值。
        throw LpkgException(
            string_format("error.bad_soname_spec", meta_path.string(), std::string(field), v));
    }
}

/**
 * 校验 `provides` 的每一项**没有使用求解器内部的 SONAME 命名空间前缀**（`so:`）。
 *
 * 为什么需要它：池里只有两个命名空间，`deps`/`provides` 走**裸名**、`needed_so`/
 * `provides_soname` 走 `so:`。`constants.hpp` 里那句"包名不可能含 `:`，所以裸名永远撞不进
 * `so:` 空间"**只对包名成立** —— `provides` 的项走的是同一条裸名路径，而 `is_safe_path_component`
 * 管不到它、`reject_unsafe_metadata_tokens` 也只拒 `\0\n\r\t`。于是
 * `provides: ["so:libfoo.so.1"]` 与 `needed_so: ["libfoo.so.1"]` 会灌出**同一个 pool id**：
 * 求解器认为 SONAME 需求已被满足，而安装期的 `soname_satisfied()`（只查 `provides_soname` /
 * `provides_soname.db` / 仓库提供者 / 系统库，四条都不看虚拟 provides）会拒绝 ——
 * 正是"求解器说能装、安装期拒装"那个分叉。
 *
 * **可达性**（别把它写成比实际更严重）：仓库来源的包撞不到 —— 索引版本块是"恰好 6 个冒号
 * 字段"，`provides` 带 `:` 会让字段数变 7、整个版本块被丢弃；且 `verify_package_metadata`
 * 会逐字段比对归档与索引。**真正可达的是本地 `.lpkg`**（`install ./x.lpkg`：计划字段与校验
 * 读的是同一份 ⇒ 比对恒等 ⇒ 放行），以及它写进 `provides.db` 之后留下的**持久幽灵提供者**。
 *
 * 不在求解器里判的原因：libsolv 的多条 requires 是 **AND** 语义、却允许被不同 solvable
 * 分别满足，表达能力上就写不出"包名 **或** 能力"——给虚拟能力再加一套前缀会破坏 `deps`
 * 的匹配语义。所以只能在**输入边界**拒绝（此处，metadata 解析的唯一出口）。
 */
void reject_reserved_provides_prefix(const std::vector<std::string>& provides,
                                     const fs::path& meta_path)
{
    for (const auto& capability : provides) {
        if (!capability.starts_with(constants::POOL_SONAME_PREFIX)) continue;
        throw LpkgException(string_format("error.provides_reserved_prefix", meta_path.string(),
                                          capability, std::string(constants::POOL_SONAME_PREFIX)));
    }
}

/** 从已解压的包目录读取 metadata.json，提取包名、版本、依赖等信息 */
void read_package_metadata(const fs::path& tmp_pkg_dir, std::string& name, std::string& version,
                           std::vector<std::string>& deps, std::vector<std::string>& provides,
                           std::vector<std::string>& provides_soname,
                           std::vector<std::string>& needed_so, std::string& man)
{
    fs::path meta_path = tmp_pkg_dir / constants::PKG_METADATA_FILE;
    // 大小上限检查在**解析之前**，且与"JSON 解析失败"是**两种不同的错**：前者 = "这份元数据
    // 本就不该这么大"（畸形/恶意归档，可定位到文件与字节数）；后者 = "内容不是合法 JSON"。
    // 取大小用带 ec 的 file_size：取不到（如文件缺失）时**不在这里报错**，交给下面的
    // ifstream 打开失败，保留原有那条 `error.open_file_failed`，报错点不重复。
    std::error_code size_ec;
    const std::uintmax_t meta_size = fs::file_size(meta_path, size_ec);
    // 上限是 `constants::ARCHIVE_MEMBER_MAX_SIZE` —— 与 `extract_file_from_archive` 共用
    // 同一个常量（别在这里另写一份魔数）。
    if (!size_ec && meta_size > constants::ARCHIVE_MEMBER_MAX_SIZE)
        throw LpkgException(string_format("error.archive_member_too_large",
                                          std::to_string(meta_size), meta_path.string()));
    json meta;
    {
        std::ifstream f(meta_path);
        if (!f.is_open())
            throw LpkgException(string_format("error.open_file_failed", meta_path.string()));
        // nlohmann 的解析异常只带行/列，**不说是哪个文件**。这里按路径包一层，与
        // `build/builder_config.cpp` 的 `error.lankebuild_parse_failed` 同一做法 ——
        // 否则坏 JSON 会一路逸出到 `main_cli.cpp` 的 `error.unexpected_error`，
        // 用户看到的是一句带行号的 "parse error"，却不知道该去看哪个包的哪份元数据。
        try {
            f >> meta;
        } catch (const std::exception& e) {
            throw LpkgException(
                string_format("error.metadata_parse_failed", meta_path.string(), e.what()));
        }
    }
    name = meta.at(std::string(constants::J_NAME)).get<std::string>();
    version = meta.at(std::string(constants::J_VERSION)).get<std::string>();
    deps = meta.value(std::string(constants::J_DEPS), std::vector<std::string>{});
    provides = meta.value(std::string(constants::J_PROVIDES), std::vector<std::string>{});
    provides_soname =
        meta.value(std::string(constants::J_PROVIDES_SONAME), std::vector<std::string>{});
    needed_so = meta.value(std::string(constants::J_NEEDED_SO), std::vector<std::string>{});
    // 这三个字段会进行式/制表符分帧的状态文件 —— 控制字符会**伪造出额外记录**（见上方注释）。
    reject_unsafe_metadata_tokens(deps, constants::J_DEPS, meta_path);
    reject_unsafe_metadata_tokens(provides, constants::J_PROVIDES, meta_path);
    reject_unsafe_metadata_tokens(provides_soname, constants::J_PROVIDES_SONAME, meta_path);
    reject_unsafe_metadata_tokens(needed_so, constants::J_NEEDED_SO, meta_path);
    // `provides` 还必须避开求解器的 SONAME 命名空间前缀（判据与理由见函数抬头）。
    reject_reserved_provides_prefix(provides, meta_path);
    reject_bad_soname_specs(provides_soname, constants::J_PROVIDES_SONAME, meta_path);
    reject_bad_soname_specs(needed_so, constants::J_NEEDED_SO, meta_path);
    man = meta.value(std::string(constants::J_MAN), "");
}

/**
 * 扫描包内容目录，返回所有可注册路径的相对路径列表。
 *
 * 实现 pacman 风格：目录以斜杠结尾（如 "usr/bin/"），普通文件不带斜杠。
 * 目录支持多包共同持有，在移除时计数归零才删除。
 *
 * 包含：
 *  - 普通文件 → "usr/bin/bash"
 *  - 符号链接（含指向目录的，如 jvm/conf → /etc/java）→ "jvm/conf"
 *  - 普通目录 → "usr/bin/", "usr/"
 *
 * 不包含：
 *  - content/ 目录本身
 *
 * 这样做的原因：
 *   builder.cpp 清理 USR-Merge 符号链后，包内的目录就是包的真实内容。
 *   目录共享（如多包共享 /usr/bin/）通过引用计数管理，在最后持有者
 *   移除时删除目录。
 */
std::vector<std::string> scan_content_files(const fs::path& content_dir)
{
    // 包的 `content/` **必须**是真目录（lstat 语义，不是符号链接）。
    // 归档成员名消毒挡不住"把 `content` 本身做成符号链接"：`content -> /etc` 的归档能正常
    // 解压（libarchive 建的就是那条链接），而 `recursive_directory_iterator(content_dir)`
    // 会**跟随起点目录**去枚举链接目标 → 安装机上的任意文件被当成"包内容"登记、复制进目标
    // root（正规包的 `content` 永远是解压出来的真目录，直接拒绝）。
    if (!is_real_directory(content_dir)) {
        throw LpkgException(string_format("error.content_not_directory", content_dir.string()));
    }
    std::vector<std::string> entries;
    for (const auto& entry : fs::recursive_directory_iterator(content_dir)) {
        std::string rel = entry.path().lexically_relative(content_dir).string();
        // 判目录用 **lstat 语义的真目录**（不跟随末段符号链接），不是
        // `entry.is_directory() && !entry.is_symlink()`：libstdc++ 的
        // `directory_entry::is_directory()` 对**符号链接**条目会走 `status()`（跟随），
        // 于是在**符号链接环**上抛 filesystem_error —— 一个自带自环链接的包**连打包/
        // 安装扫描都过不去**（`ln -s self self` 的目录里迭代，
        // `entry.is_directory()` 抛 code=40）。判据换成 lstat 后，符号链接（含环、含悬空）
        // 一律走"文件键"分支 —— 与"符号链接是包的产物、按文件登记"的既有约定一致。
        if (is_real_directory(entry.path())) {
            // 目录 → 末尾加 /，和普通文件区分
            entries.push_back(rel + "/");
        } else {
            // 文件或符号链接 → 原样保留
            entries.push_back(rel);
        }
    }
    // **排序：让遍历顺序确定**。不排序就是 `recursive_directory_iterator`
    // 的顺序 —— 那是 **readdir 顺序，由文件系统决定**（同一份包在不同机器/容器里可能不同）。
    // 这不只是"不好看"：安装/升级的**逐条目处理顺序**（进而 WAL 行序、stash 里的备份顺序）
    // 会跟着变，于是任何"在某一步观测盘面"的断言都可能变成**顺序依赖的 flaky**
    // —— CI 上已经实打实挂过（同一个提交三跑两过一挂：success / failure / success）。
    // 排序后同一份包在任何机器上都走同一个顺序。
    std::ranges::sort(entries);
    return entries;
}

// 收集已装包的 requires（deps/ + needed_so/ 文件）与 provides（provides_db）用于建模
// installed repo。provides 必须建模：libsolv 的 dontfix 反向一致性只强制"之前有已装
// provider"的 requires，installed 包不 provide 自己的能力（如 libc.so.6），该 requires
// 就被视为"之前已 broken"而忽略——升级破坏它也不报冲突。
static void collect_installed_requires(const std::string& name, solv::InstalledPkg& p)
{
    const fs::path dep_f = Config::instance().dep_dir() / name;
    if (exists_follow(dep_f)) {
        std::ifstream f(dep_f);
        // 前置只判了"名字存在"（exists_follow），没判"打得开"：真打不开时 getline 一个字符
        // 都读不到，会**静默**当成"这个包没有 requires" —— 而这两处喂的是 solver 的已装模型
        // （下面 nso_f 同理），漏掉 requires/SONAME 会让升级破坏它却不报冲突。fail-closed：
        // 点名文件报错，不静默降级。（TOCTOU/权限场景极罕见，但"绝不静默"是硬纪律。）
        if (!f.is_open())
            throw LpkgException(string_format("error.open_file_failed", dep_f.string()));
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(f, line))
            if (!line.empty()) lines.push_back(line);
        p.deps = parse_dep_strings(lines);
    }
    const fs::path nso_f = Config::instance().needed_so_dir() / name;
    if (exists_follow(nso_f)) {
        std::ifstream f(nso_f);
        if (!f.is_open())  // 同上：静默空集会让 solver 漏掉这个包的 SONAME 依赖
            throw LpkgException(string_format("error.open_file_failed", nso_f.string()));
        std::string so;
        while (std::getline(f, so))
            if (!so.empty()) p.needed_so.push_back(so);
    }
    for (const auto& cap : Cache::instance().get_package_provides(name)) p.provides.push_back(cap);
    for (const auto& so : Cache::instance().get_package_provides_soname(name))
        p.provides_soname.push_back(so);
}

/**
 * 枚举系统 /usr/lib（或 /usr/lib64）下**确实可用**的 SONAME（--use-system-soname 用）。
 *
 * 判据必须与 `Config::has_system_soname()`（config.cpp，安装期前向校验用的那个）**一致**，
 * 否则同一个 SONAME 会得到相反结论：solver 认为"系统已提供" → 不引入真实提供者包，而安装期
 * 前向校验认为不满足。只开 `--use-system-soname` 时表现为整批报 error.unresolvable_drift
 * （吵，但至少不装错）；一旦同时开了 `--missing-so-no-error`（farm bootstrap 的固定组合，
 * 见 farm/ARCH.md）就只剩一条 warning，**装出一个缺库的系统**。
 *
 * 曾经的偏离点：这里只要"名字像 `lib*.so*`"且 `is_symlink()` 为真就收，**不看链接目标在不在**。
 * 而 `has_system_soname` 用 `exists_follow(cand)`（**跟随**链接、**不抛**，`config.cpp`）→
 * 悬空链接判为"不满足"。
 * 悬空链接是现实中真会出现的形态（升级/清理删掉真实 .so、只留下 SONAME 链接）。
 * 修法：存在性判据**直接复用 `has_system_soname`**，而不是在这里再写一份 `fs::exists` ——
 * 两处判据从此是同一个谓词，结构上不可能再次漂移。（`is_regular_file || is_symlink` 的
 * 类型判断保留：它是**枚举侧**的粗筛，用来挡掉 FIFO/设备节点这类显然不是 SONAME 的条目。）
 */
static std::vector<std::string> collect_system_sonames()
{
    std::vector<std::string> out;
    for (const std::string_view sub : {constants::USR_LIB, constants::USR_LIB64}) {
        std::error_code ec;
        fs::path dir = Config::instance().root_dir() / sub;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec) && !e.is_symlink(ec)) continue;
            const std::string fn = e.path().filename().string();
            if (fn.rfind("lib", 0) != 0 || fn.find(".so") == std::string::npos) continue;
            // 悬空符号链接（目标不存在）不算"系统已提供"——与安装期前向校验同判据
            if (!Config::instance().has_system_soname(fn)) continue;
            out.push_back(fn);
        }
    }
    return out;
}

/**
 * 目标是否为"用户显式请求"（决定 hold/autoremove 保护与 --force 生效范围）。
 *
 * 除同名匹配外必须一并匹配 **provides**：按能力/SONAME 安装时（`lpkg install libssl`
 * 由 openssl 提供）目标串是能力名、解析出的真实包名不同，只比包名会把用户显式请求
 * 记成"依赖"→ 不 hold → **紧接着一条 autoremove 就把它删掉**。
 */
static bool is_explicit_target(const std::vector<std::pair<std::string, std::string>>& targets,
                               const std::string& name, const std::vector<std::string>& provides)
{
    for (const auto& [n, v] : targets) {
        if (n == name) return true;
        for (const auto& prov : provides)
            if (prov == n) return true;
    }
    return false;
}

/// 用 libsolv 求解安装/升级/重装计划，填充 InstallContext 的 plan + install_order。
/// 取代旧的手动递归解析 resolve_package_dependencies 及其配套手动校验
/// （check_plan_consistency / check_needed_so_consistency / check_forward_soname_integrity）。
void resolve_with_solver(InstallContext& ctx)
{
    // 1. 已装状态（版本 + requires——使 solver 能检测升级破坏已装依赖）
    std::map<std::string, solv::InstalledPkg> installed;
    for (const auto& [name, ver] : Cache::instance().get_all_installed()) {
        solv::InstalledPkg p;
        p.version = ver;
        collect_installed_requires(name, p);
        installed[name] = std::move(p);
    }

    // 2. 本地候选包（读 .lpkg 元数据 → PackageInfo，记 name→path）
    std::vector<PackageInfo> local_pkgs;
    std::map<std::string, fs::path> local_paths;
    for (const auto& [name, path] : ctx.local_candidates) {
        json meta = read_archive_metadata(path);
        PackageInfo pi;
        pi.name = name;
        pi.version = meta.at(std::string(constants::J_VERSION)).get<std::string>();
        pi.dependencies = parse_dep_strings(
            meta.value(std::string(constants::J_DEPS), std::vector<std::string>{}));
        pi.provides = meta.value(std::string(constants::J_PROVIDES), std::vector<std::string>{});
        pi.provides_soname =
            meta.value(std::string(constants::J_PROVIDES_SONAME), std::vector<std::string>{});
        pi.needed_so = meta.value(std::string(constants::J_NEEDED_SO), std::vector<std::string>{});
        // ⚠️ 这条路（本地 `.lpkg` 候选）走的是 `read_archive_metadata`，**绕过了**
        // `read_package_metadata` —— 所以两道校验要在这里自己补：不做的话，控制字符能伪造
        // 状态文件里的记录，非法规格能变成"永远匹配不上的裸名"（与主读入点同一对判据）。
        reject_unsafe_metadata_tokens(pi.provides_soname, constants::J_PROVIDES_SONAME, path);
        reject_unsafe_metadata_tokens(pi.needed_so, constants::J_NEEDED_SO, path);
        reject_bad_soname_specs(pi.provides_soname, constants::J_PROVIDES_SONAME, path);
        reject_bad_soname_specs(pi.needed_so, constants::J_NEEDED_SO, path);
        // `provides` 同样要校验 —— "同族判据只推了一条分支"正是本仓库反复踩的形态：SONAME
        // 两个字段补了、`provides` 漏了。它既缺分帧字符检查（控制字符能伪造 `provides.db`
        // 记录 ⇒ 幽灵提供者 ⇒ 假满足依赖），也缺 SONAME 命名空间前缀检查
        // （见 `reject_reserved_provides_prefix`）。
        reject_unsafe_metadata_tokens(pi.provides, constants::J_PROVIDES, path);
        reject_reserved_provides_prefix(pi.provides, path);
        local_pkgs.push_back(std::move(pi));
        local_paths[name] = path;
    }

    // 3. 选项
    solv::SolveOptions opts;
    opts.force_reinstall = ctx.force_reinstall;
    opts.missing_so_no_error = Config::instance().missing_so_no_error_mode();
    opts.use_system_soname = Config::instance().use_system_soname_mode();
    opts.no_deps = Config::instance().no_deps_mode();
    if (opts.use_system_soname) opts.system_sonames = collect_system_sonames();

    // 4. 求解
    auto result = solv::solve_install(ctx.repo, local_pkgs, installed, ctx.targets, opts);

    // 5. 报错
    if (!result.ok()) {
        std::string msg;
        for (const auto& p : result.problems) msg += p + "\n";
        throw LpkgException(msg);
    }

    // 6. 映射 plan + order
    for (const auto& rp : result.order) {
        auto lp = local_paths.find(rp.name);
        PackageInfo info;
        bool from_repo = false;
        if (lp != local_paths.end()) {
            for (const auto& pi : local_pkgs)
                if (pi.name == rp.name) {
                    info = pi;
                    break;
                }
        } else if (auto repo_info = ctx.repo.find_package(rp.name, rp.version)) {
            info = *repo_info;
            from_repo = true;
        } else {
            // ── 仓库解析路径上取不到 info —— **必须硬报错，绝不静默降级** ──────────────
            //
            // 此前这里什么都不做，`info` 保持**默认构造**：name/version 之外全空，其中
            // 最要命的是 `sha256` 空。空的 sha256 一路流到 installation_task：
            // `download_and_verify_package()` 里两处 `!expected_hash_.empty()` 守卫直接放行
            // → **从镜像下载下来的包文件一个字节都没校验**（哈希是下载内容的唯一完整性
            // 依据，索引与包文件都是远端可写的东西）。而"求解结果里有、仓库索引里没有"本
            // 身就是索引/求解不一致的信号（两者用的是同一个 Repository），正常路径不可能
            // 发生 —— 静默降级只是把"索引坏了"伪装成"装好了"。
            //
            // 边界的另一侧**不动**：本地 `.lpkg` 文件安装（`lp != end`）本来就允许没有
            // 哈希（离线安装，`local_paths` 里那条路的 PackageInfo 从不带 sha256）——
            // 那条路的完整性由用户手里的文件本身负责，不是这条。
            //
            // 已知的可达路径：`--force` 重装一个**已装但已不在仓库索引里**的包（
            // solve_install 末尾的 force 回填会把 installed 里的条目塞进 order，而它不在
            // repo.packages() 里）→ 这一版之前会静默地从镜像拉一个**未校验**的包装上，
            // 现在明确拒绝。fail-closed 是**有意**的取舍。
            throw LpkgException(string_format("error.plan_pkg_not_in_repo", rp.name, rp.version));
        }

        // 仓库来的包即使解析成功，哈希也可能是空的（LankeBUILD 直出的索引不带 sha256）——
        // 那种情况下载照样不校验。这不是错误（生态里合法），但**不能静默**。
        if (from_repo && info.sha256.empty())
            log_warning(string_format("warning.plan_hash_missing", rp.name, rp.version));

        InstallPlan p;
        p.name = rp.name;
        p.actual_version = rp.version;
        p.sha256 = info.sha256;
        p.is_explicit = is_explicit_target(ctx.targets, rp.name, info.provides);
        if (lp != local_paths.end()) p.local_path = lp->second;
        p.dependencies = info.dependencies;
        p.provides = info.provides;
        p.provides_soname = info.provides_soname;
        p.needed_so = info.needed_so;
        p.force_reinstall = (ctx.force_reinstall && p.is_explicit);
        ctx.plan[rp.name] = std::move(p);
        ctx.install_order.push_back(rp.name);
    }
}

std::unordered_set<std::string> get_all_required_packages()
{
    auto& cache = Cache::instance();
    std::unordered_set<std::string> req;
    {
        req = cache.get_all_held();  // 值语义快照：不再需要（也不该）自己持锁
    }
    std::vector q(req.begin(), req.end());
    size_t head = 0;
    while (head < q.size()) {
        const std::string curr = q[head++];
        auto check_and_add = [&](const std::string& name) {
            if (cache.is_installed(name) && !req.contains(name)) {
                req.insert(name);
                q.push_back(name);
            }
        };

        // 命名依赖：deps/ 文件
        const fs::path p = Config::instance().dep_dir() / curr;
        if (exists_follow(p)) {
            std::ifstream f(p);
            // 前置只判"存在"，真打不开时下方循环读到空 → 这个包的依赖不会进 required 集
            // ⇒ autoremove 会把它**当作孤儿删掉**（破坏性方向）。fail-closed：点名报错。
            if (!f.is_open())
                throw LpkgException(string_format("error.open_file_failed", p.string()));
            std::string line;
            while (std::getline(f, line)) {
                // 包名提取走 `vercmp/dep_parser` 的**唯一实现**。这里原先自己写了
                // `find_first_of(" \t<>=")`：对 `foo!=1.0` 会切出 `foo!`（`!` 不在字符集里，
                // 而 `!=` 是合法运算符），而对 `foo==1.0` 会切出 `foo`（恰好对）——
                // 同一族里对错参半，正是"第二份实现必然漂移"的形态。
                const std::string d_name = detail::dependency_name_of(line);
                if (cache.is_installed(d_name))
                    check_and_add(d_name);
                else
                    for (const auto& prov : cache.get_providers(d_name)) check_and_add(prov);
            }
        }

        // SONAME 依赖：needed_so/ 文件 → 提供者包同样是"被依赖"的。
        // 缺这段时纯 SONAME 链路拉入的包（如 gcc←libmpc.so.3→mpc）会被 autoremove
        // 误判为孤儿而删除。
        const fs::path nso_f = Config::instance().needed_so_dir() / curr;
        if (exists_follow(nso_f)) {
            std::ifstream nf(nso_f);
            if (!nf.is_open())  // 同上：静默空集会让 autoremove 误删 SONAME 提供者
                throw LpkgException(string_format("error.open_file_failed", nso_f.string()));
            std::string so;
            while (std::getline(nf, so)) {
                if (so.empty()) continue;
                for (const auto& prov : cache.get_soname_providers(so)) check_and_add(prov);
            }
        }
    }
    return req;
}

// ============================================================================
// 从 `installation_task.cpp` 拆出的共用纯函数（正文逐字搬移，未改一行）
// ============================================================================

/**
 * `.lpkgtmp` 落位前的最后一道闸：tmp 路径是**符号链接**时拒绝写入（失败要响）。
 *
 * `fs::copy(..., overwrite_existing)`、`chmod`/`lchown`、`fs::permissions` 在写 `.lpkgtmp`
 * 这一步**全都跟随末段符号链接** —— 盘上只要有一个 `usr/bin/foo.lpkgtmp → /etc/sudoers`，
 * 处理 `usr/bin/foo` 时内容与权限就写到链接目标上去了。而冲突预检只扫归档清单里的路径、
 * **不看 `<目标>.lpkgtmp`**（它不在清单里），整条路无告警；重装时确定性命中（上一轮落下的
 * 那条链接还在盘上）。所以必须在这里挡，且必须**抛**而不是静默 unlink —— 要拒绝的是"包
 * 提供/占用 `.lpkgtmp` 名字"这个信号本身，静默删掉那条链接反而替攻击者收拾了现场。
 *
 * 判据**只能是 `is_symlink`，不能是 `exists`**：上一轮崩溃留下的 `.lpkgtmp` **普通文件**
 * 是"先写 tmp 再 rename"这套原语的**正常残留形态**，必须照旧覆盖 —— 拒绝它会让一次崩溃
 * 之后的所有重装永久失败（同目录下的 `StaleRegularLpkgtmpIsStillOverwritten` 钉这个边界）。
 */
void refuse_symlink_tmp_path(const fs::path& tmp_path)
{
    // 不抛谓词：父链中间段成环时 `fs::is_symlink` 抛 ELOOP（raw
    // `filesystem_error`，无 l10n 文案），而本守卫的职责是**给出干净的错误**而不是变成
    // 另一个错误。判否时继续往下走也是安全的（写 tmp 会撞 ELOOP 失败 = fail-closed）。
    if (is_symlink_no_follow(tmp_path))
        throw LpkgException(string_format("error.tmp_path_is_symlink", tmp_path.string()));
}

/**
 * 三哈希分流的判定表。语义只有这一份（pacman `add.c` 的三条分支 + 老 DB 的退化路径），
 * 别在调用点再写一遍。
 *
 * **有旧记录**（正常路径）：
 *   ① 盘上 == 旧记录   → `InstallNew`（用户没改过，静默换新版）
 *   ② 旧记录 == 新包   → `KeepLocal`（包没改这个配置，保留用户文件、不产生 `.lpkgnew`）
 *   ③ 其余（含 `hash_local` 拿不到 —— 盘上是符号链接/读不到）→ `SaveLpkgnew`（原文件不动）
 *
 * **无旧记录**（老 DB：本特性之前装的包，`confhashes.db` 里没有这一条）——只算两份哈希：
 *   · 盘上那份 == 新包那份 → `KeepLocal`（内容一致 = 没有"新东西"要给用户审阅，
 *     也就不该产生 `.lpkgnew`）；
 *   · 不一致（或盘上那份不可读）→ `SaveLpkgnew`（无从判定用户改没改过 → 保守）。
 *   两个分支都由调用点把 **`hash_pkg`（包内那份）**的哈希写进 DB —— 这里**没有例外**：
 *   盘上那份（退化路径上往往还是**无主**文件）永不被追认（历史与后果见 `record` 处的注释）。
 *   ⚠ 代价说明（有意为之）：盘上那份与记录不相等时，包每改一次配置都会再落一份**可见的**
 *   `.lpkgnew`（吵，但绝不静默）；用户把 `.lpkgnew` 合并进盘上那份（此后盘上 == 记录）就
 *   回到正常分流。曾经的做法是"追认盘上那份"——换来"`.lpkgnew` 不刷"，代价是把用户文件
 *   当成我们装的：下一次升级满足 ① 静默覆盖它，且记录被删（升级丢弃 /etc 条目 /
 *   remove_conf_hash / 移除包，三处都会删）之后"追认"会被重新武装、反复发生。
 */
ConfigDisposition classify_config_update(std::string_view hash_local, std::string_view hash_orig,
                                         std::string_view hash_pkg)
{
    if (hash_orig.empty()) {
        if (!hash_local.empty() && hash_local == hash_pkg) return ConfigDisposition::KeepLocal;
        return ConfigDisposition::SaveLpkgnew;
    }
    if (!hash_local.empty() && hash_local == hash_orig) return ConfigDisposition::InstallNew;
    if (hash_orig == hash_pkg) return ConfigDisposition::KeepLocal;
    return ConfigDisposition::SaveLpkgnew;
}

}  // namespace detail
