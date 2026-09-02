#pragma once

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "archive.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "config/config.hpp"
#include "db/cache.hpp"
#include "i18n/localization.hpp"
#include "nlohmann/json.hpp"
#include "package_manager.hpp"
#include "repo/repository.hpp"
#include "vercmp/version.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

// InstallPlan、InstallContext、InstallTask 已在 package_manager.hpp 中前置声明

namespace detail
{

/// 在 chroot（若需要）中执行 hook 脚本（postinst / prerm）
void run_hook(std::string_view pkg_name, std::string_view hook_name);

/// 从 .lpkg 存档中读取 metadata.json（无需完整解压整个包）
/// 返回解析后的 JSON，若缺失或无法读取则抛出 LpkgException
nlohmann::json read_archive_metadata(const std::filesystem::path& archive_path);

/// 从已解压的包目录中读取 metadata.json
void read_package_metadata(const fs::path& tmp_pkg_dir, std::string& name, std::string& version,
                           std::vector<std::string>& deps, std::vector<std::string>& provides,
                           std::vector<std::string>& needed_so, std::string& man);

/// 扫描 content/ 目录，返回相对路径列表
std::vector<std::string> scan_content_files(const fs::path& content_dir);

/// 解析原始依赖字符串（如 "libfoo >= 1.0"）为 DependencyInfo 结构体
std::vector<DependencyInfo> parse_dep_strings(const std::vector<std::string>& dep_strs);

/// 用 libsolv 求解安装/升级/重装计划，填充 InstallContext 的 plan + install_order。
/// 取代旧的手动递归解析 resolve_package_dependencies 及其配套手动校验
/// （check_plan_consistency / check_needed_so_consistency / check_forward_soname_integrity）：
/// solver 建模 installed repo 的 requires（deps+needed_so），求解时原生覆盖
/// 依赖拉入、版本约束、ABI 反向一致性、缺 provider 检测。
void resolve_with_solver(InstallContext& ctx);

/// 从已持有的包开始 BFS 遍历依赖图，获取所有必需的包（供 autoremove）
std::unordered_set<std::string> get_all_required_packages();

// ============================================================================
// 目录整树删除（remove 与 upgrade 共用，见 ARCH.md §3.6）
// ============================================================================

// ============================================================================
// 每文件系统 sidecar stash + 目录元数据化删除（TODO.md 第 2 节）
// ============================================================================

/**
 * phys 所在文件系统的"顶层同设备祖先"（= 可安全放 stash 的目录）。沿 phys 所在目录
 * 向上走到 `st_dev` 变化处（设备/挂载边界）或 `Config::root_dir()` 边界为止，
 * **恒 clamp 在 root_dir() 内**（chroot 运行）。stash 放这里保证与 phys 同设备
 * → rename(2) 永不 EXDEV、永不逃出 chroot。多设备时每个文件系统各自一个 stash 根。
 */
std::filesystem::path stash_parent_dir(const std::filesystem::path& phys);

/// 返回并确保 phys 的 stash 目录：`<stash_parent_dir(phys)>/.lpkg_bak_<pkg>_<pid>`，
/// mode 0700、root-only（备份残留隔离，不被普通工具/扫描看到）。
std::filesystem::path ensure_stash_dir(const std::filesystem::path& phys, std::string_view pkg);

/**
 * 在 phys 的 stash 内分配一个唯一 bak 目标（不移动、不写 WAL）。备份文件扁平存放：
 * `<stash>/<原名>.lpkg_bak_<pkg>_<rand>`；同 basename 由随机后缀保证唯一，
 * 还原路径靠调用方写的 BACKUP/REMOVE_OLD WAL 记录映射。
 */
std::filesystem::path stash_bak_target(const std::filesystem::path& phys, std::string_view pkg);

/// 删除一个 stash 目录（整体 remove_all）。
void remove_stash_dir(const std::filesystem::path& stash);

/**
 * 删除一个空目录并记录元数据供回滚重建。write-ahead：先写
 * `DIR_RM <path> <mode> <uid> <gid>` 再 `rmdir`。前置：phys 是真实空目录（非 symlink）；
 * 非空目录（含无主内容/状态目录/conffile/其他包文件）绝不能走到这里。
 */
void remove_empty_dir_with_meta(const std::filesystem::path& phys);

}  // namespace detail
