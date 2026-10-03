#include "depend_scanner.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <ranges>
#include <sstream>
#include <unordered_map>

#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "config/config.hpp"
#include "db/cache.hpp"
#include "i18n/localization.hpp"
#include "install_common.hpp"
#include "repo/repository.hpp"
#include "repo/revdep.hpp"
#include "vercmp/version.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace depscan
{

// ═══════════════════════════════════════════════════════════════════════════
//  内部工具函数
// ═══════════════════════════════════════════════════════════════════════════

namespace
{

/** 获取包版本号；未安装时返回 l10n 的 `info.depend_version_not_installed`（原硬编码英文） */
std::string version_or_missing(const std::string& pkg)
{
    auto ver = Cache::instance().get_installed_version(pkg);
    return ver.empty() ? get_string("info.depend_version_not_installed") : ver;
}

/**
 * 前向依赖解析（用于 install 扫描）
 * 记录每个解析到的包及其是否已安装
 */
struct ResolvedDep {
    std::string name;
    std::string version;
    bool already_installed;
    std::vector<DependencyInfo> deps;
};
using DepMap = std::unordered_map<std::string, ResolvedDep>;

/// 约束的文字形态（`>= 2.0, < 3.0`）—— 只用于告警文案。
std::string constraint_text(const std::vector<Constraint>& cs)
{
    std::string out;
    for (const auto& c : cs) {
        if (!out.empty()) out += ", ";
        out += c.op + " " + c.version;
    }
    return out;
}

/**
 * 预览用的依赖版本解析。
 *
 * **约束存在但仓库里没有任何版本满足时必须返回 `nullopt`**（调用方告警 + 跳过），
 * **不能**回退到"最新版"（2026-10-03 修）：最新版正是约束排除掉的那个 ——
 * `lpkg install` 会拒绝这种依赖，预览却报"将装最新版"，预览与实做**相反**。
 * 同类处置在别处都是 fail-loud：`builder.cpp` 抛 `error.build_dep_unsatisfiable`、
 * `force_solve_conflict` 把 nullopt 当"被打破"。这里是唯一漏网处（只影响 `depend install`
 * 这个**只读预览**，不动盘）。
 */
std::optional<std::string> resolve_dep_version(Repository& repo, const DependencyInfo& dep)
{
    if (dep.constraints.empty()) return std::string(constants::VER_LATEST);
    if (auto m = repo.find_best_matching_version(dep.name, dep.constraints)) return m->version;
    return std::nullopt;
}

/// 约束不可满足时的可见告警（预览里不能静默按"最新版"继续）。
void warn_unsatisfiable(const DependencyInfo& dep)
{
    log_warning(string_format("warning.depend_constraint_unsatisfiable", dep.name,
                              constraint_text(dep.constraints)));
}

/**
 * 递归解析传递依赖，将结果写入 plan
 * 已安装的包直接记录并跳过展开，未安装的从仓库解析
 */
void resolve_transitive_deps(const std::string& pkg_name, const std::string& version_spec,
                             DepMap& plan, std::set<std::string>& visited, Repository& repo)
{
    if (visited.contains(pkg_name) || plan.contains(pkg_name)) return;

    visited.insert(pkg_name);

    auto pkg_info = (version_spec == constants::VER_LATEST || version_spec.empty())
                        ? repo.find_package(pkg_name)
                        : repo.find_package(pkg_name, version_spec);

    if (!pkg_info) {
        if (auto prov = repo.find_provider(pkg_name)) {
            pkg_info = repo.find_package(prov->name);
        }
        if (!pkg_info) {
            visited.erase(pkg_name);
            return;
        }
    }

    bool already = !Cache::instance().get_installed_version(pkg_info->name).empty();

    // 合并依赖来源：声明 deps + needed_so 推导的提供者。
    // index 的 deps 字段常为空（farm 只回填 needed_so），needed_so 才是完整真源；
    // 对齐真实 install 解析（install_common 同样走 needed_so → find_provider → 递归）。
    std::vector<DependencyInfo> deps = pkg_info->dependencies;
    if (!Config::instance().no_deps_mode()) {
        for (const auto& soname : pkg_info->needed_so) {
            if (auto prov = repo.find_provider(soname)) {
                if (prov->name == pkg_info->name) continue;
                bool dup = false;
                for (const auto& d : deps)
                    if (d.name == prov->name) {
                        dup = true;
                        break;
                    }
                if (!dup) deps.push_back({prov->name, {}});
            }
        }
    }
    plan[pkg_info->name] = {pkg_info->name, pkg_info->version, already, deps};

    // 曾在此处对"已安装的中间节点"直接 return、不再展开 → 它背后**未安装**的依赖
    // 不会出现在 `depend install` 的树里，"将要装什么"因此不准（历史 TODO G3）。
    // visited 已保证无环，继续展开即可。

    for (const auto& dep : deps) {
        if (!Config::instance().no_deps_mode()) {
            auto dv = resolve_dep_version(repo, dep);
            if (!dv) {
                warn_unsatisfiable(dep);
                continue;
            }
            resolve_transitive_deps(dep.name, *dv, plan, visited, repo);
        }
    }
    visited.erase(pkg_name);
}

/**
 * 仓库包名（排序）。存在性判定与 show_all 都需要"仓库里有哪些包"这个问题，
 * 而反向依赖表回答不了它（无依赖者的包不在表里）。
 */
std::vector<std::string> repo_package_names()
{
    std::vector<std::string> names;
    Repository repo;
    // 别把"索引加载失败"静默成"仓库里没有包"——那会让 `depend remove/abibreak` 报出错误的
    // "无受影响包"。统一入口 `load_index_or_warn()`（全仓同一条
    // `warning.repo_index_load_failed`）。
    if (!load_index_or_warn(repo)) return names;
    for (const auto& name : repo.packages() | std::views::keys) names.push_back(std::string(name));
    std::ranges::sort(names);
    return names;
}

}  // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  仓库反向依赖图构建（当目标包未安装时回退到仓库索引）
// ═══════════════════════════════════════════════════════════════════════════

namespace
{

/**
 * 从缓存的仓库索引文件构建反向依赖图：被依赖的包 → {直接依赖它的包}。
 *
 * **本函数不再有任何自己的解析/建图代码**（2026-10-03 收敛）：
 *   · 索引解析 → `Repository::load_index_from_file()`（唯一索引解析入口，见 repository.cpp）；
 *   · 建反图 → `repo::build_reverse_dependency_map()`（唯一反向依赖实现，见 repo/revdep.hpp）。
 * 收敛前这里曾是一份**独立实现**，且**只看 SONAME 边、完全忽略显式 `deps`** —— 对手写 deps
 * 的包（纯 Python 包 / `xwayland` / **dlopen** 场景，见 CLAUDE.md 的 deps 例外清单）会漏掉
 * 整条边，让 `depend remove` / `depend abibreak` 给出**少算的清单**（静默的错误答案）。
 * 更早还自带过一套要求 ≥5 字段的解析器，会把 4 字段的索引行整行丢掉 —— 那类"第二实现必漂移"
 * 的形态在这里已经消灭两轮，别再长回来。
 *
 * **只按路径读、绝不下载**：优先读 `tmp` 里那份已下载的缓存副本，回退到本地镜像路径。
 * 这不是为了省流量，而是因为本函数在"索引不存在"时返回**空图**：若改成
 * `Repository::load_index()`（它会为远程镜像 `download_file`），"没有索引"就会变成
 * "偷偷联网下载"。真要联网取索引时，那次下载由 `load_repo_revdep()` 的探测完成。
 *
 * @param edges 收哪些边 —— `depend remove` 与 `depend abibreak` **有意不同**，见 revdep.hpp
 */
std::unordered_map<std::string, std::unordered_set<std::string>> build_repo_revdep_map(
    RevdepEdges edges)
{
    // 优先读取远程缓存索引（下载到临时目录的）
    fs::path idx = Config::get_tmp_dir() / constants::REPO_INDEX_TMP;
    if (!exists_follow(idx)) {
        // 回退到本地镜像的索引文件
        try {
            std::string mirror_url = Config::instance().get_mirror_url();
            std::string arch = Config::instance().get_architecture();
            std::string path_str = mirror_url;
            if (path_str.find(constants::PROTOCOL_FILE) == 0) path_str = path_str.substr(7);
            idx = fs::path(path_str) / arch / std::string(constants::REPO_INDEX_FILE);
        } catch (const std::exception& e) {
            // 此前这里是 `catch (...) { return rev; }` —— **静默**返回空图，于是
            // `depend remove` / `depend abibreak` 打印"无受影响包"（给的是错误答案，不是报错），
            // 与下面"打不开/读不出就抛"的取向正好相反。现在至少出声。
            // 键与 `resolve_index_path()` 的同一种失败同源（都是"读不出镜像配置"）。
            log_warning(string_format("warning.repo_mirror_config", e.what()));
            return {};
        }
    }
    if (!exists_follow(idx)) return {};

    // 索引"存在但打不开"（FIFO/设备）或"读不出"（是目录 —— Linux 下 open 成功、随后读才
    // 失败；或 EIO）时必须**报错而不是静默当空**：静默会让反向依赖图残缺 ⇒ 用户拿到
    // "无受影响包"并据此删包。这是**本消费者自己的策略** —— `Repository::load_index()` 对
    // 同一个失败是"降级成空仓库 + 告警"（离线装包是常见用法），两者有意不同，
    // 所以共享的解析入口只报告结果、由这里决定 fail-closed。
    Repository repo;
    if (!repo.load_index_from_file(idx)) {
        throw LpkgException(string_format("error.read_file_failed", idx.string()));
    }
    return build_reverse_dependency_map(repo, edges);
}

/** 在仓库反向依赖图上做传递 BFS，收集所有间接依赖者 */
void repo_transitive_rdeps(
    const std::string& pkg,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& rev,
    std::unordered_set<std::string>& result, std::unordered_set<std::string>& visited)
{
    if (!visited.insert(pkg).second) return;
    auto it = rev.find(pkg);
    if (it == rev.end()) return;
    for (const auto& dep : it->second) {
        if (dep != pkg && result.insert(dep).second)
            repo_transitive_rdeps(dep, rev, result, visited);
    }
}

/**
 * 加载仓库并构建反向依赖图；索引不可用时返回空 map。
 *
 * ⚠️ 下面这次 `load_index()` **只为告警**，它**不喂给**返回的图 —— `build_repo_revdep_map()`
 * 自己按路径重读一遍索引（索引因此被加载/解析多遍，属已知取舍，见 `scan_remove_tree()` 的
 * 说明）。所以这里只用一个局部、名字也点名此意，**别**把它误读成"图的数据来源"；它的价值是
 * 让"索引读不出来"这件事落一条 `warning.repo_index_load_failed`。
 *
 * @param edges 收哪些边 —— 由调用方按语义选（`depend remove` 两种边都要、`depend abibreak`
 *              只看 SONAME 边），见 `repo/revdep.hpp`。
 */
auto load_repo_revdep(RevdepEdges edges)
    -> std::unordered_map<std::string, std::unordered_set<std::string>>
{
    // 只为告警：读一次索引，读不出来就落一条 warning（真正的图由 build_repo_revdep_map() 自建，
    // 这次的结果**不喂给它** —— 见上面的说明）。走统一入口，别在这里各写一份 try/catch。
    // 顺带一提：这一步**会**为远程镜像下载索引（那是 `load_index()` 的既有行为），
    // 而紧随其后的建图只读本地缓存 —— 所以"绝不下载"是建图那一层的契约，不是这里的。
    Repository probe;
    (void)load_index_or_warn(probe);
    return build_repo_revdep_map(edges);
}

/**
 * 递归构建"移除"依赖树（仓库路径）
 * 恒用仓库反向依赖图（needed_so → 提供者推导），计算整个仓库删除该包的影响
 * @param affected 待处理节点池，用集合跟踪已处理的节点避免重复
 */
void build_remove_tree_repo(
    ScanNode& node, const std::string& node_name,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& rev,
    std::unordered_set<std::string>& affected)
{
    auto it = rev.find(node_name);
    if (it == rev.end()) return;
    for (const auto& dep : it->second) {
        if (dep == node_name || !affected.contains(dep)) continue;
        affected.erase(dep);
        ScanNode child;
        child.name = dep;
        child.version = get_string("info.depend_version_in_repo");
        child.status = ScanStatus::REMOVED;
        child.reason = string_format("info.depend_reason_depends_on_repo", node_name);
        build_remove_tree_repo(child, dep, rev, affected);
        node.children.push_back(std::move(child));
    }
}

/**
 * 递归构建安装依赖树
 * 已安装的包标记为 KEEP（--all 才显示），需要安装的标记为 INSTALL
 * @param seen 已展开节点池，防止循环依赖导致的无限递归
 */
void build_install_tree(ScanNode* parent, const std::string& parent_name, const DepMap& plan,
                        std::set<std::string>& seen, Repository& repo, bool show_all)
{
    auto pit = plan.find(parent_name);
    if (pit == plan.end()) return;

    for (const auto& dep : pit->second.deps) {
        // 处理虚拟包（依赖的包名可能是 capabilities）
        std::string real = dep.name;
        auto dver = Cache::instance().get_installed_version(dep.name);
        if (dver.empty() && !plan.contains(dep.name)) {
            if (auto prov = repo.find_provider(dep.name)) real = prov->name;
        }

        auto dit = plan.find(real);
        if (dit == plan.end()) continue;
        if (!seen.insert(real).second) continue;

        ScanNode child;
        child.name = dit->second.name;
        child.version = dit->second.version;
        child.status = dit->second.already_installed ? ScanStatus::KEEP : ScanStatus::INSTALL;
        child.reason =
            get_string(dit->second.already_installed ? "info.depend_reason_already_installed"
                                                     : "info.depend_reason_dependency");

        build_install_tree(&child, real, plan, seen, repo, show_all);

        if (child.is_affected() || show_all) parent->children.push_back(std::move(child));
    }
}

}  // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  scan_remove_tree — 移除依赖扫描
//  若包未安装则回退到仓库分析，否则从本地缓存构建反向依赖树
// ═══════════════════════════════════════════════════════════════════════════

ScanNode scan_remove_tree(const std::string& pkg_name, bool show_all)
{
    // 恒用仓库反向依赖图：计算整个仓库删除该包的影响，不看本地装了啥。
    // **两种边都收**（SONAME 边 + 显式 deps 边）：删包问的是"谁依赖它"，而手写 deps 的包
    // （纯 Python / xwayland / dlopen）**没有** SONAME 边 —— 只看 SONAME 会漏掉它们。
    auto rev = load_repo_revdep(RevdepEdges::DepsAndSoname);
    // 仓库包名清单**只算一次**并复用：它内建 Repository + load_index + 全量排序，而
    // 同一条 `depend remove` 此前会算两遍（存在性判定一次、show_all 再一次），再加上
    // load_repo_revdep 的那次加载 —— 索引被加载/解析最多 4 遍。两次调用的输入相同、结果
    // 确定，复用同一份向量**不改变行为**（唯一差异是少打一条重复的
    // `warning.repo_index_load_failed`）。
    const std::vector<std::string> repo_names = repo_package_names();
    // 存在性判定：**仓库索引里有，或图上出现过**（并集，严格比"只看反向依赖表"宽松）。
    // 只看反向依赖表时，没有任何依赖者的包会被误报成 "not found in repository"（历史 TODO G2）；
    // 保留图上判定则覆盖索引读不到/被裁剪的场景，两者都不放过。
    if (std::ranges::find(repo_names, pkg_name) == repo_names.end() &&
        rev.find(pkg_name) == rev.end()) {
        ScanNode r;
        r.name = pkg_name;
        r.version = get_string("info.depend_version_not_found");
        r.status = ScanStatus::REMOVED;
        r.reason = get_string("info.depend_reason_not_found_repo");
        return r;
    }

    std::unordered_set<std::string> affected, visited;
    repo_transitive_rdeps(pkg_name, rev, affected, visited);
    affected.insert(pkg_name);

    // ⚠️ `build_remove_tree_repo` 会把**已经渲染过的**节点逐个从 `affected` 里 `erase`
    // （那是它防重复的机制）—— 树建完之后 `affected` 基本是空的。下面 `show_all` 的过滤若
    // 直接用它，等于**什么都没过滤**：刚刚标成"将被移除"的包会被原样再列一遍、还写着
    // "不受影响"，输出自相矛盾。先快照一份"哪些包已经展示过"。
    const std::unordered_set<std::string> already_shown = affected;

    ScanNode root;
    root.name = pkg_name;
    root.version = version_or_missing(pkg_name);
    root.status = ScanStatus::REMOVED;
    root.reason = get_string("info.depend_reason_target_repo");
    affected.erase(pkg_name);
    build_remove_tree_repo(root, pkg_name, rev, affected);

    // show_all：与 build_install_tree 的 show_all 同义——不只显示受影响节点，
    // 把其余仓库包也作为"不受影响"列出（此前该形参被丢弃，`depend remove --all`
    // 与不带 --all 完全一样，历史 TODO G2）
    if (show_all) {
        for (const auto& name : repo_names) {
            if (already_shown.contains(name)) continue;
            ScanNode keep;
            keep.name = name;
            keep.version = version_or_missing(name);
            keep.status = ScanStatus::KEEP;
            keep.reason = get_string("info.depend_reason_unaffected");
            root.children.push_back(std::move(keep));
        }
    }
    return root;
}

// ═══════════════════════════════════════════════════════════════════════════
//  scan_abibreak_tree — ABI 断裂扫描
//  只有直接依赖需要重构建，间接依赖被中间层的抽象接口屏蔽
// ═══════════════════════════════════════════════════════════════════════════

ScanNode scan_abibreak_tree(const std::string& pkg_name, bool show_all)
{
    // 恒用仓库反向依赖图：计算整个仓库里需要该包 SONAME 的直接依赖者。
    // **只看 SONAME 边**：ABI 断裂后要重构建的是**链接了那个 .so** 的包；只声明包依赖、
    // 并不链接它的包（手写 deps 的那些）不需要重构建 —— 把 deps 边并进来会让 REBUILD 清单虚增。
    auto rev = load_repo_revdep(RevdepEdges::SonameOnly);
    ScanNode root;
    root.name = pkg_name;
    root.version = version_or_missing(pkg_name);
    root.status = ScanStatus::ABI_CHANGED;
    root.reason = get_string("info.depend_reason_abi_changed");

    auto it = rev.find(pkg_name);
    if (it != rev.end()) {
        for (const auto& dep : it->second) {
            if (dep == pkg_name) continue;
            ScanNode child;
            child.name = dep;
            child.version = get_string("info.depend_version_in_repo");
            child.status = ScanStatus::REBUILD;
            child.reason = string_format("info.depend_reason_direct_dep_of_repo", pkg_name);

            // --all 模式下显示间接依赖（标记为不变）
            if (show_all) {
                auto git = rev.find(dep);
                if (git != rev.end()) {
                    for (const auto& gdep : git->second) {
                        if (gdep == dep || gdep == pkg_name) continue;
                        ScanNode k;
                        k.name = gdep;
                        k.version = get_string("info.depend_version_in_repo");
                        k.status = ScanStatus::KEEP;
                        k.reason = get_string("info.depend_reason_indirect_abi_kept");
                        child.children.push_back(std::move(k));
                    }
                }
            }
            root.children.push_back(std::move(child));
        }
    }
    return root;
}

// ═══════════════════════════════════════════════════════════════════════════
//  scan_install_tree — 安装依赖扫描
//  显示安装某包需要新增安装的传递依赖（已安装的标记为 KEEP）
// ═══════════════════════════════════════════════════════════════════════════

ScanNode scan_install_tree(const std::string& pkg_name, bool show_all)
{
    Repository repo;
    (void)load_index_or_warn(repo);  // 统一入口：失败只告警（见 repository.hpp）

    std::string target_name = pkg_name;
    std::string target_ver(constants::VER_LATEST);
    // 支持 "包名:版本号" 格式
    if (auto pos = pkg_name.find(':'); pos != std::string_view::npos) {
        target_name = pkg_name.substr(0, pos);
        target_ver = pkg_name.substr(pos + 1);
    }

    DepMap plan;
    std::set<std::string> visited;
    resolve_transitive_deps(target_name, target_ver, plan, visited, repo);

    if (plan.empty()) {
        ScanNode r;
        r.name = target_name;
        r.version = version_or_missing(target_name);
        r.status = ScanStatus::KEEP;
        r.reason = get_string("info.depend_reason_pkg_not_found_repo");
        return r;
    }

    // ⚠️ `resolve_transitive_deps` 把计划按**解析后的包名**建键（能力名会经 `find_provider`
    // 落到提供者身上），所以这里**不能**直接 `plan.find(target_name)`：`lpkg depend install
    // libc.so.6` 这类能力目标会落成 `glibc`，`plan` 里根本没有 `"libc.so.6"` 这个键 ——
    // `find` 返回 `end()`，解引用即 UB（实测：段错误，或读到垃圾包名/版本）。
    // 按**同一套**回退先把目标解析成包名再查；`build_install_tree` 同理（它内部也是
    // `plan.find(parent_name)`，传能力名会**立刻返回**、整棵树变成空的）。
    std::string resolved = target_name;
    if (!plan.contains(target_name)) {
        if (const auto prov = repo.find_provider(target_name)) resolved = prov->name;
    }
    const auto it = plan.find(resolved);
    if (it == plan.end()) {
        ScanNode r;
        r.name = target_name;
        r.version = version_or_missing(target_name);
        r.status = ScanStatus::KEEP;
        r.reason = get_string("info.depend_reason_pkg_not_found_repo");
        return r;
    }

    ScanNode root;
    root.name = it->second.name;
    root.version = it->second.version;
    root.status = it->second.already_installed ? ScanStatus::KEEP : ScanStatus::INSTALL;
    root.reason = get_string(it->second.already_installed ? "info.depend_reason_already_installed"
                                                          : "info.depend_reason_target");

    // 两个名字都要记进 `seen`：能力名与它解析出的包名指的是**同一个**根节点，
    // 漏掉任何一个都会让根在它自己的子节点里再出现一次。
    std::set<std::string> seen{target_name, resolved};
    build_install_tree(&root, resolved, plan, seen, repo, show_all);
    return root;
}

/** 从本地 .lpkg 文件扫描安装依赖（直接读取文件内 metadata.json） */
ScanNode scan_install_from_file(const fs::path& lpkg_path, bool show_all)
{
    json meta;
    std::string name;
    std::string version;
    try {
        meta = detail::read_archive_metadata(fs::absolute(lpkg_path));
        // 与安装侧（`package_manager.cpp` 的本地包参数解析）同一写法：读取与字段提取共用
        // 同一个 try。此前 `.at()` 在 try **之外**裸调用，metadata.json 存在却缺
        // `name`/`version` 时会逸出 `json::out_of_range`（未本地化、也不是 LpkgException），
        // 让 `lpkg depend install ./x.lpkg` 直接带着原始 JSON 异常崩掉。
        name = meta.at(std::string(constants::J_NAME));
        version = meta.at(std::string(constants::J_VERSION));
    } catch (const std::exception& e) {
        ScanNode r;
        r.name = lpkg_path.filename().string();
        r.status = ScanStatus::KEEP;
        r.reason = string_format("info.depend_reason_error", e.what());
        return r;
    }

    auto deps = detail::parse_dep_strings(
        meta.value(std::string(constants::J_DEPS), std::vector<std::string>{}));

    bool not_installed = Cache::instance().get_installed_version(name).empty();
    ScanNode root;
    root.name = name;
    root.version = version;
    root.status = not_installed ? ScanStatus::INSTALL : ScanStatus::KEEP;
    root.reason = get_string(not_installed ? "info.depend_reason_target_local"
                                           : "info.depend_reason_already_installed_local");

    // 解析传递依赖仓库
    Repository repo;
    (void)load_index_or_warn(repo);  // 统一入口：失败只告警（见 repository.hpp）

    DepMap plan;
    std::set<std::string> visited;
    for (const auto& dep : deps) {
        auto dv = resolve_dep_version(repo, dep);
        if (!dv) {
            warn_unsatisfiable(dep);
            continue;
        }
        resolve_transitive_deps(dep.name, *dv, plan, visited, repo);
    }

    std::set<std::string> seen{name};
    for (const auto& dep : deps) {
        std::string real = dep.name;
        std::string iv = Cache::instance().get_installed_version(dep.name);
        if (iv.empty()) {
            if (auto prov = repo.find_provider(dep.name)) real = prov->name;
        }

        auto pit = plan.find(real);
        if (pit == plan.end()) {
            if (!iv.empty() && show_all && seen.insert(real).second) {
                ScanNode c;
                c.name = real;
                c.version = iv;
                c.status = ScanStatus::KEEP;
                c.reason = get_string("info.depend_reason_already_installed");
                root.children.push_back(std::move(c));
            }
            continue;
        }
        if (!seen.insert(real).second) continue;

        ScanNode child;
        child.name = pit->second.name;
        child.version = pit->second.version;
        child.status = pit->second.already_installed ? ScanStatus::KEEP : ScanStatus::INSTALL;
        child.reason =
            get_string(pit->second.already_installed ? "info.depend_reason_already_installed"
                                                     : "info.depend_reason_dependency");

        std::set<std::string> cs{real};
        build_install_tree(&child, real, plan, cs, repo, show_all);

        if (child.is_affected() || show_all) root.children.push_back(std::move(child));
    }
    return root;
}

// ═══════════════════════════════════════════════════════════════════════════
//  显示辅助函数
// ═══════════════════════════════════════════════════════════════════════════

/** 状态对应的 l10n 键（调用方 `get_string(...)` 取译文） */
std::string_view status_label_key(ScanStatus s)
{
    switch (s) {
        case ScanStatus::REMOVED:
            return "info.depend_status_removed";
        case ScanStatus::REBUILD:
            return "info.depend_status_rebuild";
        case ScanStatus::INSTALL:
            return "info.depend_status_install";
        case ScanStatus::ABI_CHANGED:
            return "info.depend_status_abi_changed";
        case ScanStatus::KEEP:
            return "info.depend_status_keep";
    }
    return "info.depend_status_unknown";
}

namespace
{

/** 返回状态对应的 ANSI 颜色码 */
std::string_view status_color(ScanStatus s)
{
    switch (s) {
        case ScanStatus::REMOVED:
            return constants::COLOR_RED;
        case ScanStatus::REBUILD:
            return constants::COLOR_YELLOW;
        case ScanStatus::INSTALL:
            return constants::COLOR_GREEN;
        case ScanStatus::ABI_CHANGED:
            return constants::COLOR_WHITE;
        case ScanStatus::KEEP:
            return constants::COLOR_RESET;
    }
    return constants::COLOR_RESET;
}

/** 递归打印子树（使用 unicode 框线字符） */
void print_subtree(const ScanNode& node, const std::string& prefix)
{
    for (size_t i = 0; i < node.children.size(); ++i) {
        const auto& child = node.children[i];
        bool last = (i == node.children.size() - 1);
        std::cout << prefix << (last ? "└── " : "├── ") << status_color(child.status) << child.name
                  << " (" << child.version << ") "
                  << "[" << get_string(std::string(status_label_key(child.status))) << "]"
                  << constants::COLOR_RESET;
        if (!child.reason.empty()) std::cout << "  (" << child.reason << ")";
        std::cout << "\n";
        print_subtree(child, prefix + (last ? "    " : "│   "));
    }
}

}  // anonymous namespace

/** 打印整棵依赖树（彩色 + unicode 框线） */
void print_tree(const ScanNode& node)
{
    std::cout << status_color(node.status) << node.name << " (" << node.version << ") "
              << "[" << get_string(std::string(status_label_key(node.status))) << "]"
              << constants::COLOR_RESET;
    if (!node.reason.empty()) std::cout << "  (" << node.reason << ")";
    std::cout << "\n";
    print_subtree(node, "");
}

}  // namespace depscan
