#pragma once

#include "../base/exception.hpp"
#include "../config/config.hpp"
#include "../i18n/localization.hpp"
#include "cache.hpp"
#include "test_breakpoints.hpp"
#include "transaction_log.hpp"
#include "wal_op.hpp"

#include <functional>
#include <string>
#include <vector>

/**
 * run_batch_transaction — 统一批量事务执行器。
 *
 * 事务协议：
 *
 *   正向路径：
 *     BEGIN_PKGS N → Cache::write(":batch-start") → execute(batch_writer)
 *     → 逐包 Cache::write(pkg + ":installed") → COMMIT_PKGS
 *
 *   异常路径（catch）：
 *     execute() 抛异常
 *     ├── batch_rollback(success)         ← 回滚所有已成功包
 *     │     ├── reverse_execute(ops)
 *     │     ├── Cache::load()             ← 从磁盘重载恢复的 DB
 *     │     ├── DB /pkgs :batch-start
 *     │     ├── ROLLBACK/END 标记
 *     │     └── COMMIT_PKGS
 *     └── rethrow
 *
 * 不变量：
 *   - 进入前 WAL 已 trim_completed，无未完成事务
 *   - BEGIN_PKGS 写入 + fsync 后，异常路径保证 COMMIT_PKGS 被写入
 *   - COMMIT_PKGS 是批次完结的唯一标记
 *
 * 模板参数 OpT 是一个可调用对象 OpT(WalWriter&)，负责执行包级操作。
 * 它接收 WalWriter 引用用于写入 WAL 行。
 *
 * @param total       批次中包的数量
 * @param op          包级操作的可调用对象
 * @return            成功安装的包名列表
 * @throws            在操作失败时重新抛出，回滚后再抛
 */
template <typename OpT>
std::vector<std::string> run_batch_transaction(size_t total, OpT &&op) {
  // 前提：进入前 trim_completed（在顶层调用者如 install_packages 中执行）
  trim_completed();

  auto &cache = Cache::instance();
  std::vector<std::string> successfully_installed;

  try {
    // 批次开始
    wal::WalWriter batch_writer = wal::begin_batch(total);

    // 保存批次开始前的 DB 状态
    // 注意：write() 内部执行 WAL→备份→.tmp→rename→fsync 序列
    cache.write(":batch-start");

    // 执行包级操作
    std::forward<OpT>(op)(batch_writer, successfully_installed);

    // 批次提交
    wal::commit_batch();

    return successfully_installed;
  } catch (const LpkgException &) {
    // 批次回滚
    wal::batch_rollback(successfully_installed);
    // 回滚完成（COMMIT_PKGS 已写），清理 DB 备份
    cleanup_db_backups();
    trim_completed();
    throw;
  } catch (const std::exception &) {
    wal::batch_rollback(successfully_installed);
    cleanup_db_backups();
    trim_completed();
    throw;
  }
}

/**
 * 便捷包装：对已构建的 target 列表执行单一类型的包操作
 *
 * @param targets         要处理的目标列表
 * @param per_pkg_fn      每个包的处理函数 (ctx, pkg_name, pkg_version,
 * wal_writer)
 * @param install_order   安装顺序（依赖拓扑序）
 * @param get_plan_fn     获取包的 InstallPlan 的函数
 * @return                成功处理的包名列表
 */
template <typename PlanMap, typename PerPkgFn>
std::vector<std::string>
run_ordered_batch(const std::vector<std::string> &install_order, PlanMap &plan,
                  PerPkgFn &&per_pkg_fn) {
  return run_batch_transaction(
      install_order.size(),
      [&](wal::WalWriter &w, std::vector<std::string> &success) {
        auto &cache = Cache::instance();

        for (const auto &name : install_order) {
          auto &p = plan.at(name);

          // 执行包级操作
          std::forward<PerPkgFn>(per_pkg_fn)(w, p, success);

          // 包完成后 DB 里程碑
          cache.write(name + ":installed");
          success.push_back(name);
        }
      });
}
