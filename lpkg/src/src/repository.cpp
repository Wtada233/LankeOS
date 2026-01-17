#include "repository.hpp"
#include "config.hpp"
#include "downloader.hpp"
#include "utils.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "version.hpp" // Added include for version comparison
#include <sstream>
#include <fstream>
#include <iostream>
#include <algorithm>

void Repository::load_index() {
    packages_.clear();
    providers_.clear();
    std::string mirror = get_mirror_url();
    std::string arch = get_architecture();
    
    // Support local file mirror (file://) for tests
    bool is_local = mirror.find("file://") == 0;
    
    std::filesystem::path index_path;
    
    if (is_local) {
        std::string path_str = mirror.substr(7);
        index_path = std::filesystem::path(path_str) / arch / "index.txt";
    } else {
        std::string url = mirror + arch + "/index.txt";
        index_path = get_tmp_dir() / "repo_index.txt";
        download_file(url, index_path, false);
    }

    if (!std::filesystem::exists(index_path)) {
        throw LpkgException(string_format("error.repo_index_not_found", index_path.string()));
    }

    std::ifstream file(index_path);
    std::string line;
    static const std::vector<std::string> ops = {">=", "<=", "!=", "==", ">", "<", "="}; // Static to avoid recreation

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.back() == '\r') line.pop_back(); // Handle CRLF
        
        std::stringstream ss(line);
        std::string segment;
        std::vector<std::string> parts;
        
        while (std::getline(ss, segment, '|')) {
            parts.push_back(segment);
        }
        if (!line.empty() && line.back() == '|') {
            parts.push_back("");
        }

        if (parts.size() < 3) {
            continue; 
        }

        PackageInfo pkg;
        pkg.name = parts[0];
        pkg.version = parts[1];
        pkg.sha256 = parts[2];
        
        // Parse Deps: dep1>=1.0,dep2
        if (parts.size() > 3 && !parts[3].empty()) {
            std::stringstream deps_ss(parts[3]);
            std::string dep_str;
            while (std::getline(deps_ss, dep_str, ',')) {
                DependencyInfo dep;
                size_t op_pos = std::string::npos;
                
                for (const auto& op : ops) {
                    op_pos = dep_str.find(op);
                    if (op_pos != std::string::npos) {
                        dep.name = dep_str.substr(0, op_pos);
                        dep.op = op;
                        dep.version_req = dep_str.substr(op_pos + op.length());
                        break;
                    }
                }
                if (op_pos == std::string::npos) {
                    dep.name = dep_str;
                }
                pkg.dependencies.push_back(dep);
            }
        }

        // Parse Provides
        if (parts.size() > 4 && !parts[4].empty()) {
            std::stringstream prov_ss(parts[4]);
            std::string prov;
            while (std::getline(prov_ss, prov, ',')) {
                pkg.provides.push_back(prov);
                providers_[prov].push_back(pkg.name);
            }
        }

        packages_[pkg.name].push_back(pkg);
    }

    // 预先对所有包的版本进行排序，避免在查询时重复排序
    for (auto& [name, versions] : packages_) {
        std::sort(versions.begin(), versions.end(), [](const PackageInfo& a, const PackageInfo& b) {
            return version_compare(a.version, b.version);
        });
    }
}

std::optional<PackageInfo> Repository::find_provider(const std::string& capability) {
    auto it = providers_.find(capability);
    if (it == providers_.end() || it->second.empty()) return std::nullopt;
    
    // For now, just return the first provider found. 
    // In a more advanced manager, we might have a preference or scoring system.
    return find_package(it->second[0]);
}

std::optional<PackageInfo> Repository::find_package(const std::string& name) {
    auto it = packages_.find(name);
    if (it == packages_.end() || it->second.empty()) return std::nullopt;
    
    // 版本已在 load_index 中排序，直接返回最后一个（最新版）
    return it->second.back();
}

std::optional<PackageInfo> Repository::find_package(const std::string& name, const std::string& version) {
    if (packages_.find(name) == packages_.end()) return std::nullopt;
    for (const auto& pkg : packages_[name]) {
        if (pkg.version == version) return pkg;
    }
    return std::nullopt;
}

std::optional<PackageInfo> Repository::find_best_matching_version(const std::string& name, const std::string& op, const std::string& version_req) {
    auto it = packages_.find(name);
    if (it == packages_.end() || it->second.empty()) return std::nullopt;

    // Versions are already sorted in load_index, iterate from highest to lowest
    for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
        if (version_satisfies(rit->version, op, version_req)) {
            return *rit;
        }
    }
    return std::nullopt;
}
