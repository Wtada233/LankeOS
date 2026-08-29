/**
 * recover.cpp — WAL 恢复与清理
 *
 * recover_packages(): 紧急恢复 — 仅在进程因崩溃（断电/OOM/SIGKILL）
 * 而未能执行 catch 中的 batch_rollback() 时使用。
 *
 * trim_completed(): 清理已完成批次的 WAL 日志行，释放磁盘空间。
 */

#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <set>
#include <unistd.h>
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

/**
 * continue_cleanup — 崩溃续传：继续清理未完成的 .lpkg_bak 清理操作。
 * 在 recover_packages 中检测到 CLEANUP 条目时调用，也被 run_batch_transaction
 * 的异常路径复用（CLEANUP 阶段内 IO 异常 → 续删+提交而非回滚）。
 * 只清理文件/目录，不做 reverse_execute 回滚。
 */
void continue_cleanup(const std::vector<WALOp>& ops)
{
    std::vector<fs::path> all_baks;
    std::set<std::string> cleaned;

    for (const auto& op : ops) {
        if ((op.type == WALOpType::BACKUP || op.type == WALOpType::REMOVE_OLD) &&
            !op.arg2.empty()) {
            all_baks.push_back(op.arg2);
        } else if (op.type == WALOpType::CLEANUP && !op.arg1.empty()) {
            cleaned.insert(op.arg1);
            // CLEANUP 行的 bak 也要纳入清理范围：write-ahead 下"CLEANUP 已记、删除未做"
            // 的 bak 仍可能在磁盘上，续传要能定位它（尤其 trim 已裁掉 BACKUP 行的尾部场景）。
            all_baks.push_back(op.arg1);
        }
    }

    if (all_baks.empty()) {
        // 批次已进入 CLEANUP 阶段但无待清理备份（可能上一轮续删已完成但未提交）：
        // 直接提交，防止批次永久卡在"未提交"、每次启动重复进入本分支。
        Cache::instance().load();
        commit_batch();
        return;
    }

    // 去重
    std::ranges::sort(all_baks);
    auto last = std::unique(all_baks.begin(), all_baks.end());
    all_baks.erase(last, all_baks.end());

    // 最深层优先
    std::ranges::sort(all_baks, [](const fs::path& a, const fs::path& b) {
        return a.string().size() > b.string().size();
    });

    for (const auto& bak : all_baks) {
        if (!fs::exists(bak) && !fs::is_symlink(bak))
            continue;  // 已删除（不论是否在 CLEANUP 集中）

        std::error_code ec;
        bool ok = true;

        // fs::is_directory 跟随符号链接：bak 若是指向目录的 symlink（如 filesystem
        // 包的 /usr/lib64 → lib 的备份），误判为目录会递归删除其指向目录的内容。
        // symlink 必须只删自身。
        if (fs::is_directory(bak) && !fs::is_symlink(bak)) {
            std::vector<fs::path> entries;
            for (const auto& entry : fs::recursive_directory_iterator(bak, ec))
                if (!ec) entries.push_back(entry.path());
            if (!ec) {
                std::reverse(entries.begin(), entries.end());
                for (const auto& e : entries) {
                    if (!fs::remove(e, ec)) ok = false;
                }
            }
            if (!fs::remove(bak, ec)) ok = false;
        } else {
            if (!fs::remove(bak, ec)) ok = false;
        }

        if (ok) {
            if (!cleaned.contains(bak.string())) log_wal_line("CLEANUP " + bak.string());
        } else {
            log_warning(string_format("warning.cleanup_failed", bak.string()));
        }
    }

    Cache::instance().load();
    commit_batch();
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
    // 1. 定位：最后一个未配对 BEGIN_PKGS（未提交批次起点）与最后一个 COMMIT_PKGS
    ssize_t unpaired = -1;
    ssize_t last_commit = -1;
    int depth = 0;
    for (ssize_t i = static_cast<ssize_t>(lines.size()) - 1; i >= 0; --i) {
        auto op = wal::parse_op(lines[i]);
        if (op.arg1 == "__INVALID__") continue;
        if (op.type == wal::WALOpType::COMMIT_PKGS) {
            if (last_commit < 0) last_commit = i;
            ++depth;
        } else if (op.type == wal::WALOpType::BEGIN_PKGS) {
            if (depth > 0)
                --depth;
            else {
                unpaired = i;
                break;
            }
        }
    }

    // 2. 收集 bak：
    //    - 已提交区域（[0, unpaired) 或全部）的 BACKUP/REMOVE_OLD dst；
    //    - post-commit 尾部（最后一个 COMMIT_PKGS 之后、未提交批次之前）的
    //      CLEANUP 行引用的 bak。绝不动未提交批次的 CLEANUP（那是 continue_cleanup 的活）。
    std::vector<fs::path> baks;
    const size_t region_end = (unpaired < 0) ? lines.size() : static_cast<size_t>(unpaired);
    for (size_t i = 0; i < region_end; ++i) {
        auto op = wal::parse_op(lines[i]);
        if (op.arg1 == "__INVALID__") continue;
        if ((op.type == wal::WALOpType::BACKUP || op.type == wal::WALOpType::REMOVE_OLD) &&
            !op.arg2.empty())
            baks.push_back(op.arg2);
    }
    const size_t tail_start = (last_commit < 0) ? 0 : static_cast<size_t>(last_commit) + 1;
    for (size_t i = tail_start; i < region_end; ++i) {
        auto op = wal::parse_op(lines[i]);
        if (op.arg1 == "__INVALID__") continue;
        if (op.type == wal::WALOpType::CLEANUP && !op.arg1.empty()) baks.push_back(op.arg1);
    }
    if (baks.empty()) return;

    // 3. 去重 + 最深层优先（文件先于目录，子目录先于父目录）
    std::ranges::sort(baks);
    auto last = std::unique(baks.begin(), baks.end());
    baks.erase(last, baks.end());
    std::ranges::sort(baks, [](const fs::path& a, const fs::path& b) {
        return a.string().size() > b.string().size();
    });

    // 4. 删除仍存在的 bak（幂等：已删的跳过）
    for (const auto& bak : baks) {
        if (!fs::exists(bak) && !fs::is_symlink(bak)) continue;

        std::error_code ec;
        bool ok = true;
        // 同 continue_cleanup：symlink 备份必须只删自身，不能跟随成目录递归删除。
        if (fs::is_directory(bak) && !fs::is_symlink(bak)) {
            std::vector<fs::path> entries;
            for (const auto& e : fs::recursive_directory_iterator(bak, ec))
                if (!ec) entries.push_back(e.path());
            if (!ec) {
                std::reverse(entries.begin(), entries.end());
                for (const auto& e : entries)
                    if (!fs::remove(e, ec)) ok = false;
            }
            if (!fs::remove(bak, ec)) ok = false;
        } else {
            if (!fs::remove(bak, ec)) ok = false;
        }

        if (!ok) log_warning(string_format("warning.cleanup_failed", bak.string()));
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

    std::vector<BatchInfo> uncommitted_batches;
    int depth = 0;
    size_t batch_start = 0;

    for (size_t i = 0; i < lines.size(); ++i) {
        auto op = wal::parse_op(lines[i]);
        if (op.arg1 == "__INVALID__") continue;

        if (op.type == wal::WALOpType::BEGIN_PKGS) {
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
            if (op.arg1 != "__INVALID__") ops.push_back(op);
        }

        if (ops.empty()) continue;

        // b) 检查是否已有 CLEANUP 条目（CLEANUP 阶段已开始，不可回滚）
        bool has_cleanup = false;
        for (const auto& op : ops) {
            if (op.type == wal::WALOpType::CLEANUP) {
                has_cleanup = true;
                break;
            }
        }

        if (has_cleanup) {
            // 已有 CLEANUP → 继续清理，不回滚（CLEANUP 不可逆）
            continue_cleanup(ops);
        } else {
            // 无 CLEANUP → 正常 reverse_execute 回滚
            wal::reverse_execute(ops, true);
            Cache::instance().load();
            wal::commit_batch();
        }
    }

    // 4. 清理残留的 .lpkg_db_bak_before:* 备份文件
    cleanup_db_backups();
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

    // 先找到最后一个未配对的 BEGIN_PKGS 的位置
    ssize_t last_unpaired_begin = -1;
    int depth = 0;

    for (ssize_t i = static_cast<ssize_t>(lines.size()) - 1; i >= 0; --i) {
        auto op = wal::parse_op(lines[i]);
        if (op.arg1 == "__INVALID__") continue;

        if (op.type == wal::WALOpType::COMMIT_PKGS) {
            depth++;
        } else if (op.type == wal::WALOpType::BEGIN_PKGS) {
            if (depth > 0) {
                depth--;
            } else {
                // 未配对的 BEGIN_PKGS
                last_unpaired_begin = i;
                break;
            }
        }
    }

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
            if (op.arg1 == "__INVALID__") continue;
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
