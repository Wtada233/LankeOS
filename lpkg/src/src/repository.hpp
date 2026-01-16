#pragma once

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>

struct DependencyInfo {
    std::string name;
    std::string op;          // e.g., ">=", "<", or empty if no constraint
    std::string version_req; // e.g., "1.0.0"
};

struct PackageInfo {
    std::string name;
    std::string version;
    std::string sha256;
    std::vector<DependencyInfo> dependencies;
    std::vector<std::string> provides;
};

class Repository {
public:
    void load_index();
    std::optional<PackageInfo> find_package(const std::string& name);
    std::optional<PackageInfo> find_package(const std::string& name, const std::string& version); // exact match

private:
    std::unordered_map<std::string, std::vector<PackageInfo>> packages_; // name -> list of versions
};
