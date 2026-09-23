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
        if (line.empty()) continue;
        auto op = parse_op(line);
        if (!op.is_valid()) continue;
        if ((op.type == WALOpType::BACKUP || op.type == WALOpType::REMOVE_OLD) && !op.arg2.empty())
            roots.insert(stash_root_of_bak(op.arg2).lexically_normal());
        else if (op.type == WALOpType::CLEANUP && !op.arg1.empty())
            roots.insert(stash_root_of_bak(op.arg1).lexically_normal());
    }
    return roots;
}

}  // namespace wal

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
 * 只处理"已提交区域"（最后一个未配对 BEGIN_PKGS 之前）的 BACKUP/REMOVE_OLD dst，
 * 以及尾部 post-commit CLEANUP 行引用的 bak；绝不动未提交批次的 bak
 * （那是 reverse_execute 回滚要用的）。
 */
static void continue_post_commit_cleanup(const std::vector<std::string>& lines)
{
    // 1. 定位：**第一个**未配对 BEGIN_PKGS（未提交区域起点）与最后一个 COMMIT_PKGS。
    //
    //    必须取"第一个"而不是"最后一个"：一次崩溃可能留下**两个**未提交批次（前一批
    //    回滚自身失败后同进程又开了新批次），夹在它们之间的 bak 属于**还没还原**的前一批，
    //    若按"已提交区域"删掉就是永久丢文件（TODO.md X6）。已提交批次区间用配对记账
    //    （depth 回到 0）识别，未配对的那个 BEGIN_PKGS 之后的一切都归未提交区域。
    ssize_t last_commit = -1;
    ssize_t open_begin = -1;  // 当前未配对批次的起点；配对成功即清空
    int depth = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto op = wal::parse_op(lines[i]);
        if (!op.is_valid()) continue;
        if (op.type == wal::WALOpType::BEGIN_PKGS) {
            if (depth == 0) open_begin = static_cast<ssize_t>(i);
            ++depth;
        } else if (op.type == wal::WALOpType::COMMIT_PKGS) {
            last_commit = static_cast<ssize_t>(i);
            if (depth > 0 && --depth == 0) open_begin = -1;  // 该批次已配对提交
        }
    }
    const ssize_t unpaired = open_begin;

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
    for (auto& bak : baks) {
        roots.push_back(wal::stash_root_of_bak(bak));
    }
    std::ranges::sort(roots);
    auto last = std::unique(roots.begin(), roots.end());
    roots.erase(last, roots.end());

    // 4. 删除仍存在的 stash 根（幂等：已删的跳过）
    for (const auto& root : roots) {
        if (!fs::exists(root) && !fs::is_symlink(root)) continue;
        std::error_code ec;
        fs::remove_all(root, ec);
        if (ec) log_warning(string_format("warning.cleanup_failed", root.string()));
    }
}

// ============================================================================
// recover_packages — 断电/崩溃恢复
// ============================================================================

void recover_packages()
{
    std::string wpath = wal::wal_log_path();
    if (!fs::exists(wpath)) return;

    // 1. 读取所有行
    std::vector<std::string> lines;
    {
        std::ifstream file(wpath);
        if (!file.is_open()) return;
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    if (lines.empty()) return;

    // 1.5 续传 post-commit 清理：删除已提交批次残留的 .lpkg_bak。
    //     必须在处理未提交批次之前做，且必须早于任何 trim——一旦 trim 把完成事务
    //     连同其 BACKUP 行清掉，残留 bak 的来源记录就没了。
    continue_post_commit_cleanup(lines);

    // 2. 状态机扫描：找到所有未完成的批次
    //    BEGIN_PKGS → in_txn=true, 开始积累 ops
    //    COMMIT_PKGS → in_txn=false, 清空 ops
    //    EOF + in_txn=true → 需要恢复

    struct BatchInfo {
        size_t start_line;
        size_t end_line;  // 批次最后一行（不含，即 lines.size() 如果到 EOF）
    };

    bool any_batch_failed = false;
    std::vector<BatchInfo> uncommitted_batches;
    int depth = 0;
    size_t batch_start = 0;

    for (size_t i = 0; i < lines.size(); ++i) {
        auto op = wal::parse_op(lines[i]);
        if (!op.is_valid()) continue;

        if (op.type == wal::WALOpType::BEGIN_PKGS) {
            // 区域起点 = **第一个未配对** BEGIN_PKGS（depth 0→1 的那一个），一次逆序回滚
            // 整个未提交区域（最近的批次先逆、更早的后逆）。
            //
            // 曾改成"记最后一个 BEGIN_PKGS"以求"每轮只收尾最近一批、逐 pass 收敛"——**错的**：
            // 已提交批次的行仍留在 WAL 里，下一轮扫描的起点仍落在它上面，更早那批永远轮不到
            // （实测两轮下来文件一次都没被还原）。而"区域横跨两个未提交批次"曾经危险，只是因为
            // 当时存在 `has_cleanup ⇒ continue_cleanup` 分支会对整个区域 remove_all（会删掉前一批
            // 尚未还原的 stash）；该分支已删除，故一次性回滚整个区域正确且真正收敛（TODO.md
            // X6/Z5）。
            if (depth == 0) batch_start = i;
            ++depth;
        } else if (op.type == wal::WALOpType::COMMIT_PKGS) {
            if (depth > 0) --depth;
        }
    }

    if (depth > 0) {
        uncommitted_batches.push_back({batch_start, lines.size()});
    }

    if (uncommitted_batches.empty()) {
        // 没有未完成的批次，清理整个日志。
        // 同时清理孤儿 .lpkg_db_bak_before：已提交批次崩溃在"COMMIT_PKGS 之后、
        // post-batch cleanup 之前"时留下的备份，此处一并清掉（启动时无活动批次，
        // DBLock 保证单进程，安全）。
        trim_completed();
        cleanup_db_backups();
        return;
    }

    // 3. 对每个未完成事务进行恢复
    for (const auto& batch : uncommitted_batches) {
        // a) 解析操作行
        std::vector<wal::WALOp> ops;
        for (size_t i = batch.start_line; i < batch.end_line; ++i) {
            auto op = wal::parse_op(lines[i]);
            if (op.is_valid()) ops.push_back(op);
        }

        if (ops.empty()) continue;

        // 未提交批次一律反序回滚（CLEANUP 只可能出现在事务之外，见 ARCH.md §11.3；
        // 旧版二进制写入的"批次内 CLEANUP"形状不在支持范围）。
        // **单个批次失败不得卡死整个恢复**：告警、保持该批次未提交（WAL 与备份原样保留，
        // 下次 rec 可重试），继续处理其余批次；并且此后不清理 DB 备份（那是重试的依据）。
        std::string first_pkg;
        for (const auto& op : ops) {
            if ((op.type == wal::WALOpType::BEGIN || op.type == wal::WALOpType::RM_BEGIN) &&
                !op.arg1.empty()) {
                first_pkg = op.arg1;
                break;
            }
        }
        try {
            wal::reverse_execute(ops, true);
        } catch (const std::exception& e) {
            log_warning(string_format("warning.rollback_remove_failed", first_pkg, e.what()) +
                        " [rec: 该批次保持未提交，可重试]");
            any_batch_failed = true;
            continue;
        }
        wal::purge_consumed_stashes(ops);  // stash 里的文件已还原 → 清空 stash 根
        Cache::instance().load();
        wal::commit_batch();
    }

    // 4. 清理残留的 .lpkg_db_bak_before:* 备份文件。
    //    有批次恢复失败时**跳过**：那些备份是重试还原 DB 的唯一依据（TODO.md A3 同理）。
    if (!any_batch_failed) cleanup_db_backups();
}

// ============================================================================
// trim_completed — 清理已完成的 WAL 条目
// ============================================================================

void trim_completed()
{
    std::string wpath = wal::wal_log_path();
    if (!fs::exists(wpath)) return;

    std::vector<std::string> lines;
    {
        std::ifstream file(wpath);
        if (!file.is_open()) return;
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    if (lines.empty()) {
        // 空文件 → 删除
        std::error_code ec;
        fs::remove(wpath, ec);
        return;
    }

    // 从后向前找到最后一个 COMMIT_PKGS
    // 如果最后的 COMMIT_PKGS 之后还有行（异常情况），保留它们
    // 如果最后一行是 COMMIT_PKGS，找到对应的 BEGIN_PKGS 并保留最后一个
    // 未提交的批次

    // 找**第一个**未配对 BEGIN_PKGS 的位置（= 需保留区域的起点）。
    //
    // 必须取"第一个"而不是"最后一个"：一次崩溃可能留下两个未提交批次（前一批回滚失败后
    // 同进程又开了新批次），若按"最后一个未配对"裁剪，前一批的 BEGIN/BACKUP 行会被当作
    // 已完成内容删掉 —— 恢复依据就此消失（TODO.md X6/Z5）。配对用前向 depth 记账识别，
    // 与 continue_post_commit_cleanup、recover_packages 的区域判定一致。
    ssize_t first_unpaired_begin = -1;
    int depth = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto op = wal::parse_op(lines[i]);
        if (!op.is_valid()) continue;
        if (op.type == wal::WALOpType::BEGIN_PKGS) {
            if (depth == 0) first_unpaired_begin = static_cast<ssize_t>(i);
            ++depth;
        } else if (op.type == wal::WALOpType::COMMIT_PKGS) {
            if (depth > 0 && --depth == 0) first_unpaired_begin = -1;  // 该批次已配对
        }
    }
    const ssize_t last_unpaired_begin = first_unpaired_begin;

    if (last_unpaired_begin < 0) {
        // 所有事务都已提交。但可能还有 post-commit 清理记录未完成（install/upgrade
        // 的 CLEANUP 行写在事务之外、I-BAK-2；或已提交批次的 .lpkg_bak 因清理失败/
        // 崩溃仍未删除）。trim 只清理"完全完成"的内容：
        //   - 已提交批次的 BACKUP/REMOVE_OLD bak 仍残留，或
        //   - trailing 的 CLEANUP 行引用的 bak 仍残留
        // → 清理未完成，**保留整个文件**（BACKUP 上下文仍在，recover 会续传），
        //   不做任何裁剪；只有所有 bak 都删干净了才允许清空。
        bool pending = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            auto op = wal::parse_op(lines[i]);
            if (!op.is_valid()) continue;
            if ((op.type == wal::WALOpType::BACKUP || op.type == wal::WALOpType::REMOVE_OLD) &&
                !op.arg2.empty() && (fs::exists(op.arg2) || fs::is_symlink(op.arg2))) {
                pending = true;
                break;
            }
            if (op.type == wal::WALOpType::CLEANUP && !op.arg1.empty() &&
                (fs::exists(op.arg1) || fs::is_symlink(op.arg1))) {
                pending = true;
                break;
            }
        }
        if (pending) return;  // 清理未完成 → 保留整个文件，等 recover 续传
        // 清理完成 → 清空整个日志文件（全是完成事务/历史清理记录）
        std::ofstream(wpath, std::ios::trunc).close();
        return;
    }

    // 保留从 last_unpaired_begin 开始的所有行
    if (last_unpaired_begin == 0) {
        // 没有需要清理的已完成批次
        return;
    }

    // 写入保留的行。保留下来的未提交批次是恢复数据，必须先 fsync 再 rename
    // （I-FSYNC-5：write 用 .tmp + fsync + rename），否则断电可能丢失恢复点。
    std::string tmp_path = wpath + ".trim_tmp";
    {
        std::ofstream out(tmp_path);
        for (size_t i = static_cast<size_t>(last_unpaired_begin); i < lines.size(); ++i) {
            out << lines[i] << "\n";
        }
        out.flush();
        if (!out) throw LpkgException(string_format("error.db_write_failed", tmp_path));
    }
    {
        int fd = ::open(tmp_path.c_str(), O_WRONLY);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
    }

    safe_rename(tmp_path, wpath);
}

// ============================================================================
// cleanup_db_backups — 清理孤立的 .lpkg_db_bak_before:* 文件
// ============================================================================

void cleanup_db_backups()
{
    std::error_code ec;
    // 递归扫描 DBRM 创建的备份。除 state_dir（deps/、needed_so/ 等子目录）外，
    // man 备份由 write_string_file_wal 写在 docs/ 目录（state_dir 之外），
    // 漏扫会导致每次安装/升级都残留 *.man.lpkg_db_bak_before:* 文件。
    for (const fs::path& base : {Config::instance().state_dir(), Config::instance().docs_dir()}) {
        if (!fs::exists(base) || !fs::is_directory(base)) continue;

        for (const auto& entry : fs::recursive_directory_iterator(base, ec)) {
            if (ec) break;

            const std::string fname = entry.path().filename().string();
            if (fname.find(".lpkg_db_bak_before:") != std::string::npos) {
                fs::remove(entry.path(), ec);
            }
        }
        ec.clear();
    }
}
