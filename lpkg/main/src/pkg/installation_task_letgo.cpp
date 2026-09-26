/**
 * 让开趟（②a 归档条目 / ②b DB 旧键）—— 本文件负责「写入新版本之前，先把盘面清出来」：
 *
 *   · `InstallationTask::backup_existing_files()`：新版本会碰的每个路径 —— 挡路物搬进 stash
 *     （`BACKUP`）、`/etc` 配置搬进 stash 待三哈希判定、该 `.lpkgsave` 的整树改名、建目录；
 *     并在这一趟把"让开**之前**"的路径事实（连同 `/etc` 的三哈希结论）记进 `ProbeLedger`，
 *     供写入趟**逐字复用**（见 `op_sink.hpp` 的 `PathRecord`）。
 *   · `InstallationTask::remove_obsolete_files()`：旧版本有、新版本不再提供的触碰面 ——
 *     搬进 stash（`REMOVE_OLD`）/ 撤所有权（`/etc`）/ 空目录 `rmdir`（`DIR_RM`）。
 *   · `trace_remove()`：`LPKG_TRACE_REMOVE=1` 时的逐条决策追踪（默认静默）。
 *
 * 两个入口函数各自退化成"算公共输入 → 分派"：让开趟前半按**决策表动作**分派给五个 handler
 * （见下面 `LetGoEntry` 那一节；主循环只剩 probe 事实 → 查表 → 分派 → 记账），后半按
 * **阶段 1 / 阶段 2** 分成两个函数。handler 与阶段函数都是本 TU 的匿名 namespace 自由函数，
 * 参数显式传入 —— `InstallationTask` 的声明在 package_manager.hpp，本文件不碰它。
 *
 * ⚠️ 拆分是**纯代码搬移**（详见 installation_task.cpp 顶部说明）；`classify_config_update()`
 *    因为写入趟的回退路径也要用，下沉到了 `install_common.*`。
 */

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "archive.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "config/config.hpp"
#include "crypto/hash.hpp"
#include "db/cache.hpp"
#include "db/test_breakpoints.hpp"
#include "db/transaction_log.hpp"
#include "db/wal_op.hpp"
#include "downloader.hpp"
#include "i18n/localization.hpp"
#include "install_common.hpp"
#include "op_sink.hpp"
#include "trigger/trigger.hpp"
#include "vercmp/version.hpp"

extern std::atomic<bool> sigint_graceful;

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
/**
 * **升级侧**的废弃文件/目录清理追踪。设 LPKG_TRACE_REMOVE=1 时逐条打印废弃文件/目录的
 * 决策与底层操作（与 strace 的 unlink/rmdir/rename 对照，定位"哪些目录没被删、为什么"）。
 * 默认静默，不引入日志噪音。
 *
 * ⚠️ 只有升级路径打追踪（本文件的 `remove_obsolete_files()`）。移除侧
 * （`do_remove_package()`）走的是**同一套**目录删除语义（`remove_empty_owned_dirs`），
 * 但**不打追踪** —— 这条注释原先写"升级/移除"，与代码不符（移除侧从未调用过本函数）；
 * 保持移除侧静默是有意的：它的可观测输出（日志/追踪）在这次重构前后逐字未变。
 */
void trace_remove(const std::string& msg)
{
    if (const char* v = std::getenv("LPKG_TRACE_REMOVE"); v && std::string_view(v) == "1")
        fprintf(stderr, "[lpkg-remove-trace] %s\n", msg.c_str());
}

// ============================================================================
// `remove_obsolete_files()` 的两个阶段（阶段 1 = 废弃文件/符号链接、阶段 2 = 废弃目录）
//
// 两段原先逐字写在同一个函数体里、只靠注释分界；分开是因为它们的**判据不同**：
//   · 阶段 1 = 决策表的 `DropOwnership`（`/etc`：只撤所有权，不碰盘）或 `StashObsolete`
//     （其余：搬进 stash）；
//   · 阶段 2 = 决策表的 `RemoveDir`（空则 rmdir），且**多一道时序守卫**（见函数内注释）。
// 两段各自"查表 → 执行"的形状完全一样，差异只在动作与守卫上 —— 抽成两个函数后
// `remove_obsolete_files()` 退化成"算公共输入（旧键 / 新集合 / root）→ 依次调用"。
// ============================================================================

/**
 * 阶段 1：废弃的普通文件（含符号链接）→ rename 进 stash（WAL `REMOVE_OLD`）。
 *
 * 决策表（唯一决策点）：DB 旧键的登记趟动作只有三种 —— `Noop`（新版本仍提供 / 还有别的
 * 持有者 / 盘上本来就没有 / 新版本已把它变成目录）、`DropOwnership`（`/etc` 的废弃条目：
 * 只撤所有权 + 配置哈希记录，**不搬、不删**）、`StashObsolete`（其余：搬进 stash）。
 * 归属事实要按"谁持有"算，故分两步查表。
 *
 * `root` 由调用方传入（`Config::instance().root_dir()`）—— 它只在这一段用得上。
 */
void obsolete_files_pass(Cache& cache, const std::string& pkg_name, const fs::path& root,
                         const std::unordered_set<std::string>& old_files,
                         const std::unordered_set<std::string>& new_set, detail::OpSink& sink)
{
    const auto to_phys = [&](const std::string& logical) -> fs::path {
        return fs::path(logical).is_absolute() ? root / fs::path(logical).relative_path()
                                               : root / logical;
    };

    for (const auto& old_file : old_files) {
        if (old_file.ends_with('/')) continue;  // 目录 → 阶段 2

        detail::PathFacts facts;
        facts.logical = old_file;
        facts.in_archive = false;
        facts.is_config = old_file.starts_with(std::string(constants::DIR_ETC_PREFIX));
        facts.entry_is_dir = false;
        facts.obsolete = !new_set.contains(old_file);

        if (facts.is_config) {
            // `/etc` 的废弃条目（2026-09-26 改）：表给 `SaveConfigObsolete`（**文件/符号链接**）
            // 或 `DropOwnership`（**目录**，有意例外）。前者先把原物改名 `<路径>.lpkgsave`
            // —— write-ahead WAL `SAVE_CONF`，逆操作就是把名字改回来，所以整批回滚能逐字节
            // 还原；然后再撤所有权 + 撤配置哈希记录（这两步与改前逐字相同）。
            // 新版本仍提供它时表给 `Noop`（什么都不做）。
            const detail::PathAction act = detail::decide_path(facts).reg;
            // ⚠️ **别动"新版本以目录形态提供"的那个路径**（键形态差一个尾斜杠）：
            // 旧键是**裸的**（`etc/x`，v1 发的是文件），而新版本登记的是**目录键**
            // （`etc/x/`）—— 于是这条旧键在 `new_set` 里查不到、被判成"废弃"，而**让开趟
            // 已经按"非目录 → 目录"那一格处理过它了**（`SaveConfigAndMkDir` 把旧物改名、
            // 又建好了新目录）。此刻再改名，搬走的就是**刚建好的那个目录**（实测：留下一个
            // 空的 `<路径>.lpkgsave/` 空壳，还把原来的 `.lpkgsave` 挤成 `.lpkgsave.1`）。
            // 判据只能用键形态：`new_set` 里有没有 `<裸键>/`。
            const bool provided_as_dir = new_set.contains(old_file + "/");
            if (act == detail::PathAction::SaveConfigObsolete && !provided_as_dir) {
                const fs::path phys = to_phys(old_file);
                // 盘上那份可能早就不在了（用户删了 / 更早的批次撤过），而 DB 仍记着我们持有
                // 它 —— 对不存在的源 rename 是 ENOENT，会把整批拖进回滚。没有可保留的东西
                // 就只撤记录（与"用户自己删掉了那份配置"一致）。
                if (exists_no_follow(phys)) sink.save_config(phys);
            }
            if (act == detail::PathAction::SaveConfigObsolete ||
                act == detail::PathAction::DropOwnership) {
                cache.remove_file_owner(old_file, pkg_name);
                cache.remove_conf_hash(old_file, pkg_name);
            }
            continue;
        }
        if (!facts.obsolete) continue;  // 表：新版本仍提供 → 归 ②③ 处理

        auto owners = cache.get_file_owners(old_file);
        if (!owners.contains(pkg_name)) continue;
        cache.remove_file_owner(old_file, pkg_name);
        facts.last_owner = cache.get_file_owners(old_file).empty();
        // 只剩本包持有时才可能搬走；盘面形态参与判定（新版本是否把它变成了目录）
        const fs::path phys = strip_trailing_slash(to_phys(old_file));
        // 新版本把该路径变成了**目录**（文件→目录的升级，TODO E4）：内容已由拷贝阶段
        // 替换好，这里绝不能再把它当"废弃旧文件"搬进 stash——否则刚建好的目录被搬走，
        // 升级"成功"但目录消失（实测）。目录的清理由阶段 2 负责。
        // 盘上是 symlink→目录、而新版本在该路径登记了**目录条目**（`path/`）：同理 —— 拷贝
        // 阶段已**穿过链接**把内容写进真实目录，把链接搬进 stash 会让这个路径整个消失
        // （实测：v1 发 symlink `var/run` → v2 发目录 `var/run/`，升级后 /var/run 干脆不存在，
        // 比原事故更糟 —— 原事故至少还留下一个实体目录）。
        facts.new_dir_entry = new_set.contains(old_file + "/");
        // 包在盘上的路径：环上不抛（同上）
        facts.disk_exists = exists_no_follow(phys);
        facts.disk_is_dir = is_real_directory(phys);
        if (detail::decide_path(facts).reg != detail::PathAction::StashObsolete) continue;

        trace_remove("obsolete FILE " + old_file + " → rename into stash");
        log_info(string_format("info.removing_obsolete_file", old_file));
        // WAL: REMOVE_OLD + rename 进 stash（一次调用，顺序不可能写反）
        // 断点：`REMOVE_OLD` 行已落、rename 未做 —— 2026-09-26 补上（此前这个窗口注入不进去，
        // 见 op_sink.hpp 里 `backup_obsolete` 的说明）。
        sink.backup_obsolete(phys, "remove_old_after_wal_" + pkg_name);
    }
}

/**
 * 阶段 2：废弃的目录（最深优先，仅最后持有者）→ 空则 `DIR_RM`（rmdir + 元数据）。
 *
 * 候选集 = 通过决策表（登记趟动作 = `RemoveDir`）的物理路径 —— 也就是"新版本不再提供
 * 且本包是最后持有者"的那些；真正的删除判据（目录键剥尾斜杠 / 最深优先 / 真目录 /
 * 此刻为空 / 挂载点保留）在 `remove_empty_owned_dirs` 里**与移除侧共用一份**
 * （本函数只管候选集的筛选、时序守卫与逐目录报告）。
 */
void obsolete_dirs_pass(Cache& cache, const std::string& pkg_name,
                        const std::unordered_set<std::string>& old_files,
                        const std::unordered_set<std::string>& new_set, detail::OpSink& sink)
{
    std::vector<std::string> obsolete_dir_keys;
    for (const auto& old_file : old_files) {
        if (!old_file.ends_with('/')) continue;

        detail::PathFacts facts;
        facts.logical = old_file;
        facts.in_archive = false;
        // 配置目录从不物理删除（与 /etc 文件一致，保留系统配置）
        facts.is_config = old_file.starts_with(std::string(constants::DIR_ETC_PREFIX));
        facts.entry_is_dir = true;
        facts.obsolete = !new_set.contains(old_file);

        if (facts.is_config) {
            // 表的结论与阶段 1 同构：`DropOwnership`（废弃）/ `Noop`（新版本仍提供）
            if (detail::decide_path(facts).reg == detail::PathAction::DropOwnership) {
                cache.remove_file_owner(old_file, pkg_name);
                cache.remove_conf_hash(old_file, pkg_name);  // 记录随归属一起撤（同阶段 1）
            }
            continue;
        }
        if (!facts.obsolete) continue;  // 表：新版本仍提供 → 不动

        // **"此刻为空"的判据要跟着时序走**（第③步新增）：本阶段现在跑在写入**之前**，
        // 而原来是写入**之后** —— 新版本若在该目录下有条目，原来那一刻它已经非空、
        // 于是被跳过；现在提前看还是空的，会被 rmdir 掉，接着又被写入趟重建（丢原
        // mode/uid、白写两条 WAL）。显式排除这种目录，把判据钉回与原来同一口径。
        if (std::any_of(new_set.begin(), new_set.end(), [&](const std::string& k) {
                return k.size() > old_file.size() && k.starts_with(old_file);
            }))
            continue;

        auto owners = cache.get_file_owners(old_file);
        if (!owners.contains(pkg_name)) continue;
        cache.remove_file_owner(old_file, pkg_name);
        facts.last_owner = cache.get_file_owners(old_file).empty();
        // 表：只有"仍是最后持有者"才列为删除候选。真正的 rmdir 由 remove_empty_owned_dirs
        // 按"真目录 + 此刻为空"（最深优先）再判 —— 那五条判据与移除侧**共用一份实现**。
        if (detail::decide_path(facts).reg != detail::PathAction::RemoveDir) continue;
        obsolete_dir_keys.push_back(old_file);
    }

    // 逐目录报告（**只影响日志/追踪，不参与判据**）：升级侧原先由自己那圈循环打的
    // `info.removing_obsolete_file` 与 `LPKG_TRACE_REMOVE` 追踪原样保留在这里 ——
    // 移除侧不传回调，于是它既不新增日志也不新增追踪（两侧输出逐字不变）。
    detail::remove_empty_owned_dirs(
        cache, pkg_name, obsolete_dir_keys, sink, [](const fs::path& phys, bool removed) {
            if (!removed) {
                trace_remove("obsolete DIR  " + phys.string() + " → SKIP（含无主内容，整树保留）");
                return;
            }
            trace_remove("obsolete DIR  " + phys.string() + " → rmdir + DIR_RM");
            log_info(string_format("info.removing_obsolete_file", phys.string()));
        });
}
/**
 * 撤销"本包声明过、**新版本不再声明**"的目录 xattr 键。
 *
 * 为什么需要这一趟（而不是靠"写入侧只写声明过的键"就够了）：写入侧从不**删**键
 * （`copy_xattrs` 的语义就是"只写我有的"，见 base/utils.hpp）—— 于是新版本**撤掉**一个
 * 键时它留在盘上继续生效。陈旧键不是无害的杂物：`system.posix_acl_default` 是**该目录下
 * 新建文件的继承权限**，`security.selinux` 是标签 —— 一个已经被上游删掉的
 * `posix_acl_default` 会继续给新文件放权限。所以这属于**安全性质**，不是清理洁癖。
 *
 * 判据（三条，与撤销那个共用的实现里逐条落实）：
 *   1. **只撤本包声明过的键**（清单来自 `xattrkeys.db`，不是"扫盘上有什么"）；
 *   2. 这个键**还有别的属主** → 只摘本包的登记、**不动盘**（xattr 按目录共用，
 *      "这个目录还有没有别的属主"是错的判据 —— 那会撤掉别的包的键）；
 *   3. 撤之前把旧值写进 WAL（`OpSink::unset_xattr` 内部），整批回滚能逐字节还原。
 *
 * "新版本声明了哪些键"的来源 = **包内那份解压出来的目录**（`content/<相对路径>`）——
 * 打包/解包两侧本来就带 xattr（disk reader 带、解包开了 `ARCHIVE_EXTRACT_XATTR|ACL`），
 * 所以 `llistxattr` 读它拿到的就是"新版本声明的全部"。**目录在新版本里不存在 / 不再是目录**
 * （类型变化、或整条废弃）→ 声明集为空 → 本包那些键全撤（正合语义：新版本没这个目录了）。
 *
 * 只做**目录**：普通文件的 xattr 永远随"`.lpkgtmp` + rename"进一个**新 inode**，
 * 不存在"上一版留下的陈旧键"这回事（旧 inode 由 BACKUP/REMOVE_OLD 连内容一起收走）。
 */
void revoke_undeclared_xattrs(Cache& cache, const std::string& pkg_name, const fs::path& root,
                              const fs::path& content_dir, detail::OpSink& sink)
{
    auto owned = cache.get_package_xattr_keys(pkg_name);
    if (owned.empty()) return;  // 绝大多数包一个目录 xattr 都没有 → 这一步是空的

    // 按**目录**分组：声明集要按目录读一次 `llistxattr`，逐个键各读一遍是白扫。
    std::map<std::string, std::vector<std::string>> by_dir;
    for (auto& [logical, key] : owned) by_dir[logical].push_back(std::move(key));

    for (const auto& [logical, keys] : by_dir) {
        std::unordered_set<std::string> declared;
        // 逻辑目录键（`/usr/share/x/`）→ 包内解压出来的那份（`content/usr/share/x/`）。
        // `is_real_directory` 而不是 `exists`：类型变化那一格盘上/包内可能已经不是目录。
        const fs::path src = content_dir / fs::path(logical).relative_path();
        if (is_real_directory(src)) {
            for (auto& k : list_xattr_keys(src)) declared.insert(std::move(k));
        }
        for (const auto& key : keys) {
            if (declared.contains(key)) continue;  // 新版本仍声明 → 归写入侧管，不撤
            if (revoke_xattr_key_if_unowned(cache, pkg_name, logical, key, sink, root)) {
                log_info(string_format("info.xattr_key_revoked", key, logical));
                trace_remove("obsolete XATTR " + logical + " " + key + " → lremovexattr");
            }
        }
    }
}

}  // namespace

/**
 * 让开趟（后半，第②b 步）：移除**旧版本不再提供**的触碰面。
 *
 * ── 为什么单独成一个"趟"（第③步的核心）────────────────────────────────────────
 * 这段逻辑原先在 `commit_without_file_ops()`（**注册**趟）里，跑在两趟之间：让开趟
 * （`backup_existing_files`）之后、写入趟（`copy_package_files`）**之前**它还没跑 ——
 * 也就是"旧版本的文件还躺在盘上、新版本的内容已经写上去了"。第③步把它整体前移到写入
 * **之前**，于是升级变成 pacman 的那个形状：
 *
 *     先移除旧版本的全部触碰面  →  再安装新版本
 *
 * 两个后果（都是这一步想要的）：
 *   · **类型转换不再是特例**：新条目落位时，旧那个已经被搬走/删掉了。
 *   · 每个被触碰的路径在写入阶段都是**确定的空位**，"两个时刻的盘面事实各查一次表"这件事
 *     在废弃这一族上彻底消失（决策表的输入 `in_archive=false` 那一侧全程只有这一趟在跑）。
 *
 * ── 只改时序，不改语义 ──────────────────────────────────────────────────────
 * 判据（决策表 `decide_path` 的登记趟动作）、WAL 行（`REMOVE_OLD` / `DIR_RM`）、回滚方式
 * （`reverse_execute`）一字未变。**唯一**需要显式对齐的是目录阶段那个"此刻为空"判据：
 * 它原来的时刻在写入之后，新版本若往某个废弃目录里写东西，那时它**非空** ⇒ 跳过；
 * 现在提前了，所以显式排除"新版本有条目落在该目录下"的目录 —— 就是阶段 2 里那个
 * `std::any_of(new_set…)` 守卫（**没有单独的函数名**；本行原写"见下面 `has_new_descendant`"，
 * 而全仓并无此函数，2026-09-26 订正）。
 *
 * ── `/etc` 废弃条目：**文件/符号链接改名 `.lpkgsave`；目录只撤所有权、不搬不删** ──────
 * 目录那半是移除侧既定政策的对称（「`<dir>.lpkgsave` 会把不知名的目录整个搬走，宁可不碰、
 * 只告警」）；文件/符号链接那半是 2026-09-26 改的（原先也只撤所有权、留在原位）。
 * **都不能改成「搬进 stash」**：stash 在提交后会被清掉，那等于静默丢掉一份配置 ——
 * `.lpkgsave` 才是「留在盘上等用户处理」的形态。这正是 `/etc` 天然无法满足
 * 「盘面 == 新版本形态」的原因（`ARCH.md` §5.4 不变量 2 的例外）。
 */
void InstallationTask::remove_obsolete_files()
{
    if (old_version_to_replace_.empty()) return;  // 全新安装：没有"旧版本"可言

    auto& cache = Cache::instance();
    const auto old_files = cache.get_package_files(pkg_name_);
    log_info(string_format("info.upgrade_old_files_check", pkg_name_, old_version_to_replace_,
                           actual_version_, old_files.size()));
    if (old_files.empty()) return;

    // 写入层原语：废弃文件/目录的 WAL 行与物理操作成对发生（见 op_sink.hpp）
    detail::OpSink sink(pkg_name_, &stashes_);
    const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
    auto new_files = detail::scan_content_files(content_dir);
    // 新版本提供的全部路径（`/` 起头的逻辑形态）—— 两阶段共用的一份"是否仍提供"判据
    std::unordered_set<std::string> new_set;
    for (const auto& f : new_files) new_set.insert((fs::path("/") / f).string());

    // 阶段 1 与阶段 2 是两段独立实现（见文件上部各自的说明）：**顺序不得交换** ——
    // 阶段 2 的候选集与"此刻为空"判据都建立在阶段 1 已经把废弃文件搬走之上。
    obsolete_files_pass(cache, pkg_name_, Config::instance().root_dir(), old_files, new_set, sink);
    obsolete_dirs_pass(cache, pkg_name_, old_files, new_set, sink);
    // 目录 xattr 的撤销（2026-09-26 补）：与上面两阶段**没有顺序约束** —— 它撤的是"本包声明过、
    // 新版本不再声明"的键，与"废弃文件/目录"是两套独立的判据（一个目录可以仍然存在、仍归本包，
    // 但少声明了一个键）。放在这里是因为它同属"让开趟：把旧版本留下的东西收干净"，且需要
    // 已经算好的 `content_dir`。
    revoke_undeclared_xattrs(cache, pkg_name_, Config::instance().root_dir(), content_dir, sink);
}

namespace
{
// ============================================================================
// `backup_existing_files()`（让开趟·前半）的**条目 handler**
//
// 主循环只做三件事：probe 盘面事实 → 查决策表 → 按 `decision.let_go` 把条目交给下面五个
// handler 之一。为什么按**决策表动作**切、而不是按"归档条目是目录 / 符号链接 / 普通文件"切：
//   · `Stash` 这一格**同时**覆盖"归档条目是普通文件"与"归档条目是符号链接"两种形态 ——
//     单次 rename 对两者都成立，代码里**没有任何分叉**，按类型切只会切出两个内容逐字
//     相同的函数；
//   · 「盘上是真目录、而新条目非目录」那一格不是独立的一"格"，它按路径分流到两个动作上：
//     `/etc` → `SaveConfig`（整树改名 `.lpkgsave`）、非 `/etc` → `Stash`（搬进 stash）。
// 按动作切才与决策表同构（`PathAction` 的一个值 ↔ 一个 handler，`MakeDir` / `StashAndMkDir` /
// `SaveConfigAndMkDir` 三个共用同一个 —— 它们的差别只在"用哪个原语让开"，而那已经由决策表定下）。
//
// handler 一律是本 TU 的文件内自由函数、参数显式传入（与 installation_task_copy.cpp 同款）：
// `InstallationTask` 的声明在 package_manager.hpp，本趟不碰它。要落回 task 的东西
// （`new_dirs_` / `new_files_` / `probe_ledger_` / `rec`）由主循环以出参、或在自己那侧写账。
// ============================================================================

/// 一条归档条目在让开趟的上下文（主循环 probe 完算好，五个 handler 共用）
struct LetGoEntry {
    std::string name;               ///< 归档内相对路径（决策表与记录表的键）
    fs::path physical_path;         ///< 解到 root 之后的落点（目录条目带尾斜杠）
    fs::path phys_dir;              ///< **真正的父目录**：先剥尾斜杠再 `parent_path()`
                                    ///< （目录条目带尾斜杠时 `parent_path()` 给的是它自己，
                                    ///< 见赋值处的说明）
    detail::PathFacts facts;        ///< 让开**之前**的盘面事实 + 归档条目形态
    detail::PathDecision decision;  ///< 上面那份事实查决策表的结论
    bool wants_cfg_record = false;  ///< `/etc` 的非目录、非符号链接条目（三哈希那一族）
};

/// 让开趟的批次级上下文（对每个条目都一样的那几样）
struct LetGoBatch {
    detail::OpSink& sink;
    const fs::path& content_dir;  ///< 包内 `content/`（目录条目的元数据来源）
    const std::string& pkg_name;  ///< 断点名与配置归属的组成部分
};

/**
 * 归档要求**目录**条目（`MakeDir` / `StashAndMkDir` / `SaveConfigAndMkDir`）：
 * 按决策表让开挡路物，然后建目录、套元数据。
 *
 * 归档要求目录：盘上是非目录（普通文件 / 符号链接，containing symlink→目录）
 * → 先**让开**再建目录；盘上本来没有 → 直接建。
 *
 * **不写穿 symlink→目录**：pacman 明确不支持该语义（"We do not support treating
 * symlinks to directories as directories. They are considered a file."），而且包
 * 内容会落进链接目标（`/var/run` 这类通常是 tmpfs，重启即蒸发），DB 记的逻辑
 * 路径与实际落点也会脱节。挡住这类接管的责任在 check_for_file_conflicts：只要
 * 该路径不是**本包旧版本**以非目录形态持有（E4），就在那里判冲突、整批中止。
 *
 * 让开落点按路径分流：
 *   · `/etc/` → **绝不能进 stash**：stash 是"回滚用"的，提交后整个 stash 目录
 *     被 remove_all 清掉 —— 一份用户改过的配置就此无声蒸发，而且连 `.lpkgnew`
 *     都没有（三哈希只在"归档条目是普通文件"那条路上跑，这里归档条目是目录，
 *     判定表压根不参与）。改用 save_config 改名保留成 `<路径>.lpkgsave`：与移除
 *     侧对 `/etc` 的政策完全一致（非 `--purge-config` 一律 `.lpkgsave`），用户可
 *     恢复、回滚可 rename 回来。于是同一份 `/etc` 文件不会"走移除能留、走类型
 *     变更就没了"。
 *   · 非 `/etc/` → 进 stash（那是包自己的文件；pacman 对应语义见 libalpm
 *     conflict.c 的 "replacing package file with a directory, not a conflict"，
 *     条件是挡路文件由**本包已装版本**持有 —— 由 check_for_file_conflicts 保证）。
 */
void let_go_make_dir(const LetGoBatch& batch, const LetGoEntry& e, std::vector<fs::path>& new_dirs)
{
    const fs::path probe = strip_trailing_slash(e.physical_path);
    if (e.decision.let_go == detail::PathAction::SaveConfigAndMkDir) {
        batch.sink.save_config(probe);
    } else if (e.decision.let_go == detail::PathAction::StashAndMkDir) {
        // WAL: BACKUP + rename 进 stash（一次调用；此路径无 write-ahead 断点）
        batch.sink.backup(probe);
    }
    new_dirs.push_back(e.physical_path);
    // WAL: NEW_DIR <path>  (write-ahead: 先写 WAL 再做实际操作)
    batch.sink.new_dir(e.physical_path);
    // 断点：`NEW_DIR` 行已落、`create_directories` 未做 —— write-ahead 窗口。
    // （此前**目录分支没有断点**（只有普通文件的 COPY 有），"WAL 已写、目录未建"
    //  这个中间态根本注入不进去；lpkg/CLAUDE.md §2 记着这个空档。）
    BreakpointManager::instance().hit("newdir_after_wal_" + batch.pkg_name);
    // 建父链 + 建目录本体，**失败即报**。复用唯一的实现 `ensure_dir_exists()`：它先用不抛的
    // `exists_follow` 判否、`create_directories` 带 ec、报错**点名路径**并附原因
    // （`error.create_dir_failed`；路径存在但不是目录则给 `error.path_not_dir`）。
    // 此前这两行的 `ec` 被**整个丢掉**：创建失败时 `NEW_DIR` 行已经写了而目录没建成 ——
    // 盘面与 WAL 描述的世界不一致，后续元数据/落位全建立在一个不存在的目录上，而报错要等到
    // 很久以后以别的形态冒出来（2026-09-26 修）。
    ensure_dir_exists(e.phys_dir);
    ensure_dir_exists(e.physical_path);
    std::string dir_rel = e.name;
    if (dir_rel.ends_with('/')) dir_rel.pop_back();
    const fs::path src_dir = batch.content_dir / dir_rel;
    // **一律用剥过尾斜杠的 `probe`**（本函数开头就剥了）：`fs::is_symlink("link/")` 会
    // **跟随**尾斜杠（实测返回 false —— 见 CLAUDE.md 的谓词说明），拿 `e.physical_path`
    // （目录条目恒带尾斜杠）去判，守卫在"链接→目录"这一格**恒假**，`lchown`/`chmod`/
    // `lsetxattr` 会穿透去改**链接目标**（`/var/run -> ../run` 落到的 tmpfs、或别的包持有的
    // 目录）。这个形态今天到不了这里（本函数只处理"路径本来不存在 / 挡路物刚被搬走"两格，
    // 走到元数据块时目录必是刚建出来的真目录），所以这是**潜伏**缺陷 —— 但守卫写成假的就是
    // 假的，别留一个下次有人挪动调用顺序时才会炸的坑（2026-09-26 实测订正）。
    struct stat st;
    if (!is_symlink_no_follow(probe) && lstat(src_dir.c_str(), &st) == 0) {
        // 元数据经 OpSink：**先把改前值写进 WAL 再动盘**（见 op_sink.hpp 的 dir_meta）。
        // `record_previous = false`：本函数的目录**一定是本批次刚建出来的**（三个入格
        // —— `MakeDir`（让开趟探到路径不存在）、`StashAndMkDir` / `SaveConfigAndMkDir`
        // （挡路物刚被搬走/改名）—— 之后 `create_directories` 刚落位），它的收尾由
        // `NEW_DIR` 的逆操作（删除）负责，记一行改前元数据纯属冗余。
        // ⚠️ 传 false 的前提就是上面这句"一定是新建的"：哪天有人让本函数处理**已存在**的
        // 目录，必须同时改成 true，否则那个目录的元数据回滚不回去。
        batch.sink.dir_meta(probe, st.st_mode & constants::PERM_MASK_ALL, st.st_uid, st.st_gid,
                            /*record_previous=*/false);
    }
    // 目录的 **xattr**（2026-09-26 补，同日改为逐键经 OpSink）。此前 `copy_xattrs` 只有
    // 普通文件那两个调用点，**目录的 xattr 从来没被拷过** —— 而目录的 xattr 正是 POSIX ACL
    // 的存放处（`system.posix_acl_default` = 该目录下新建文件的继承权限），丢了会静默退回
    // umask 默认值。打包/解包两侧本来就是全的（disk reader 带 xattr、解包开了
    // ARCHIVE_EXTRACT_XATTR|ACL），漏的只有写入系统这一步。
    // 逐键走 OpSink 而不是整份 `copy_xattrs`，为的是**每键一行 WAL**（改前有值 → `XATTR_SET`、
    // 本来没有 → `XATTR_NEW`）与**按键盘归属**（`xattrkeys.db`）—— 前者让回滚还原得回来，
    // 后者让升级时能精确撤掉"新版本不再声明"的键（判据是"这个键还有没有别的属主"）。
    // 边界同元数据块：目标是符号链接 → 整块跳过（xattr 会落到链接目标，那可能是别的包
    // 持有的目录）。`copy_xattrs` 的"只写 from 有的键"这条性质在这里自动满足（我们本来就
    // 只遍历包内 `src_dir` 的键）。
    if (!is_symlink_no_follow(probe) && is_real_directory(probe)) {
        for (const std::string& key : list_xattr_keys(src_dir)) {
            const auto val = read_xattr(src_dir, key);
            if (!val) continue;  // 两次读之间被改掉（罕见）→ 不为空动作写 WAL 行
            if (!batch.sink.set_xattr(probe, key, *val, "xattrset_after_wal_" + batch.pkg_name))
                continue;
            // 逻辑路径 = 目录键形态（`/usr/share/x/`），与 files.db 的目录键同形。
            Cache::instance().add_xattr_key_owner((fs::path("/") / e.name).string(), key,
                                                  batch.pkg_name);
        }
    }
}

/**
 * 表说"本趟认领它，但不动盘"（`Noop`）。两种情况：
 *   · 目录条目、盘上已是真目录 → 只需保证父目录在（目录本体与元数据由写入趟刷）
 *   · `/etc` 的**符号链接**条目、或盘上本来就没有的条目 → 不接管（见
 *     decide_path 里那一格的说明）：符号链接条目绝不能被搬进 stash —— 它是
 *     "用户可能改过的那份配置"之外的形态，落点政策是"原文件留原样、v2 退
 *     `.lpkgnew`」（落点规则见 `ARCH.md` §6.3），写入趟据同一份事实执行。
 *     **连父目录都不建**：那条早退排在父目录创建之前（今天的形态，原样保留）。
 */
void let_go_noop(const LetGoEntry& e)
{
    if (e.decision.write == detail::PathAction::WriteDirMetadata) {
        std::error_code ec;
        fs::create_directories(e.phys_dir, ec);
    }
}

/**
 * 归档是**文件/符号链接**、盘上已被占（普通文件 / 符号链接 / **真目录**）→ 搬进
 * 每文件系统 stash 让开。三者走同一个动作：单次 rename 对三者都成立。
 *
 * **真目录挡路**（dir → 非目录的接管）以前在这里"什么都不做、注释写由目录逻辑
 * 处理"，而"目录逻辑"是 `commit_without_file_ops()` 的阶段 2 —— 它跑在**拷贝
 * 之后**，来不及：第③步的 `rename(.lpkgtmp → 该路径)` 撞 EISDIR、
 * `create_symlink` 撞 EEXIST，于是这条升级路径被整批预检挡在门外
 * （DirToFile / DirToSymlink 两格）。预检（`dir_tree_entirely_ours`）放行之后
 * **必须在这里把路让开**。
 * is_directory 会跟随符号链接：symlink→目录 会被误判为"目录"而跳过备份。
 * 但 copy_package_files 的 symlink/普通文件分支都会直接替换该路径（且无 WAL
 * 记录），回滚时没有 BACKUP 可恢复 → 旧符号链接永久丢失。故符号链接（含指向
 * 目录的）一律备份。
 * check_for_file_conflicts 已处理文件冲突，此处无需重复检测；
 * WAL: BACKUP + rename（断点位于 write-ahead 窗口内，只能在 sink 里命中）
 *
 * `/etc` 的普通文件条目：配置保护从"完全不碰盘"改成"先搬走、判定后再搬回"
 * （`ARCH.md` §6.3 与 §9.2 的 `UNSTASH` 行）—— **可观测行为不变**：
 *   · 盘上那份进 stash（`BACKUP`，断点仍是 `conf_replace_after_wal_<pkg>`：
 *     那个窗口的语义没变，只是从写入趟前移到了让开趟）；
 *   · 三哈希的 `hash_local` 改从**这份 stash 副本**读（同一个 inode，逐字节
 *     相同），结论与落点因此不再依赖"判定时刻盘上还在不在"；
 *   · 判为"保留"（KeepLocal / SaveLpkgnew）时写入趟用 `un_stash` 搬回原位。
 * 既然搬走了，写入趟**不许**再 backup 一次（否则搬的是空路径）——它靠这条
 * 记录里的 `stashed` 知道，见 copy_package_files。
 */
void let_go_stash(const LetGoBatch& batch, const LetGoEntry& e, detail::PathRecord& rec)
{
    {
        std::error_code ec;
        fs::create_directories(e.phys_dir, ec);
    }
    rec.stashed = (e.decision.let_go == detail::PathAction::Stash);

    // `/etc` 配置的 **mode** —— 内容改没改，三哈希看得出来；**只改权限看不出来**。
    // 用户 `chmod 600` 过的那份配置，会在写入趟被 `stage_regular_file` 用**包内条目**的
    // mode/uid/gid 落位（实测：0600 → 0644），而三哈希判的是**内容哈希**、看不到这一层 ⇒
    // 此前是**完全静默**的替换（目录那边至少有 `warning.dir_perm_mismatch`，文件这边没有）。
    // 与目录那边对称：**先告警、再纠正** —— 只把"静默"变"可见"，**不改语义**
    // （"内容未变时是否该保留用户改的 mode"是一条独立的策略问题，未定）。
    // **此处是唯一还看得见盘上那份的时刻**：下一步就把它搬进 stash 了，而写入趟落位时
    // 原位已空（`/etc` 的这一格由让开趟 `Stash` 清空）。判据取 **lstat**（不跟随末段链接），
    // 两侧都是；只比 **mode**（与目录那条告警同口径，不含 uid/gid）。
    if (e.facts.is_config && e.decision.let_go == detail::PathAction::Stash) {
        struct stat pkg_st{};
        struct stat cur_st{};
        if (lstat((batch.content_dir / e.name).c_str(), &pkg_st) == 0 &&
            lstat(strip_trailing_slash(e.physical_path).c_str(), &cur_st) == 0) {
            const mode_t pkg_mode = pkg_st.st_mode & constants::PERM_MASK_ALL;
            const mode_t cur_mode = cur_st.st_mode & constants::PERM_MASK_ALL;
            if (cur_mode != pkg_mode) {
                log_warning(string_format("warning.file_perm_mismatch", e.physical_path.string(),
                                          static_cast<int>(cur_mode), static_cast<int>(pkg_mode)));
            }
        }
    }

    const fs::path bak = batch.sink.backup(
        e.physical_path, e.decision.let_go == detail::PathAction::Stash && e.facts.is_config
                             ? "conf_replace_after_wal_" + batch.pkg_name
                             : "backup_after_wal_" + batch.pkg_name);
    if (rec.stashed && e.wants_cfg_record) rec.bak = bak;
}

/**
 * `/etc` 的**非目录**条目撞盘上真目录（dir → 文件 / 符号链接）：整树改名成
 * `<路径>.lpkgsave`（内容一个不丢、`SAVE_CONF` 可回滚），让开之后由写入趟
 * **就地**落位。**不建目录、不进 new_files_**（那条路径由写入趟的 COPY / NEW
 * 记账，这里只负责让路）。
 */
void let_go_save_config(const LetGoBatch& batch, const LetGoEntry& e)
{
    batch.sink.save_config(strip_trailing_slash(e.physical_path));
}

/// 盘上本来就没有 → 只登记（WAL `NEW`；回滚据此删除本包要落位的那个路径）
void let_go_register_new(const LetGoBatch& batch, const LetGoEntry& e,
                         std::vector<fs::path>& new_files)
{
    {
        std::error_code ec;
        fs::create_directories(e.phys_dir, ec);
    }
    new_files.push_back(e.physical_path);
    batch.sink.new_file(e.physical_path);
}

/**
 * 从**stash 里那份**读 `hash_local`（读不到 → 空串 ⇒ 无从判定 ⇒ 保守走 `SaveLpkgnew`）。
 *
 * 判据与原来逐字同构（`!is_symlink && is_regular_file`），只是路径换成 stash 里那份 ——
 * rename 不换 inode，所以两者本是同一份内容的两个名字。
 */
std::string hash_stashed_copy(const fs::path& bak)
{
    // 判据写成**一次 lstat**（2026-09-26 修）：原来的 `!exists_no_follow(bak) ||
    // fs::is_symlink(bak) || !fs::is_regular_file(bak)` 里，第三个操作数是**跟随**语义的
    // `fs::is_regular_file`（内部走 `status`）—— `bak` 完全可以是**环本身**（包自带的
    // 符号链接环被 rename 进 stash 之后还是个环），那时它对 ELOOP **抛** code=40，而这里
    // 在让开趟的判定路径上：抛出去 = 整批回滚、那个包再也升不上去。
    // lstat 一次同时表达"名字被占"+"是普通文件"（`S_ISREG` 已经排除符号链接/目录/FIFO/
    // 设备），语义与原来三条逐条等价，且不抛。
    if (!is_regular_file_no_follow(bak)) return {};
    try {
        return calculate_sha256(bak);
    } catch (const std::exception&) {
        return {};  // 读不到 → 无从判定 → 保守（SaveLpkgnew）
    }
}

/**
 * 记录（第③步）：**每个**归档条目都记下"让开之前"那一刻的事实 —— 写入趟只读这条记录去查
 * 决策表（`decide_path` 因此只被一份一致的 `PathFacts` 查询），不再自己 probe 盘面。
 *
 * 三哈希（只有 `/etc` 的非目录、非符号链接条目才有）在这里算**一次**：
 * `hash_pkg` = 包内那份；`hash_local` = **stash 副本**（不再依赖
 * "判定时刻盘上那份还在不在"，因为让开趟已经把它搬走了）；`hash_orig` =
 * confhashes 里记的旧值。结论写进 `rec.facts.cfg`，写入趟据此决定落点，
 * **不再自己 probe、也不再自己算** —— `/etc` 这一族上"两个时刻各查一次表"到此消失。
 * 判据与写入趟的入口条件同一个口径（`disk_exists && !disk_is_dir` ⇒ 走三哈希），
 * 只是这里问的是"**让开之前**盘上被占且非目录"，用 `rec.stashed` 表达最省事：
 * 让开趟的 `Stash` 恰好只在那一格发生（其余格是 `Noop`/`SaveConfig*`）。
 */
void record_let_go_facts(const LetGoBatch& batch, const LetGoEntry& e, detail::ProbeLedger& ledger,
                         detail::PathRecord rec)
{
    const fs::path src_path = batch.content_dir / e.name;
    // 包内那份读不到 → 写入趟会整条 `continue`（同一个判据），记录留给它也无用
    if (!exists_no_follow(src_path)) {
        // **配置永不静默丢失**：包内那份读不到时，写入趟**按新扫描的结果**遍历条目，
        // 那个条目连出现都不会出现（等同于 `continue`）—— 而让开趟已经把它搬进 stash
        // 了，提交后随 stash 一起被 remove_all 掉 = 静默丢一份 `/etc` 配置。
        // 所以在这里**放回原位**（`UNSTASH` 与刚才的 `BACKUP` 成对，净效果 = 没动过），
        // 并且**不写记录**：写入趟回退到"自己 probe 的"老路径，落点与重构前逐字一致。
        //
        // 可达性：**生产路径上不可达** —— 这段判据与刚才那次搬移在**同一次循环迭代**里相隔
        // 几行（`let_go_stash` → 本函数），盘上没有任何东西会在这几行之间动**包内**的
        // `content/`。这是**纵深防御**，判据（"包内那份不存在"）与写入趟同源。
        //
        // 但"不可达"不等于"不必注入"：断点**必须传下去**，否则这个窗口永远造不出来、
        // 这条防御就无法验证（"接了断点却没传"= 一个测不到的接线）。用例
        // `LetGoUnstashDefenseTest.MissingArchiveCopyPutsStashedConfigBack` 用
        // `conf_replace_after_wal_<pkg>`（让开趟 stash 那一刻）从断点里删掉包内那份，
        // 把这个状态显式造出来 —— 正是故障注入设施存在的意义。
        if (e.wants_cfg_record && rec.stashed) {
            batch.sink.un_stash(rec.bak, e.physical_path, "unstash_after_wal_" + batch.pkg_name);
        }
        return;
    }

    if (e.wants_cfg_record) {
        rec.hash_pkg = calculate_sha256(src_path);
        if (rec.stashed) {
            rec.facts.cfg = detail::classify_config_update(
                hash_stashed_copy(rec.bak),
                Cache::instance().get_conf_hash(e.facts.logical, batch.pkg_name), rec.hash_pkg);
        }
    }
    ledger.record(e.name, rec);
}
}  // namespace

/** 让开趟（前半）：把**新版本会触碰的每个路径**先清出来，并记下写入趟要用的事实 */
void InstallationTask::backup_existing_files()
{
    // 写入层原语：BACKUP/NEW/NEW_DIR 的 WAL 行与物理操作成对发生（见 op_sink.hpp）
    detail::OpSink sink(pkg_name_, &stashes_);
    const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
    auto files = detail::scan_content_files(content_dir);
    const fs::path root = Config::instance().root_dir();
    // 本包这一轮的事实记录表（**生产者**是这一趟，消费者是写入趟；见 op_sink.hpp）
    probe_ledger_.begin();
    const LetGoBatch batch{sink, content_dir, pkg_name_};

    for (const auto& f : files) {
        if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

        fs::path rel_f = f;
        if (rel_f.is_absolute()) rel_f = rel_f.relative_path();
        LetGoEntry e;
        e.name = f;
        e.physical_path = root / rel_f;
        // **先剥尾斜杠、再取父目录**。目录条目的 `physical_path` 恒带尾斜杠，而实测
        // `fs::path("/a/b/").parent_path()` 给的是 **"/a/b"（它自己）**、`filename()` 是空串
        // —— 于是 `phys_dir` 对目录条目**等于 `physical_path`**，"确保父目录存在"这个意图
        // 没有兑现（那几个 `create_directories(e.phys_dir)` 建的是它自己）。非目录条目本来
        // 就不带尾斜杠 ⇒ 剥是 no-op、行为不变（2026-09-26 修）。
        e.phys_dir = strip_trailing_slash(e.physical_path).parent_path();

        // ── 决策表（唯一决策点）：本趟只执行表里归"让开趟"的那一个动作 ────────────────
        // 事实在这里 probe **一次**，写入趟复用同一条记录（`PathRecord`）而不是重新 probe
        // —— 这正是第③步要消掉的"两个时刻各查一次表"。
        // `entry_is_symlink` 是**归档条目形态**（与盘面无关，两趟必然一致），本趟也要据实
        // 填：`/etc` 的符号链接条目正是靠它决定「不接管、不搬」（见 `ARCH.md` §6.3）。
        e.facts.logical = (fs::path("/") / f).string();
        e.facts.in_archive = true;
        e.facts.is_config = f.starts_with(std::string(constants::DIR_ETC));
        e.facts.entry_is_dir = f.ends_with('/');
        // 不抛谓词：判的是**包内**那条路径，而包自带符号链接环是**允许的**（归档成员名
        // 消毒不管这个，`scan_content_files` 也专门用 lstat 语义绕开了环）—— 中间段成环时
        // `fs::is_symlink` 抛，`!entry_is_dir` 对它为真 ⇒ 必然求值到它。
        e.facts.entry_is_symlink = !e.facts.entry_is_dir && is_symlink_no_follow(content_dir / f);
        {
            // 判之前先剥尾斜杠：目录条目的 physical_path 以 '/' 结尾，而
            // `fs::exists("/x/foo/")` 对**已存在的普通文件**返回 false（尾斜杠要求它是目录）
            // ——直接把 physical_path 拿去判会把"这里有个文件"误判成"路径不存在"。而且
            // `is_symlink` 会把末尾的符号链接**解引用**（pacman 为此专门写了 llstat()，
            // 见 FS#51377 / commit 16b91f79）。
            // 判定一律走**不抛**的谓词：`probe` 是**包在盘上的路径**，环（两个包互指、
            // 用户/hook 后建）能让 `fs::exists`/`fs::is_directory` 抛 ELOOP 从而打断整批升级。
            // `exists_no_follow` = 原来的 `exists || is_symlink`（lstat 语义），
            // `is_real_directory` = 原来的 `is_directory && !is_symlink`。
            const fs::path probe = strip_trailing_slash(e.physical_path);
            e.facts.disk_exists = exists_no_follow(probe);
            e.facts.disk_is_dir = is_real_directory(probe);
            // 盘上是符号链接（只在"存在且不是真目录"时有意义）—— `/etc` 的落点规则按
            // **类型是否变化**分流，而 `disk_exists` + `disk_is_dir` 区分不出"普通文件"与
            // "符号链接"，故补这一个事实（见 op_sink.hpp 的字段说明）。`fs::is_symlink`
            // 走 lstat、对环不抛（base/utils.hpp 的谓词说明里逐条列了哪个抛哪个不抛）。
            // 不抛谓词 —— 同 installation_task_copy.cpp 的 write_facts：中间段成环时
            // `fs::is_symlink` 抛，而 `!disk_is_dir` 对环为真 ⇒ 必然求值到它。
            e.facts.disk_is_symlink = !e.facts.disk_is_dir && is_symlink_no_follow(probe);
        }
        e.decision = detail::decide_path(e.facts);
        // 写入趟要用的记录（**每个归档条目**都记：事实部分对所有形态都有意义，写入趟的
        // 决策表查询一律读它；`/etc` 的非目录、非符号链接条目额外记三哈希结论与 stash 落点）。
        e.wants_cfg_record =
            e.facts.is_config && !e.facts.entry_is_dir && !e.facts.entry_is_symlink;

        detail::PathRecord rec;
        rec.facts = e.facts;

        switch (e.decision.let_go) {
            case detail::PathAction::MakeDir:
            case detail::PathAction::StashAndMkDir:
            case detail::PathAction::SaveConfigAndMkDir:
                let_go_make_dir(batch, e, new_dirs_);
                break;
            case detail::PathAction::Noop:
                let_go_noop(e);
                break;
            case detail::PathAction::Stash:
                let_go_stash(batch, e, rec);
                break;
            case detail::PathAction::SaveConfig:
                let_go_save_config(batch, e);
                break;
            case detail::PathAction::RegisterNew:
                let_go_register_new(batch, e, new_files_);
                break;
            default:
                // 其余动作都不归"让开趟"：决策表保证归档条目的 `let_go` 只可能是上面 6 个之一
                // （`decide_path` 的后置条件在入口处已经检查过）。
                break;
        }

        record_let_go_facts(batch, e, probe_ledger_, rec);
    }
}
