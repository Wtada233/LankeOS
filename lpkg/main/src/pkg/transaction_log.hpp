#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

/**
 * WAL 风格的事务日志。记录每个文件操作的意图和结果。
 * 日志写入 Config::instance().lock_dir() / "transaction.log"
 *
 * 每行使用单次 write() 写入，在 O_APPEND 模式下 < 4096 字节即为原子写入。
 * 断电不会损坏已有行——最多丢失最后一行（部分写入行不合法，在恢复时被忽略）。
 *
 * 格式：每行一个操作
 *   BEGIN <pkg> <version>         事务开始
 *   BACKUP <src> → <dst>          备份文件 → .lpkgbak
 *   COPY <src> → <dst>            复制文件（.lpkgtmp → rename）
 *   NEW <path>                    新文件（回滚时需删除）
 *   COMMIT <pkg> <version>        事务提交（包注册完成）
 *   ROLLBACK <pkg> <version>      事务回滚
 *   END <pkg> <version>           事务结束
 */
class TransactionLog {
public:
  TransactionLog();
  ~TransactionLog();

  void begin(const std::string &pkg, const std::string &version);
  void backup(const fs::path &src, const fs::path &dst);
  void copy(const fs::path &src, const fs::path &dst);
  void new_file(const fs::path &path);
  void commit(const std::string &pkg, const std::string &version);
  void rollback(const std::string &pkg, const std::string &version);
  void end(const std::string &pkg, const std::string &version);

  /** 检查是否有未完成事务，返回未提交的包名（空串表示无） */
  static std::string check_pending();

  /**
   * 压缩日志：删除所有已完结事务的记录，保留未完成事务。
   *
   * 调用时机：每项新事务开始时（install/remove/upgrade 之前）。
   * 已完结事务的判断规则：
   *   - INSTALL:  BEGIN → COMMIT + END 或 ROLLBACK + END
   *   - REMOVE:   RM_BEGIN → RM_COMMIT + RM_END
   *   - BATCH:    BEGIN_PKGS → COMMIT_PKGS（内部所有子事务自动完结）
   * 未完结的事务及其之前的所有行均保留，以确保恢复状态机能够正确定位回滚起点。
   * 若日志中无任何未完结事务，则完全清空日志文件。
   *
   * 安全性：使用 .tmp + rename 替换，替换过程中断电不会损坏原文件。
   *         不删除未完成事务的任一行，确保 rec 恢复所需上下文完整。
   */
  static void trim_completed();

  /** 直接写入一行日志（不加时间戳，用于在无 TransactionLog 实例处写日志） */
  static void log_raw(const std::string &line);

  /** 写入一行日志但不 fsync（用于非关键路径，如回滚过程中的追加日志） */
  static void log_raw_no_fsync(const std::string &line);

private:
  int log_fd_ = -1;
  bool opened_ok_ = false;
  fs::path log_path_;

  void write(const std::string &line);
  static std::string timestamp();
};
