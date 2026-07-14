#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "vercmp/version.hpp"

/**
 * 依赖信息：包名 + 可选的多个版本约束
 *
 * 支持复合约束表达区间，格式（索引 / metadata.json）：
 *   "glibc >= 2.0.0 < 3.0.0"
 * 解析时先按包名分隔符（逗号）切割，再在一个 dep 内提取所有 (op, version) 对。
 */
struct DependencyInfo {
  std::string name;                    // 依赖包名
  std::vector<Constraint> constraints; // 版本约束列表，为空则无版本要求
};

/**
 * 包信息：从仓库索引中解析的完整包描述
 */
struct PackageInfo {
  std::string name;                         // 包名
  std::string version;                      // 版本号
  std::string sha256;                       // 包文件 SHA256 校验值
  std::vector<DependencyInfo> dependencies; // 依赖列表
  std::vector<std::string> provides;  // 提供的能力列表（含 SONAME、虚拟包等）
  std::vector<std::string> needed_so; // 包声明的 DT_NEEDED SONAME 列表
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
  void update_package_info(const std::string &name, const std::string &version,
                           const std::vector<DependencyInfo> &deps,
                           const std::vector<std::string> &provides,
                           const std::vector<std::string> &needed_so = {});
  /** 查找包（不指定版本时返回最新版本） */
  std::optional<PackageInfo> find_package(const std::string &name);
  /** 精确查找指定版本的包 */
  std::optional<PackageInfo> find_package(const std::string &name,
                                          const std::string &version);
  /** 查找满足单一版本约束的最佳匹配版本 */
  std::optional<PackageInfo>
  find_best_matching_version(const std::string &name, const std::string &op,
                             const std::string &version_req);
  /** 查找满足复合版本约束的最佳匹配版本（支持区间，如 >= 2.0.0 < 3.0.0） */
  std::optional<PackageInfo>
  find_best_matching_version(const std::string &name,
                             const std::vector<Constraint> &constraints);
  /** 查找提供某能力的包 */
  std::optional<PackageInfo> find_provider(const std::string &capability);

private:
  std::unordered_map<std::string, std::vector<PackageInfo>>
      packages_; // 包名 -> 版本列表
  std::unordered_map<std::string, std::vector<std::string>>
      providers_; // 能力 -> 提供该能力的包名列表
};
