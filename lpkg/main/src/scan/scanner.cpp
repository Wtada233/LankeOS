#include "scanner.hpp"
#include "db/cache.hpp"
#include "config/config.hpp"
#include "base/utils.hpp"
#include "i18n/localization.hpp"
#include <filesystem>
#include <iostream>
#include <vector>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

/**
 * 扫描系统中不受任何包管理的孤立文件
 * 遍历 /usr、/etc、/opt、/var 等目录，跳过已知的系统路径和包管理器路径，
 * 检查每个文件是否在缓存中有对应的包所有者记录
 */
void scan_orphans(const std::string& scan_root_override) {
    log_info(get_string("info.scan_loading_db"));
    Cache& cache = Cache::instance();

    fs::path actual_root = Config::instance().root_dir();
    if (!scan_root_override.empty()) {
        actual_root = scan_root_override;
    }

    std::vector<fs::path> scan_roots = {
        actual_root / "usr",
        actual_root / "etc",
        actual_root / "opt",
        actual_root / "var"
    };

    std::unordered_set<std::string> ignored_prefixes = {
        (actual_root / "usr/man").string(),
        (actual_root / "usr/sbin").string(),
        (actual_root / "usr/lib64").string(),
        (actual_root / "usr/share/lpkg").string(),
        (actual_root / "var/lib/lpkg").string(),
        (actual_root / "var/cache").string(),
        (actual_root / "var/log").string(),
        (actual_root / "var/tmp").string(),
        (actual_root / "var/run").string(),
        (actual_root / "proc").string(),
        (actual_root / "sys").string(),
        (actual_root / "dev").string(),
        (actual_root / "run").string(),
        (actual_root / "tmp").string(),
        (actual_root / "lib").string(),
        (actual_root / "lib64").string(),
        (actual_root / "sbin").string(),
        (actual_root / "usr/lib/python").string(),
        (actual_root / "etc/ssl/certs").string(),
        (actual_root / "etc/pki").string(),
        (actual_root / "etc/lpkg").string(),
    };

    log_info(get_string("info.scan_start"));

    long long scanned_count = 0;
    long long orphan_count = 0;

    for (const auto& root : scan_roots) {
        if (!fs::exists(root)) continue;

        // 跳过本身就是符号链接的根目录（例如 /bin -> /usr/bin）
        if (fs::is_symlink(root)) continue;

        for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
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
                         fs::path relative = fs::relative(entry.path(), actual_root);
                         key = "/" + relative.string();
                    }

                    if (cache.get_file_owners(key).empty()) {
                        std::cout << path << std::endl;
                        orphan_count++;
                    }
                }
            } catch (...) { continue; }
        }
    }
    log_info(string_format("info.scan_complete", scanned_count, orphan_count));
}
