#pragma once

#include "package_manager.hpp"
#include "repo/repository.hpp"
#include "archive.hpp"
#include "base/constants.hpp"
#include "config/config.hpp"
#include "db/cache.hpp"
#include "base/exception.hpp"
#include "i18n/localization.hpp"
#include "vercmp/version.hpp"
#include "base/utils.hpp"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// InstallPlan、InstallContext、InstallTask 已在 package_manager.hpp 中前置声明

namespace detail {

/// 在 chroot（若需要）中执行 hook 脚本（postinst / prerm）
void run_hook(std::string_view pkg_name, std::string_view hook_name);

/// 从 .lpkg 存档中读取 metadata.json（无需完整解压整个包）
/// 返回解析后的 JSON，若缺失或无法读取则抛出 LpkgException
nlohmann::json read_archive_metadata(const std::filesystem::path& archive_path);

/// 从已解压的包目录中读取 metadata.json
void read_package_metadata(const fs::path& tmp_pkg_dir, std::string& name, std::string& version,
                           std::vector<std::string>& deps, std::vector<std::string>& provides,
                           std::vector<std::string>& needed_so,
                           std::string& man);

/// 扫描 content/ 目录，返回相对路径列表
std::vector<std::string> scan_content_files(const fs::path& content_dir);

/// 解析原始依赖字符串（如 "libfoo >= 1.0"）为 DependencyInfo 结构体
std::vector<DependencyInfo> parse_dep_strings(const std::vector<std::string>& dep_strs);

/// 解析一个包及其传递依赖，将其加入共享安装计划
/// depth 用于限制递归深度，防止栈溢出（默认 64）
void resolve_package_dependencies(const std::string& pkg_name, const std::string& version_spec,
                                  bool is_explicit, InstallContext& ctx,
                                  std::set<std::string>& visited_stack,
                                  int depth = 0);

/// 检查计划中的包升级是否会破坏已安装的包
std::set<std::string> check_plan_consistency(const std::map<std::string, InstallPlan>& plan);

/// 检查计划升级是否会破坏已安装包的 needed_so
/// 例如：libM-1.0（提供 libM.so.1）被升级到 libM-2.0（不再提供 libM.so.1）
/// 而已安装的 app 需要 libM.so.1 → app 被标记为 broken
std::set<std::string> check_needed_so_consistency(const std::map<std::string, InstallPlan>& plan);

/// 从已持有的包开始 BFS 遍历依赖图，获取所有必需的包
std::unordered_set<std::string> get_all_required_packages();

/// 向前 needed_so 完整性校验。
/// 验证每个包声明的 SONAME 在 plan（版本精准）/ 已安装缓存 / repo（版本精准）
/// 中有提供者，不存在则抛出 LpkgException。
/// 与 check_needed_so_consistency（检查升级会破坏已安装包）成对使用：
///   一个管"新包依赖的 ABI 能否解析"（向前），
///   一个管"升级后旧依赖者的 ABI 是否断裂"（向后）。
void check_forward_soname_integrity(
    const std::map<std::string, InstallPlan>& plan,
    Repository& repo);

} // namespace detail
