/**
 * recover.cpp — WAL 恢复与清理
 *
 * recover_packages(): 紧急恢复 — 仅在进程因崩溃（断电/OOM/SIGKILL）
 * 而未能执行 catch 中的 batch_rollback() 时使用。
 *
 * trim_completed(): 清理已完成批次的 WAL 日志行，释放磁盘空间。
 */

#include "../base/constants.hpp"
#include "../base/utils.hpp"
#include "../config/config.hpp"
#include "../i18n/localization.hpp"
#include "cache.hpp"
#include "transaction_log.hpp"
#include "wal_op.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <set>
#include <vector>

namespace fs = std::filesystem;

namespace {

/**
 * continue_cleanup — 崩溃续传：继续清理未完成的 .lpkg_bak 清理操作。
 * 在 recover_packages 中检测到 CLEANUP 条目时调用。
 * 只清理文件/目录，不做 reverse_execute 回滚。
 */
void continue_cleanup(const std::vector<wal::WALOp> &ops) {
  std::vector<fs::path> all_baks;
  std::set<std::string> cleaned;

  for (const auto &op : ops) {
    if ((op.type == wal::WALOpType::BACKUP ||
         op.type == wal::WALOpType::REMOVE_OLD) &&
        !op.arg2.empty()) {
      all_baks.push_back(op.arg2);
    } else if (op.type == wal::WALOpType::CLEANUP && !op.arg1.empty()) {
      cleaned.insert(op.arg1);
    }
  }

  if (all_baks.empty())
    return;

  // 去重
  std::ranges::sort(all_baks);
  auto last = std::unique(all_baks.begin(), all_baks.end());
  all_baks.erase(last, all_baks.end());

  // 最深层优先
  std::ranges::sort(all_baks, [](const fs::path &a, const fs::path &b) {
    return a.string().size() > b.string().size();
  });

  for (const auto &bak : all_baks) {
    if (!fs::exists(bak) && !fs::is_symlink(bak))
      continue;  // 已删除（不论是否在 CLEANUP 集中）

    std::error_code ec;
    bool ok = true;

    if (fs::is_directory(bak)) {
      std::vector<fs::path> entries;
      for (const auto &entry : fs::recursive_directory_iterator(bak, ec))
        if (!ec) entries.push_back(entry.path());
      if (!ec) {
        std::reverse(entries.begin(), entries.end());
        for (const auto &e : entries) {
          if (!fs::remove(e, ec))
            ok = false;
        }
      }
      if (!fs::remove(bak, ec))
        ok = false;
    } else {
      if (!fs::remove(bak, ec))
        ok = false;
    }

    if (ok) {
      if (!cleaned.contains(bak.string()))
        wal::log_wal_line("CLEANUP " + bak.string());
    } else {
      log_warning(string_format("warning.cleanup_failed", bak.string()));
    }
  }

  Cache::instance().load();
  wal::commit_batch();
}

} // anonymous namespace

// ============================================================================
// recover_packages — 断电/崩溃恢复
// ============================================================================

void recover_packages() {
  std::string wpath = wal::wal_log_path();
  if (!fs::exists(wpath))
    return;

  // 1. 读取所有行
  std::vector<std::string> lines;
  {
    std::ifstream file(wpath);
    if (!file.is_open())
      return;
    std::string line;
    while (std::getline(file, line)) {
      if (line.empty())
        continue;
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      lines.push_back(line);
    }
  }

  if (lines.empty())
    return;

  // 2. 状态机扫描：找到所有未完成的批次
  //    BEGIN_PKGS → in_txn=true, 开始积累 ops
  //    COMMIT_PKGS → in_txn=false, 清空 ops
  //    EOF + in_txn=true → 需要恢复

  struct BatchInfo {
    size_t start_line;
    size_t end_line; // 批次最后一行（不含，即 lines.size() 如果到 EOF）
  };

  std::vector<BatchInfo> uncommitted_batches;
  int depth = 0;
  size_t batch_start = 0;

  for (size_t i = 0; i < lines.size(); ++i) {
    auto op = wal::parse_op(lines[i]);
    if (op.arg1 == "__INVALID__")
      continue;

    if (op.type == wal::WALOpType::BEGIN_PKGS) {
      if (depth == 0)
        batch_start = i;
      ++depth;
    } else if (op.type == wal::WALOpType::COMMIT_PKGS) {
      if (depth > 0)
        --depth;
    }
  }

  if (depth > 0) {
    uncommitted_batches.push_back({batch_start, lines.size()});
  }

  if (uncommitted_batches.empty()) {
    // 没有未完成的批次，清理整个日志
    trim_completed();
    return;
  }

  // 3. 对每个未完成事务进行恢复
  for (const auto &batch : uncommitted_batches) {
    // a) 解析操作行
    std::vector<wal::WALOp> ops;
    for (size_t i = batch.start_line; i < batch.end_line; ++i) {
      auto op = wal::parse_op(lines[i]);
      if (op.arg1 != "__INVALID__")
        ops.push_back(op);
    }

    if (ops.empty())
      continue;

    // b) 检查是否已有 CLEANUP 条目（CLEANUP 阶段已开始，不可回滚）
    bool has_cleanup = false;
    for (const auto &op : ops) {
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
      wal::reverse_execute(ops, "", true);
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

void trim_completed() {
  std::string wpath = wal::wal_log_path();
  if (!fs::exists(wpath))
    return;

  std::vector<std::string> lines;
  {
    std::ifstream file(wpath);
    if (!file.is_open())
      return;
    std::string line;
    while (std::getline(file, line)) {
      if (line.empty())
        continue;
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
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
    if (op.arg1 == "__INVALID__")
      continue;

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
    // 所有批次都已完成 → 清空整个日志文件
    std::ofstream(wpath, std::ios::trunc).close();
    return;
  }

  // 保留从 last_unpaired_begin 开始的所有行
  if (last_unpaired_begin == 0) {
    // 没有需要清理的已完成批次
    return;
  }

  // 写入保留的行
  std::string tmp_path = wpath + ".trim_tmp";
  {
    std::ofstream out(tmp_path);
    for (size_t i = static_cast<size_t>(last_unpaired_begin); i < lines.size();
         ++i) {
      out << lines[i] << "\n";
    }
  }

  fs::rename(tmp_path, wpath);
  fsync_parent_dir(wpath);
}

// ============================================================================
// cleanup_db_backups — 清理孤立的 .lpkg_db_bak_before:* 文件
// ============================================================================

void cleanup_db_backups() {
  const fs::path state_dir = Config::instance().state_dir();
  if (!fs::exists(state_dir) || !fs::is_directory(state_dir))
    return;

  std::error_code ec;
  // 递归扫描：DBRM 创建的备份在 deps/、needed_so/ 等子目录中
  for (const auto &entry : fs::recursive_directory_iterator(state_dir, ec)) {
    if (ec)
      break;

    const std::string fname = entry.path().filename().string();
    if (fname.find(".lpkg_db_bak_before:") != std::string::npos) {
      fs::remove(entry.path(), ec);
    }
  }
}
