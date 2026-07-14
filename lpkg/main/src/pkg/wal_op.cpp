#include "wal_op.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "i18n/localization.hpp"
#include "transaction_log.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

// WAL 日志中使用的箭头分隔符（UTF-8 编码）
// " \xe2\x86\x92 " = space + U+2192 + space = 5 字节
static constexpr std::string_view ARROW_SEP = " \xe2\x86\x92 ";
static constexpr size_t ARROW_SEP_LEN = 5;

namespace wal {

std::optional<WALOp> parse_op(const std::string &line) {
  if (line.starts_with("BACKUP ")) {
    // BACKUP <src> → <dst>
    auto arrow = line.find(ARROW_SEP, 7);
    if (arrow == std::string::npos)
      return std::nullopt;
    return WALOp{"BACKUP", line.substr(7, arrow - 7),
                 line.substr(arrow + ARROW_SEP_LEN)};

  } else if (line.starts_with("COPY ")) {
    auto arrow = line.find(ARROW_SEP, 5);
    if (arrow == std::string::npos)
      return std::nullopt;
    return WALOp{"COPY", line.substr(5, arrow - 5),
                 line.substr(arrow + ARROW_SEP_LEN)};

  } else if (line.starts_with("REMOVE_OLD ")) {
    auto arrow = line.find(ARROW_SEP, 11);
    if (arrow == std::string::npos)
      return std::nullopt;
    return WALOp{"REMOVE_OLD", line.substr(11, arrow - 11),
                 line.substr(arrow + ARROW_SEP_LEN)};

  } else if (line.starts_with("NEW_DIR ")) {
    return WALOp{"NEW_DIR", line.substr(8), ""};

  } else if (line.starts_with("NEW ")) {
    return WALOp{"NEW", line.substr(4), ""};

  } else if (line.starts_with("RM_DIR ")) {
    return WALOp{"RM_DIR", line.substr(6), ""};

  } else if (line.starts_with("RM_BAK_CLN ")) {
    return WALOp{"RM_BAK_CLN", line.substr(11), ""};

  } else if (line.starts_with("DBRM ")) {
    std::string rest = line.substr(5);
    auto sp = rest.find(' ');
    if (sp == std::string::npos)
      return std::nullopt;
    return WALOp{"DBRM", rest.substr(0, sp), rest.substr(sp + 1)};

  } else if (line.starts_with("DBNEW ")) {
    std::string rest = line.substr(6);
    auto sp = rest.find(' ');
    if (sp == std::string::npos)
      return std::nullopt;
    return WALOp{"DBNEW", rest.substr(0, sp), rest.substr(sp + 1)};

  } else if (line.starts_with("DB ")) {
    std::string rest = line.substr(3);
    auto sp = rest.find(' ');
    if (sp == std::string::npos)
      return std::nullopt;
    return WALOp{"DB", rest.substr(0, sp), rest.substr(sp + 1)};
  }

  return std::nullopt;
}

std::pair<int, int> reverse_execute(const std::vector<WALOp> &ops,
                                    const fs::path & /*root*/) {
  int restored = 0;
  int cleaned = 0;

  for (int i = static_cast<int>(ops.size()) - 1; i >= 0; --i) {
    const auto &op = ops[i];
    std::error_code ec;

    if (op.type == "BACKUP" || op.type == "REMOVE_OLD") {
      // BACKUP <src> → <dst>  /  REMOVE_OLD <src> → <dst>
      // 恢复备份：dst → src
      if (fs::exists(op.arg2) || fs::is_symlink(op.arg2)) {
        fs::rename(op.arg2, op.arg1, ec);
        if (!ec) {
          fsync_parent_dir(op.arg1);
          TransactionLog::log_raw("RESTORE " + op.arg2 + " → " + op.arg1);
          restored++;
        } else {
          log_warning(string_format("warning.recover_rename_failed", op.arg2,
                                    op.arg1, ec.message()));
        }
      }

    } else if (op.type == "COPY") {
      // COPY <src> → <dst>
      // 反向：删除 dst（已复制过去的目标），清理 src（.lpkgtmp 临时文件）
      //
      // 安全检查：如果 dst 文件存在，检查正向顺序中是否有对应的 BACKUP 操作
      // 将同一路径备份到 .lpkg_bak。如果有且备份文件已不存在，说明此 COPY
      // 已被回滚过——dst 是 BACKUP 恢复回来的文件而非新写入的文件，不能删除。
      bool removed = false;
      if (fs::exists(op.arg2) || fs::is_symlink(op.arg2)) {
        bool skip_remove = false;
        // 在反向遍历时，正向顺序中在 COPY 之前的 ops 位于 [0, i-1]。
        // BACKUP 在正向顺序中在 COPY 之前（先备份再复制），
        // 所以匹配的 BACKUP 需要在 [0, i-1] 中查找。
        for (int j = 0; j < i; ++j) {
          if (ops[j].type == "BACKUP" && ops[j].arg1 == op.arg2) {
            if (!fs::exists(ops[j].arg2)) {
              // 备份文件已不存在（已被 BACKUP 恢复消费），
              // 说明此 COPY 反向已被执行过——跳过
              log_warning(string_format("warning.recover_skip_copy", op.arg2));
              skip_remove = true;
            }
            break;
          }
        }
        if (!skip_remove) {
          fs::remove(op.arg2, ec);
          if (!ec) {
            cleaned++;
            removed = true;
          }
        }
      }
      if (fs::exists(op.arg1)) {
        fs::remove(op.arg1, ec);
        if (!ec) {
          cleaned++;
          if (!removed)
            log_info(string_format("info.recover_cleaned_tmp", op.arg1));
        }
      }

    } else if (op.type == "NEW") {
      // NEW <path>
      if (fs::exists(op.arg1) || fs::is_symlink(op.arg1)) {
        fs::remove(op.arg1, ec);
        if (!ec) {
          log_info(string_format("info.recover_cleaned_tmp", op.arg1));
          cleaned++;
        }
      }

    } else if (op.type == "NEW_DIR") {
      // NEW_DIR <path> — 反向时内部文件已在之前被个体条目清理
      const std::string &path = op.arg1;
      if (fs::exists(path) && fs::is_directory(path) && !fs::is_symlink(path)) {
        // 清扫目录内的 .lpkg_bak 残留
        std::error_code ec_scan;
        for (auto &entry : fs::recursive_directory_iterator(path, ec_scan)) {
          const std::string fname = entry.path().filename().string();
          if (fname.find(".lpkg_bak_") != std::string::npos) {
            fs::remove(entry.path(), ec_scan);
          }
        }
        std::error_code ec_empty;
        if (fs::is_empty(path, ec_empty) && !ec_empty) {
          if (fs::remove(path, ec) && !ec) {
            log_info(string_format("info.recover_cleaned_tmp", path));
            cleaned++;
          }
        } else if (!ec_empty && !fs::is_empty(path, ec_empty)) {
          log_warning(string_format("warning.recover_dir_not_empty", path));
        }
      }

    } else if (op.type == "RM_DIR") {
      // RM_DIR <path> — 回滚时重建目录，使 BACKUP rename 能成功
      std::error_code ec_create;
      fs::create_directories(op.arg1, ec_create);
      if (!ec_create) {
        log_info(string_format("info.recover_restored", "(dir)", op.arg1));
        restored++;
      }

    } else if (op.type == "RM_BAK_CLN") {
      // RM_BAK_CLN <path> — 反向操作无需复位，跳过
      continue;

    } else if (op.type == "DBRM") {
      // DBRM <dbpath> <tag> — 回滚时从 .lpkg_db_bak_<tag> 恢复
      fs::path bak = op.arg1 + ".lpkg_db_bak_" + op.arg2;
      if (fs::exists(bak)) {
        fs::rename(bak, op.arg1, ec);
        if (!ec) {
          fsync_parent_dir(op.arg1);
          log_info(string_format("info.recover_restored", op.arg1, "(db_bak)"));
          restored++;
        } else {
          log_warning(string_format("warning.recover_rename_failed",
                                    bak.string(), op.arg1, ec.message()));
        }
      }

    } else if (op.type == "DBNEW") {
      // DBNEW <dbpath> <tag> — 未提交则删除新建的 DB 文件
      if (fs::exists(op.arg1) || fs::is_symlink(op.arg1)) {
        fs::remove(op.arg1, ec);
        if (!ec) {
          log_info(string_format("info.recover_cleaned_tmp", op.arg1));
          cleaned++;
        }
      }

    } else if (op.type == "DB") {
      // DB <dbpath> <tag> — 从 .lpkg_db_bak_<tag> 恢复
      fs::path bak = op.arg1 + ".lpkg_db_bak_" + op.arg2;
      if (fs::exists(bak)) {
        fs::rename(bak, op.arg1, ec);
        if (!ec) {
          fsync_parent_dir(op.arg1);
          log_info(string_format("info.recover_restored", op.arg1, "(db_bak)"));
          restored++;
        } else {
          log_warning(string_format("warning.recover_rename_failed",
                                    bak.string(), op.arg1, ec.message()));
        }
      }
    }
  }

  return {restored, cleaned};
}

} // namespace wal
