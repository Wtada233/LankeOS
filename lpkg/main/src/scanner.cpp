#include "scanner.hpp"
#include "cache.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "localization.hpp"
#include <filesystem>
#include <iostream>
#include <vector>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

void scan_orphans(const std::string& scan_root_override) {
    log_info(get_string("info.scan_loading_db"));
    Cache& cache = Cache::instance(); 
    
    fs::path actual_root = ROOT_DIR;
    if (!scan_root_override.empty()) {
        actual_root = scan_root_override;
    }

    std::vector<fs::path> scan_roots = {
        actual_root / "bin",
        actual_root / "sbin",
        actual_root / "lib",
        actual_root / "lib64",
        actual_root / "usr",
        actual_root / "etc",
        actual_root / "opt"
    };

    std::unordered_set<std::string> ignored_prefixes = {
        (actual_root / "usr/share/man").string(),
        (actual_root / "usr/share/doc").string(),
        (actual_root / "var/lib/lpkg").string(),
        (actual_root / "etc/lpkg").string(),
        (actual_root / "proc").string(),
        (actual_root / "sys").string(),
        (actual_root / "dev").string(),
        (actual_root / "run").string(),
        (actual_root / "tmp").string()
    };

    log_info(get_string("info.scan_start"));
    
    long long scanned_count = 0;
    long long orphan_count = 0;

    for (const auto& root : scan_roots) {
        if (!fs::exists(root)) continue;
        
        for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
            if (entry.is_symlink() || entry.is_regular_file()) {
                scanned_count++;
                std::string path = entry.path().string();
                
                bool ignored = false;
                for (const auto& prefix : ignored_prefixes) {
                    if (path.find(prefix) == 0) {
                        ignored = true;
                        break;
                    }
                }
                if (ignored) continue;

                std::string key = path;
                // Normalize path to be relative to the logical root "/"
                // If actual_root is different from /, we strip it to check against DB
                // DB stores paths like "/usr/bin/ls"
                if (actual_root != "/") {
                     fs::path relative = fs::relative(entry.path(), actual_root);
                     key = "/" + relative.string();
                } else if (path.find("/") == 0) {
                     // already absolute, key is path
                }

                if (cache.get_file_owners(key).empty()) {
                    std::cout << path << std::endl;
                    orphan_count++;
                }
            }
        }
    }
    log_info(string_format("info.scan_complete", orphan_count));
}