#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../base/exception.hpp"
#include "../config/config.hpp"
#include "../i18n/localization.hpp"
#include "cache.hpp"
#include "test_breakpoints.hpp"
#include "transaction_log.hpp"
#include "wal_op.hpp"

/**
 * run_batch_transaction — 统一批量事务执行器。
 *
 * 事务协议：
 *
 *   正向路径：
 *     BEGIN_PKGS → Cache::write(":batch-start") → execute()
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
 * 模板参数 OpT 是一个可调用对象 OpT(std::vector<std::string>& success)，
 * 负责执行包级操作；包级 WAL 写入统一走 wal::log_wal_line()。
 * （曾向 OpT 传 WalWriter& 但调用方从未使用——所有写都走 log_wal_line，
 *   持有无用 fd 反而迷惑，故移除。）
 *
 * @param op          包级操作的可调用对象
 * @return            成功安装的包名列表
 * @throws            在操作失败时重新抛出，回滚后再抛
 */
template <typename OpT>
std::vector<std::string> run_batch_transaction(OpT&& op)
{
    // 前提：进入前 trim_completed（在顶层调用者如 install_packages 中执行）
    trim_completed();

    auto& cache = Cache::instance();
    std::vector<std::string> successfully_installed;

    try {
        // 批次开始（BEGIN_PKGS 不带包数——批次开启时无法预知最终包数，
        // 且恢复逻辑不读取该数）。直接写 WAL 行，无需持有 WalWriter。
        wal::log_wal_line("BEGIN_PKGS");

        // 保存批次开始前的 DB 状态
        // 注意：write() 内部执行 WAL→备份→.tmp→rename→fsync 序列
        cache.write(":batch-start");

        // 执行包级操作
        std::forward<OpT>(op)(successfully_installed);

        // 批次提交
        wal::commit_batch();

        return successfully_installed;
    } catch (const std::exception&) {
        // LpkgException 是 std::runtime_error 的子类，一并覆盖。
        // 批次回滚 → 回滚完成（COMMIT_PKGS 已写）→ 清理 DB 备份 → 重抛原异常。
        try {
            // CLEANUP 感知：一旦批次已进入 CLEANUP 阶段（remove 的 .lpkg_bak 清理开始，
            // 即该批次包含的所有 remove 都已 RM_COMMIT、DB 已落盘），系统状态稳定，
            // 只剩 .lpkg_bak 临时文件待清——**不回滚**，走 continue_cleanup 续删+提交
            // （移除保持最终）。回滚会恢复 DB 但被删的 bak 回不来 → 不一致；且
            // "bak 是否被删"无法可靠判定（父目录被删/dangling 路径都会让 exists 误判）。
            // 崩溃路径的 recover_packages 已有同一判断；这里补上异常路径的同一判断。
            const auto ops = wal::extract_current_batch_ops(wal::wal_log_path());
            bool has_cleanup = false;
            for (const auto& op : ops) {
                if (op.type == wal::WALOpType::CLEANUP) {
                    has_cleanup = true;
                    break;
                }
            }
            if (has_cleanup) {
                wal::continue_cleanup(ops);
            } else {
                wal::batch_rollback(successfully_installed);
            }
            cleanup_db_backups();
            trim_completed();
        } catch (...) {
            // **回滚自身失败**（如 reverse_execute 的 safe_rename 中途报错）：
            // 绝不清理 DB 备份、不 trim——保留 WAL 的未提交批次与全部
            // .lpkg_db_bak_before:* / .lpkg_bak，交给下次 recover_packages 幂等续传。
            // 曾无条件执行 cleanup_db_backups()：reverse_execute 尚未消费的
            // DB 备份被删 → 恢复时 DB 无法还原，磁盘文件与 DB 永久不一致。
        }
        throw;
    }
}

// （曾提供 run_ordered_batch 便捷包装，但从未被任何调用点使用，已移除。）

