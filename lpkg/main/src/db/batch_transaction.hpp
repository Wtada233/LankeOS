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
 *     → Cache::write(":batch-end") → COMMIT_PKGS
 *
 *   `:batch-end` 那一次由 **op 自己在末尾调用**（执行器不认识 Cache），全批次**只写一次**。
 *   曾经是**逐包** `Cache::write(pkg + ":installed")`：每包把 6 个 DB 文件全量重写一遍、
 *   各留一份全量备份，本机实测 `files.db` 19.8 MB ⇒ 100 包批次落下 ~2 GB 临时备份。
 *   改为一次的依据有两条，都是查实的：① **批次进行中没有任何读取器读盘上的 DB**
 *   （`Cache::load()` 的调用点全在批次之外，循环内一律走内存 `Cache`）；② 未提交批次
 *   **一律整体回滚**，所以中途的盘上状态既不可观测、也不可能成为最终状态。
 *   崩溃窗口逐条对过仍然安全：提交前崩溃 ⇒ 该行不存在、官方文件从未被碰；写 `:batch-end`
 *   的中途崩溃 ⇒ 普通的 `DbBakExists` 守卫处理（不是 `:batch-start` 那条跳过判据）。
 *   ⚠️ 里程碑名**不能**用 `:batch-start`：`batch_start_db_still_in_place` 会把它判成
 *   "可跳过"，批次后的 DB 就永远回滚不回来了。
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
    // 前提：进入前 WAL 无未完成事务（顶层调用者 = `main_cli.cpp` 的 `init_database_for`，
    // 它先 recover_packages）。
    // 这里再 trim 一次已完成的批次，保证新批次从干净的日志开始。
    trim_completed();

    // **入口守卫（2026-10-03 补）**：上面那条"前提"此前只是**假设** —— 调用方不一定做得到。
    // `recover_packages()` 在"有撤销动作真的没成功"时会**故意不封口**（留给下次 rec 重做），
    // 而 `init_database_for()` 不返回恢复成败、同进程继续执行用户命令 ⇒ 新批次就开在了未封口的
    // WAL 上，造出 `BEGIN₁ …(未封口) BEGIN₂ … COMMIT₂` 这种"已提交批次**嵌套**在未提交批次里"
    // 的形状。下一轮恢复的配对记账会因此把**已提交那一批**也回滚掉（静默撤销上一轮成功的安装）。
    // fail-closed：点名原因、让用户先处理那个撤不掉的路径，再用 `lpkg rec` 重试 —— 宁可拒绝
    // 执行，也不把"已提交批次被连带回滚"的风险留给下一轮。
    if (wal_has_unpaired_batch()) {
        throw LpkgException(get_string("error.wal_unpaired_batch_blocks_new_batch"));
    }

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
            // 未提交批次**一律回滚**：CLEANUP 只出现在事务之外（post-commit 收尾记录，
            // 由 finish_committed_batch 写），事务内不可能有 CLEANUP 行——因此不需要
            // "见到 CLEANUP 就不回滚"的分岔。旧版 lpkg 把 CLEANUP 写在批次内，其遗留 WAL
            // **不在支持范围**：lpkg 经 lpkg 升级时旧二进制会先 recover_packages() 处理掉
            // 遗留 WAL、新二进制才上线；手工替换二进制不受支持（ARCH.md §11.3）。
            wal::RollbackStats rs;
            if (wal::batch_rollback(successfully_installed, &rs)) {
                // 有撤销动作**真的没成功**（EROFS / immutable / 权限…）⇒ 批次虽已封口，
                // 但 DB 备份是**唯一还能重试的还原点**：留着它，下次 `lpkg rec` 或人工处理
                // 还有第二次机会。宁可留残留，也不删未还原的数据 —— 与
                // `purge_consumed_stashes` 的收敛判据同一取向。
                // 告警已由 `reverse_execute` 统一发出（那是唯一看得到全部失败行的地方）。
                if (rs.failures == 0) {
                    cleanup_db_backups();
                    trim_completed();
                }
            }
            // NOLINTNEXTLINE(bugprone-empty-catch) — 下面的注释就是理由：绝不清理，留给下次 rec
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
