#include "repository.hpp"
#include "config.hpp"
#include "downloader.hpp"
#include "utils.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "version.hpp"
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
    try { mirror = get_mirror_url(); } catch (...) { return; }
    std::string arch = get_architecture();
    
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
        if (!std::filesystem::exists(index_path)) return;
    } catch (...) { return; }

    std::ifstream file(index_path);
    std::string line;
    static const std::vector<std::string> ops = {">=", "<=", "!=", "==", ">", "<", "="};

    auto split = [](std::string_view s, char delim) {
        std::vector<std::string_view> res;
        size_t start = 0, end = 0;
        while ((end = s.find(delim, start)) != std::string_view::npos) {
            res.push_back(s.substr(start, end - start));
            start = end + 1;
        }
        res.push_back(s.substr(start));
        return res;
    };

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::string_view sv = line;
        if (!sv.empty() && sv.back() == '\r') sv.remove_suffix(1);
        
        auto parts = split(sv, '|');
        if (parts.size() < 2) continue;

        std::string pkg_name(parts[0]);
        std::string_view versions_sv = parts[1];
        std::string_view deps_sv = (parts.size() > 2) ? parts[2] : "";
        std::string_view prov_sv = (parts.size() > 3) ? parts[3] : "";

        std::vector<DependencyInfo> common_deps;
        if (!deps_sv.empty()) {
            for (auto dep_str : split(deps_sv, ',')) {
                DependencyInfo dep;
                size_t op_pos = std::string_view::npos;
                for (const auto& op : ops) {
                    if ((op_pos = dep_str.find(op)) != std::string_view::npos) {
                        dep.name = std::string(dep_str.substr(0, op_pos));
                        dep.op = op;
                        dep.version_req = std::string(dep_str.substr(op_pos + op.length()));
                        break;
                    }
                }
                if (op_pos == std::string_view::npos) dep.name = std::string(dep_str);
                common_deps.push_back(std::move(dep));
            }
        }

        if (!prov_sv.empty()) {
            for (auto prov : split(prov_sv, ',')) {
                providers_[std::string(prov)].push_back(pkg_name);
            }
        }

        for (auto ver_hash : split(versions_sv, ',')) {
            auto vh = split(ver_hash, ':');
            if (vh.empty()) continue;
            PackageInfo pkg;
            pkg.name = pkg_name;
            pkg.version = std::string(vh[0]);
            if (vh.size() > 1) pkg.sha256 = std::string(vh[1]);
            pkg.dependencies = common_deps;
            packages_[pkg.name].push_back(std::move(pkg));
        }
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
    return find_package(it->second[0]);
}

std::optional<PackageInfo> Repository::find_package(const std::string& name) {
    auto it = packages_.find(name);
    if (it == packages_.end() || it->second.empty()) return std::nullopt;
    return it->second.back();
}

std::optional<PackageInfo> Repository::find_package(const std::string& name, const std::string& version) {
    auto it = packages_.find(name);
    if (it == packages_.end()) return std::nullopt;
    for (const auto& pkg : it->second) {
        if (pkg.version == version) return pkg;
    }
    return std::nullopt;
}

std::optional<PackageInfo> Repository::find_best_matching_version(const std::string& name, const std::string& op, const std::string& version_req) {
    auto it = packages_.find(name);
    if (it == packages_.end() || it->second.empty()) return std::nullopt;
    for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
        if (version_satisfies(rit->version, op, version_req)) {
            return *rit;
        }
    }
    return std::nullopt;
}
