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

// 事务类型
enum class TxnType { NONE, INSTALL, REMOVE, BATCH };

/**
 * 从日志行中提取操作线内容（去掉时间戳前缀 [YYYY-MM-DD HH:MM:SS]）
 */
static std::string_view strip_timestamp(const std::string_view line) {
    auto ts_end = line.find(']');
    if (ts_end == std::string::npos) return "";
    return line.substr(ts_end + 2); // 跳过 "] "
}

/**
 * 从事务的第一行提取包名
 * "BEGIN pkg ver" → "pkg"
 * "RM_BEGIN pkg ver" → "pkg"
 * "BEGIN_PKGS n" → "batch"
 */
static std::string extract_pkg_name(const std::string& first_op) {
    std::stringstream ss(first_op);
    std::string type, name;
    ss >> type; // "BEGIN" / "RM_BEGIN" / "BEGIN_PKGS"
    if (type == "BEGIN_PKGS") return "batch";
    ss >> name;
    return name;
}

/**
 * lpkg rec — 从中断的事务中恢复系统状态。
 *
 * 读取 WAL 事务日志（transaction.log），查找未完成的事务
 * （单包：BEGIN 后没有 COMMIT/END；移除：RM_BEGIN 后没有 RM_COMMIT；
 *  批量：BEGIN_PKGS 后没有 COMMIT_PKGS），然后回滚。
 *
 * 完全基于 WAL 日志恢复，不扫描文件系统。
 *
 * 逆向执行委托给 wal::reverse_execute()，与 InstallationTask::rollback()
 * 共享同一套撤销逻辑。
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

    // ── 状态机：逐行扫描，按事务类型积累操作 ──────────────────────────
    // 事务类型：
    //   INSTALL:  BEGIN <pkg> → ... → COMMIT/ROLLBACK+END → 完成；否则未提交
    //   REMOVE:   RM_BEGIN <pkg> → ... → RM_COMMIT/RM_END → 完成；否则未提交
    //   BATCH:    BEGIN_PKGS <n> → ... → COMMIT_PKGS → 完成；否则整个批量未提交
    // 在 BATCH 模式下，内部的 COMMIT/END 不清除积累的操作——只有 COMMIT_PKGS 可以。
    TxnType txn_type = TxnType::NONE;
    std::vector<std::string> current_ops;
    std::vector<std::vector<std::string>> uncommitted_txns;

    std::string raw_line;
    while (std::getline(f, raw_line)) {
        std::string_view content = strip_timestamp(raw_line);
        if (content.empty()) continue;

        if (content.starts_with("BEGIN_PKGS ")) {
            // 开始批量事务
            if (txn_type != TxnType::NONE && !current_ops.empty())
                uncommitted_txns.push_back(std::move(current_ops));
            txn_type = TxnType::BATCH;
            current_ops.clear();
            current_ops.push_back(std::string(content));

        } else if (content.starts_with("COMMIT_PKGS")) {
            // 批量提交成功——清空积累的操作
            if (txn_type == TxnType::BATCH) {
                txn_type = TxnType::NONE;
                current_ops.clear();
            }

        } else if (content.starts_with("RM_BEGIN ")) {
            // 开始移除事务
            if (txn_type == TxnType::NONE) {
                if (!current_ops.empty())
                    uncommitted_txns.push_back(std::move(current_ops));
                txn_type = TxnType::REMOVE;
                current_ops.clear();
            }
            current_ops.push_back(std::string(content));

        } else if (content.starts_with("RM_COMMIT ") || content.starts_with("RM_END ")) {
            if (txn_type == TxnType::REMOVE) {
                // 移除事务正常结束
                txn_type = TxnType::NONE;
                current_ops.clear();
            } else if (txn_type == TxnType::BATCH) {
                // 在批量事务内，仅积累
                current_ops.push_back(std::string(content));
            }

        } else if (content.starts_with("BEGIN ")) {
            if (txn_type == TxnType::NONE) {
                // 新单包事务开始
                if (!current_ops.empty())
                    uncommitted_txns.push_back(std::move(current_ops));
                txn_type = TxnType::INSTALL;
                current_ops.clear();
            }
            current_ops.push_back(std::string(content));

        } else if (content.starts_with("COMMIT ") || content.starts_with("ROLLBACK ")) {
            if (txn_type == TxnType::INSTALL) {
                // 单包事务正常结束
                txn_type = TxnType::NONE;
                current_ops.clear();
            } else if (txn_type == TxnType::BATCH) {
                // 批量事务内：积累但不清除
                current_ops.push_back(std::string(content));
            }

        } else if (content.starts_with("END ")) {
            if (txn_type == TxnType::INSTALL) {
                txn_type = TxnType::NONE;
                current_ops.clear();
            } else if (txn_type == TxnType::BATCH) {
                current_ops.push_back(std::string(content));
            }

        } else if (txn_type != TxnType::NONE) {
            // BACKUP / COPY / NEW / 其他操作行
            current_ops.push_back(std::string(content));
        }
    }

    // 文件末尾仍有未完成事务
    if (txn_type != TxnType::NONE && !current_ops.empty())
        uncommitted_txns.push_back(std::move(current_ops));

    if (uncommitted_txns.empty()) {
        log_info(get_string("info.recover_no_pending"));
        Cache::instance().cleanup_db_backups();
        return;
    }

    // ── 回滚每个未完成事务（委托给 wal::reverse_execute） ─────────────
    for (const auto& txn_ops : uncommitted_txns) {
        std::string pkg_name;
        if (!txn_ops.empty())
            pkg_name = extract_pkg_name(txn_ops[0]);

        log_info(string_format("info.recover_rollback_txn",
                                pkg_name.empty() ? "unknown" : pkg_name));

        // 将原始日志行解析为 WALOp 列表（parse_op 会跳过格式不匹配的行）
        std::vector<wal::WALOp> ops;
        for (const auto& line : txn_ops) {
            if (auto parsed = wal::parse_op(line))
                ops.push_back(std::move(*parsed));
        }

        // 核心：共享的逆向执行
        auto [r, c] = wal::reverse_execute(ops, Config::instance().root_dir());
        restored += r;
        cleaned += c;

        // 标记事务已回滚
        if (!pkg_name.empty()) {
            TransactionLog::log_raw_no_fsync("ROLLBACK " + pkg_name + " (recovery)");
            TransactionLog::log_raw_no_fsync("END " + pkg_name + " (recovery)");
        }
    }

    // 清理残留的 .lpkg_db_bak 文件：包括已提交事务遗留的、以及回滚过程中
    // 恢复后未清理的备份
    Cache::instance().cleanup_db_backups();

    log_info(string_format("info.recover_done", restored, cleaned));
}
