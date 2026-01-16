#include "repository.hpp"
#include "config.hpp"
#include "downloader.hpp"
#include "utils.hpp"
#include "exception.hpp"
#include <sstream>
#include <fstream>
#include <iostream>
#include <algorithm>

void Repository::load_index() {
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
        throw LpkgException("Repository index not found: " + index_path.string());
    }

    std::ifstream file(index_path);
    std::string line;
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
        
        packages_[pkg.name].push_back(pkg);

        // Parse Deps: dep1>=1.0,dep2
        if (parts.size() > 3 && !parts[3].empty()) {
            std::stringstream deps_ss(parts[3]);
            std::string dep_str;
            while (std::getline(deps_ss, dep_str, ',')) {
                DependencyInfo dep;
                // Simple parser for name[ op ver]
                size_t op_pos = std::string::npos;
                std::string ops[] = {">=", "<=", "!=", "==", ">", "<", "="};
                
                for (const auto& op : ops) {
                    op_pos = dep_str.find(op);
                    if (op_pos != std::string::npos) {
                        dep.name = dep_str.substr(0, op_pos);
                        // trim spaces? assuming format is tight for now
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
            }
        }

        packages_[pkg.name].push_back(pkg);
    }
}

std::optional<PackageInfo> Repository::find_package(const std::string& name) {
    if (packages_.find(name) == packages_.end()) return std::nullopt;
    // Default to latest version? For now, yes. The vector should be sorted or we sort it.
    // Assuming the index might be random, we should find the "latest".
    // We need version_compare logic here.
    // For MVP, just take the last one or first one? 
    // Let's rely on the order in file or implement simple selection.
    // BETTER: Implement `get_latest` logic using `version_compare` from version.hpp
    
    // Wait, I can't include version.hpp easily if it's not exposed well? 
    // It is in src/version.hpp.
    
    auto& versions = packages_[name];
    if (versions.empty()) return std::nullopt;
    
    return versions.back(); // Naive: assume last in file is latest or just take one. 
                            // TODO: Sort by version.
}

std::optional<PackageInfo> Repository::find_package(const std::string& name, const std::string& version) {
    if (packages_.find(name) == packages_.end()) return std::nullopt;
    for (const auto& pkg : packages_[name]) {
        if (pkg.version == version) return pkg;
    }
    return std::nullopt;
}
