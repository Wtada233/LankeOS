#pragma once

#include <functional>
#include <string>
#include <vector>

/**
 * BreakpointManager — 测试断点注入框架。
 *
 * 仅在 Config::testing_mode() == true 时生效。
 * 允许测试代码在关键路径注入故障，验证主动回滚（batch_rollback）
 * 而非仅被动恢复（recover_packages）。
 *
 * 使用方式：
 *   BreakpointManager::instance().set("copy_pkg_file_3", []{
 *       throw LpkgException("injected disk full");
 *   });
 *   install_packages(...); // 会在复制第3个文件时触发断点 → 回滚
 *
 * 断点名称约定：
 *   backup_after_wal_<N>     BACKUP WAL 写入后、rename 前
 *   backup_after_rename_<N>  BACKUP rename 后
 *   copy_before_<N>          COPY 开始前
 *   copy_after_<N>           COPY rename 后
 *   commit_before            包 COMMIT 前
 *   file_<N>_of_<total>      处理到 N/total 个文件时
 */
class BreakpointManager {
public:
  static BreakpointManager &instance();

  /// 设置断点：当被调用时执行 action（通常抛异常）
  /// 每个断点只触发一次，之后自动清除
  void set(const std::string &name, std::function<void()> action);

  /// 触发断点（生产代码在关键路径调用）
  /// 返回 true 表示断点被触发（action 已执行）
  bool hit(const std::string &name);

  /// 移除指定断点
  void clear(const std::string &name);

  /// 移除所有断点
  void clear_all();

  /// 是否在 testing mode
  bool enabled() const;

private:
  BreakpointManager() = default;
  std::vector<std::pair<std::string, std::function<void()>>> breakpoints_;
};
