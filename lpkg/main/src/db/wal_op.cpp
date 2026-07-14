#include "wal_op.hpp"

#include "../base/constants.hpp"
#include "../base/exception.hpp"
#include "../base/utils.hpp"
#include "../config/config.hpp"
#include "../i18n/localization.hpp"
#include "cache.hpp"

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace wal {

// ============================================================================
// 类型名映射
// ============================================================================

static constexpr std::pair<std::string_view, WALOpType> TYPE_MAP[] = {
    {"BEGIN_PKGS", WALOpType::BEGIN_PKGS},
    {"COMMIT_PKGS", WALOpType::COMMIT_PKGS},
    {"BEGIN", WALOpType::BEGIN},
    {"COMMIT", WALOpType::COMMIT},
    {"ROLLBACK", WALOpType::ROLLBACK},
    {"END", WALOpType::END},
    {"BACKUP", WALOpType::BACKUP},
    {"NEW", WALOpType::NEW},
    {"NEW_DIR", WALOpType::NEW_DIR},
    {"COPY", WALOpType::COPY},
    {"REMOVE_OLD", WALOpType::REMOVE_OLD},
    {"RM_BEGIN", WALOpType::RM_BEGIN},
    {"RM_COMMIT", WALOpType::RM_COMMIT},
    {"RM_END", WALOpType::RM_END},
    {"RM_DIR", WALOpType::RM_DIR},
    {"DB", WALOpType::DB},
    {"DBNEW", WALOpType::DBNEW},
    {"DBRM", WALOpType::DBRM},
    {"RESTORE_FILE", WALOpType::RESTORE_FILE},
    {"RESTORE_DB", WALOpType::RESTORE_DB},
    {"RESTORE_DIR", WALOpType::RESTORE_DIR},
    {"REMOVE_FILE", WALOpType::REMOVE_FILE},
    {"REMOVE_DIR", WALOpType::REMOVE_DIR},
};

std::string_view walop_type_name(WALOpType t) {
  for (const auto &[name, type] : TYPE_MAP)
    if (type == t)
      return name;
  return "UNKNOWN";
}

WALOpType walop_type_from_name(std::string_view name) {
  for (const auto &[n, type] : TYPE_MAP)
    if (n == name)
      return type;
  throw LpkgException(std::string("Unknown WAL op type: ") + std::string(name));
}

// ============================================================================
// WAL 行解析
// ============================================================================

// 解析格式:
//   TYPE arg1 [arg2 [arg3 [arg4 [arg5 [arg6]]]]]
// 多参数操作（如 BACKUP，COPY）使用 "→" 作分隔符
// RM_DIR 有 4 个参数: path mode uid gid

static std::vector<std::string_view> split_line(std::string_view line) {
  std::vector<std::string_view> parts;

  // 先找第一个空格 → type
  auto space = line.find(' ');
  if (space == std::string_view::npos) {
    parts.push_back(line);
    return parts;
  }

  parts.push_back(line.substr(0, space));
  std::string_view rest = line.substr(space + 1);

  // 检查是否包含 "→"（多参数格式: BACKUP src → dst 或 COPY src → dst）
  auto arrow = rest.find(" \xe2\x86\x92 "); // " → " in UTF-8
  if (arrow != std::string_view::npos) {
    parts.push_back(rest.substr(0, arrow));
    rest.remove_prefix(arrow + 5); // skip " → " (3 bytes + 2 spaces)
    // rest 现在是 arg2（可能后面还有空格参数）
    auto sp = rest.find(' ');
    if (sp != std::string_view::npos) {
      parts.push_back(rest.substr(0, sp));
      parts.push_back(rest.substr(sp + 1));
    } else {
      parts.push_back(rest);
    }
  } else {
    // 简单空格分割
    while (!rest.empty()) {
      auto sp = rest.find(' ');
      if (sp == std::string_view::npos) {
        parts.push_back(rest);
        break;
      }
      parts.push_back(rest.substr(0, sp));
      rest.remove_prefix(sp + 1);
    }
  }
  return parts;
}

WALOp parse_op(const std::string &line) {
  WALOp op;
  op.raw = line;

  auto parts = split_line(std::string_view(line));
  if (parts.empty())
    return op;

  try {
    op.type = walop_type_from_name(parts[0]);
  } catch (const LpkgException &) {
    // 未知类型 — 返回空 op（调用者跳过）
    op.type = WALOpType::BEGIN_PKGS; // 哨兵，由 skip 逻辑处理
    op.arg1 = "__INVALID__";
    return op;
  }

  if (parts.size() > 1)
    op.arg1 = std::string(parts[1]);
  if (parts.size() > 2)
    op.arg2 = std::string(parts[2]);
  if (parts.size() > 3)
    op.arg3 = std::string(parts[3]);
  if (parts.size() > 4)
    op.arg4 = std::string(parts[4]);
  if (parts.size() > 5)
    op.arg5 = std::string(parts[5]);
  if (parts.size() > 6)
    op.arg6 = std::string(parts[6]);

  return op;
}

// ============================================================================
// checkpoint: DB 条目是否表示 batch-start 最终状态
// ============================================================================

static bool is_batch_start_milestone(const WALOp &op) {
  if (op.type != WALOpType::DB && op.type != WALOpType::DBNEW &&
      op.type != WALOpType::DBRM)
    return false;
  DbMilestone m = DbMilestone::from_string(op.arg2);
  return m.is_batch_start();
}

// ============================================================================
// reverse_execute — 逆向执行引擎
// ============================================================================

/// 向 WAL 日志追加一行（不使用 fsync——审计行在上下文中已由外部 fsync）
static void wal_append_raw(const std::string &line) {
  std::string path = wal_log_path();
  int fd =
      ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0)
    return;
  std::string l = line + "\n";
  ::write(fd, l.data(), l.size());
  ::fsync(fd);
  ::close(fd);
}

/// 获取 .lpkg_db_bak 备份文件的路径
static std::string db_bak_path(const std::string &db_path,
                               const std::string &milestone) {
  return db_path + ".lpkg_db_bak_before:" + milestone;
}

/// 安全的 rename + fsync 父目录
static bool safe_rename(const fs::path &from, const fs::path &to) {
  std::error_code ec;
  fs::rename(from, to, ec);
  if (!ec) {
    fsync_parent_dir(to);
    return true;
  }
  return false;
}

/// 安全删除文件
static bool safe_remove(const fs::path &p) {
  std::error_code ec;
  return fs::remove(p, ec);
}

RollbackStats reverse_execute(const std::vector<WALOp> &ops,
                              const std::string &milestone_target,
                              bool write_audit) {
  RollbackStats stats;

  // 逆序遍历
  for (int i = static_cast<int>(ops.size()) - 1; i >= 0; --i) {
    const auto &op = ops[i];

    // 跳过无效行
    if (op.arg1 == "__INVALID__")
      continue;

    // 跳过元数据和 RESTORE 审计行
    if (op.skip_in_reverse())
      continue;

    // 跳过 :batch-start DB 条目（最终状态标记）
    if (is_batch_start_milestone(op))
      continue;

    switch (op.type) {
    // ── BACKUP / REMOVE_OLD ──────────────────────────────────────────
    case WALOpType::BACKUP:
    case WALOpType::REMOVE_OLD: {
      // arg1 = src (原始路径), arg2 = dst (.lpkg_bak 路径)
      fs::path bak_path = op.arg2;
      fs::path orig_path = op.arg1;

      // fs::exists 对 dangling symlink 返回 false，必须用 is_symlink 补检
      if (fs::exists(bak_path) || fs::is_symlink(bak_path)) {
        safe_rename(bak_path, orig_path);
        stats.files_restored++;

        if (write_audit) {
          wal_append_raw("RESTORE_FILE " + op.arg2 + " \xe2\x86\x92 " +
                         op.arg1);
        }
      }
      // bak 不存在 → 已被消费，跳过（幂等）
      break;
    }

    // ── COPY ─────────────────────────────────────────────────────────
    case WALOpType::COPY: {
      // arg2 = dst（目标文件路径）
      // 逆向：删除目标文件（含 dangling symlink）
      fs::path dst = op.arg2;
      if (fs::exists(dst) || fs::is_symlink(dst)) {
        safe_remove(dst);
        stats.files_cleaned++;

        if (write_audit) {
          wal_append_raw("REMOVE_FILE " + op.arg2);
        }
      }
      break;
    }

    // ── NEW ──────────────────────────────────────────────────────────
    case WALOpType::NEW: {
      // arg1 = 文件路径（含 dangling symlink）
      fs::path p = op.arg1;
      if (fs::exists(p) || fs::is_symlink(p)) {
        safe_remove(p);
        stats.files_cleaned++;

        if (write_audit) {
          wal_append_raw("REMOVE_FILE " + op.arg1);
        }
      }
      break;
    }

    // ── NEW_DIR ──────────────────────────────────────────────────────
    case WALOpType::NEW_DIR: {
      // arg1 = 目录路径
      fs::path p = op.arg1;
      if (fs::exists(p) && fs::is_directory(p)) {
        std::error_code ec;
        if (fs::is_empty(p, ec)) {
          fs::remove(p, ec);
          if (!ec && write_audit) {
            wal_append_raw("REMOVE_DIR " + op.arg1);
          }
        }
      }
      break;
    }

    // ── RM_DIR ───────────────────────────────────────────────────────
    case WALOpType::RM_DIR: {
      // arg1=path, arg2=mode, arg3=uid, arg4=gid (split_line 顺序分配)
      fs::path dir = op.arg1;
      if (!fs::exists(dir)) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (!ec) {
          if (!op.arg2.empty()) {
            mode_t m = static_cast<mode_t>(std::stoul(op.arg2, nullptr, 8));
            chmod(dir.c_str(), m);
          }
          if (!op.arg3.empty() && !op.arg4.empty()) {
            uid_t u = static_cast<uid_t>(std::stoul(op.arg3));
            gid_t g = static_cast<gid_t>(std::stoul(op.arg4));
            chown(dir.c_str(), u, g);
          }
          stats.dirs_recreated++;

          if (write_audit) {
            wal_append_raw("RESTORE_DIR " + op.arg1);
          }
        }
      }
      // 目录已存在 → 跳过（幂等）
      break;
    }

    // ── DB ───────────────────────────────────────────────────────────
    case WALOpType::DB: {
      // arg1 = DB 文件路径, arg2 = 里程碑
      std::string bak = db_bak_path(op.arg1, op.arg2);
      if (fs::exists(bak)) {
        safe_rename(bak, op.arg1);
        stats.db_restored++;

        if (write_audit) {
          wal_append_raw("RESTORE_DB " + bak + " \xe2\x86\x92 " + op.arg1);
        }
      }
      // bak 不存在 → WAL 已写但备份未完成 → 原文件还在 → 跳过（幂等）
      break;
    }

    // ── DBNEW ────────────────────────────────────────────────────────
    case WALOpType::DBNEW: {
      // arg1 = DB 文件路径, arg2 = 里程碑
      std::string bak = db_bak_path(op.arg1, op.arg2);
      if (fs::exists(bak)) {
        safe_rename(bak, op.arg1);
        stats.db_restored++;

        if (write_audit) {
          wal_append_raw("RESTORE_DB " + bak + " \xe2\x86\x92 " + op.arg1);
        }
      } else {
        // 无备份 → 文件是新建的 → 删除
        if (fs::exists(op.arg1)) {
          safe_remove(op.arg1);
          stats.files_cleaned++;

          if (write_audit) {
            wal_append_raw("REMOVE_FILE " + op.arg1);
          }
        }
      }
      break;
    }

    // ── DBRM ─────────────────────────────────────────────────────────
    case WALOpType::DBRM: {
      // arg1 = DB 文件路径, arg2 = 里程碑
      std::string bak = db_bak_path(op.arg1, op.arg2);
      if (fs::exists(bak)) {
        safe_rename(bak, op.arg1);
        stats.db_restored++;

        if (write_audit) {
          wal_append_raw("RESTORE_DB " + bak + " \xe2\x86\x92 " + op.arg1);
        }
      }
      // bak 不存在 → 跳过（幂等）
      break;
    }

    default:
      break;
    }

    // 检查里程碑停止条件
    if (!milestone_target.empty() &&
        (op.type == WALOpType::DB || op.type == WALOpType::DBNEW ||
         op.type == WALOpType::DBRM)) {
      if (op.arg2 == milestone_target) {
        return stats;
      }
    }
  }

  return stats;
}

// ============================================================================
// 批次操作提取
// ============================================================================

std::vector<WALOp> extract_current_batch_ops(const std::string &wal_path) {
  std::vector<WALOp> ops;
  std::ifstream file(wal_path);
  if (!file.is_open())
    return ops;

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty())
      continue;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(line);
  }

  // 从最后一个 BEGIN_PKGS 开始收集
  int start_idx = -1;
  for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i) {
    auto op = parse_op(lines[i]);
    if (op.type == WALOpType::BEGIN_PKGS) {
      start_idx = i;
      break;
    }
    if (op.type == WALOpType::COMMIT_PKGS) {
      // 最后一个块已完成，无未提交批次
      return {};
    }
  }

  if (start_idx < 0)
    return {};

  for (size_t i = start_idx; i < lines.size(); ++i) {
    auto op = parse_op(lines[i]);
    if (op.arg1 != "__INVALID__")
      ops.push_back(op);
  }

  return ops;
}

std::vector<std::vector<WALOp>>
extract_all_uncommitted_batches(const std::string &wal_path) {
  std::vector<std::vector<WALOp>> result;
  std::ifstream file(wal_path);
  if (!file.is_open())
    return result;

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty())
      continue;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(line);
  }

  int depth = 0;
  int batch_start = -1;
  bool in_uncommitted = false;

  for (size_t i = 0; i < lines.size(); ++i) {
    auto op = parse_op(lines[i]);
    if (op.arg1 == "__INVALID__")
      continue;

    if (op.type == WALOpType::BEGIN_PKGS) {
      depth++;
      if (depth == 1) {
        batch_start = static_cast<int>(i);
        in_uncommitted = true;
      }
    } else if (op.type == WALOpType::COMMIT_PKGS) {
      depth--;
      if (depth == 0 && in_uncommitted) {
        in_uncommitted = false;
      }
    }
  }

  if (in_uncommitted && batch_start >= 0) {
    std::vector<WALOp> ops;
    for (size_t i = static_cast<size_t>(batch_start); i < lines.size(); ++i) {
      auto op = parse_op(lines[i]);
      if (op.arg1 != "__INVALID__")
        ops.push_back(op);
    }
    result.push_back(std::move(ops));
  }

  return result;
}

// ============================================================================
// 批次回滚
// ============================================================================

void batch_rollback(const std::vector<std::string> &successfully_installed) {
  std::string wpath = wal_log_path();
  auto ops = extract_current_batch_ops(wpath);
  if (ops.empty())
    return;

  // 1. 清理内存 cache 状态（remove_installed 内部已有锁，不要外层加锁！）
  auto &cache = Cache::instance();
  for (const auto &pkg : successfully_installed) {
    cache.remove_installed(pkg);
  }

  // 2. 逆向执行操作
  reverse_execute(ops, ":batch-start", true);

  // 3. 重载 Cache（从磁盘恢复的 DB 文件）
  cache.load();

  // 4. DB :batch-start（保存回滚后的状态）
  cache.write(":batch-start");

  // 5. 对每个已成功（已回滚）包写 ROLLBACK + END
  for (const auto &pkg : successfully_installed) {
    std::string ver = Cache::instance().get_installed_version(pkg);
    wal_append_raw("ROLLBACK " + pkg + " " + ver);
    wal_append_raw("END " + pkg + " " + ver);
  }

  // 6. COMMIT_PKGS
  wal_append_raw("COMMIT_PKGS");
}

// ============================================================================
// WAL 文件路径
// ============================================================================

std::string wal_log_path() {
  return (Config::instance().state_dir() / "transaction.log").string();
}

} // namespace wal
