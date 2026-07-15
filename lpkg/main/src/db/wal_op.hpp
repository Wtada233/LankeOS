#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wal {

// ============================================================================
// DbMilestone — 确定性的 DB 状态标签
// ============================================================================

struct DbMilestone {
  std::string pkg;   // 包名，":batch-start" 时 pkg=""
  std::string state; // "installed" | "removed" | "batch-start"

  std::string to_string() const {
    if (pkg.empty())
      return ":" + state;
    return pkg + ":" + state;
  }

  static DbMilestone from_string(const std::string &s) {
    auto colon = s.find(':');
    if (colon == std::string::npos)
      return {"", s};
    if (colon == 0)
      return {"", s.substr(1)};
    return {s.substr(0, colon), s.substr(colon + 1)};
  }

  bool is_batch_start() const { return state == "batch-start"; }
};

// ============================================================================
// WALOp — 单个 WAL 操作行
// ============================================================================

enum class WALOpType {
  // 批次边界
  BEGIN_PKGS,  // BEGIN_PKGS <N>
  COMMIT_PKGS, // COMMIT_PKGS

  // 安装操作
  BEGIN,    // BEGIN <pkg> <ver>
  COMMIT,   // COMMIT <pkg> <ver>
  ROLLBACK, // ROLLBACK <pkg> <ver>
  END,      // END <pkg> <ver>

  // 文件操作
  BACKUP,     // BACKUP <src> → <dst>
  NEW,        // NEW <path>
  NEW_DIR,    // NEW_DIR <path>
  COPY,       // COPY <src> → <dst>
  REMOVE_OLD, // REMOVE_OLD <src> → <dst>

  // 移除操作
  RM_BEGIN,  // RM_BEGIN <pkg> <ver>
  RM_COMMIT, // RM_COMMIT <pkg> <ver>
  RM_END,    // RM_END <pkg> <ver>
  RM_DIR,    // RM_DIR <path> <mode> <uid> <gid>

  // DB 操作
  DB,    // DB <path> <milestone>
  DBNEW, // DBNEW <path> <milestone>
  DBRM,  // DBRM <path> <milestone>

  // 回滚审计 — 描述实际文件动作，而非正向操作名
  // 恢复备份: rename .bak → 原位
  RESTORE_FILE, // RESTORE_FILE <bak> → <orig>
  RESTORE_DB,   // RESTORE_DB <bak> → <db>
  RESTORE_DIR,  // RESTORE_DIR <path>
  // 删除操作: 无备份可恢复，直接删除以回退到安装前状态
  RESTORE_FILE_RM, // RESTORE_FILE_RM <path>   (COPY/NEW 逆操作)
  RESTORE_DIR_RM,  // RESTORE_DIR_RM <path>    (NEW_DIR 逆操作)
  RESTORE_DB_RM,   // RESTORE_DB_RM <path>     (DBNEW 无备份 逆操作)
  // 旧名称 — 仅用于解析旧 WAL 文件，不再写入
  REMOVE_FILE,  // 已废弃 → RESTORE_FILE_RM
  REMOVE_DIR,   // 已废弃 → RESTORE_DIR_RM
};

struct WALOp {
  WALOpType type;
  std::string raw;  // 原始行文本（调试用）
  std::string arg1; // 参数1
  std::string arg2; // 参数2
  std::string arg3; // 参数3
  std::string arg4; // 参数4（RM_DIR: gid，split_line 顺序分配）
  std::string arg5; // 参数5（预留）
  std::string arg6; // 参数6（预留）

  bool is_metadata() const {
    return type == WALOpType::ROLLBACK || type == WALOpType::END ||
           type == WALOpType::COMMIT || type == WALOpType::BEGIN ||
           type == WALOpType::RM_BEGIN || type == WALOpType::RM_COMMIT ||
           type == WALOpType::RM_END || type == WALOpType::BEGIN_PKGS ||
           type == WALOpType::COMMIT_PKGS;
  }

  bool is_restore_audit() const {
    return type == WALOpType::RESTORE_FILE || type == WALOpType::RESTORE_DB ||
           type == WALOpType::RESTORE_DIR || type == WALOpType::RESTORE_FILE_RM ||
           type == WALOpType::RESTORE_DIR_RM || type == WALOpType::RESTORE_DB_RM ||
           type == WALOpType::REMOVE_FILE ||   // 旧名称兼容
           type == WALOpType::REMOVE_DIR;      // 旧名称兼容
  }

  /// reverse_execute 需要跳过的行
  bool skip_in_reverse() const { return is_metadata() || is_restore_audit(); }
};

// ============================================================================
// 类型名转换
// ============================================================================

std::string_view walop_type_name(WALOpType t);
WALOpType walop_type_from_name(std::string_view name);

// ============================================================================
// WAL 行解析
// ============================================================================

/// 解析单行 WAL 日志为 WALOp
WALOp parse_op(const std::string &line);

// ============================================================================
// 回滚统计
// ============================================================================

struct RollbackStats {
  int files_restored = 0;
  int files_cleaned = 0;
  int dirs_recreated = 0;
  int db_restored = 0;
};

// ============================================================================
// 逆向执行引擎
// ============================================================================

/**
 * 逆向执行一组 WAL 操作。
 *
 * 对每条操作按类型执行逆向，每个操作后写入 RESTORE_* 审计行。
 * 跳过 RESTORE_x/REMOVE_x/元数据行和 :batch-start DB 条目。
 *
 * @param ops              待逆向执行的操作（正向顺序）
 * @param milestone_target
 * 目标里程碑（":batch-start"），达到后停止回滚。空=全部逆序
 * @param write_audit      是否写 RESTORE WAL 审计行
 * @return RollbackStats
 */
RollbackStats reverse_execute(const std::vector<WALOp> &ops,
                              const std::string &milestone_target = "",
                              bool write_audit = true);

// ============================================================================
// 批次操作提取
// ============================================================================

/// 从 WAL 日志文件提取当前（最后一个未完成的）批次的操作行列表
std::vector<WALOp> extract_current_batch_ops(const std::string &wal_path);

/// 从 WAL 日志文件中提取所有未完成批次的列表
/// 返回每批的操作行（从最后一个 BEGIN_PKGS 到文件末尾）
std::vector<std::vector<WALOp>>
extract_all_uncommitted_batches(const std::string &wal_path);

// ============================================================================
// 批次回滚
// ============================================================================

/**
 * 完整的批次回滚。
 * 1. 提取当前批次 WAL 行
 * 2. 清理内存 cache
 * 3. reverse_execute(ops, ":batch-start")
 * 4. DB /pkgs :batch-start
 * 5. ROLLBACK pkg + END pkg 对每个已回滚包
 * 6. COMMIT_PKGS
 */
void batch_rollback(const std::vector<std::string> &successfully_installed);

// ============================================================================
// WAL 文件路径
// ============================================================================

/// 获取 WAL 日志文件的路径
std::string wal_log_path();

} // namespace wal
