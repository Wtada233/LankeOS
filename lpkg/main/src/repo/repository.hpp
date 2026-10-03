#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vercmp/dep_parser.hpp"  // `DependencyInfo`（纯语法层类型，见那里的说明）
#include "vercmp/version.hpp"

/**
 * 包信息：从仓库索引中解析的完整包描述
 */
struct PackageInfo {
    std::string name;                          // 包名
    std::string version;                       // 版本号
    std::string sha256;                        // 包文件 SHA256 校验值
    std::vector<DependencyInfo> dependencies;  // 依赖列表
    std::vector<std::string> provides;         // 提供的能力列表（含 SONAME、虚拟包等）
    std::vector<std::string> needed_so;        // 包声明的 DT_NEEDED SONAME 列表
};

/**
 * 仓库管理器
 *
 * 从远程或本地仓库加载包索引，提供包查询、版本匹配与 provider 查找功能。
 */
class Repository
{
public:
    /** 加载并解析仓库索引文件 */
    void load_index();
    /** 更新或添加包信息到索引 */
    void update_package_info(const std::string& name, const std::string& version,
                             const std::vector<DependencyInfo>& deps,
                             const std::vector<std::string>& provides,
                             const std::vector<std::string>& needed_so = {});
    /** 查找包（不指定版本时返回最新版本） */
    std::optional<PackageInfo> find_package(const std::string& name);
    /** 精确查找指定版本的包 */
    std::optional<PackageInfo> find_package(const std::string& name, const std::string& version);
    /** 查找满足单一版本约束的最佳匹配版本 */
    std::optional<PackageInfo> find_best_matching_version(const std::string& name,
                                                          const std::string& op,
                                                          const std::string& version_req);
    /** 查找满足复合版本约束的最佳匹配版本（支持区间，如 >= 2.0.0 < 3.0.0） */
    std::optional<PackageInfo> find_best_matching_version(
        const std::string& name, const std::vector<Constraint>& constraints);
    /** 查找提供某能力的包 */
    std::optional<PackageInfo> find_provider(const std::string& capability) const;
    /** 全部包（包名 -> 版本列表；供 solver 构建 libsolv pool） */
    const std::unordered_map<std::string, std::vector<PackageInfo>>& packages() const
    {
        return packages_;
    }

private:
    /** 吸收索引里的一行（该行的**全部**版本块）到包表与 provider 表 */
    void absorb_index_line(std::string_view line);

    std::unordered_map<std::string, std::vector<PackageInfo>> packages_;  // 包名 -> 版本列表
    std::unordered_map<std::string, std::vector<std::string>>
        providers_;  // 能力 -> 提供该能力的包名列表
};

/**
 * 加载仓库索引 —— **失败只告警、不抛** 的唯一入口（2026-10-03 收敛）。
 *
 * 为什么必须收敛成一处：`load_index()` 失败时"降级成仓库暂时不可用"这条语义，在
 * 6 个调用点上**必须逐字一致**（同一条 `warning.repo_index_load_failed`）。此前每个调用点
 * 各抄一遍 `try { repo.load_index(); } catch (...) { log_warning(...); }` —— 抄漏一次，
 * 失败就变成**静默的"仓库里没有这个包"**，而 `depend remove` / `depend abibreak` 会据此
 * 报出错误的"无受影响包"（给的是错误答案，不是报错）。日志键本身也属这条契约的一部分。
 *
 * @param repo 待加载的仓库。失败后它保持**已构造、未加载**（即空仓库）的状态 —— 这是既有
 *             语义，不是本函数新增的：调用点可以照常继续用它。
 * @return true = 加载成功；false = 失败（已发告警）。是否提前返回由调用点按自身语义决定
 *         （有的调用点失败即 `return`，有的继续走空仓库）。
 */
bool load_index_or_warn(Repository& repo);
