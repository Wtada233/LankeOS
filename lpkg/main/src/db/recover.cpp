/**
 * recover.cpp — WAL 恢复与清理
 *
 * recover_packages(): 紧急恢复 — 仅在进程因崩溃（断电/OOM/SIGKILL）
 * 而未能执行 catch 中的 batch_rollback() 时使用。
 *
 * trim_completed(): 清理已完成批次的 WAL 日志行，释放磁盘空间。
 */

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <set>
#include <vector>

#include "base/constants.hpp"
#include "base/utils.hpp"
#include "cache.hpp"
#include "config/config.hpp"
#include "i18n/localization.hpp"
#include "transaction_log.hpp"
#include "wal_op.hpp"

namespace fs = std::filesystem;

namespace wal
{

std::set<fs::path> referenced_stash_roots()
{
    std::set<fs::path> roots;
    std::ifstream file(wal_log_path());
    if (!file.is_open()) return roots;
    std::string line;
    while (std::getline(file, line)) {
        // 与 read_wal_lines 同一口径剥 `\r`（这里是手写读取，此前漏了）：CRLF 的 WAL 会让
        // op.arg2/arg1 尾上粘一个 `\r`，算出来的 stash 根带 `\r` ⇒ 用这个集合做 confinement
        // 白名单 / cleanup_orphan_stashes 的 keep 集时会漏保护真正的 stash 根。
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto op = parse_op(line);
        if (!op.is_valid()) continue;
        if ((op.type == WALOpType::BACKUP || op.type == WALOpType::REMOVE_OLD) && !op.arg2.empty())
            roots.insert(stash_root_of_bak(op.arg2).lexically_normal());
        // `UNSTASH <bak> → <orig>`：**stash 侧是 arg1**（与 BACKUP 相反 —— BACKUP 的 arg1 是
        // 原位、arg2 是 stash 侧）。实践上它总与配对的 BACKUP 同根、已被上面那行收集，
        // 所以这里是**冗余**的；但"WAL 仍引用哪些 stash 根"这个集合的定义应该是"凡是 WAL 里
        // 出现过的 stash 侧路径"，不该依赖"总有配对的 BACKUP"这条论证 —— 机制 > 论证。
        // `UNSTASH` 与 `CLEANUP` 的 stash 侧**都是 arg1**，动作也逐字相同 ⇒ 合成一支。
        // （不是"长得像就合并"：两支的判据与动作完全一致，分开写只会让 clang-tidy 报
        //   branch-clone。合并后与上面那段"凡是 WAL 里出现过的 stash 侧路径"的定义一致。）
        else if (!op.arg1.empty() &&
                 (op.type == WALOpType::UNSTASH || op.type == WALOpType::CLEANUP))
            roots.insert(stash_root_of_bak(op.arg1).lexically_normal());
    }
    return roots;
}

}  // namespace wal

// ============================================================================
// 共用小工具：读 WAL / 批次配对扫描
// ============================================================================

/**
 * 读 WAL 的全部行（丢弃空行与行尾 \r）。
 *
 * 返回 nullopt = "没有可处理的日志"（路径不存在 / 打不开）；返回**空** vector = "文件存在但
 * 为空"。两者不能合并成一个返回值：trim_completed 对空文件要 remove、对"打不开"只是返回，
 * 而 recover_packages 对两者都直接返回。
 *
 * 判定不抛：WAL 路径被符号链接环占着时 `fs::exists` 会抛 —— 而这是在**崩溃恢复**的入口上，
 * 抛出去等于恢复永远跑不完（见 base/path_predicates.hpp 的谓词说明）。
 */
static std::optional<std::vector<std::string>> read_wal_lines(const std::string& wpath)
{
    if (!exists_follow(wpath)) return std::nullopt;

    std::ifstream file(wpath);
    if (!file.is_open()) return std::nullopt;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        // 先剥 `\r` 再判空：反过来的话，一行只有 "\r" 会被"非空"放行、剥完变成空串推进去
        // （下游按 INVALID 丢弃，`rewrite_wal_tail` 还会把空行写回去）。
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        lines.push_back(line);
    }
    return lines;
}

/**
 * WAL 的批次配对情况 —— 前向 depth 记账的唯一实现。
 *
 * 只有这里定义"什么是未提交区域"，其余各处都读它的结果。
 *
 * "未提交区域起点"必须是**第一个**未配对 BEGIN_PKGS，而不是最后一个：一次崩溃可能留下
 * **两个**未提交批次（前一批回滚自身失败后同进程又开了新批次），按"最后一个"裁剪/回滚会让
 * 更早那批的 BEGIN/BACKUP 行被当成已完成内容处理 —— 恢复依据就此消失；而且已提交批次的行
 * 仍留在 WAL 里、下一轮扫描的起点仍落在它上面，更早那批永远轮不到（两轮下来文件一次都
 * 没被还原）。配对成功的批次（depth 回到 0）即清空起点。
 */
struct BatchPairing {
    ssize_t unpaired_begin = -1;  // **第一个**未配对 BEGIN_PKGS 的行号；-1 = 无未提交区域
    ssize_t last_commit = -1;     // **最后一个** COMMIT_PKGS 的行号；-1 = 一条都没有
    size_t unpaired_count = 0;    // 扫描结束时仍开着的批次数（= 需要补写的 COMMIT 条数）
    // `committed_line[i] == true` ⇒ 第 i 行属于一个**已经配对提交**的批次。
    //
    // 它只在一种形状下非空：已提交批次**嵌套**在更早的未提交批次里
    // （`BEGIN₁ …(未封口) BEGIN₂ … COMMIT₂`）。这种形状下，未提交区域 `[unpaired_begin, EOF)`
    // 会**把已提交的那一批包进去**，而整段回滚会把上一轮"明明装成功并提交了"的成果一并撤销
    // （详见 `rollback_uncommitted_region`）。回滚必须**跳过**这些行。
    //
    // 这种形状从哪来：`rollback_uncommitted_region` 在"有撤销动作真的没成功"时**故意不 seal**
    // （留给下次 rec 重做），而 `init_database_for` 不返回恢复成败、同进程继续执行用户命令 ⇒
    // 新批次在未封口的 WAL 上开了。`run_batch_transaction` 的入口守卫堵住了这条
    // 产生路径；这里保留跳过逻辑，是为了**已有现场**（旧二进制留下的 WAL）也不被误回滚。
    std::vector<bool> committed_line;

    bool is_committed_line(size_t i) const
    {
        return i < committed_line.size() && committed_line[i];
    }
};

static BatchPairing scan_batch_pairing(const std::vector<std::string>& lines)
{
    BatchPairing pairing;
    pairing.committed_line.assign(lines.size(), false);
    // 尚未配对的 BEGIN_PKGS 行号，LIFO —— 配对语义是"COMMIT 关掉**最近一个**开着的批次"。
    std::vector<size_t> open_begins;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto op = wal::parse_op(lines[i]);
        if (!op.is_valid()) continue;
        if (op.type == wal::WALOpType::BEGIN_PKGS) {
            if (open_begins.empty()) pairing.unpaired_begin = static_cast<ssize_t>(i);
            open_begins.push_back(i);
        } else if (op.type == wal::WALOpType::COMMIT_PKGS) {
            pairing.last_commit = static_cast<ssize_t>(i);
            if (open_begins.empty()) continue;  // 多余的 COMMIT：不配对任何东西（与过去一致）
            const size_t begin = open_begins.back();
            open_begins.pop_back();
            if (!open_begins.empty()) {
                // 刚被提交的这一批**嵌套在**更早的未提交批次里 ⇒ 它的行不得被回滚。
                for (size_t j = begin; j <= i; ++j) pairing.committed_line[j] = true;
            }
            if (open_begins.empty()) pairing.unpaired_begin = -1;  // 该批次已配对提交
        }
    }
    pairing.unpaired_count = open_begins.size();
    return pairing;
}

// ============================================================================
// 续传 post-commit 清理
// ============================================================================

/**
 * 删除"已提交批次"残留的 .lpkg_bak（post-commit 清理的崩溃续传）。
 *
 * 背景：install/upgrade 的 .lpkg_bak 清理发生在 COMMIT_PKGS 之后（I-BAK-2），
 * 通过 cleanup_baks 写 CLEANUP 行（write-ahead）。若崩溃在"COMMIT 之后、
 * 清理完成之前"，磁盘上残留 bak，其记录（BACKUP/CLEANUP 行）仍在 WAL 中——
 * 本函数据此续删，保证随后 trim 能安全地把完成事务连同清理记录一起清掉，
 * 不再出现孤儿 .lpkg_bak（曾因 install 清理裸 fs::remove 无 WAL 且 trim 清空
 * 整个文件，导致残留永远留在磁盘）。
 *
 * 只处理"已提交区域"（**第一个**未配对 BEGIN_PKGS 之前）的 BACKUP/REMOVE_OLD dst，
 * 以及尾部 post-commit CLEANUP 行引用的 bak；绝不动未提交批次的 bak
 * （那是 reverse_execute 回滚要用的）。
 */
static void continue_post_commit_cleanup(const std::vector<std::string>& lines)
{
    // 1. 定位：**第一个**未配对 BEGIN_PKGS（未提交区域起点）与最后一个 COMMIT_PKGS。
    //
    //    夹在两个未提交批次之间的 bak 属于**还没还原**的前一批，若按"已提交区域"删掉就是
    //    永久丢文件；配对记账的语义见 scan_batch_pairing 的说明。
    const BatchPairing pairing = scan_batch_pairing(lines);
    const ssize_t unpaired = pairing.unpaired_begin;
    const ssize_t last_commit = pairing.last_commit;

    // 2. 收集 bak：
    //    - 已提交区域（[0, unpaired) 或全部）的 BACKUP/REMOVE_OLD dst；
    //    - post-commit 尾部（最后一个 COMMIT_PKGS 之后、未提交批次之前）的
    //      CLEANUP 行引用的 bak。未提交批次内的东西一个都不碰（回滚要用）。
    std::vector<fs::path> baks;
    const size_t region_end = (unpaired < 0) ? lines.size() : static_cast<size_t>(unpaired);
    for (size_t i = 0; i < region_end; ++i) {
        auto op = wal::parse_op(lines[i]);
        if (!op.is_valid()) continue;
        if ((op.type == wal::WALOpType::BACKUP || op.type == wal::WALOpType::REMOVE_OLD) &&
            !op.arg2.empty())
            baks.push_back(op.arg2);
    }
    const size_t tail_start = (last_commit < 0) ? 0 : static_cast<size_t>(last_commit) + 1;
    for (size_t i = tail_start; i < region_end; ++i) {
        auto op = wal::parse_op(lines[i]);
        if (!op.is_valid()) continue;
        if (op.type == wal::WALOpType::CLEANUP && !op.arg1.empty()) baks.push_back(op.arg1);
    }
    if (baks.empty()) return;

    // 3. 归一为 stash 根再整目录 remove_all（文件备份在 stash 内；CLEANUP 行即 stash 根）
    std::vector<fs::path> roots;
    roots.reserve(baks.size());
    for (auto& bak : baks) {
        roots.push_back(wal::stash_root_of_bak(bak));
    }
    std::ranges::sort(roots);
    auto last = std::unique(roots.begin(), roots.end());
    roots.erase(last, roots.end());

    // 4. 删除仍存在的 stash 根（幂等：已删的跳过）
    for (const auto& root : roots) {
        // **只删真正的 stash 根**（名字以 `.lpkg_bak_` 开头）—— 纵深防御。
        //
        // 合法行（BACKUP/REMOVE_OLD 的 stash 侧、CLEANUP 行）引用的**永远**是 lpkg 自己造的
        // `<某层>/.lpkg_bak_<pkg>_<pid>` 目录（`install_common.cpp`）—— `stash_root_of_bak`
        // 也正是靠"父目录名以 `.lpkg_bak_` 开头"判它是不是 stash 根。而被篡改/损坏的 WAL 里
        // 一条 `CLEANUP /etc` 会让它**原样返回**那个路径（父目录名不匹配），于是这里就
        // `remove_all("/etc")` —— 这条清理路径**不经过** `reverse_execute` 的 confinement，
        // 它是另一个 WAL 消费者（见 test_wal_confinement.cpp 的
        // PostCommitCleanupDoesNotRemoveNonStashTargets）。
        //
        // 判据用**名字**而不是"落在 root 内"：生产形态 `root=="/"` 下包含判定**恒真**、拦不住
        // 任何东西（那正是最该防的形态），而名字判据与 root 无关。名字不匹配 → 跳过并告警，
        // **绝不抛**（恢复路径上的判定不能有能力打断事务）。
        if (!is_stash_dir_name(root.filename().string())) {
            log_warning(string_format("warning.cleanup_skipped_not_stash", root.string()));
            continue;
        }
        // lstat 语义：**任何**占着这个名字的东西（含悬空/自环符号链接）都要 remove_all ——
        // 原来写成 `fs::exists || fs::is_symlink`，那在符号链接环上会抛 filesystem_error
        // （见 base/path_predicates.hpp 的谓词说明），把 post-commit 清理打断。
        if (!exists_no_follow(root)) continue;
        std::error_code ec;
        fs::remove_all(root, ec);
        if (ec) log_warning(string_format("warning.cleanup_failed", root.string()));
    }
}

// ============================================================================
// recover_packages — 断电/崩溃恢复
// ============================================================================

/** 批次里第一个包级 BEGIN/RM_BEGIN 的包名（告警定位用）；没有则空串 */
static std::string first_package_of(const std::vector<wal::WALOp>& ops)
{
    for (const auto& op : ops) {
        if ((op.type == wal::WALOpType::BEGIN || op.type == wal::WALOpType::RM_BEGIN) &&
            !op.arg1.empty())
            return op.arg1;
    }
    return {};
}

/**
 * 反序回滚整段未提交区域 [region_start, EOF)，并做该段的收尾（stash 收尸 / 重载 Cache /
 * 写 COMMIT_PKGS）。返回"是否回滚失败"——调用方据此决定要不要跳过 DB 备份清理。
 *
 * 区域是**一整段**而不是"每批一次"：CLEANUP 只可能出现在事务之外（ARCH.md §11.3，旧版
 * 二进制写入的"批次内 CLEANUP"形状不在支持范围），故未提交批次一律整体逆序回滚。
 * **回滚失败不得卡死整个恢复**：告警、保持现场（WAL 与备份原样保留，下次 rec 可重试），
 * 并把失败上报给调用方；此后不清理 DB 备份（那是重试的依据）。
 */
static bool rollback_uncommitted_region(const std::vector<std::string>& lines, size_t region_start,
                                        const BatchPairing& pairing)
{
    // a) 解析操作行
    //    **跳过"落在未提交区域之内、但属于已提交批次"的行**（嵌套提交形状，见 BatchPairing
    //    的 `committed_line` 说明）：它们的成果已经提交过，反过来执行 = 静默撤销上一轮已完成的
    //    安装。未提交区域 [region_start, EOF) 是按"第一个未配对 BEGIN"划的，它并不**等于**
    //    "全都是未提交的行" —— 这条跳过就是那个差集。
    std::vector<wal::WALOp> ops;
    for (size_t i = region_start; i < lines.size(); ++i) {
        if (pairing.is_committed_line(i)) continue;
        auto op = wal::parse_op(lines[i]);
        if (op.is_valid()) ops.push_back(op);
    }
    if (ops.empty()) return false;

    const std::string first_pkg = first_package_of(ops);
    wal::RollbackStats stats;
    try {
        stats = wal::reverse_execute(ops, true);
    } catch (const std::exception& e) {
        log_warning(string_format("warning.rollback_remove_failed", first_pkg, e.what()) + " " +
                    get_string("warning.rec_kept_uncommitted"));
        return true;
    }

    if (stats.failures > 0) {
        // 逆向执行跑完了，但**有若干行的撤销动作真的没成功** ⇒ **绝不 seal**
        // （不 `purge_consumed_stashes`、不 `commit_batch`）：让这一段保持"未提交"，
        // 下次 `lpkg rec` 会**幂等重做**，那些没撤掉的还有机会。
        // 这与上面 catch 分支的处置同源 —— 那里也是 `return true`（保持未提交）。
        // 告警已由 `reverse_execute` 统一发出。
        return true;
    }
    wal::purge_consumed_stashes(ops);  // stash 里的文件已还原 → 清空 stash 根
    // 容忍"pkgs/holdpkgs 不存在"：reverse_execute 已把能从备份还原的库都还原了，这里再
    // 撞上一个缺失的库就说明它连备份都没有（真的丢了）—— 一条缺失记录不该把整个恢复
    // 作废（stash 收尸、WAL 收尾、备份清理都还要做），但它**不静默**：load 会逐个告警；
    // 正常操作路径（install/remove 入口的 Cache::load()）仍是硬错误。
    Cache::instance().load(/*tolerate_missing_set_files=*/true);
    // seal：**补足**与未配对 BEGIN_PKGS **条数相等**的 COMMIT_PKGS。
    // 只写一条是错的 —— WAL 里有两个未配对 BEGIN 时（前一批回滚失败后同进程又开了新批），
    // 一条 COMMIT 让 depth 停在 1 ⇒ "未提交区域"永远存在：下一轮恢复把整段再回滚一遍，
    // `trim_completed` 永不裁剪、WAL 无限增长。配对的语义是"一条 COMMIT 关掉一批"。
    for (size_t n = 0; n < pairing.unpaired_count; ++n) wal::commit_batch();
    return false;
}

void recover_packages()
{
    // 0. 读取所有行。"没有可处理的日志"（路径不存在/打不开/为空）→ 没有任何事要做。
    auto maybe_lines = read_wal_lines(wal::wal_log_path());
    if (!maybe_lines || maybe_lines->empty()) return;
    const std::vector<std::string>& lines = *maybe_lines;

    // 1. 续传 post-commit 清理：删除已提交批次残留的 .lpkg_bak。
    //     必须在处理未提交批次之前做，且必须早于任何 trim——一旦 trim 把完成事务
    //     连同其 BACKUP 行清掉，残留 bak 的来源记录就没了。
    continue_post_commit_cleanup(lines);

    // 2. 状态机扫描：未提交区域 = [**第一个**未配对 BEGIN_PKGS, EOF)
    //    （BEGIN_PKGS 开批、COMMIT_PKGS 配对；扫描语义见 scan_batch_pairing）
    const BatchPairing pairing = scan_batch_pairing(lines);
    const ssize_t unpaired_begin = pairing.unpaired_begin;

    if (unpaired_begin < 0) {
        // 没有未完成的批次，清理整个日志。
        // 同时清理孤儿 .lpkg_db_bak_before：已提交批次崩溃在"COMMIT_PKGS 之后、
        // post-batch cleanup 之前"时留下的备份，此处一并清掉（启动时无活动批次，
        // DBLock 保证单进程，安全）。
        trim_completed();
        cleanup_db_backups();
        return;
    }

    // 3. 一次逆序回滚整段未提交区域（跳过其中属于**已提交**批次的行，见 BatchPairing）
    const bool any_batch_failed =
        rollback_uncommitted_region(lines, static_cast<size_t>(unpaired_begin), pairing);

    // 4. 清理残留的 .lpkg_db_bak_before:* 备份文件。
    //    有批次恢复失败时**跳过**：那些备份是重试还原 DB 的唯一依据。
    if (!any_batch_failed) cleanup_db_backups();
}

// ============================================================================
// trim_completed — 清理已完成的 WAL 条目
// ============================================================================

/**
 * "post-commit 清理还没做完" 的判据（I-BAK-2 的 CLEANUP 行写在事务之外；已提交批次的
 * .lpkg_bak 也可能因清理失败/崩溃仍未删除）。
 *
 * trim 只清理"完全完成"的内容：只要有已提交批次的 BACKUP/REMOVE_OLD 目标、或 CLEANUP 行
 * 引用的 bak **仍占着盘上那个名字**，就返回 true（调用方据此**保留整个 WAL**：BACKUP 上下文
 * 还在，recover 才能续传），只有 bak 全删干净了才 false。
 *
 * lstat 语义（exists_no_follow），**不是** `fs::exists || fs::is_symlink`：行里的 bak 路径
 * 可能正是**符号链接环**（原路径是环，被 rename 进 stash 后依然是个环）—— 那条写法在它上面
 * 抛 filesystem_error，于是**每次启动的 trim 都失败**、WAL 永远裁剪不掉（见
 * base/path_predicates.hpp 的谓词说明）。判据本身不变。
 */
static bool post_commit_cleanup_pending(const std::vector<std::string>& lines)
{
    for (const auto& line : lines) {
        auto op = wal::parse_op(line);
        if (!op.is_valid()) continue;
        if ((op.type == wal::WALOpType::BACKUP || op.type == wal::WALOpType::REMOVE_OLD) &&
            !op.arg2.empty() && exists_no_follow(op.arg2))
            return true;
        if (op.type == wal::WALOpType::CLEANUP && !op.arg1.empty() && exists_no_follow(op.arg1))
            return true;
    }
    return false;
}

/**
 * 把 [keep_from, EOF) 的行重写回 WAL，丢掉前面的已完成批次。
 *
 * 保留下来的未提交批次是恢复数据，必须先 fsync 再 rename（I-FSYNC-5：write 用 .tmp + fsync
 * + rename），否则断电可能丢失恢复点。
 */
static void rewrite_wal_tail(const std::string& wpath, const std::vector<std::string>& lines,
                             size_t keep_from)
{
    std::string tmp_path = wpath + ".trim_tmp";
    {
        std::ofstream out(tmp_path);
        for (size_t i = keep_from; i < lines.size(); ++i) {
            out << lines[i] << "\n";
        }
        out.flush();
        if (!out) throw LpkgException(string_format("error.db_write_failed", tmp_path));
    }
    {
        int fd = ::open(tmp_path.c_str(), O_WRONLY);
        // 打开失败或 fsync 失败都必须中止：否则 safe_rename 会把这个**没落盘**的截断 WAL
        // 当成新 WAL 覆盖上去 —— 被 trim 掉的行就此静默消失（原来两处都忽略返回值）。
        if (fd < 0) throw LpkgException(string_format("error.open_file_failed", tmp_path));
        if (::fsync(fd) != 0) {
            ::close(fd);
            throw LpkgException(string_format("error.wal_fsync_failed", tmp_path));
        }
        ::close(fd);
    }

    safe_rename(tmp_path, wpath);
}

void trim_completed()
{
    const std::string wpath = wal::wal_log_path();
    auto maybe_lines = read_wal_lines(wpath);  // 判定不抛；见 base/path_predicates.hpp 的谓词说明
    if (!maybe_lines) return;

    const std::vector<std::string>& lines = *maybe_lines;
    if (lines.empty()) {
        // 空文件 → 删除（删不掉就留待下次，但**要出声**，别静默）
        std::error_code ec;
        fs::remove(wpath, ec);
        if (ec) log_warning(string_format("warning.cleanup_failed", wpath));
        return;
    }

    // 找**第一个**未配对 BEGIN_PKGS 的位置（= 需保留区域的起点）。
    // 语义（含"为什么必须是第一个"）与 continue_post_commit_cleanup、recover_packages
    // 完全一致 —— 三处共用 scan_batch_pairing 这一份记账。
    const ssize_t unpaired_begin = scan_batch_pairing(lines).unpaired_begin;

    if (unpaired_begin < 0) {
        // 所有事务都已提交 —— 但**不无条件清空**：先确认 post-commit 清理没有留下未删的 bak。
        if (post_commit_cleanup_pending(lines))
            return;  // 清理未完成 → 保留整个文件，等 recover 续传
        // 清理完成 → 清空整个日志文件（全是完成事务/历史清理记录）。
        // 必须检查流状态：ENOSPC/EIO 时 WAL **实际没被清空**，原先丢弃结果会把这种失败
        // 当成功 —— 完成批次的记录一直留着（非致命，但 trim 会每次都白跑一遍）。失败按
        // 上方空文件分支的 fs::remove 失败同样方式告警，不静默。
        std::ofstream ofs(wpath, std::ios::trunc);
        ofs.close();
        if (!ofs) log_warning(string_format("warning.cleanup_failed", wpath));
        return;
    }

    // 保留从 unpaired_begin 开始的所有行
    if (unpaired_begin == 0) {
        return;  // 没有需要清理的已完成批次
    }
    rewrite_wal_tail(wpath, lines, static_cast<size_t>(unpaired_begin));
}

// ============================================================================
// cleanup_db_backups — 清理孤立的 .lpkg_db_bak_before:* 文件
// ============================================================================

/**
 * WAL 里是否还留着未配对的 BEGIN_PKGS（= 存在未提交批次）。
 *
 * **公开**（声明在 `cache.hpp`）：除 `cleanup_db_backups()` 用它当"别删还原点"的门控外，
 * `run_batch_transaction()` 也用它当**入口守卫** —— 未封口的 WAL 上不许再开新批次
 * （否则会造出"已提交批次嵌套在未提交批次里"的形状，见 `scan_batch_pairing`）。
 *
 * 读不到 WAL 文件时保守返回 true（"有"）——宁可让备份多留一会儿，也不误删唯一还原点。
 * 文件不存在 / 为空则明确是"没有"（正常路径：trim 之后）。
 *
 * 配对判定复用 scan_batch_pairing（唯一实现），**行读取与规范化也复用 read_wal_lines**
 * （同一个唯一实现）。
 *
 * ⚠️ **行必须剥 `\r`** —— 不剥给出的答案落在**危险的那一侧**：
 *
 *   · `BEGIN_PKGS` / `COMMIT_PKGS` 是**裸行**（`begin_batch()` → `w.log("BEGIN_PKGS")`、
 *     `commit_batch()` → `log_wal_line("COMMIT_PKGS")`，**不带任何载荷**）。
 *   · 于是 CRLF 行的 `\r` 粘在**类型 token** 上：`parse_op("BEGIN_PKGS\r")` 先按第一个空格切
 *     类型（这里压根没有空格）→ `walop_type_from_name("BEGIN_PKGS\r")` 未知 → **INVALID**
 *     （已有用例 `test_wal_core.cpp::ParseOpWithTrailingWhitespace` 钉住这条）。
 *   · 配对扫描只认 BEGIN/COMMIT 类型 ⇒ 整份 CRLF WAL 里**一个批次都看不见** ⇒
 *     `unpaired_begin = -1` ⇒ 守卫判"没有未提交批次" ⇒ **把唯一还原点删光**。
 *     而同一份 WAL 上其余三个消费者（recover / trim / continue_post_commit_cleanup）走
 *     read_wal_lines、剥了 \r，看到的是"有未提交批次" —— 同一份输入，两个相反的答案。
 *
 * 可达性：lpkg 只写 `\n`（`log_wal_line` 恒拼 `"\n"`）⇒ 今天不可达。但守卫存在的**全部意义**
 * 就是"拿不准时偏保守"，让它在一份被外部工具/编辑器改成 CRLF 的 WAL 上反转成"删备份"，
 * 与它的既定偏向正好相反。剥 `\r` 只会让它**更保守**（多留备份），同向。
 *
 * 注意：仅当类型后面**没有载荷**时 `\r` 才会粘到类型 token 上。生产写入的正是裸形态，
 * 所以这里必须用裸形态复现（部分既有用例写的是 `BEGIN_PKGS 1` 这种带载荷的形态 ——
 * 那种形态下 `\r` 落在 arg1 里，类型照样解析成功，**测不出这个缺陷**）。
 */
bool wal_has_unpaired_batch()
{
    const std::string path = wal::wal_log_path();
    // ⚠️ 必须用 **lstat 语义**（`exists_no_follow`）：`fs::exists` 是**跟随**的，而它在
    // stat 失败（ELOOP 自环、EACCES……）时**返回 false** —— 于是"存在却打不开"这种情形
    // 恰好落到了注释所说的**反面**：判成"没有未配对批次"，`cleanup_db_backups()` 随即把
    // `.lpkg_db_bak_before:*` 这些**唯一还原点**删掉。这个守卫的全部意义就是"拿不准时偏
    // 保守"，`exists_no_follow` 才是它想要的判据（名字被占就算存在，含悬空链接与环）。
    const bool exists = exists_no_follow(path);
    const auto lines = read_wal_lines(path);
    // lines == nullopt 把两种情形合在了一起（不存在 / 打不开），用 exists 拆开：
    // 不存在 → "没有"（正常路径：trim 之后）；存在却打不开 → 保守判"有"。
    if (!lines) return exists;
    return scan_batch_pairing(*lines).unpaired_begin >= 0;
}

void cleanup_db_backups()
{
    // **有未提交批次时一律不清理**：*.lpkg_db_bak_before:* 是重试还原 DB 的**唯一**依据。
    //
    // 守卫放在本函数而不是各调用点，是因为调用点有三个而此前只有一个带守卫：
    //   ① recover_packages() 自己判 any_batch_failed 后跳过 —— 唯一正确的那处；
    //   ② `lpkg rec` 分支（`main_cli.cpp` 的 `run_rec_command()`）：recover_packages()
    //      之后**无条件**再调一次，
    //      于是"恢复失败"时刚被特意保留的还原点立刻被删光，CLI 还照打"恢复完成"；
    //   ③ finish_committed_batch()：任何一次成功提交都会把**上一个未配对批次**的重试
    //      依据一并扫掉（trim_completed 只删已配对块，未配对区域仍在，但备份没了）。
    // 后果是"文件被逆向还原了、DB 却停在已安装"的不可恢复状态。
    if (wal_has_unpaired_batch()) return;

    // 递归扫描 DBRM 创建的备份。除 state_dir（deps/、needed_so/ 等子目录）外，
    // man 备份由 write_string_file_wal 写在 docs/ 目录（state_dir 之外），
    // 漏扫会导致每次安装/升级都残留 *.man.lpkg_db_bak_before:* 文件。
    //
    // ⚠️ 三个坑：
    //   · **单个删除失败不能中断整轮**：此前删除与迭代器共用同一个 `ec`，一个删不掉的
    //     备份（EACCES/EROFS/immutable）会让下一轮 `if (ec) break` 直接跳出，该 base 下
    //     **其余备份全被跳过**。
    //   · **自增不能用抛型重载**：`range-for` 的 `operator++` 会抛，而本函数在
    //     `finish_committed_batch()`（批次**已提交之后**）被调用、那里没有 try/catch ——
    //     一次遍历错误就把"装好了"报成"命令失败"。改用显式 `increment(ec)`。
    //   · 删除失败的备份**保留**（下次再试），只记一条警告。
    for (const fs::path& base : {Config::instance().state_dir(), Config::instance().docs_dir()}) {
        // 判定不抛（ELOOP 会让 fs::exists 抛，见 base/path_predicates.hpp
        // 的谓词说明）；仍用**跟随** 语义（状态目录可以是符号链接）。
        if (!is_directory_follow(base)) continue;

        std::error_code walk_ec;
        for (auto it = fs::recursive_directory_iterator(base, walk_ec);
             it != fs::recursive_directory_iterator(); it.increment(walk_ec)) {
            if (walk_ec) {  // 遍历错误（权限/竞态）→ 该 base 到此为止，其余 base 继续
                walk_ec.clear();
                break;
            }
            const std::string fname = it->path().filename().string();
            if (fname.find(".lpkg_db_bak_before:") == std::string::npos) continue;
            std::error_code rm_ec;
            fs::remove(it->path(), rm_ec);
            if (rm_ec) log_warning(string_format("warning.cleanup_failed", it->path().string()));
        }
    }
}
