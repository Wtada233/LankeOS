#include "package_manager.hpp"
#include "transaction_log.hpp"
#include "config/config.hpp"
#include "db/cache.hpp"
#include "i18n/localization.hpp"
#include "base/utils.hpp"
#include "base/constants.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

#include "base/exception.hpp"

namespace fs = std::filesystem;

// WAL 日志中使用的箭头分隔符（UTF-8 编码，与 transaction_log.cpp 保持一致）
// " \xe2\x86\x92 " = space + U+2192 + space = 5 字节
static constexpr std::string_view ARROW_SEP = " \xe2\x86\x92 ";
static constexpr size_t ARROW_SEP_LEN = 5;

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
 * 回滚逻辑：
 *   - BACKUP <src> → <dst>：将备份文件 <dst> 重命名回 <src>（恢复旧文件）
 *   - COPY <src> → <dst>：删除已复制到 <dst> 的文件，清理 <src> 临时文件
 *   - NEW <path>：删除新创建的文件
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

    // ── 回滚每个未完成事务 ──────────────────────────────────────────────
    for (const auto& txn_ops : uncommitted_txns) {
        std::string pkg_name;
        if (!txn_ops.empty())
            pkg_name = extract_pkg_name(txn_ops[0]);

        log_info(string_format("info.recover_rollback_txn",
                                pkg_name.empty() ? "unknown" : pkg_name));

        // 反向回滚（后执行的操作先撤销）
        for (int i = static_cast<int>(txn_ops.size()) - 1; i >= 0; --i) {
            const std::string& op = txn_ops[i];

            // 跳过非操作行
            if (op.starts_with("BEGIN ") || op.starts_with("BEGIN_PKGS ") ||
                op.starts_with("COMMIT ") || op.starts_with("ROLLBACK ") ||
                op.starts_with("END ") ||
                op.starts_with("RM_BEGIN ") || op.starts_with("RM_COMMIT ") ||
                op.starts_with("RM_END ") ||
                op.starts_with("COMMIT_PKGS"))
                continue;

            std::error_code ec;

            if (op.starts_with("BACKUP ")) {
                // BACKUP <src> → <dst>
                // 恢复备份：dst → src
                auto arrow_pos = op.find(ARROW_SEP, 7);
                if (arrow_pos == std::string::npos) continue;

                std::string src = op.substr(7, arrow_pos - 7);
                std::string dst = op.substr(arrow_pos + ARROW_SEP_LEN);

                if (fs::exists(dst) || fs::is_symlink(dst)) {
                    fs::rename(dst, src, ec);
                    if (!ec) {
                        log_info(string_format("info.recover_restored", dst, src));
                        restored++;
                    } else {
                        log_warning(string_format("warning.recover_rename_failed",
                                                  dst, src, ec.message()));
                    }
                }
            } else if (op.starts_with("REMOVE_OLD ")) {
                // REMOVE_OLD <src> → <dst>
                // 恢复旧版本升级时被移除的废弃文件
                auto arrow_pos = op.find(ARROW_SEP, 11);
                if (arrow_pos == std::string::npos) continue;

                std::string src = op.substr(11, arrow_pos - 11);
                std::string dst = op.substr(arrow_pos + ARROW_SEP_LEN);

                if (fs::exists(dst) || fs::is_symlink(dst)) {
                    fs::rename(dst, src, ec);
                    if (!ec) {
                        log_info(string_format("info.recover_restored", dst, src));
                        restored++;
                    } else {
                        log_warning(string_format("warning.recover_rename_failed",
                                                  dst, src, ec.message()));
                    }
                }
            } else if (op.starts_with("COPY ")) {
                // COPY <src> → <dst>
                auto arrow_pos = op.find(ARROW_SEP, 5);
                if (arrow_pos == std::string::npos) continue;

                std::string src = op.substr(5, arrow_pos - 5);
                std::string dst = op.substr(arrow_pos + ARROW_SEP_LEN);

                bool removed = false;
                if (fs::exists(dst) || fs::is_symlink(dst)) {
                    fs::remove(dst, ec);
                    if (!ec) {
                        cleaned++;
                        removed = true;
                    }
                }
                if (fs::exists(src)) {
                    fs::remove(src, ec);
                    if (!ec) {
                        cleaned++;
                        if (!removed)
                            log_info(string_format("info.recover_cleaned_tmp", src));
                    }
                }
            // ── DB 文件 WAL 回滚 ────────────────────────────────────────
            // 这三种条目使用相同的 .lpkg_db_bak 机制：
            //   DB /path tag    — 修改已有 DB 文件（备份 → 写 → 替换）
            //   DBNEW /path tag — 新建 DB 文件（不存在则创建）
            //   DBRM /path tag  — 删除 DB 文件（备份后删除）
            // 恢复只在 .lpkg_db_bak 存在时进行（文件系统状态指示操作已发生）。
            // 若 .bak 不存在，说明 crash 发生在 DB_WAL 条目写入后但实际操作前，
            // 此时文件系统未变，无需恢复——WAL 条目仅标记意图，无副效应。
            //
            // 注意：这三种条目均不会匹配到 "DB " 前缀
            // （DBRM/DBNEW 分别以 "DBRM " / "DBNEW " 开头），依赖检查顺序正确。
            // ─────────────────────────────────────────────────────────────────

            } else if (op.starts_with("DBRM ")) {
                // DBRM /path tag — 文件曾存在，被备份到 .lpkg_db_bak_<tag> 后删除
                // 格式："DBRM /var/lib/lpkg/pkgs pkg1"
                {
                    std::string rest = op.substr(5);  // 去掉 "DBRM "
                    auto sp = rest.find(' ');
                    if (sp != std::string::npos) {
                        std::string dbpath = rest.substr(0, sp);
                        std::string tag = rest.substr(sp + 1);
                        fs::path bak = dbpath + ".lpkg_db_bak_" + tag;
                        if (fs::exists(bak)) {
                            fs::rename(bak, dbpath, ec);
                            if (!ec) {
                                log_info(string_format("info.recover_restored", dbpath, "(db_bak)"));
                                restored++;
                            } else {
                                log_warning(string_format("warning.recover_rename_failed", bak.string(), dbpath, ec.message()));
                            }
                        }
                    }
                }
            } else if (op.starts_with("DBNEW ")) {
                // DBNEW /path tag — 新建数据库文件，未提交则删除
                {
                    std::string rest = op.substr(6);  // 去掉 "DBNEW "
                    auto sp = rest.find(' ');
                    if (sp != std::string::npos) {
                        std::string dbpath = rest.substr(0, sp);
                        if (fs::exists(dbpath) || fs::is_symlink(dbpath)) {
                            fs::remove(dbpath, ec);
                            if (!ec) {
                                log_info(string_format("info.recover_cleaned_tmp", dbpath));
                                cleaned++;
                            }
                        }
                    }
                }
            } else if (op.starts_with("DB ")) {
                // DB /path tag — 修改已有 DB 文件。恢复 .lpkg_db_bak_<tag> 到原位。
                {
                    std::string rest = op.substr(3);  // 去掉 "DB "
                    auto sp = rest.find(' ');
                    if (sp != std::string::npos) {
                        std::string dbpath = rest.substr(0, sp);
                        std::string tag = rest.substr(sp + 1);
                        fs::path bak = dbpath + ".lpkg_db_bak_" + tag;
                        if (fs::exists(bak)) {
                            fs::rename(bak, dbpath, ec);
                            if (!ec) {
                                log_info(string_format("info.recover_restored", dbpath, "(db_bak)"));
                                restored++;
                            } else {
                                log_warning(string_format("warning.recover_rename_failed", bak.string(), dbpath, ec.message()));
                            }
                        }
                    }
                }

            } else if (op.starts_with("RM_DIR ")) {
                // RM_DIR <path> — 回滚时重建目录，使 BACKUP rename 能成功
                std::string dirpath = op.substr(6);
                fs::create_directories(dirpath, ec);
                if (!ec) {
                    log_info(string_format("info.recover_restored", "(dir)", dirpath));
                    restored++;
                }
            } else if (op.starts_with("NEW_DIR ")) {
                // NEW_DIR <path> — 新建目录。反向顺序中内部文件已在之前被
                // 个体条目（NEW/COPY/BACKUP）清理，此时目录应为空。
                // 空才删（fs::remove = rmdir），非空则警告用户而不强制铲除，
                // 避免 WAL 中 NEW/COPY 条目的记录与文件系统状态不一致。
                std::string path = op.substr(8);
                if (fs::exists(path) && fs::is_directory(path) && !fs::is_symlink(path)) {
                    std::error_code ec_empty;
                    if (fs::is_empty(path, ec_empty) && !ec_empty) {
                        if (fs::remove(path, ec) && !ec) {
                            TransactionLog::log_raw_no_fsync("ROLLBACK_DEL " + path);
                            log_info(string_format("info.recover_cleaned_tmp", path));
                            cleaned++;
                        }
                    } else if (!ec_empty && !fs::is_empty(path, ec_empty)) {
                        log_warning(string_format("warning.recover_dir_not_empty", path));
                    }
                }
            } else if (op.starts_with("NEW ")) {
                // NEW <path>
                std::string path = op.substr(4);
                if (fs::exists(path) || fs::is_symlink(path)) {
                    fs::remove(path, ec);
                    if (!ec) {
                        log_info(string_format("info.recover_cleaned_tmp", path));
                        cleaned++;
                    }
                }
            }
        }

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
