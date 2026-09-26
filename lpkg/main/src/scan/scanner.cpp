#include "scanner.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "base/utils.hpp"
#include "config/config.hpp"
#include "db/cache.hpp"
#include "i18n/localization.hpp"

namespace fs = std::filesystem;

/**
 * 扫描系统中不受任何包管理的孤立文件
 * 遍历 /usr、/etc、/opt、/var 等目录，跳过已知的系统路径和包管理器路径，
 * 检查每个文件是否在缓存中有对应的包所有者记录
 */
void scan_orphans(const std::string& scan_root_override)
{
    log_info(get_string("info.scan_loading_db"));
    Cache& cache = Cache::instance();

    fs::path actual_root = Config::instance().root_dir();
    if (!scan_root_override.empty()) {
        actual_root = scan_root_override;
    }

    std::vector<fs::path> scan_roots = {actual_root / "usr", actual_root / "etc",
                                        actual_root / "opt", actual_root / "var"};

    std::unordered_set<std::string> ignored_prefixes = {
        (actual_root / "usr/man").string(),       (actual_root / "usr/sbin").string(),
        (actual_root / "usr/lib64").string(),     (actual_root / "usr/share/lpkg").string(),
        (actual_root / "var/lib/lpkg").string(),  (actual_root / "var/cache").string(),
        (actual_root / "var/log").string(),       (actual_root / "var/tmp").string(),
        (actual_root / "var/run").string(),       (actual_root / "proc").string(),
        (actual_root / "sys").string(),           (actual_root / "dev").string(),
        (actual_root / "run").string(),           (actual_root / "tmp").string(),
        (actual_root / "lib").string(),           (actual_root / "lib64").string(),
        (actual_root / "sbin").string(),          (actual_root / "usr/lib/python").string(),
        (actual_root / "etc/ssl/certs").string(), (actual_root / "etc/pki").string(),
        (actual_root / "etc/lpkg").string(),
    };

    log_info(get_string("info.scan_start"));

    long long scanned_count = 0;
    long long orphan_count = 0;

    for (const auto& root : scan_roots) {
        // 两处判定都走**不抛**的谓词（2026-09-26 修）：这两个 root 是拼出来的已知路径，
        // 其中**中间段**成环时抛型重载会以 ELOOP 打断整趟扫描 —— 而这不是"某个文件读不了"
        // （那种有下面的 try/catch 兜着），是整趟 `lpkg scan` 直接失败。
        // 判据本身保持原语义：`exists_follow` = 跟随语义的 `fs::exists`；
        // `is_symlink_no_follow` = `fs::is_symlink` 的 lstat 孪生（这个 root 名字**本身**
        // 是符号链接就跳过，例如 /bin -> /usr/bin）。
        if (!exists_follow(root)) continue;

        // 跳过本身就是符号链接的根目录（例如 /bin -> /usr/bin）
        if (is_symlink_no_follow(root)) continue;

        // 显式迭代器循环（而非范围 for）：范围 for 的自增发生在循环体**之外**，
        // 目录在遍历中被删时 increment 抛出的 filesystem_error 会逃出整趟扫描
        // （所有 root 一起失败）。用 ec 重载 → 该 root 停止、其余 root 继续。
        std::error_code walk_ec;
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied);
             it != fs::recursive_directory_iterator(); it.increment(walk_ec)) {
            if (walk_ec) {
                walk_ec.clear();
                continue;
            }
            const auto& entry = *it;
            try {
                if (entry.is_symlink() || entry.is_regular_file()) {
                    scanned_count++;
                    std::string path = entry.path().string();

                    bool ignored = false;
                    for (const auto& prefix : ignored_prefixes) {
                        if (path.compare(0, prefix.size(), prefix) == 0) {
                            ignored = true;
                            break;
                        }
                    }
                    if (ignored) continue;

                    std::string key = path;
                    if (actual_root != "/") {
                        // lexically_relative 而非 fs::relative：后者会**解析符号链接**，
                        // 非 / root 下 `lib64 -> lib` 这类路径会被换成真实路径、
                        // 与登记的属主键对不上 → 假孤儿
                        fs::path relative = entry.path().lexically_relative(actual_root);
                        key = "/" + relative.string();
                    }

                    if (cache.get_file_owners(key).empty()) {
                        std::cout << path << std::endl;
                        orphan_count++;
                    }
                }
            } catch (const std::exception& e) {
                log_warning(
                    string_format("warning.scan_file_error", entry.path().string(), e.what()));
                continue;
            }
        }
    }
    log_info(string_format("info.scan_complete", scanned_count, orphan_count));
}
