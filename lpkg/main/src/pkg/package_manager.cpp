#include "package_manager.hpp"

#include "install_common.hpp"

#include "archive.hpp"
#include "db/cache.hpp"
#include "trigger/trigger.hpp"
#include "config/config.hpp"
#include "base/testing_breakpoints.hpp"
#include "downloader.hpp"
#include "base/exception.hpp"
#include "crypto/hash.hpp"
#include "i18n/localization.hpp"
#include "base/utils.hpp"
#include "vercmp/version.hpp"
#include "repo/repository.hpp"
#include "base/constants.hpp"
#include "transaction_log.hpp"

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
#include <random>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ── 事务回滚：撤销一个已成功安装的包 ────────────────────────────────
// 这是 register_package + copy_package_files 的逆操作。
// 与 remove_package 的语义区别：
//   remove_package → 用户主动删除（写 RM_BEGIN/BACKUP/RM_COMMIT，运行 prerm 钩子）
//   rollback       → 事务回滚（直接删文件，不运行钩子，写 ROLLBACK WAL 行）
// 审计视角：WAL 中 ROLLBACK + END 标记明确表示"因事务失败而回滚"。
static void rollback_installed_package(const std::string& pkg_name) {
    auto& cache = Cache::instance();
    const std::string ver = cache.get_installed_version(pkg_name);
    if (ver.empty()) return;

    log_info(string_format("info.batch_rollback_pkg", pkg_name));

    // 1. 清理文件归属 → 删除文件
    // get_package_files 返回 /usr/bin/... 格式的绝对路径，需用
    // .relative_path() 与 root_dir 正确拼接（否则 fs::path 忽略 root_dir）。
    auto owned_files = cache.get_package_files(pkg_name);
    for (const auto& f : owned_files) {
        if (f.ends_with('/')) continue;
        const fs::path phys = Config::instance().root_dir() / fs::path(f).relative_path();
        std::error_code ec;
        if (fs::exists(phys) || fs::is_symlink(phys))
            fs::remove(phys, ec);
        cache.remove_file_owner(f, pkg_name);
    }

    // 2. 清理目录（仅最后持有者时删除）
    for (const auto& f : owned_files) {
        if (!f.ends_with('/')) continue;
        cache.remove_file_owner(f, pkg_name);
        if (cache.get_file_owners(f).empty()) {
            const fs::path phys = Config::instance().root_dir() / fs::path(f).relative_path();
            std::error_code ec;
            if (fs::exists(phys) && fs::is_directory(phys) && fs::is_empty(phys))
                fs::remove(phys, ec);
        }
    }

    // 3. 撤销依赖关系
    std::error_code ec;
    const fs::path dep_file = Config::instance().dep_dir() / pkg_name;
    if (fs::exists(dep_file)) {
        std::ifstream f(dep_file);
        std::string line;
        while (std::getline(f, line)) {
            std::stringstream ss(line);
            std::string dn;
            if (ss >> dn)
                cache.remove_reverse_dep(dn, pkg_name);
        }
    }
    fs::remove(dep_file, ec);

    // 4. 撤销 needed_so
    fs::remove(Config::instance().needed_so_dir() / pkg_name, ec);

    // 5. 撤销 man page
    fs::remove(Config::instance().docs_dir() / (pkg_name + std::string(constants::SUFFIX_MAN)), ec);

    // 6. 撤销 hooks
    fs::remove_all(Config::instance().hooks_dir() / pkg_name, ec);

    // 7. 撤销 providers
    for (const auto& cap : cache.get_package_provides(pkg_name))
        cache.remove_provider(cap, pkg_name);

    // 8. 撤销注册
    cache.remove_installed(pkg_name);

    // 9. 将内存缓存的变更落盘（WAL 保护），写入 DB/DBNEW 日志，
    //    配合外层 COMMIT_PKGS 构成完整的事务记录。
    cache.write(pkg_name);

    // 10. WAL：ROLLBACK + END 明确标记"事务回滚"
    TransactionLog::log_raw("ROLLBACK " + pkg_name + " " + ver);
    TransactionLog::log_raw("END " + pkg_name + " " + ver);
}

// ── 批量回滚：撤销同一批次中所有已成功安装的包 ────────────────────
static void rollback_committed_packages(std::vector<std::string>& installed) {
    for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
        try {
            rollback_installed_package(*it);
        } catch (const std::exception& e) {
            log_warning(string_format("warning.batch_rollback_failed", *it, e.what()));
        }
    }
    installed.clear();
}

/**
 * 在统一批次事务中执行一组安装/升级操作。
 *
 * 事务协议：
 *   正向：BEGIN_PKGS → body() → Cache::write(wal_tag) → COMMIT_PKGS
 *   异常：body() 抛异常 → rollback_committed_packages → COMMIT_PKGS → rethrow
 *
 * 测试断点（由 body 外的框架层管理）：
 *   break_after_begin_pkgs   — BEGIN_PKGS 刚写入后
 *   break_before_db_write    — 所有包操作完成、Cache::write 前
 *   break_before_commit_pkgs — Cache::write 后、COMMIT_PKGS 前
 *
 * 与 install_packages_internal / upgrade_packages 的内层 catch 协作：
 *   内层 catch 已做文件级回滚并清空 success 列表 → 外层调用是空循环 → 幂等。
 *   外层 breakpoint 触发时 install 已成功返回（success 未清空）→ 外层完成回滚。
 *
 * @param success  跟踪已成功操作的包名列表（用于回滚），异常时被清空
 * @param wal_tag  Cache::write 的 WAL 标签（如 "pkgs"、"upgrade"）
 * @param total    本次事务涉及的包数量
 * @param body     实际安装/升级操作的回调
 */
static void with_batch_transaction(
    std::vector<std::string>& success,
    const std::string& wal_tag,
    size_t total,
    const std::function<void()>& body)
{
    TransactionLog::log_raw("BEGIN_PKGS " + std::to_string(total));

    try {
        // 测试断点：BEGIN_PKGS 后立即检查（必须在 try 内，确保 catch 补 COMMIT_PKGS）
        if (Config::instance().testing_mode())
            testing::check_and_break(testing::break_after_begin_pkgs);

        body();

        // 测试断点：所有包操作完成、DB 写入前
        if (Config::instance().testing_mode())
            testing::check_and_break(testing::break_before_db_write);

        // 先落盘数据库（WAL 保护），再写 COMMIT_PKGS。
        // 若 crash 在 Cache::write 与 COMMIT_PKGS 之间，
        // 恢复时因无 COMMIT_PKGS 而回滚整个事务（含 DB 备份）。
        Cache::instance().write(wal_tag);

        // 测试断点：DB 写入后、COMMIT_PKGS 前
        if (Config::instance().testing_mode())
            testing::check_and_break(testing::break_before_commit_pkgs);

        TransactionLog::log_raw("COMMIT_PKGS");
    } catch (...) {
        // 回滚同一批次中所有已成功安装的包。
        // 内层 catch（如 install_packages_internal）已清空 success 时：
        //   空循环直接返回，幂等安全。
        // 外层 breakpoint 触发时（如 break_before_db_write）：
        //   install 已完成，success 非空 → 全量回滚。
        rollback_committed_packages(success);

        // 补写 COMMIT_PKGS 标记批次完结。文件级清理已由内层 ROLLBACK+END
        // 完成或本层 rollback_installed_package 完成。批次关闭后 rec 跳过。
        TransactionLog::log_raw("COMMIT_PKGS");
        throw;
    }
}

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
    TransactionLog::trim_completed();  // 新事务开始前压缩已完结日志
    Cache::instance().load();
    TmpDirManager tmp;
    Repository repo;
    try { repo.load_index(); }
    catch (const std::exception& e) {
        log_warning(string_format("warning.repo_index_load_failed", e.what()));
    }

    // 测试断点：安装开始前（可用于模拟安装前崩溃）
    if (Config::instance().testing_mode())
        testing::check_and_break(testing::break_before_install);

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

    // 一致性重试循环。每次迭代至少移除一个冲突包，系统包集单调递减
    // → 必然终止，无需固定重试上限（原递归设计也无上限，仅受栈深度限制）。
    // 每次重试时清除当前 plan 并重新解析，反映移除后的最新依赖状态。
    while (true) {
        plan.clear();
        order.clear();
        ctx.plan = plan;
        ctx.install_order = order;
        ctx.successfully_installed.clear();
        ctx.installed_set.clear();

        // 第一阶段：依赖解析
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

        if (plan.empty()) {
            log_info(get_string("info.all_packages_already_installed"));
            return;
        }

        // ── 第二阶段：静态一致性检查 ──────────────────────────────────
        if (auto broken = detail::check_plan_consistency(plan); !broken.empty()) {
            log_error(get_string("error.dependency_conflict_title"));
            if (user_confirms(get_string("prompt.remove_conflict_pkgs"))) {
                for (const auto& pkg : broken) remove_package(pkg, true);
                Cache::instance().write();
                continue;  // 重新解析
            }
            log_info(get_string("info.installation_aborted"));
            return;
        }

        // ── 第二·五阶段：needed_so 升级一致性检查 ────────────────────
        if (auto nso_broken = detail::check_needed_so_consistency(plan);
            !nso_broken.empty()) {
            log_error(get_string("error.dependency_conflict_title"));
            std::string nso_msg;
            for (const auto& pkg : nso_broken)
                nso_msg += "  " + pkg + "\n";
            log_error(nso_msg);
            if (user_confirms(get_string("prompt.remove_conflict_pkgs"))) {
                for (const auto& pkg : nso_broken) remove_package(pkg, true);
                Cache::instance().write();
                continue;  // 重新解析
            }
            log_info(get_string("info.installation_aborted"));
            return;
        }

        // 全部检查通过
        break;
    }

    // ── 第三阶段：needed_so 完整性校验 ──────────────────────────────────
    // 委托给共享函数，install 和 upgrade 使用相同的 3-tier 逻辑
    detail::check_forward_soname_integrity(plan, repo);

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
    // 跨包的事务回滚交由 with_batch_transaction 统一管理。
    ctx.successfully_installed.clear();
    ctx.installed_set.clear();

    // ── 统一 WAL 事务协议 ──────────────────────────────────────────────
    // 无论单包还是批量安装，都用 BEGIN_PKGS / COMMIT_PKGS 包裹，
    // 保证只要 COMMIT_PKGS 未写入，恢复机制就能完整回滚（含 DB）。
    // 单包走批量路径后，Cache::write("pkgs") 走 WAL 保护路径，
    // .lpkg_db_bak 在恢复时可还原 DB 至安装前状态。
    with_batch_transaction(ctx.successfully_installed, "pkgs",
        ctx.install_order.size(),
        [&ctx]() { install_packages_internal(ctx); });

    // 测试断点：COMMIT_PKGS 后
    if (Config::instance().testing_mode())
        testing::check_and_break(testing::break_after_commit_pkgs);

    Cache::instance().cleanup_db_backups();
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

        // 测试断点：每个包安装前
        if (Config::instance().testing_mode())
            testing::check_and_break(testing::break_before_each_pkg_install);

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
                // 元数据不匹配时：更新仓库信息，重新解析依赖
                log_info(string_format("info.resolving_metadata", p.name));
                ctx.repo.update_package_info(p.name, p.actual_version,
                    actual_deps, actual_provides, actual_needed_so);
                ctx.local_candidates[p.name] = check_task.archive_path();

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
        } catch (...) {
            // 当前包已由 InstallationTask::run() 的 catch 完成文件级回滚。
            // 同一批次中已成功安装的前序包也需全部撤销，以保证批次原子性。
            rollback_committed_packages(ctx.successfully_installed);
            throw;
        }

        // 测试断点：每个包安装后
        if (Config::instance().testing_mode())
            testing::check_and_break(testing::break_after_each_pkg_install);
    }
}

/**
 * 移除已安装的包
 * 检查是否为 essential 包、是否有其他包依赖它、是否有包依赖其提供的虚拟包名
 * force 模式下跳过所有安全检查
 */
void remove_package(const std::string& pkg_name, bool force, bool wrap_in_txn) {
    if (wrap_in_txn)
        TransactionLog::trim_completed();
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

    // 测试断点：移除开始前（安全检查已通过、RM_BEGIN 尚未写）
    if (Config::instance().testing_mode())
        testing::check_and_break(testing::break_before_remove);

    log_info(string_format("info.removing_package", pkg_name));

    // ── WAL：统一事务开始（移除也走 BEGIN_PKGS / COMMIT_PKGS 模型） ──
    if (wrap_in_txn)
        TransactionLog::log_raw("BEGIN_PKGS 1");
    TransactionLog::log_raw("RM_BEGIN " + pkg_name + " " + ver);

    // 测试断点：RM_BEGIN 后、备份前
    if (Config::instance().testing_mode())
        testing::check_and_break(testing::break_after_rm_begin);

    detail::run_hook(pkg_name, std::string(constants::PRERM_SH));

    auto& cache = Cache::instance();
    auto owned_entries = cache.get_package_files(pkg_name);

    // ── 共享文件检查（修改文件前执行，防止备份后才发现冲突） ──────────
    if (!force && !owned_entries.empty()) {
        std::vector<std::pair<std::string, std::string>> shared;
        for (const auto& entry : owned_entries) {
            if (entry.ends_with('/')) continue;
            auto owners = cache.get_file_owners(entry);
            std::string others;
            for (const auto& owner : owners) {
                if (owner != pkg_name) {
                    if (!others.empty()) others += ", ";
                    others += owner;
                }
            }
            if (!others.empty()) shared.emplace_back(entry, others);
        }
        if (!shared.empty()) {
            std::string msg = get_string("error.shared_file_header") + "\n";
            for (const auto& [file, owners] : shared)
                msg += "  " + string_format("error.shared_file_entry", file, owners) + "\n";
            throw LpkgException(msg + get_string("error.removal_aborted"));
        }
    }

    // ── 备份阶段：将每个包文件 rename 到 .lpkg_bak（WAL 先写后操作） ───
    std::vector<std::pair<fs::path, fs::path>> backups;
    int file_count = 0;

    if (!owned_entries.empty()) {
        std::vector<fs::path> paths;
        for (const auto& e : owned_entries) paths.emplace_back(e);
        std::ranges::sort(paths, std::greater<>{});

        for (const auto& p : paths) {
            std::string path_str = p.string();
            if (path_str.ends_with('/')) continue;
            const fs::path phys = p.is_absolute()
                ? Config::instance().root_dir() / fs::path(p).relative_path()
                : Config::instance().root_dir() / p;

            if (fs::exists(phys) || fs::is_symlink(phys)) {
                fs::path bak = phys;
                bak += std::string(constants::SUFFIX_LPKG_BAK) + pkg_name;
                // WAL 顺序：先写 BACKUP 日志，再 rename
                TransactionLog::log_raw("BACKUP " + phys.string() + " → " + bak.string());
                fs::rename(phys, bak);
                backups.emplace_back(phys, bak);
                ++file_count;

                // 测试断点：每个文件备份时检查（模拟逐文件中断）
                if (Config::instance().testing_mode())
                    testing::check_and_break(testing::break_during_rm_backup);
            }
        }
    }

    if (file_count > 0)
        log_info(string_format("info.files_removed", file_count));

    // 测试断点：备份完成后
    if (Config::instance().testing_mode())
        testing::check_and_break(testing::break_after_rm_backup);

    // ── 阶段 2：从磁盘移除文件（rm）→ 清 .bak（仅清理目录内的）→ 删目录（RM_DIR） ──
    remove_package_files(pkg_name, force);

    // ── 阶段 3：释放目录所有权 → 删目录（含 .lpkg_bak 清扫） → WAL 记录 ─────────
    // 设计说明：
    //   .lpkg_bak 备份文件的清理必须在 RM_COMMIT 之后（保证能回滚），
    //   但 .lpkg_bak 若留在目标目录内会阻塞 fs::remove(dir)。
    //   因此这里分两步：
    //     (a) 目录删除前：只清理目标目录内属于本包的 .lpkg_bak（记录在 WAL）
    //     (b) 目录删除后：在 RM_COMMIT 之后才清理全部 .lpkg_bak
    //   这样既保证 RM_DIR 能成功，又保证回滚时非目录内的备份文件还在。
    //
    // 为什么需要 RM_BAK_CLN WAL 记录：
    //   若 crash 在 RM_DIR 之后 / RM_COMMIT 之前，恢复时 BACKUP 条目
    //   的 .bak 可能已被 (a) 清除。Recovery 看到 RM_BAK_CLN 就知道
    //   该 .bak 是"为清空目录而提前删除的"，而不是"意外丢失"。
    //   此时文件内容在 RM_DIR 重建的目录中不可恢复，但系统一致性保持。
    std::error_code ec;

    // 3a：收集需释放的目录列表
    std::vector<fs::path> dir_paths;
    for (const auto& e : owned_entries)
        if (e.ends_with('/'))
            dir_paths.emplace_back(fs::path(e));
    std::ranges::sort(dir_paths, std::greater<>{});

    // 测试断点：RM_DIR 前
    if (Config::instance().testing_mode())
        testing::check_and_break(testing::break_before_rm_dir);

    // 3b：为每个目录释放所有权 → 清扫 .lpkg_bak → 删除目录
    int dir_count = 0;
    for (const auto& p : dir_paths) {
        cache.remove_file_owner(p.string(), pkg_name);
        if (!cache.get_file_owners(p.string()).empty()) continue;

        const fs::path phys = p.is_absolute()
            ? Config::instance().root_dir() / p.relative_path()
            : Config::instance().root_dir() / p;
        if (!fs::exists(phys) || !fs::is_directory(phys)) continue;

        // 在尝试删除目录前，先清扫内部本包的 .lpkg_bak 文件
        // （这些文件是阶段 1 的备份 rename 遗留在目录内的）
        const std::string bak_suffix = std::string(constants::SUFFIX_LPKG_BAK) + pkg_name;
        for (auto& entry : fs::recursive_directory_iterator(phys, ec)) {
            if (entry.path().filename().string().ends_with(bak_suffix)) {
                TransactionLog::log_raw("RM_BAK_CLN " + entry.path().string());
                fs::remove(entry.path(), ec);
            }
        }

        std::error_code ec2;
        fs::remove(phys, ec2);
        if (!ec2) {
            TransactionLog::log_raw("RM_DIR " + phys.string());
            ++dir_count;
        }
    }
    if (dir_count > 0)
        log_info(string_format("info.dirs_removed", dir_count));

    // 测试断点：RM_DIR 完成后
    if (Config::instance().testing_mode())
        testing::check_and_break(testing::break_after_rm_dir);

    // ── 阶段 4：清理依赖、文档和钩子文件 ─────────────────────────────
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
    // 使用 WAL 保护的 DB 删除：rename 到 .lpkg_db_bak + DBRM 日志，
    // 确保若事务未提交（crash 后 RM_COMMIT 未写），rec 能恢复这些文件。
    // dep 和 needed_so 文件位于 state_dir 子目录中，cleanup_db_backups()
    // 使用 recursive_directory_iterator 可清扫其 .lpkg_db_bak 残留。
    Cache::instance().remove_db_file(dep_file, pkg_name);
    Cache::instance().remove_db_file(Config::instance().needed_so_dir() / pkg_name, pkg_name);
    Cache::instance().remove_db_file(Config::instance().docs_dir() / (pkg_name + std::string(constants::SUFFIX_MAN)), pkg_name);
    fs::remove_all(Config::instance().hooks_dir() / pkg_name, ec);
    cache.remove_installed(pkg_name);

    // 测试断点：RM DB 写入前
    if (Config::instance().testing_mode())
        testing::check_and_break(testing::break_before_rm_db_write);

    // ── 阶段 5：数据库落盘（WAL 保护）─ 必须在 RM_COMMIT 之前 ───────
    Cache::instance().write(pkg_name);

    // 测试断点：RM COMMIT 前
    if (Config::instance().testing_mode())
        testing::check_and_break(testing::break_before_rm_commit);

    // ── 阶段 6：RM_COMMIT — 事务完成 ────────────────────────────────
    TransactionLog::log_raw("RM_COMMIT " + pkg_name + " " + ver);

    // ── 阶段 7：安全清理全部 .lpkg_bak（事务已提交，无需回滚） ────
    // 注意：阶段 3b 已清扫目录内的 .lpkg_bak（写入 RM_BAK_CLN），
    // 此处清扫剩余的（父目录未被删除的）。
    for (const auto& [orig, bak] : backups) {
        fs::remove(bak, ec);
    }

    TransactionLog::log_raw("RM_END " + pkg_name + " " + ver);
    Cache::instance().cleanup_db_backups();

    // 统一事务完结（仅在独立移除时包裹；递归移除由外层包裹）
    if (wrap_in_txn) {
        TransactionLog::log_raw("COMMIT_PKGS");
        Cache::instance().cleanup_db_backups();
    }

    // 测试断点：移除完全完成后
    if (Config::instance().testing_mode())
        testing::check_and_break(testing::break_after_rm_cleanup);

    log_info(string_format("info.package_removed_successfully", pkg_name));
}

/**
 * 删除包的所有文件。
 *
 * pacman 风格：目录（路径末尾含 /）支持多包共同持有，不计入文件共享检查；
 * 普通文件不允许共享（除非 --force）。目录在最后持有者释放时被删除，
 * 并级联清理空父目录。
 */
void remove_package_files(const std::string& pkg_name, bool force) {
    auto& cache = Cache::instance();
    auto owned_entries = cache.get_package_files(pkg_name);
    if (owned_entries.empty()) return;

    // ── 阶段 1：共享文件检查（仅检查普通文件，不检查目录） ──────────
    if (!force) {
        std::vector<std::pair<std::string, std::string>> shared;
        for (const auto& entry : owned_entries) {
            if (entry.ends_with('/')) continue; // 目录允许多包持有
            auto owners = cache.get_file_owners(entry);
            std::string others;
            for (const auto& owner : owners) {
                if (owner != pkg_name) {
                    if (!others.empty()) others += ", ";
                    others += owner;
                }
            }
            if (!others.empty()) shared.emplace_back(entry, others);
        }
        if (!shared.empty()) {
            std::string msg = get_string("error.shared_file_header")
                            + std::string(constants::NL);
            for (const auto& [file, owners] : shared) {
                msg += "  "
                    + string_format("error.shared_file_entry", file, owners)
                    + std::string(constants::NL);
            }
            throw LpkgException(msg + get_string("error.removal_aborted"));
        }
    }

    // ── 阶段 2：按逆字典序排序 → 子条目先于父条目 ────────────────
    std::vector<fs::path> paths;
    for (const auto& e : owned_entries) paths.emplace_back(e);
    std::ranges::sort(paths, std::greater<>{});

    int file_count = 0;
    for (const auto& p : paths) {
        std::string path_str = p.string();
        const fs::path phys = p.is_absolute()
            ? Config::instance().root_dir() / fs::path(p).relative_path()
            : Config::instance().root_dir() / p;

        if (path_str.ends_with('/')) {
            // 目录：完全跳过——所有权释放和 fs::remove 由 remove_package()
            // 在 RM_DIR 阶段统一处理（此时 .bak 已清理，目录为空）
            continue;
        } else {
            // ── 普通文件：从磁盘删除 + 移除所有权 ──────────────
            if (fs::exists(phys) || fs::is_symlink(phys)) {
                std::error_code ec;
                fs::remove(phys, ec);
                if (!ec) ++file_count;
            }
            cache.remove_file_owner(path_str, pkg_name);
        }
    }

    if (file_count > 0) {
        log_info(string_format("info.files_removed", file_count));
    }

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
            try { remove_package(n, true); } catch (const std::exception& e) {
                log_warning(string_format("warning.autoremove_remove_failed", n, e.what()));
            }
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
 *   - 单个包升级失败不影响其他包（错误日志记录后继续）
 *   - 使用版比较算法确保 6.16.1 > 6.6.1
 */
void upgrade_packages() {
    extern std::atomic<bool> sigint_graceful;
    TransactionLog::trim_completed();
    log_info(get_string("info.checking_upgradable"));
    TmpDirManager tmp;
    Repository repo;
    try {
        repo.load_index();
    } catch (const std::exception& e) {
        log_warning(string_format("warning.repo_index_load_failed", e.what()));
        return;
    }

    // ── 快照已安装包列表 ─────────────────────────────────────────────────
    std::vector<std::pair<std::string, std::string>> installed;
    {
        std::lock_guard lock(Cache::instance().get_mutex());
        for (const auto& [name, ver] : Cache::instance().get_all_installed()) {
            installed.emplace_back(name, ver);
        }
    }

    // ── 构建升级计划（同时检查 sigint） ──────────────────────────────────
    struct UpgradeEntry {
        std::string name;
        std::string old_ver;
        std::string new_ver;
        std::string hash;
        bool held;
    };
    std::vector<UpgradeEntry> plan;
    std::map<std::string, InstallPlan> consistency_plan;
    for (const auto& [n, curr] : installed) {
        if (sigint_graceful.load()) {
            log_info(get_string("info.sigint_aborted"));
            return;
        }
        auto opt = repo.find_package(n);
        if (!opt) continue;
        if (!version_compare(curr, opt->version)) continue;
        const bool held = Cache::instance().is_held(n);
        plan.push_back({n, curr, opt->version, opt->sha256, held});

        // 构建一致性检查用的 InstallPlan（复用 repo 中已解析的元数据）
        InstallPlan ip;
        ip.name = n;
        ip.actual_version = opt->version;
        ip.dependencies = opt->dependencies;
        ip.provides = opt->provides;
        ip.needed_so = opt->needed_so;
        ip.is_explicit = held;
        consistency_plan[n] = std::move(ip);
    }

    if (plan.empty()) {
        log_info(get_string("info.all_packages_latest"));
        return;
    }

    // ── 一致性检查（与 install 同款：正向 SONAME + 反向版本约束 + 反向 SONAME） ──
    detail::check_forward_soname_integrity(consistency_plan, repo);

    if (auto broken = detail::check_plan_consistency(consistency_plan); !broken.empty()) {
        log_error(get_string("error.dependency_conflict_title"));
        std::string msg;
        for (const auto& p : broken) msg += "  " + p + "\n";
        log_error(msg);
        throw LpkgException(msg);
    }

    if (auto nso_broken = detail::check_needed_so_consistency(consistency_plan);
        !nso_broken.empty()) {
        log_error(get_string("error.dependency_conflict_title"));
        std::string msg;
        for (const auto& p : nso_broken) msg += "  " + p + "\n";
        log_error(msg);
        throw LpkgException(msg);
    }

    // ── 用户确认 ─────────────────────────────────────────────────────────
    std::string prompt;
    for (const auto& e : plan) {
        prompt += "  " + e.name + " " + e.old_ver + " → " + e.new_ver + "\n";
    }
    if (!user_confirms(prompt + get_string("info.confirm_proceed"))) {
        log_info(get_string("info.installation_aborted"));
        return;
    }

    // ── 执行升级（统一批次事务） ─────────────────────────────────────────
    // 使用与 install_packages 相同的事务协议。任一包升级失败（含 sigint）
    // 都会触发 rollback_committed_packages 将本批次中已经升级成功的包
    // 全部降级，保证系统状态一致。
    std::vector<std::string> success;
    with_batch_transaction(success, "upgrade", plan.size(), [&]() {
        for (const auto& e : plan) {
            if (sigint_graceful.load())
                throw LpkgException(get_string("info.sigint_aborted"));

            // 测试断点：每个包安装前/后（与 install_packages_internal 一致）
            if (Config::instance().testing_mode())
                testing::check_and_break(testing::break_before_each_pkg_install);

            log_info(string_format("info.upgrading_package",
                     e.name, e.old_ver, e.new_ver));
            InstallationTask t(e.name, e.new_ver, e.held,
                               e.old_ver, "", e.hash, false);
            t.run();
            success.push_back(e.name);

            if (Config::instance().testing_mode())
                testing::check_and_break(testing::break_after_each_pkg_install);
        }
    });

    // 测试断点：事务提交后
    if (Config::instance().testing_mode())
        testing::check_and_break(testing::break_after_commit_pkgs);

    if (!success.empty()) {
        log_info(string_format("info.upgraded_packages", success.size()));
    } else {
        log_info(get_string("info.all_packages_latest"));
    }
    Cache::instance().cleanup_db_backups();
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
        } catch (const std::exception& e) {
            log_warning(string_format("warning.reinstall_metadata_read_failed", arg, e.what()));
        }
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
        } catch (const std::exception& e) {
            log_warning(string_format("warning.query_path_resolve_failed", filename) + ": " + e.what());
        }
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

// =====================================================================
// 递归移除
// =====================================================================

namespace {

/** 生成 N 位随机大写字母数字验证码 */
std::string generate_code(size_t len = 6) {
    static const char chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::random_device rd;
    std::string code;
    for (size_t i = 0; i < len; ++i)
        code += chars[rd() % (sizeof(chars) - 1)];
    return code;
}

/** 获取某包及其传递反向依赖的集合 */
std::unordered_set<std::string> collect_recursive_remove_set(
    const std::string& pkg_name)
{
    std::unordered_set<std::string> result;
    std::unordered_set<std::string> visited;
    std::vector<std::string> queue = {pkg_name};

    while (!queue.empty()) {
        auto current = std::move(queue.back());
        queue.pop_back();
        if (!visited.insert(current).second) continue;
        result.insert(current);

        // 本包的反向依赖
        auto rdeps = Cache::instance().get_reverse_deps(current);
        // 本包提供的虚拟包的反向依赖
        for (const auto& cap : Cache::instance().get_package_provides(current)) {
            auto cap_rdeps = Cache::instance().get_reverse_deps(cap);
            rdeps.insert(cap_rdeps.begin(), cap_rdeps.end());
        }
        for (const auto& rdep : rdeps) {
            if (rdep != current && !visited.contains(rdep))
                queue.push_back(rdep);
        }
    }
    return result;
}

} // anonymous namespace

// main.cpp 中定义的 SIGINT 标志
extern std::atomic<bool> sigint_graceful;

/**
 * 递归移除包及其所有受影响的依赖者。
 *
 * 流程：
 *   1. 收集所有受影响的包（传递反向依赖）
 *   2. 排除 essential / held 包并显示列表
 *   3. 3 轮验证码确认（非交互模式跳过）
 *   4. 按叶子优先的顺序逐个移除（force 模式）
 *   5. 整体包裹在 BEGIN_PKGS / COMMIT_PKGS 中保证原子性
 */
void remove_package_recursive(const std::string& pkg_name) {
    if (sigint_graceful.load())
        throw LpkgException(get_string("info.sigint_aborted"));
    Cache::instance().load();
    log_info(string_format("info.recursive_remove_start", pkg_name));

    const std::string ver = Cache::instance().get_installed_version(pkg_name);
    if (ver.empty()) {
        log_info(string_format("info.package_not_installed", pkg_name));
        return;
    }

    // ── 1. 收集影响集合 ─────────────────────────────────────────────────
    auto affected = collect_recursive_remove_set(pkg_name);
    if (affected.empty()) return;

    // ── 2. 排除受保护的包，显示列表 ────────────────────────────────────
    // 跳过 essential 包（由 remove_package 内部保护），不跳 held 包——
    // held 是 autoremove 概念（标记显式安装的包根），不应用于阻止递归移除。
    // 用户如要保护包请使用 essential 机制。
    std::vector<std::string> to_remove;
    std::vector<std::string> essential_pkgs;
    for (const auto& p : affected) {
        if (Cache::instance().is_essential(p)) {
            essential_pkgs.push_back(p);
            continue;
        }
        to_remove.push_back(p);
    }

    if (to_remove.empty()) {
        log_info(get_string("info.recursive_nothing_to_remove"));
        return;
    }

    if (!essential_pkgs.empty()) {
        std::string msg = get_string("info.recursive_protected_header") + "\n";
        for (const auto& p : essential_pkgs)
            msg += "  " + p + "\n";
        log_warning(msg);
    }

    // 显示要移除的包
    log_info(get_string("info.recursive_remove_header"));
    for (const auto& p : to_remove)
        log_info(string_format("info.recursive_remove_item", p));

    // 按反向依赖数量升序排列（叶子先删）
    std::ranges::sort(to_remove, [](const std::string& a, const std::string& b) {
        return Cache::instance().get_reverse_deps(a).size()
             < Cache::instance().get_reverse_deps(b).size();
    });

    // ── 3. 3 轮验证码确认 ──────────────────────────────────────────────
    bool confirmed = true;
    if (Config::instance().non_interactive_mode() == NonInteractiveMode::INTERACTIVE) {
        for (int i = 0; i < 3; ++i) {
            std::string code = generate_code();
            log_info(string_format("info.recursive_confirm_prompt",
                                    std::to_string(i + 1), code));
            std::string input;
            std::cin >> input;
            if (input != code) {
                log_info(get_string("info.recursive_confirm_failed"));
                confirmed = false;
                break;
            }
        }
    }
    if (!confirmed) {
        log_info(get_string("info.installation_aborted"));
        return;
    }

    // ── 4. 原子移除（包裹在统一事务中） ──────────────────────────────
    TransactionLog::trim_completed();
    TransactionLog::log_raw("BEGIN_PKGS " + std::to_string(to_remove.size()));

    try {
        for (const auto& p : to_remove) {
            log_info(string_format("info.recursive_removing", p));
            // wrap_in_txn=false — 递归移除自行管理外层事务包裹
            remove_package(p, true, false);
        }
    } catch (...) {
        // rec 会通过 reverse_execute 回滚
        throw;
    }

    // 统一外层事务提交
    Cache::instance().write("recursive-remove");
    TransactionLog::log_raw("COMMIT_PKGS");
    Cache::instance().cleanup_db_backups();

    log_info(get_string("info.recursive_remove_done"));
}
