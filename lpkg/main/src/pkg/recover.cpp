#include "package_manager.hpp"
#include "transaction_log.hpp"
#include "config/config.hpp"
#include "db/cache.hpp"
#include "i18n/localization.hpp"
#include "base/utils.hpp"
#include "base/constants.hpp"
#include "wal_op.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

#include "base/exception.hpp"

namespace fs = std::filesystem;

/**
 * 从日志行中提取操作内容（去掉时间戳前缀 [YYYY-MM-DD HH:MM:SS]）
 */
static std::string_view strip_timestamp(const std::string_view line) {
    auto ts_end = line.find(']');
    if (ts_end == std::string::npos) return "";
    return line.substr(ts_end + 2);
}

/**
 * lpkg rec — 从中断的事务中恢复系统状态。
 *
 * 统一事务模型——没有 INSTALL/REMOVE/BATCH 模式之分：
 *   BEGIN_PKGS <n>      → 事务开始
 *   中间所有行一律作为操作积累（包括 BEGIN、END、COMMIT、ROLLBACK、
 *     RM_BEGIN、RM_COMMIT、RM_END、BACKUP、COPY、NEW 等）
 *   COMMIT_PKGS          → 事务完结
 *
 * 所有操作——install、remove、upgrade、reinstall——都走这个模型。
 * 单个包安装是 BEGIN_PKGS 1，移除也由 BEGIN_PKGS 包裹。
 */
void recover_packages() {
    log_info(get_string("info.recover_start"));

    const fs::path log_path = Config::instance().lock_dir() / "transaction.log";
    int restored = 0, cleaned = 0;

    // 读取并解析事务日志
    std::ifstream f(log_path);
    if (!f.is_open()) {
        log_info(get_string("info.recover_no_log"));
        return;
    }

    // ── 统一状态机 ───────────────────────────────────────────────────────
    // 只识别两个标记：
    //   BEGIN_PKGS → 进入事务
    //   COMMIT_PKGS → 事务完结
    // 其他所有行在事务内一律积累，不做语义区分。
    bool in_txn = false;
    std::vector<std::string> current_ops;
    std::vector<std::vector<std::string>> uncommitted_txns;

    std::string raw_line;
    while (std::getline(f, raw_line)) {
        std::string_view content = strip_timestamp(raw_line);
        if (content.empty()) continue;

        if (content.starts_with("BEGIN_PKGS ")) {
            if (in_txn && !current_ops.empty())
                uncommitted_txns.push_back(std::move(current_ops));
            in_txn = true;
            current_ops.clear();
            current_ops.push_back(std::string(content));

        } else if (content.starts_with("COMMIT_PKGS")) {
            in_txn = false;
            current_ops.clear();

        } else if (in_txn) {
            // 事务内所有行一律积累
            current_ops.push_back(std::string(content));
        }
    }

    if (in_txn && !current_ops.empty())
        uncommitted_txns.push_back(std::move(current_ops));

    if (uncommitted_txns.empty()) {
        log_info(get_string("info.recover_no_pending"));
        Cache::instance().cleanup_db_backups();
        return;
    }

    // ── 回滚每个未完成事务（委托给 wal::reverse_execute） ─────────────
    for (const auto& txn_ops : uncommitted_txns) {
        log_info(string_format("info.recover_rollback_txn",
                                txn_ops.empty() ? "unknown"
                                : txn_ops[0]));

        // 将原始日志行解析为 WALOp 列表（parse_op 跳过格式不匹配的行）
        std::vector<wal::WALOp> ops;
        for (const auto& line : txn_ops) {
            if (auto parsed = wal::parse_op(line))
                ops.push_back(std::move(*parsed));
        }

        // 核心：共享的逆向执行
        auto [r, c] = wal::reverse_execute(ops, Config::instance().root_dir());
        restored += r;
        cleaned += c;

        // 统一写 COMMIT_PKGS 标记事务已完结
        // 只有 COMMIT_PKGS 能让状态机将事务视为完结，
        // ROLLBACK/END 等行在统一模型中只是普通操作行。
        TransactionLog::log_raw("COMMIT_PKGS");
    }

    // 清理残留的 .lpkg_db_bak 文件
    Cache::instance().cleanup_db_backups();

    log_info(string_format("info.recover_done", restored, cleaned));
}
