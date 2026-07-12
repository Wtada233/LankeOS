#include "package_manager.hpp"
#include "transaction_log.hpp"
#include "config/config.hpp"
#include "db/cache.hpp"
#include "i18n/localization.hpp"
#include "base/utils.hpp"
#include "base/constants.hpp"
#include <filesystem>
#include <regex>
#include <iostream>

namespace fs = std::filesystem;

/**
 * lpkg rec — 从中断的事务中恢复系统状态。
 *
 * 扫描整个 root 目录查找残留的 .lpkg_bak_<pkg> 文件（由中断的备份阶段留下），
 * 以及 .lpkgtmp 文件（由原子复制中断留下）。
 *
 * 恢复策略：
 *   - .lpkg_bak_<pkg> 文件：如果原路径不存在，重命名回原路径（恢复旧文件）
 *   - .lpkgtmp 文件：直接删除（这些是复制中途留下的临时文件）
 *
 * 同时检查事务日志，报告未完成的事务信息。
 */
void recover_packages() {
    log_info(get_string("info.recover_start"));

    const fs::path root = Config::instance().root_dir();
    const std::string bak_suffix(constants::SUFFIX_LPKG_BAK);
    const std::string tmp_ext = ".lpkgtmp";

    int restored = 0, cleaned = 0;

    // 扫描 root 下所有文件
    std::error_code ec;
    for (auto& entry : fs::recursive_directory_iterator(root, ec)) {
        const fs::path path = entry.path();
        const std::string filename = path.filename().string();

        // 处理 .lpkgtmp 临时文件
        if (filename.ends_with(tmp_ext)) {
            fs::remove(path, ec);
            if (!ec) {
                log_info(string_format("info.recover_cleaned_tmp", path.string()));
                cleaned++;
            }
            continue;
        }

        // 处理 .lpkg_bak_<pkg> 备份文件
        // 格式: original_path.lpkg_bak_<pkgname>
        auto pos = filename.find(bak_suffix);
        if (pos == std::string::npos) continue;

        // 重建原始路径（去掉 .lpkg_bak_<pkg> 后缀）
        std::string orig_name = filename.substr(0, pos);
        fs::path orig_path = path.parent_path() / orig_name;

        // 始终恢复备份。原文件存在也要覆盖——它可能是新包写进去的（回滚未完成）。
        // .lpkgbak 是中断前备份的旧版本，必须还原。
        fs::rename(path, orig_path, ec);
        if (!ec) {
            log_info(string_format("info.recover_restored", path.string(), orig_path.string()));
            restored++;
        } else {
            log_warning(string_format("warning.recover_rename_failed", path.string(), orig_path.string(), ec.message()));
        }
    }

    // 检查事务日志
    std::string pending = TransactionLog::check_pending();
    if (!pending.empty()) {
        log_warning(string_format("warning.recover_pending_txn", pending));
    }

    log_info(string_format("info.recover_done", restored, cleaned));
}
