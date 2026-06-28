#include "package_manager.hpp"

#include "install_common.hpp"

#include "archive.hpp"
#include "db/cache.hpp"
#include "trigger/trigger.hpp"
#include "config/config.hpp"
#include "downloader.hpp"
#include "base/exception.hpp"
#include "crypto/hash.hpp"
#include "i18n/localization.hpp"
#include "base/utils.hpp"
#include "vercmp/version.hpp"
#include "repo/repository.hpp"
#include "base/constants.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

// =====================================================================
// 公开 API
// =====================================================================

/** 将缓存数据写回磁盘 */
void write_cache() {
    Cache::instance().write();
}

/**
 * 安装包的主入口
 * 流程：解析参数 -> 初始化仓库和缓存 -> 解析依赖 -> 静态一致性检查 ->
 * 用户确认 -> 实际安装（含元数据验证）-> 回滚处理 -> 触发运行
 */
void install_packages(const std::vector<std::string>& pkg_args,
                      const std::string& hash_file_path, bool force_reinstall) {
    Cache::instance().load();
    TmpDirManager tmp;
    Repository repo;
    try { repo.load_index(); }
    catch (const std::exception& e) {
        log_warning(string_format("warning.repo_index_load_failed", e.what()));
    }

    std::map<std::string, InstallPlan> plan;
    std::vector<std::string> order;
    std::map<std::string, fs::path> locals;
    std::vector<std::pair<std::string, std::string>> targets;

    std::string provided_hash;
    if (!hash_file_path.empty()) {
        std::ifstream hf(hash_file_path);
        if (!(hf >> provided_hash))
            throw LpkgException(get_string("error.read_hash_failed"));
    }

    // ── 安装参数解析 ─────────────────────────────────────────────────────
    // 支持三种参数格式：
    //   1. 本地包文件：以 .zst / .lpkg 结尾，或包含 '/' 路径分隔符
    //      → 从文件内 metadata.json 读取包名和版本
    //   2. 远程仓库包名+版本：格式 包名:版本号  (如 bash:5.2)
    //      → 从远程仓库下载匹配版本
    //   3. 仅包名：不含 ':' 的纯字符串（如 vim）
    //      → 从远程仓库获取最新版本
    // 解析结果写入 targets 列表，后续由 resolve_package_dependencies 处理
    for (const auto& arg : pkg_args) {
        const fs::path p(arg);
        if (p.extension() == constants::EXT_ZST
            || p.extension() == constants::EXT_LPKG
            || arg.find('/') != std::string::npos) {
            if (fs::exists(p)) {
                try {
                    json meta = detail::read_archive_metadata(fs::absolute(p));
                    std::string n = meta.at(std::string(constants::J_NAME));
                    std::string v = meta.at(std::string(constants::J_VERSION));
                    locals[n] = fs::absolute(p);
                    targets.emplace_back(n, v);
                } catch (const std::exception& e) {
                    log_error(string_format("warning.skip_invalid_local_pkg", arg, e.what()));
                }
            } else {
                log_error(string_format("error.local_pkg_not_found", arg));
            }
        } else {
            std::string n = arg, v = std::string(constants::VER_LATEST);
            if (const auto pos = arg.find(':'); pos != std::string::npos) {
                n = arg.substr(0, pos);
                v = arg.substr(pos + 1);
            }
            targets.emplace_back(n, v);
        }
    }

    InstallContext ctx{repo, plan, order, locals, targets,
                       force_reinstall, /*top_level=*/true, {}};

    // 第一阶段：初始依赖解析
    for (const auto& [n, v] : targets) {
        std::set<std::string> vs;
        detail::resolve_package_dependencies(n, v, true, ctx, vs);
    }

    if (!provided_hash.empty()) {
        if (locals.empty())
            throw LpkgException(get_string("error.hash_requires_local"));
        for (auto& [n, p] : plan)
            if (!p.local_path.empty()) p.sha256 = provided_hash;
    }

    // ── 第二阶段：静态一致性检查 ────────────────────────────────────────
    // 检查安装计划（即将安装的新版本）是否与已安装的其他包兼容。
    // 例如：已安装的 libfoo 依赖 libbar >= 2.0，但计划将 libbar 降级到 1.x。
    // 此阶段只做依赖图拓扑分析，无需下载包文件。
    // 动态元数据验证（下载包后对比实际 metadata.json）将在用户确认后、
    // install_packages_internal 中进行。

    if (plan.empty()) {
        log_info(get_string("info.all_packages_already_installed"));
        return;
    }

    if (auto broken = detail::check_plan_consistency(plan); !broken.empty()) {
        log_error(get_string("error.dependency_conflict_title"));
        if (user_confirms(get_string("prompt.remove_conflict_pkgs"))) {
            for (const auto& pkg : broken) remove_package(pkg, true);
            Cache::instance().write();
            install_packages(pkg_args, hash_file_path, force_reinstall);
            return;
        }
        log_info(get_string("info.installation_aborted"));
        return;
    }

    // ── 第三阶段：needed_so 完整性校验 ──────────────────────────────────
    // 在展示安装计划给用户之前，验证每个包声明的 SONAME 依赖
    // 在 repo / plan / 已安装缓存中有提供者。这是"唯一真相"的强制校验。
    {
        bool all_so_ok = true;
        std::string so_errors;
        for (const auto& [pname, pplan] : plan) {
            for (const auto& soname : pplan.needed_so) {
                bool provided = false;

                if (repo.find_provider(soname))
                    provided = true;

                if (!provided) {
                    for (const auto& [pn2, pp2] : plan) {
                        for (const auto& prov : pp2.provides) {
                            if (prov == soname) { provided = true; break; }
                        }
                        if (provided) break;
                    }
                }

                if (!provided) {
                    auto providers = Cache::instance().get_providers(soname);
                    for (const auto& p : providers) {
                        if (Cache::instance().is_installed(p) && !plan.contains(p)) {
                            provided = true; break;
                        }
                    }
                }

                if (!provided) {
                    all_so_ok = false;
                    so_errors += "  " + string_format("error.unresolved_soname", soname) + "\n";
                }
            }
        }
        if (!all_so_ok) {
            log_error(so_errors);
            throw LpkgException(string_format("error.dependency_conflict_title") + "\n" + so_errors);
        }
    }

    // ── 第四阶段：用户确认 ──────────────────────────────────────────────
    // 向用户展示将要安装/升级的包列表（区分用户显式指定的和自动解析的依赖），
    // 请求 y/n 确认。非交互模式 (-y/-n) 自动响应。

    std::string prompt;
    for (const auto& n : order) {
        const auto& p = plan.at(n);
        prompt += "  "
            + string_format(p.is_explicit ? "info.package_list_item"
                                          : "info.package_list_item_dep",
                            p.name, p.actual_version)
            + "\n";
    }
    if (!user_confirms(prompt + get_string("info.confirm_proceed"))) {
        log_info(get_string("info.installation_aborted"));
        return;
    }

    // ── 第五阶段：实际安装（含元数据验证和自动回滚） ──────────────────
    // install_packages_internal 对每个包执行以下子流程：
    //   1. 下载包 → 读取真实 metadata.json（验证仓库索引的 deps/provides）
    //   2. 若元数据与索引不匹配 → 更新仓库信息 → 回滚已安装包 → 重解析依赖 → 从头重试
    //   3. 匹配 → 执行 InstallationTask::run()（解压 → 依赖检查 → 冲突检测 → 写文件）
    // 任何安装异常触发部分回滚：逆序移除已成功安装的包
    ctx.successfully_installed.clear();
    ctx.installed_set.clear();
    try {
        install_packages_internal(ctx);
    } catch (const std::exception& e) {
        log_error(get_string("error.installation_failed_rolling_back"));
        for (const auto& name : ctx.successfully_installed | std::views::reverse) {
            try { remove_package(name, true); } catch (...) {}
        }
        Cache::instance().write();
        throw;
    }

    Cache::instance().write();
    TriggerManager::instance().run_all();
    log_info(get_string("info.install_complete"));
}

/**
 * 内部安装循环：按安装顺序逐个安装包
 * 每个包在安装前会进行元数据验证（下载并读取实际 metadata.json），
 * 若元数据与仓库索引不匹配，则更新计划并重新解析依赖后从头开始安装。
 * 这是确保依赖一致性的关键机制。
 */
/** 在 main.cpp 中声明，由 SIGINT 信号处理函数设置 */
extern std::atomic<bool> sigint_graceful;

void install_packages_internal(InstallContext& ctx) {
    size_t i = 0;
    while (i < ctx.install_order.size()) {
        if (sigint_graceful.load())
            throw LpkgException(get_string("info.sigint_aborted"));

        const std::string& n = ctx.install_order[i];
        ++i;

        if (ctx.installed_set.contains(n)) {
            continue;
        }

        auto& p = ctx.plan.at(n);

        // ── 元数据验证 ──────────────────────────────────────────────────
        // 仓库索引中的 deps/provides 可能过时或不完整，因此每个包在安装前
        // 都需要下载并读取真实的 metadata.json 进行核对。
        //
        // 验证流程：
        //   1. 下载实际包文件（或重用本地包路径）
        //   2. 从 tar.zst 中提取 metadata.json（非完整解压，仅读元数据）
        //   3. 对比实际依赖与仓库索引中的依赖列表
        //   4. 若不一致 → 更新仓库信息 → 回滚当前事务 → 重新解析依赖 → 从头安装
        //   5. 一致 → 保存已下载路径避免重复下载，继续安装
        //
        // 注意：已下载的归档路径会保存到 plan.local_path 中，这样后续
        // InstallationTask 可以跳过 download_and_verify 直接使用本地文件。
        if (!p.metadata_verified) {
            InstallationTask check_task(p.name, p.actual_version, p.is_explicit,
                Cache::instance().get_installed_version(p.name),
                p.local_path, p.sha256, p.force_reinstall);
            ensure_dir_exists(check_task.tmp_pkg_dir());
            check_task.download_and_verify_package();

            // 直接从归档读取 metadata.json，无需完整解压
            json meta = detail::read_archive_metadata(check_task.archive_path());
            std::vector<std::string> dep_strs = meta.value(
                std::string(constants::J_DEPS), std::vector<std::string>{});
            auto actual_deps = detail::parse_dep_strings(dep_strs);
            std::vector<std::string> actual_provides = meta.value(
                std::string(constants::J_PROVIDES), std::vector<std::string>{});
            std::vector<std::string> actual_needed_so = meta.value(
                std::string(constants::J_NEEDED_SO), std::vector<std::string>{});

            bool metadata_differs = (actual_deps.size() != p.dependencies.size())
                || (actual_provides != p.provides)
                || (actual_needed_so != p.needed_so);
            if (!metadata_differs) {
                for (size_t di = 0; di < actual_deps.size(); ++di) {
                    if (actual_deps[di].name != p.dependencies[di].name
                        || actual_deps[di].constraints != p.dependencies[di].constraints) {
                        metadata_differs = true; break;
                    }
                }
            }

            if (metadata_differs) {
                // 元数据不匹配时：回滚已安装的包，更新仓库信息，重新解析依赖
                log_info(string_format("info.resolving_metadata", p.name));
                ctx.repo.update_package_info(p.name, p.actual_version,
                    actual_deps, actual_provides, actual_needed_so);
                ctx.local_candidates[p.name] = check_task.archive_path();

                // 回滚当前事务中已安装的包
                for (const auto& done_name : ctx.successfully_installed | std::views::reverse) {
                    try { remove_package(done_name, true); } catch (...) {}
                }
                ctx.successfully_installed.clear();
                ctx.installed_set.clear();

                ctx.plan.clear();
                ctx.install_order.clear();
                for (const auto& [tn, tv] : ctx.targets) {
                    std::set<std::string> vs;
                    detail::resolve_package_dependencies(tn, tv, true, ctx, vs);
                }
                i = 0; // 从头开始重新安装
                continue;
            }

            // 保存已下载的归档路径，避免后续任务重复下载
            p.local_path = check_task.archive_path();
            p.metadata_verified = true;
        }

        // 执行实际安装
        InstallationTask task(p.name, p.actual_version, p.is_explicit,
                              Cache::instance().get_installed_version(p.name),
                              p.local_path, p.sha256, p.force_reinstall);
        try {
            task.run(&ctx);
            ctx.successfully_installed.push_back(p.name);
            ctx.installed_set.insert(p.name);
        } catch (const std::exception& e) {
            throw;
        }
    }
}

/**
 * 移除已安装的包
 * 检查是否为 essential 包、是否有其他包依赖它、是否有包依赖其提供的虚拟包名
 * force 模式下跳过所有安全检查
 */
void remove_package(const std::string& pkg_name, bool force) {
    const std::string ver = Cache::instance().get_installed_version(pkg_name);
    if (ver.empty()) {
        log_info(string_format("info.package_not_installed", pkg_name));
        return;
    }

    if (!force) {
        if (Cache::instance().is_essential(pkg_name)) {
            log_error(string_format("error.skip_remove_essential", pkg_name));
            return;
        }
        if (auto rdeps = Cache::instance().get_reverse_deps(pkg_name); !rdeps.empty()) {
            std::string list;
            for (const auto& d : rdeps) list += d + " ";
            log_info(string_format("info.skip_remove_dependency", pkg_name, list));
            return;
        }
        for (const auto& cap : Cache::instance().get_package_provides(pkg_name)) {
            if (auto rdeps = Cache::instance().get_reverse_deps(cap); !rdeps.empty()) {
                std::string list;
                for (const auto& d : rdeps) list += d + " ";
                log_info(string_format("info.skip_remove_dependency", cap, list));
                return;
            }
        }
    }

    log_info(string_format("info.removing_package", pkg_name));
    detail::run_hook(pkg_name, std::string(constants::PRERM_SH));
    remove_package_files(pkg_name, force);

    // 清理依赖、文档和钩子文件，删除逆向依赖记录
    auto& cache = Cache::instance();
    const fs::path dep_file = Config::instance().dep_dir() / pkg_name;
    if (fs::exists(dep_file)) {
        std::ifstream f(dep_file);
        std::string l;
        while (std::getline(f, l)) {
            std::stringstream ss(l);
            std::string dn;
            if (ss >> dn) cache.remove_reverse_dep(dn, pkg_name);
        }
    }

    fs::remove(dep_file);
    fs::remove(Config::instance().needed_so_dir() / pkg_name);
    fs::remove(Config::instance().docs_dir() / (pkg_name + std::string(constants::SUFFIX_MAN)));
    fs::remove_all(Config::instance().hooks_dir() / pkg_name);
    cache.remove_installed(pkg_name);
    log_info(string_format("info.package_removed_successfully", pkg_name));
}

/**
 * 删除包的所有文件
 * 非 force 模式会检查是否有其他包共享这些文件，若有则拒绝删除
 */
void remove_package_files(const std::string& pkg_name, bool force) {
    auto& cache = Cache::instance();
    auto owned_files = cache.get_package_files(pkg_name);
    if (owned_files.empty()) return;

    if (!force) {
        std::vector<std::string> shared;
        for (const auto& f : owned_files) {
            auto owners = cache.get_file_owners(f);
            for (const auto& owner : owners) {
                if (owner != pkg_name) {
                    shared.push_back(f);
                    break;
                }
            }
        }
        if (!shared.empty()) {
            std::string msg = get_string("error.shared_file_header")
                            + std::string(constants::NL);
            for (const auto& file : shared) {
                msg += "  "
                    + string_format("error.shared_file_entry", file, "other packages")
                    + std::string(constants::NL);
            }
            throw LpkgException(msg + get_string("error.removal_aborted"));
        }
    }

    std::vector<fs::path> paths;
    for (const auto& f : owned_files) paths.emplace_back(f);
    std::ranges::sort(paths, std::greater<>{});

    int count = 0;
    for (const auto& p : paths) {
        const fs::path phys = p.is_absolute()
            ? Config::instance().root_dir() / fs::path(p).relative_path()
            : Config::instance().root_dir() / p;
        if (fs::exists(phys) || fs::is_symlink(phys)) {
            fs::remove(phys);
            ++count;
        }
        cache.remove_file_owner(p.string(), pkg_name);
    }
    log_info(string_format("info.files_removed", count));

    for (const auto& cap : cache.get_package_provides(pkg_name)) {
        cache.remove_provider(cap, pkg_name);
    }
}

/**
 * 自动移除不再被任何包依赖的孤立包
 *
 * 算法：
 *   1. 从所有 held 包（用户显式标记的"根"包）出发，BFS 遍历依赖图，
 *      收集所有"必需"包（held 包 + 它们的传递依赖）。
 *   2. 已安装包中不在"必需"集合内的 → 视为孤立包，自动移除。
 *
 * 这样确保只保留用户需要的包及其传递依赖，其他自动安装的依赖可安全清理。
 * 隐式安装的包（作为依赖被引入）若不被任何 held 包依赖，则会被清理。
 */
void autoremove() {
    log_info(get_string("info.checking_autoremove"));
    const auto req = detail::get_all_required_packages();
    std::vector<std::string> to_rem;
    auto& cache = Cache::instance();
    {
        std::lock_guard lock(cache.get_mutex());
        for (const auto& name : cache.get_all_installed() | std::views::keys) {
            if (!req.contains(name)) to_rem.push_back(name);
        }
    }

    if (to_rem.empty()) {
        log_info(get_string("info.no_autoremove_packages"));
    } else {
        log_info(string_format("info.autoremove_candidates", to_rem.size()));
        for (const auto& n : to_rem) {
            try { remove_package(n, true); } catch (...) {}
        }
        log_info(string_format("info.autoremove_complete", to_rem.size()));
    }
}

/**
 * 升级所有已安装的包
 * 从仓库索引获取最新版本，与当前版本比较，对有更新的包执行升级安装
 *
 * 升级策略：
 *   - 遍历所有已安装包，对每个包在仓库中查找最新版本
 *   - 若最新版本 > 当前版本 → 创建 InstallationTask（传入旧版本号以触发文件清理）
 *   - 跳过 held 包（锁定包不会被升级）
 *   - 单个包升级失败不影响其他包（错误日志记录后继续）
 *   - 使用版比较算法确保 6.16.1 > 6.6.1
 */
void upgrade_packages() {
    log_info(get_string("info.checking_upgradable"));
    TmpDirManager tmp;
    Repository repo;
    try {
        repo.load_index();
    } catch (const std::exception& e) {
        log_warning(string_format("warning.repo_index_load_failed", e.what()));
        return;
    }

    // 在持有锁的情况下获取已安装包列表的快照（避免拷贝全量 map）
    std::vector<std::pair<std::string, std::string>> installed;
    {
        std::lock_guard lock(Cache::instance().get_mutex());
        for (const auto& [name, ver] : Cache::instance().get_all_installed()) {
            installed.emplace_back(name, ver);
        }
    }

    int count = 0;
    for (const auto& [n, curr] : installed) {
        auto opt = repo.find_package(n);
        if (!opt) continue;
        const std::string& lat = opt->version;

        if (version_compare(curr, lat)) {
            log_info(string_format("info.upgradable_found", n, curr, lat));
            try {
                log_info(string_format("info.upgrading_package", n, curr, lat));
                const std::string hash = opt->sha256;
                const bool held = Cache::instance().is_held(n);
                InstallationTask t(n, lat, held, curr, "", hash, false);
                t.run();
                ++count;
            } catch (const std::exception& e) {
                log_error(string_format("error.upgrade_failed", n, e.what()));
            }
        }
    }

    if (count > 0)
        log_info(string_format("info.upgraded_packages", count));
    else
        log_info(get_string("info.all_packages_latest"));
    Cache::instance().write();
}

/** 显示包的 man 页面内容 */
void show_man_page(const std::string& pkg_name) {
    const fs::path p = Config::instance().docs_dir() / (pkg_name + ".man");
    if (!fs::exists(p))
        throw LpkgException(string_format("error.no_man_page", pkg_name));
    std::ifstream f(p);
    if (!f.is_open())
        throw LpkgException(string_format("error.open_man_page_failed", p.string()));
    std::cout << f.rdbuf();
}

/**
 * 重新安装一个包
 * 支持本地包文件路径或包名，强制覆盖模式以确保文件被替换
 *
 * 参数类型判断规则：
 *   - 包含 '/' 或以 .lpkg 结尾 → 视为本地文件路径，从 metadata.json 读取包名
 *   - 否则 → 视为已安装的包名
 *
 * 若包未安装则退化为 install 操作。安装期间强制开启 --force-overwrite
 * 以确保旧文件被替换（而非产生 .lpkgnew 冲突）。
 */
void reinstall_package(const std::string& arg) {
    std::string name = arg;
    if (arg.find('/') != std::string::npos || arg.ends_with(".lpkg")) {
        try {
            json meta = detail::read_archive_metadata(fs::absolute(arg));
            name = meta.at(std::string(constants::J_NAME)).get<std::string>();
        } catch (...) {}
    }

    if (Cache::instance().get_installed_version(name).empty()) {
        install_packages({arg});
        return;
    }

    log_info(string_format("info.reinstalling_package", name));
    const bool old_ovr = Config::instance().force_overwrite_mode();
    Config::instance().set_force_overwrite_mode(true);
    try {
        install_packages({arg}, "", true);
    } catch (...) {
        Config::instance().set_force_overwrite_mode(old_ovr);
        throw;
    }
    Config::instance().set_force_overwrite_mode(old_ovr);
}

/** 查询指定包安装的所有文件列表 */
void query_package(const std::string& pkg_name) {
    if (Cache::instance().get_installed_version(pkg_name).empty()) {
        log_info(string_format("info.package_not_installed", pkg_name));
        return;
    }
    log_info(string_format("info.package_files", pkg_name));
    auto files = Cache::instance().get_package_files(pkg_name);
    for (const auto& f : files) {
        std::cout << "  " << f << "\n";
    }
}

/** 查询指定文件属于哪个包 */
void query_file(const std::string& filename) {
    auto& cache = Cache::instance();
    std::string target = filename;
    auto owners = cache.get_file_owners(target);

    // 尝试使用绝对路径解析
    if (owners.empty()) {
        try {
            const fs::path abs_p = fs::absolute(filename);
            if (abs_p.string().starts_with(Config::instance().root_dir().string())) {
                const std::string logical = "/" + fs::relative(abs_p, Config::instance().root_dir()).string();
                owners = cache.get_file_owners(logical);
                if (!owners.empty()) target = logical;
            }
        } catch (...) {}
    }

    // 尝试添加 / 前缀作为绝对路径
    if (owners.empty() && !fs::path(filename).is_absolute()) {
        const std::string fallback = (fs::path("/") / filename).string();
        owners = cache.get_file_owners(fallback);
        if (!owners.empty()) target = fallback;
    }

    if (owners.empty()) {
        log_info(string_format("info.file_not_owned", filename));
    } else {
        std::string os;
        for (auto it = owners.begin(); it != owners.end(); ++it) {
            os += *it + (std::next(it) == owners.end() ? "" : ", ");
        }
        log_info(string_format("info.file_owned_by", target, os));
    }
}
