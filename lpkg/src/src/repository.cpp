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

#include <ranges>
#include <string_view>

void Repository::load_index() {
    packages_.clear();
    providers_.clear();
    std::string mirror;
    try {
        mirror = get_mirror_url();
    } catch (...) {
        return; // No mirror, no index.
    }
    std::string arch = get_architecture();
    
    // Support local file mirror (file:// or plain path) for tests/local repos
    bool is_local = mirror.find("file://") == 0 || mirror.find("/") == 0;
    
    std::filesystem::path index_path;
    
    try {
        if (is_local) {
            std::string path_str = (mirror.find("file://") == 0) ? mirror.substr(7) : mirror;
            index_path = std::filesystem::path(path_str) / arch / "index.txt";
        } else {
            std::string url = mirror + arch + "/index.txt";
            index_path = get_tmp_dir() / "repo_index.txt";
            download_file(url, index_path, false);
        }

        if (!std::filesystem::exists(index_path)) {
            if (get_testing_mode()) return;
            throw LpkgException(string_format("error.repo_index_not_found", index_path.string()));
        }
    } catch (...) {
        if (get_testing_mode()) return;
        throw;
    }

    std::ifstream file(index_path);
    std::string line;
    static const std::vector<std::string> ops = {">=", "<=", "!=", "==", ">", "<", "="};

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::string_view sv = line;
        if (sv.back() == '\r') sv.remove_suffix(1);
        
        auto parts = sv | std::views::split('|') | std::views::transform([](auto&& rng) {
            return std::string_view(&*rng.begin(), std::ranges::distance(rng));
        });

        auto it = parts.begin();
        if (it == parts.end()) continue;
        PackageInfo pkg;
        pkg.name = *it++;
        if (it == parts.end()) continue;
        pkg.version = *it++;
        if (it == parts.end()) continue;
        pkg.sha256 = *it++;
        
        // Parse Deps
        if (it != parts.end()) {
            std::string_view deps_sv = *it++;
            if (!deps_sv.empty()) {
                auto deps_parts = deps_sv | std::views::split(',') | std::views::transform([](auto&& rng) {
                    return std::string_view(&*rng.begin(), std::ranges::distance(rng));
                });
                for (auto dep_str : deps_parts) {
                    DependencyInfo dep;
                    size_t op_pos = std::string_view::npos;
                    for (const auto& op : ops) {
                        if (op_pos = dep_str.find(op); op_pos != std::string_view::npos) {
                            dep.name = dep_str.substr(0, op_pos);
                            dep.op = op;
                            dep.version_req = dep_str.substr(op_pos + op.length());
                            break;
                        }
                    }
                    if (op_pos == std::string_view::npos) dep.name = dep_str;
                    pkg.dependencies.push_back(dep);
                }
            }
        }

        // Parse Provides
        if (it != parts.end()) {
            std::string_view prov_sv = *it++;
            if (!prov_sv.empty()) {
                auto prov_parts = prov_sv | std::views::split(',') | std::views::transform([](auto&& rng) {
                    return std::string_view(&*rng.begin(), std::ranges::distance(rng));
                });
                for (auto prov : prov_parts) {
                    pkg.provides.emplace_back(prov);
                    providers_[std::string(prov)].push_back(pkg.name);
                }
            }
        }

        packages_[pkg.name].push_back(pkg);
    }

    for (auto& versions : packages_ | std::views::values) {
        std::ranges::sort(versions, [](const PackageInfo& a, const PackageInfo& b) {
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
