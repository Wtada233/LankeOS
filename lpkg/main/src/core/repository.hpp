#pragma once

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>

/**
 * 依赖信息：包名 + 可选版本约束
 */
struct DependencyInfo {
    std::string name;           // 依赖包名
    std::string op;             // 版本操作符，如 ">="、"<"，无约束时为空
    std::string version_req;    // 版本要求，如 "1.0.0"
};

/**
 * 包信息：从仓库索引中解析的完整包描述
 */
struct PackageInfo {
    std::string name;                         // 包名
    std::string version;                      // 版本号
    std::string sha256;                       // 包文件 SHA256 校验值
    std::vector<DependencyInfo> dependencies;  // 依赖列表
    std::vector<std::string> provides;         // 提供的能力列表
};

/**
 * 仓库管理器
 *
 * 从远程或本地仓库加载包索引，提供包查询、版本匹配与 provider 查找功能。
 */
class Repository {
public:
    /** 加载并解析仓库索引文件 */
    void load_index();
    /** 更新或添加包信息到索引 */
    void update_package_info(const std::string& name, const std::string& version, const std::vector<DependencyInfo>& deps, const std::vector<std::string>& provides);
    /** 查找包（不指定版本时返回最新版本） */
    std::optional<PackageInfo> find_package(const std::string& name);
    /** 精确查找指定版本的包 */
    std::optional<PackageInfo> find_package(const std::string& name, const std::string& version);
    /** 查找满足版本约束的最佳匹配版本 */
    std::optional<PackageInfo> find_best_matching_version(const std::string& name, const std::string& op, const std::string& version_req);
    /** 查找提供某能力的包 */
    std::optional<PackageInfo> find_provider(const std::string& capability);

private:
    std::unordered_map<std::string, std::vector<PackageInfo>> packages_;  // 包名 -> 版本列表
    std::unordered_map<std::string, std::vector<std::string>> providers_; // 能力 -> 提供该能力的包名列表
};
