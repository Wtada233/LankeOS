#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../repo/repository.hpp"

namespace solv
{

/// 一次求解产出的单个包操作（按依赖先装的执行序）
struct ResolvedPkg {
    std::string name;
    std::string version;
    bool is_install = false;   ///< 新装（false = 升级/重装已装包）
    bool is_explicit = false;  ///< 用户显式请求（非传递拉入）
};

struct SolveResult {
    std::vector<ResolvedPkg> order;     ///< 依赖先装的执行序
    std::vector<std::string> problems;  ///< 致命错误（非容忍类）；空 = 求解成功
    bool ok() const
    {
        return problems.empty();
    }
};

struct SolveOptions {
    bool force_reinstall = false;             ///< 强制重装（已装同版本也要进计划）
    bool missing_so_no_error = false;         ///< --missing-so-no-error：缺 provider 的 SONAME 容忍
    bool use_system_soname = false;           ///< --use-system-soname：系统 .so 视为已满足
    bool no_deps = false;                     ///< 不解析依赖（仅目标包自身）
    std::vector<std::string> system_sonames;  ///< use_system_soname 时的系统 SONAME 集合
};

/// 已装包（供 installed repo 建模 requires + provides）。
///
/// libsolv 的反向一致性（升级/降级不得破坏已装依赖）依赖 `dontfix` 机制：
/// 已装包的 requires 只在"之前能满足"（有一个已装 provider）时才建规则强制。
/// 所以要建模 provides——installed glibc 必须显式 provide libc.so.6，
/// app 的 libc.so.6 才是"健康"的，升级破坏它才会冲突（与 dnf 的系统 repo 一致）。
struct InstalledPkg {
    std::string version;
    std::vector<DependencyInfo> deps;
    std::vector<std::string> needed_so;
    std::vector<std::string> provides;
};

/// 用 libsolv 求解安装/升级/重装计划。
/// repo：可用仓库（权威 provider 源）；local：本地 .lpkg 候选包（也进 available repo）；
/// installed：已装包名 -> 版本+requires；targets：(包名, 版本说明)；"latest"=选最佳版本。
SolveResult solve_install(const class Repository& repo, const std::vector<class PackageInfo>& local,
                          const std::map<std::string, InstalledPkg>& installed,
                          const std::vector<std::pair<std::string, std::string>>& targets,
                          const SolveOptions& opts);

/// 用 libsolv 求整仓反向依赖：谁 requires target 提供的 capability（soname/包名）。
std::set<std::string> repo_revrequires(const class Repository& repo, const std::string& target);

}  // namespace solv
