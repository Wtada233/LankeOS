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

#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

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
  bool in_txn = false;
  size_t batch_start = 0;

  for (size_t i = 0; i < lines.size(); ++i) {
    auto op = wal::parse_op(lines[i]);
    if (op.arg1 == "__INVALID__")
      continue;

    if (op.type == wal::WALOpType::BEGIN_PKGS) {
      if (!in_txn) {
        batch_start = i;
        in_txn = true;
      }
      // 嵌套 BEGIN_PKGS（不应发生，但健壮处理）：记录新的起点
    } else if (op.type == wal::WALOpType::COMMIT_PKGS) {
      if (in_txn) {
        in_txn = false;
        // 批次已完成，不加入未提交列表
      }
    }
  }

  if (in_txn) {
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

    // b) reverse_execute — 跳过 RESTORE_*/REMOVE_*/元数据行和 :batch-start
    //    只执行正向操作的逆向
    wal::reverse_execute(ops, "", true);

    // c) 重载 Cache（从磁盘恢复的 DB 文件）
    Cache::instance().load();

    // d) 写入 COMMIT_PKGS 以关闭批次
    wal::commit_batch();
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
  for (const auto &entry : fs::directory_iterator(state_dir, ec)) {
    if (ec)
      break;

    const std::string fname = entry.path().filename().string();
    // 匹配 .lpkg_db_bak_before:* 模式
    if (fname.find(".lpkg_db_bak_before:") != std::string::npos) {
      fs::remove(entry.path(), ec);
    }
  }
}
