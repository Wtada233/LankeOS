/**
 * 写入趟（第③步：跑在让开趟之后、注册趟之前）—— 本文件负责
 * `InstallationTask::copy_package_files()`： 把包内 `content/` 的每个条目落到盘上（普通文件经
 * `.lpkgtmp` + fsync + `COPY`、符号链接直接 落位、目录条目只刷元数据），`/etc`
 * 配置按让开趟算好的三哈希结论分流到 原位 / 保留不碰盘（`un_stash` 搬回）/ `.lpkgnew`。
 *
 * 决策表的**事实**全部来自让开趟的 `ProbeLedger`（写入趟不再自己 probe 盘面）；每一条
 * WAL 行都由 `detail::OpSink` 与物理操作成对写出。
 *
 * 「做什么」由决策表定（`detail::decide_path`），「怎么做」按**条目形态**拆成三个 handler
 * （`write_symlink_entry` / `write_dir_entry` / `write_regular_entry`）：主循环只做
 * 「取记录 → 查表 → 按形态分派」。三个 handler 都是本 TU 的文件内函数，参数显式传入
 * （没有一个是成员 —— `InstallationTask` 的声明在 package_manager.hpp，本趟不碰它）。
 *
 * ⚠️ 拆分是**纯代码搬移**（详见 installation_task.cpp 顶部说明）：WAL 行的名字与顺序、
 *    断点名、两条安全底线（`hash_local` 只从 stash 副本读；confhashes 记录**永远**写
 *    `hash_pkg`）一字未改。
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
#include <ranges>
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

/** 一条归档条目在本趟的落位上下文（主循环算好，三个形态 handler 共用）。 */
struct ContentEntry {
    std::string name;        ///< 归档内相对路径（决策表 / 记录表的键）
    fs::path src_path;       ///< 包内 `content/` 下那份
    fs::path physical_path;  ///< 解到 root 之后的落点
};

/**
 * 决策表的盘面事实（写入趟）。**只有回退路径**才走这里 —— 正常路径下每个归档条目的事实
 * 都来自让开趟记下的 `PathRecord`（同一份事实、同一个时刻），写入趟不再自己 probe，
 * 从根上消掉"两趟各看一次盘面"的漂移可能。走这里的情形只有一种：**测试直接调用本函数**
 * （没有让开趟，记录表是空的）。
 */
detail::PathFacts write_facts(const std::string& entry, const fs::path& physical, bool entry_is_dir,
                              bool entry_is_symlink)
{
    detail::PathFacts facts;
    facts.logical = (fs::path("/") / entry).string();
    facts.in_archive = true;
    facts.is_config = entry.starts_with(std::string(constants::DIR_ETC));
    facts.entry_is_dir = entry_is_dir;
    facts.entry_is_symlink = entry_is_symlink;
    // 目标路径：与让开趟同一套**不抛**谓词（环不得把升级打断）
    const fs::path probe = strip_trailing_slash(physical);
    facts.disk_exists = exists_no_follow(probe);
    facts.disk_is_dir = is_real_directory(probe);
    // 不抛谓词（**必须**）：`fs::is_symlink` 在**中间段**成环时抛 code=40，而这里
    // `!facts.disk_is_dir` 为真（`is_real_directory` 对环判 false）⇒ 抛型那半边**必然**
    // 被求值（短路方向问题，不是"用了抛型"本身）。见 base/utils.hpp 的谓词说明。
    facts.disk_is_symlink = !facts.disk_is_dir && is_symlink_no_follow(probe);
    return facts;
}

/**
 * 把包内那份普通文件 staged 成 `<dst>.lpkgtmp`（内容 + xattr + 属主/权限 + fsync），
 * 返回 tmp 路径交给 `sink.commit_copy` 落位。两个落点（原位 / `.lpkgnew`）共用这一份：
 * 原先两处逐字重复，而"先 fsync .lpkgtmp 再写 WAL"的纪律靠人肉同步。
 */
fs::path stage_regular_file(const fs::path& src, const fs::path& dst)
{
    fs::path tmp_path = dst;
    tmp_path += ".lpkgtmp";
    // 落位前先挡住"tmp 路径是符号链接"（写 tmp 会跟随链接写到链接目标上）
    detail::refuse_symlink_tmp_path(tmp_path);
    fs::copy(src, tmp_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    copy_xattrs(src, tmp_path);  // 先搬到 .lpkgtmp，rename 后 xattr 随之生效
    struct stat st;
    if (lstat(src.c_str(), &st) == 0) {
        (void)lchown(tmp_path.c_str(), st.st_uid, st.st_gid);
        if (!S_ISLNK(st.st_mode)) {
            (void)chmod(tmp_path.c_str(), st.st_mode & constants::PERM_MASK_ALL);
        }
    }
    // fsync .lpkgtmp 后再写 WAL（断电丢内容的话，WAL 指向的就是空文件）
    if (durable_fsync_enabled()) {
        if (int fd = ::open(tmp_path.c_str(), O_RDONLY); fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
    }
    return tmp_path;
}

/**
 * 本函数负责：把一条**符号链接**归档条目落到盘上 —— 查决策表（`WriteLpkgnew` 时退到
 * `<路径>.lpkgnew`）、挡路目录按结构性冲突拒绝、用写入层原语让开旧物后 `create_symlink`。
 */
void write_symlink_entry(const ContentEntry& e, const detail::ProbeLedger& ledger,
                         detail::OpSink& sink, const std::string& pkg_name,
                         bool& has_config_conflicts)
{
    const fs::path src_path = e.src_path;
    fs::path link_target = fs::read_symlink(src_path);
    fs::path dest = e.physical_path;

    // ── 决策表（唯一决策点）：归档条目形态 = 符号链接 ───────────────────────
    // 注意 `fs::is_directory` 会跟随符号链接：`/etc/x -> /some/dir` 会被判成"目录"
    // 从而绕过配置保护（既不备份也不留 .lpkgnew，直接替换）。故**盘上那份是普通文件/
    // 符号链接**时符号链接条目一律按配置冲突处理 —— 不接管，v2 退到 `.lpkgnew`。
    //
    // ⚠️ 表里 `/etc` 且**盘上是真目录**那一格是 `SaveConfig`：让开趟先把那棵目录树
    //    整树改名成 `<路径>.lpkgsave/`，路径让开后本分支**就地**落链接（**不落**
    //    `.lpkgnew`）。这个差异**不是手工特例、是落点规则的推论**：符号链接条目与盘上
    //    真目录是**类型变化**（symlink vs dir），按规则一律「原物 `.lpkgsave` + 新物就地」；
    //    只有**类型未变**（symlink→symlink）才退 `.lpkgnew`。规则见 `ARCH.md` §6.3。
    // 事实来自让开趟的记录（**让开之前**那一刻：盘上真目录 → 表给 `WriteInPlace`），
    // 与"让开之后盘面已空"这个二次 probe 恰好同结论 —— 但同结论是**算出来的**，
    // 不是靠"再 probe 一次碰巧一样"（见下面 `ProbeLedger` 的说明）。
    const detail::PathRecord* rec = ledger.find(e.name);
    const detail::PathDecision decision =
        detail::decide_path(rec ? rec->facts
                                : write_facts(e.name, e.physical_path, /*entry_is_dir=*/false,
                                              /*entry_is_symlink=*/true));
    if (decision.write == detail::PathAction::WriteLpkgnew) {
        dest += std::string(constants::SUFFIX_LPKG_NEW);
        log_warning(
            string_format("warning.config_conflict", e.physical_path.string(), dest.string()));
        has_config_conflicts = true;
    }

    // 目录无法被符号链接替换：让开趟本该把挡路目录整树搬走（`/etc` → `<路径>.lpkgsave/`，
    // 其余进 stash），若没搬走（预检漏判 / 盘面中途变了），静默 remove 会留下无 WAL
    // 记录的破坏且回滚无法恢复 —— 作为文件冲突拒绝。
    // **第二道防线**：正常路径下让开趟已经把目录让开了，这里够不到。
    // 不抛形态：`dest` 是目标路径，环上 `fs::is_directory` 会抛。
    // `is_real_directory` 与原来的 `is_directory && !is_symlink` 逐字同义。
    if (is_real_directory(dest)) {
        throw LpkgException(string_format("error.copy_failed_rollback", e.name,
                                          e.physical_path.string(),
                                          get_string("error.dir_replaced_by_symlink")));
    }

    // 落位这份链接同样是"这个批次改了盘面"（改的不是配置本体而已），**必须能回滚**：
    // 裸 `fs::remove(dest)` + `fs::create_symlink(...)` 是**事务外的副作用** —— 批次被
    // 回滚后包根本没装上，/etc 上却多出一份「请审阅」链接（dest 可能是 `<配置>.lpkgnew`），
    // 盘面与 WAL 描述的世界不一致；批次前已有的那份也会被无条件删掉、回不来。
    // 故与普通文件分支共用**同一套写入层原语**（OpSink）：
    //   · 目标已存在（上一次留下的 .lpkgnew / 被链接接管的旧文件）→ 先 `backup` 把它
    //     **让开**（WAL BACKUP + 搬进 stash）。这里**不能**再单独 fs::remove 一次 ——
    //     旧的那份已经进 stash 了，删掉的是别人（它在回滚时要被 rename 回来）；
    //   · 无论哪条路径，随后先写 `NEW <dest>` 行（逆操作 = 删除该路径，对符号链接
    //     幂等），再 `create_symlink` —— 此时 dest 必然不存在，是**单次原子操作**。
    // 行序 BACKUP → NEW 不可反：逆序回滚才会"先撤新链接（NEW 的逆操作）、再还原旧那份
    // （BACKUP 的逆操作）"。
    if (exists_no_follow(dest)) {
        // 按表让开（**正常路径下让开趟已经做过**，此刻 dest 是空的、这里够不到）：
        //   · 类型变化（盘上原本是普通文件）→ `SaveConfig`：原物改名 `<路径>.lpkgsave`；
        //   · 其余（symlink→symlink 退 `.lpkgnew`、以及回退路径里"上一次留下的 .lpkgnew"）
        //     → `backup` 把它搬进 stash（逆序回滚才会先撤新那份、再还原旧那份）。
        if (decision.let_go == detail::PathAction::SaveConfig)
            sink.save_config(dest);
        else
            sink.backup(dest);  // 环（lstat 存在）也要让开
    }
    sink.new_file(dest);
    // 断点：`NEW <dest>` 行已落、`create_symlink` 未做 —— write-ahead 窗口。
    // （此前**符号链接分支没有断点**，只有普通文件的 COPY 有：lpkg/CLAUDE.md §2
    //  记着"链接已写 WAL、尚未 create_symlink"这个中间态无法注入故障。补上之后
    //  这一族才有"失败注入 → 回滚保真"的用例。）
    BreakpointManager::instance().hit("symlink_after_wal_" + pkg_name);
    fs::create_symlink(link_target, dest);
    struct stat st;
    if (lstat(src_path.c_str(), &st) == 0) {
        (void)lchown(dest.c_str(), st.st_uid, st.st_gid);
    }
    fsync_parent_dir(dest);
    TriggerManager::instance().check_file((fs::path("/") / e.name).string());
}

/**
 * 本函数负责：一条**目录**归档条目 —— 查决策表（只可能是 `WriteDirMetadata`）、保证目录
 * 存在、刷目录本体元数据（落在 symlink→目录 上时整个元数据块跳过）。
 */
void write_dir_entry(const ContentEntry& e, const detail::ProbeLedger& ledger, detail::OpSink& sink,
                     const std::string& pkg_name)
{
    // ── 决策表：归档条目形态 = 目录 ─────────────────────────────────────────
    // 本趟对目录条目只有一个动作（`WriteDirMetadata`）：刷目录本体元数据。目录本体由
    // 让开趟（`MakeDir` 族）或下面的 `ensure_dir_exists` 建 —— 表的后置条件保证
    // `write` 非 `Unclaimed`，这里按表执行。
    // 事实同样来自让开趟的记录（目录条目的写入动作与事实无关，记录只为"每格都读同一份
    // 事实"这条不变量服务）。
    const detail::PathRecord* rec = ledger.find(e.name);
    const detail::PathFacts* facts =
        rec ? &rec->facts
            : nullptr;  // 回退路径（测试直连本函数）没有记录，见下面 record_previous 的取值
    const detail::PathDecision decision =
        detail::decide_path(rec ? rec->facts
                                : write_facts(e.name, e.physical_path, /*entry_is_dir=*/true,
                                              /*entry_is_symlink=*/false));
    if (decision.write != detail::PathAction::WriteDirMetadata) {
        throw LpkgException(string_format("error.copy_failed_rollback", e.name,
                                          e.physical_path.string(),
                                          "决策表把归档目录条目的写入动作判成了非 "
                                          "WriteDirMetadata"));
    }
    // 目录条目的物理路径带尾斜杠：先规范化（尾斜杠会让下面的 lstat/chmod 解引用链接）
    const fs::path probe = strip_trailing_slash(e.physical_path);
    bool existed = exists_follow(probe);  // 目标路径：环上不抛
    ensure_dir_exists(e.physical_path);
    // 落在 symlink→目录 上（`/lib64 -> usr/lib`、`/var/run -> ../run`）：内容要**穿过**
    // 链接写进真实目录，但目录条目的 uid/mode **不能**跟着穿过去 —— lchown/chmod 会跟随
    // 链接，把**别的包持有的**目录（如 /usr/lib）的属主/权限改成包内值（对普通用户直接
    // 变成不可读/不可执行）。链接目标与包内目录本就不是同一个对象，也谈不上"权限不一致"，
    // 故整个元数据块跳过。
    //
    // 判据一律用**剥过尾斜杠**的 `probe`：`fs::is_symlink("link/")` 会**跟随**尾斜杠
    // （实测返回 false，见 CLAUDE.md 的谓词说明），于是守卫在"链接→目录"这一格失效 ——
    // 后面两处（元数据、xattr）**都**要用 `probe`，别用 `e.physical_path`。
    struct stat st;
    if (!is_symlink_no_follow(probe) && lstat(e.src_path.c_str(), &st) == 0) {
        mode_t pkg_mode = st.st_mode & constants::PERM_MASK_ALL;
        if (existed) {
            struct stat dst_st;
            if (lstat(probe.c_str(), &dst_st) == 0) {
                mode_t cur_mode = dst_st.st_mode & constants::PERM_MASK_ALL;
                if (cur_mode != pkg_mode) {
                    log_warning(string_format("warning.dir_perm_mismatch", probe.string(),
                                              static_cast<int>(cur_mode),
                                              static_cast<int>(pkg_mode)));
                }
            }
        }
        // 元数据经 OpSink（2026-09-26）：**先把改前值写进 WAL（`DIR_META`）再动盘**。
        // 此前这两行是裸 `lchown`/`chmod`，一个 WAL 行都不写 —— 注入失败回滚后目录会保留
        // **新**版本的 mode/uid（目录是"就地改活对象"，不像普通文件那样有 BACKUP 保住旧
        // inode），与不变量 3 冲突。这不是潜伏问题：任何"新版本改了某个已存在目录的 mode"
        // 的批次失败都会踩到。
        //
        // `record_previous`：只有当这个目录**在本次事务之前就存在**时才需要记 —— 本批次
        // 刚建出来的目录由 `NEW_DIR` 的逆操作（删除）收尾，记一行改前值纯属冗余。
        // 判据取让开趟记下的**原始**事实（`disk_exists && disk_is_dir` = "让开之前它是个真
        // 目录"），而不是此刻的 `existed`（此刻它可能正是让开趟刚建出来的）。
        // 回退路径（`facts == nullptr`，只有测试直连才有）取 true：**保守地记** —— 宁可多
        // 一行冗余，不可少一行该有的。
        const bool pre_existed = facts ? (facts->disk_exists && facts->disk_is_dir) : true;
        sink.dir_meta(probe, pkg_mode, st.st_uid, st.st_gid, pre_existed,
                      "dirmeta_after_wal_" + pkg_name);
    }
    // 目录的 **xattr**：逐键经 OpSink（每键一行 WAL —— 改前有值 → `XATTR_SET`、本来没有
    // → `XATTR_NEW`），并**登记归属**（`xattrkeys.db`）。三件事都由这一处负责：
    //   · **回滚**：写入侧改的是活目录，旧值必须在行里才还原得回来；
    //   · **撤销**：升级时"本包声明过、新版本不再声明"的键要能精确撤掉 —— 判据是"这个键
    //     还有没有**别的**属主"，所以归属必须**按键**记（xattr 是按目录共用的，一个目录被
    //     多个包持有是常态）；
    //   · **边界**（与相邻元数据块同源，见上）：`symlink→目录` 整块跳过 —— xattr 写到
    //     `/var/run/` 这种链接上会落到**链接目标**（`/run`，可能是别的包持有的目录）。
    // 判据同样用 `probe`（剥过尾斜杠），理由见上。
    if (!is_symlink_no_follow(probe) && is_real_directory(probe)) {
        for (const std::string& key : list_xattr_keys(e.src_path)) {
            const auto val = read_xattr(e.src_path, key);
            if (!val) continue;  // 两次读之间被改掉（罕见）→ 不为一个空动作写 WAL 行
            if (!sink.set_xattr(probe, key, *val, "xattrset_after_wal_" + pkg_name)) continue;
            // 逻辑路径用**目录键**形态（带尾斜杠），与 files.db 的目录键同形 —— 撤销趟
            // 拿 DB 里的键直接拼真路径，两种形态混用会让它拼错。
            Cache::instance().add_xattr_key_owner((fs::path("/") / e.name).string(), key, pkg_name);
        }
    }
}

/**
 * 本函数负责：一条**普通文件**归档条目 —— `/etc` 条目的三哈希记录与决策表分流
 * （`WriteInPlace` 就地换新版 / `KeepOnDisk` 把 stash 那份搬回原位 / `WriteLpkgnew` 落
 * `<路径>.lpkgnew`），内容经 `.lpkgtmp` + WAL `COPY` 落位。
 */
void write_regular_entry(const ContentEntry& e, const detail::ProbeLedger& ledger,
                         detail::OpSink& sink, const std::string& pkg_name,
                         bool& has_config_conflicts,
                         std::vector<std::string>& silently_updated_configs)
{
    const bool is_config = e.name.starts_with(std::string(constants::DIR_ETC));
    fs::path final_dest = e.physical_path;

    // ── 事实来源：让开趟的记录（第③步）────────────────────────────────────
    // **每个归档条目**的事实都来自记录（**同一份事实**、**同一个时刻** —— 让开之前
    // 那次 probe），写入趟不再自己 probe 盘面：`decide_path` 现在只被一份一致的
    // `PathFacts` 查询，"两个时刻各查一次表"在**所有**格子上消失。
    // `/etc` 的非目录、非符号链接条目还额外带着让开趟算好的三哈希结论（`cfg`）与
    // stash 落点 —— 这里只**消费**让开趟记下的结论，不重算。
    // 没有记录 → 测试直接调用本函数的回退路径：按盘面现算（老行为）。
    const detail::PathRecord* rec = ledger.find(e.name);

    // 盘上该路径已有**非目录**物。符号链接也算"已被占用"：`fs::is_directory` 会
    // 跟随链接，指向目录的链接会被误判成"目录"，那样配置保护整段被绕过（既不备份
    // 也不留 .lpkgnew，直接替换）—— 故 `target_taken`/`target_is_dir` 显式排掉符号链接
    // （与决策表同一个口径：`disk_is_dir` 就是 lstat 语义的真目录）。
    detail::PathFacts facts = rec ? rec->facts
                                  : write_facts(e.name, e.physical_path, /*entry_is_dir=*/false,
                                                /*entry_is_symlink=*/false);
    const bool target_taken = facts.disk_exists;
    const bool target_is_dir = facts.disk_is_dir;
    // 让开趟搬走的那份（判为"保留"时要搬回原位；判为"换新版"时它随 stash 被清理）。
    // 只在 `/etc` 条目上取：记录表现在覆盖**全部**归档条目，非 `/etc` 条目让开趟也会
    // 搬（`Stash`），但那些条目的写入动作是固定的 `WriteInPlace`、从不用回搬 —— 而它们
    // 的 `bak` 本来就不记（见让开趟：只有 `/etc` 的非目录非符号链接条目才填 `bak`）。
    std::optional<fs::path> stashed_bak;
    if (is_config && rec && rec->stashed) stashed_bak = rec->bak;

    // ── 三哈希分流（pacman add.c）────────────────────────────────────────
    // 判定表只有一份：classify_config_update()。这里只负责**备料**（三份哈希）与
    // **记录**（把"这次装进去的内容"写回 DB，供下次升级当 hash_orig）。
    // 记录**只对 `/etc` 条目**成立：事实记录表现在覆盖全部归档条目，但"往 DB 里写
    // confhashes"这件事只属于配置（非 `/etc` 路径走 `rec` 也不能顺手写一条）。
    // 能在本块出现且带记录的 `/etc` 条目一定是"非目录、非符号链接"（符号链接条目
    // 在上面就 `continue` 了），因此 `wants_cfg_record` 为真、`rec->hash_pkg` 有效。
    if (is_config) {
        if (rec) {
            // 备料与判定已在让开趟做过（记录在案）→ 这里只做"记录"那一半：**永远**写
            // 包内内容的哈希。语义与下面那条分支逐字相同，只是数值来自记录。
            Cache::instance().set_conf_hash((fs::path("/") / e.name).string(), pkg_name,
                                            rec->hash_pkg);
        } else {
            const std::string logical_path = (fs::path("/") / e.name).string();
            const std::string hash_pkg = calculate_sha256(e.src_path);  // 包内那份
            // 盘上那份（hash_local）：只有**普通文件**才有可比的内容。符号链接、
            // 目录、读不到的路径 → 留空（= 无从判定 → 保守路径）。
            std::string hash_local;
            if (target_taken && !is_symlink_no_follow(e.physical_path) &&
                is_regular_file_no_follow(e.physical_path)) {
                try {
                    hash_local = calculate_sha256(e.physical_path);
                } catch (const std::exception&) {
                    hash_local.clear();
                }
            }
            auto& cache = Cache::instance();
            const std::string hash_orig = cache.get_conf_hash(logical_path, pkg_name);
            if (target_taken && !target_is_dir) {
                facts.cfg = detail::classify_config_update(hash_local, hash_orig, hash_pkg);
            }
            // 记录值（判定表之外的**另一半**语义，见 classify_config_update 的说明）：
            // 永远是**包内内容**的哈希 —— 记录只声明"这个包的这个版本提供过什么"。
            // 用户的文件**永不**被追认成我们的内容：追认它，下一次升级就满足
            // "盘上 == 旧记录"而把用户改过的配置**静默覆盖**（踩中底线）。而退化路径
            // （无旧记录 + 盘上已有这份配置）上那份往往正是**用户**的文件（无主文件撞
            // 包内文件时 `--overwrite` 是唯一合法入口），追认它就是把"用户那份"宣布成
            // 我们装的 —— 且这不是一次性的：记录有三处会按设计被删除（升级丢弃 /etc
            // 条目、remove_conf_hash、移除包），该路径重新归本包时"追认"会被重新武装。
            // 代价（有意为之）：老 DB 的配置在用户把 `.lpkgnew` **合并进**盘上那份之前，
            // 包每改一次配置都会再落一份**可见的** `.lpkgnew`（吵，但绝不静默）；合并后
            // 盘上 == 记录，回到正常分流。判定表本身不受影响：无记录时仍是"两份一致 →
            // 保留原文件、不一致 → 落 `.lpkgnew` + 告警"，用户可见行为不变。
            cache.set_conf_hash(logical_path, pkg_name, hash_pkg);
        }
    }

    // ── 决策表（唯一决策点）：本趟对这个路径做什么 ─────────────────────────
    // 表的三条结论直接对应 pacman add.c 的三条分支：落原位（①）/ 保留不碰盘（②）/
    // 落 `.lpkgnew`（③）。`is_config && target_taken && !target_is_dir` 是"三哈希参与"
    // 的入口条件 —— 表内部按同一条件分流（非 `/etc`、盘上没被占、盘上是真目录
    // 都落到 `WriteInPlace`）。有记录时 `facts.cfg` 已由让开趟补齐（上面的分支不覆盖它）。
    const detail::PathDecision decision = detail::decide_path(facts);

    switch (decision.write) {
        case detail::PathAction::WriteInPlace: {
            // ① 盘上 == 上次装进去的（用户没改过）→ 静默换新版。**会真的改盘**，
            // 所以先把盘上那份搬进 stash（BACKUP，可回滚）：批次失败时 reverse_execute
            // 把它 rename 回原位，静默替换不留"改得动、撤不回"的缺口。
            // （提交后它随 stash 清理掉是预期的：用户没改过，没有要保留的内容；
            //   "保留"是移除侧 .lpkgsave 的语义。）
            // 第③步之后 `/etc` 的这一格由**让开趟**搬（它搬完才好读 stash 副本算
            // `hash_local`），所以这里**不许**再搬一次：盘上此刻已是空的，再搬就是
            // 对着空路径写 BACKUP 行 + rename ENOENT。有无记录是唯一判据。
            if (is_config && target_taken && !target_is_dir) {
                // 让开趟**按表**清过路了（`Stash` = 三哈希要读 stash 副本；`SaveConfig` =
                // 类型变化已把原物改名 `.lpkgsave`）—— **有记录即已执行**，这里不得再动盘：
                // 对着已清空的路径再 `backup` 就是写一行 BACKUP 行 + rename ENOENT。
                // 只有**回退路径**（无记录 ⇒ 压根没有让开趟）才补执行一次表的让开动作。
                if (!rec) {
                    if (decision.let_go == detail::PathAction::SaveConfig)
                        sink.save_config(e.physical_path, "conf_replace_after_wal_" + pkg_name);
                    else
                        sink.backup(e.physical_path, "conf_replace_after_wal_" + pkg_name);
                }
                // 降噪：此处**不打日志**（逐配置文件一行会是几百行）。记账，循环结束后按包
                // 聚合一行 —— 条数与完整路径都在那一行里（见下）。
                // **只有三哈希那条路**（`Stash`：用户没改过 → 静默换新版、不留任何副本）才计入；
                // 类型变化那条路把原物留成了 `.lpkgsave`，不是"静默"（用户能拿到那份）。
                if (decision.let_go == detail::PathAction::Stash)
                    silently_updated_configs.push_back(e.physical_path.string());
            }
            // WAL: COPY <tmp> → <dst> (write-ahead: WAL 先于 rename)
            // （断点位于 write-ahead 窗口内，只能在 sink 里命中）
            const fs::path tmp_path = stage_regular_file(e.src_path, final_dest);
            sink.commit_copy(tmp_path, final_dest, "copy_after_wal_" + pkg_name);
            break;
        }
        case detail::PathAction::KeepOnDisk:
            // ② 包本身没改这个配置（旧记录 == 新包）→ 保留用户文件，**连
            // .lpkgnew 都不产生**：没有"新东西"要给用户审阅，产生它只会让 /etc
            // 越堆越多。
            // 第③步之后"完全不碰盘"变成"先搬走、再搬回"：让开趟为了读 `hash_local`
            // 已经把盘上那份搬进了 stash，判定为"保留"就必须**搬回原位**
            // （`un_stash` 原语，见 `ARCH.md` §9.2 的 `UNSTASH` 行）。可观测行为不变：rename 不改
            // inode， 内容/属主/权限/xattr/硬链接关系逐字不变。没搬过（回退路径）→ 不动盘。
            if (stashed_bak)
                sink.un_stash(*stashed_bak, e.physical_path, "unstash_after_wal_" + pkg_name);
            break;
        case detail::PathAction::WriteLpkgnew: {
            // ③ 三者互异（含"无从判定"）→ 新版落 .lpkgnew + 告警（原行为）
            final_dest += std::string(constants::SUFFIX_LPKG_NEW);
            log_warning(string_format("warning.config_conflict", e.physical_path.string(),
                                      final_dest.string()));
            has_config_conflicts = true;
            // 同上：原位那份是"用户改过、必须留住"的配置 —— 先把它从 stash 搬回原位，
            // 再落 `.lpkgnew`。**顺序不可反**：UNSTASH 行必须在该路径后续的
            // BACKUP/COPY 之前（那些行针对的是 `.lpkgnew` 这个**另一个**路径），
            // 回滚逆序才会"先撤 `.lpkgnew`、再撤 UNSTASH、最后撤让开趟的 BACKUP"。
            if (stashed_bak)
                sink.un_stash(*stashed_bak, e.physical_path, "unstash_after_wal_" + pkg_name);
            // "请审阅"文件也**必须能回滚**：落 .lpkgnew 同样是"这个批次改了盘面"，
            // 只是改的不是配置本身。裸 fs::copy 是事务外的副作用 —— 批次回滚后包
            // 没装上、配置也退回了批次前，却平白多出一份（还盖掉了上一次留下的）
            // "请审阅"副本，盘面与 WAL 描述的世界不一致。故与落原位分支走**同一套
            // 写入层原语**：内容先写进 .lpkgtmp（+ xattr + 属主/权限 + fsync），再
            // commit_copy 落位（WAL COPY）。
            const fs::path tmp_path = stage_regular_file(e.src_path, final_dest);
            // 目标已存在（上一次留下的 .lpkgnew，用户可能还没审阅）→ 先 BACKUP 进
            // stash 再落新的：回滚时它是"把旧那份 rename 回来"，而不是连带删掉
            // 一份与本批次无关、用户尚未处理的审阅文件（顺序也不可反：BACKUP 行
            // 必须先于 COPY 行，逆序回滚才会"先撤新那份、再还原旧那份"）。
            if (exists_no_follow(final_dest)) sink.backup(final_dest);
            sink.commit_copy(tmp_path, final_dest);
            break;
        }
        default:
            // 其余动作都不归"写入趟"（决策表保证归档非目录条目的 `write` 只可能是
            // 上面三个之一，见 decide_path 的后置条件）
            throw LpkgException(string_format("error.copy_failed_rollback", e.name,
                                              e.physical_path.string(),
                                              "决策表给归档非目录条目的写入动作不是 "
                                              "WriteInPlace/KeepOnDisk/WriteLpkgnew"));
    }

    TriggerManager::instance().check_file((fs::path("/") / e.name).string());
}

}  // namespace

void InstallationTask::copy_package_files()
{
    log_info(string_format("info.copying_files", pkg_name_));
    // 写入层原语：COPY 的 WAL 行与 rename 成对发生（见 op_sink.hpp）
    detail::OpSink sink(pkg_name_, &stashes_);
    const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
    auto files = detail::scan_content_files(content_dir);
    // ① 分支（用户没改过 → 静默换新版）本次碰到的配置路径：**只记账，循环结束后按包
    // 聚合一行**。逐配置文件打日志在真实场景里是数量级的：`lpkg upgrade` 一次几百个包、
    // 每个包几个 /etc 条目 ⇒ 光这一句就刷几百行（pacman 对静默替换一行都不打）。
    std::vector<std::string> silently_updated_configs;

    for (const auto& f : files) {
        if (on_before_file_copy) on_before_file_copy();

        if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

        fs::path rel_f = f;
        if (rel_f.is_absolute()) rel_f = rel_f.relative_path();
        const fs::path src_path = content_dir / f;
        const fs::path physical_path = Config::instance().root_dir() / rel_f;

        // 判定走**不抛**的 `exists_no_follow`（lstat 语义 = 原来的 `exists || is_symlink`）：
        // `src_path` 是**包内内容**，包自己的 `content/` 里带符号链接环时 `fs::exists` 会抛
        // `filesystem error: status: Symbolic link loop [...]` —— 自带环的包因此根本装不上
        // （扫描那一关已修，这一关曾是下一个炸点）。见
        // tests/integration/test_symlink_loop_install.cpp。
        if (!exists_no_follow(src_path)) continue;

        fs::path parent = physical_path.parent_path();
        std::vector<fs::path> to_create;
        // 同样是**包要落位的目标路径**（不是 lpkg 自己的命名空间）：父链上出现环时
        // `fs::exists` 会抛。用**跟随语义**的不抛版（与 `fs::exists` 逐字同义）——
        // 解不开的父目录在这里判"不存在"，交给 ensure_dir_exists 去报一个点名的
        // LpkgException，而不是让 std::filesystem 的原始异常穿出去。
        while (!parent.empty() && !exists_follow(parent)) {
            to_create.push_back(parent);
            if (parent == Config::instance().root_dir()) break;
            parent = parent.parent_path();
        }
        for (const auto& d : to_create | std::views::reverse) {
            ensure_dir_exists(d);
        }

        const ContentEntry entry{f, src_path, physical_path};

        if (is_symlink_no_follow(src_path)) {
            write_symlink_entry(entry, probe_ledger_, sink, pkg_name_, has_config_conflicts_);
            continue;
        }

        // 包内内容：`fs::is_directory` 对环抛（上面 is_symlink 分支已经接住了环，这里换成
        // 不抛形态只是为了"凡输入来自包内容/包在盘上路径的判定一律不抛"这条规则不留例外）。
        if (is_directory_follow(src_path)) {
            write_dir_entry(entry, probe_ledger_, sink, pkg_name_);
            continue;
        }

        try {
            write_regular_entry(entry, probe_ledger_, sink, pkg_name_, has_config_conflicts_,
                                silently_updated_configs);
        } catch (const std::exception& e) {
            throw LpkgException(
                string_format("error.copy_failed_rollback", f, physical_path.string(), e.what()));
        }
    }

    // ① 分支的降噪出口：每包**一行**（条数 + 完整路径清单）。信息一点没丢 —— 用户仍然
    // 查得到"到底哪些配置被静默换了"，只是不再一个文件占一行。放在循环之后（而不是每
    // 次碰到就打印）是聚合的前提；批次中途失败时这一行不会打，但那时整批回滚、盘面回到
    // 批次前，没有"静默替换"可言。
    //
    // 让开趟的记录到此消费完毕（本包只有这两趟读它）→ 释放，别让长批次里逐包累积。
    probe_ledger_.clear();
    if (!silently_updated_configs.empty()) {
        std::string joined;
        for (const auto& p : silently_updated_configs) {
            if (!joined.empty()) joined += ", ";
            joined += p;
        }
        log_info(
            string_format("info.config_updated_batch", silently_updated_configs.size(), joined));
    }
    if (has_config_conflicts_) log_warning(get_string("info.config_review_reminder"));
}
