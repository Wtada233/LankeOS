#include "package_manager.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "archive.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "config/config.hpp"
#include "crypto/hash.hpp"
#include "db/batch_transaction.hpp"
#include "db/cache.hpp"
#include "db/test_breakpoints.hpp"
#include "db/transaction_log.hpp"
#include "db/wal_op.hpp"
#include "downloader.hpp"
#include "i18n/localization.hpp"
#include "install_common.hpp"
#include "repo/repository.hpp"
#include "trigger/trigger.hpp"
#include "vercmp/version.hpp"

namespace fs = std::filesystem;

/** 在 main.cpp 中声明，由 SIGINT 信号处理函数设置 */
extern std::atomic<bool> sigint_graceful;

// =====================================================================
// 公开 API
// =====================================================================

/**
 * 清理一批 .lpkg_bak 文件/目录（CLEANUP 阶段，不可回滚）。
 *
 * **write-ahead 顺序：先写 CLEANUP WAL 行，再物理删除。**
 * 原实现是"先删后记"：若在删除后、日志写入前崩溃，且批次中尚无任何 CLEANUP 行，
 * 恢复走 reverse_execute → DB 恢复到 pkg:installed 但 .bak 已删 → 磁盘与 DB 不一致。
 * 改为先记日志后：
 *   - 崩溃在"日志后、删除前"→ 恢复看到 CLEANUP → continue_cleanup 续删 → 一致
 *   - 崩溃在"删除后、下一条日志前"→ 已有 CLEANUP 行 → continue_cleanup 续删 → 一致
 *   - 崩溃在首条 CLEANUP 前 → 无 CLEANUP 行 → reverse_execute 整体恢复 → 一致
 * 删除失败仅告警（残留 .bak 由下次 rec/cleanup 续删），不中断事务。
 *
 * **调用时机**：
 *   - remove：批次内（COMMIT_PKGS 前，RM_COMMIT 后）。
 *   - install/upgrade：批次提交后（COMMIT_PKGS 之后，I-BAK-2 要求 bak 存活到提交）。
 *     此时写出的 CLEANUP 行位于事务之外（trailing 记录），由 trim_completed 保留
 *     （清理未完成时）+ recover_packages 续传，完成后随下一次 trim 一并清掉。
 */
void cleanup_baks(std::vector<std::pair<fs::path, fs::path>>& backups)
{
    if (backups.empty()) return;

    std::vector<fs::path> cleanup_paths;
    for (const auto& [orig, bak] : backups) cleanup_paths.push_back(bak);

    // 最深层优先（文件先于目录，子目录先于父目录）
    std::ranges::sort(cleanup_paths, [](const fs::path& a, const fs::path& b) {
        return a.string().size() > b.string().size();
    });
    auto last = std::unique(cleanup_paths.begin(), cleanup_paths.end());
    cleanup_paths.erase(last, cleanup_paths.end());

    for (const auto& p : cleanup_paths) {
        if (!fs::exists(p) && !fs::is_symlink(p)) continue;

        // write-ahead：先记日志再删除（见函数注释）
        wal::log_wal_line("CLEANUP " + p.string());

        // 断点：CLEANUP 日志写入后、物理删除前 —— 测试 write-ahead 崩溃窗口
        // （此刻 .bak 仍在磁盘，异常/崩溃可由 batch_rollback/rec 完整恢复）
        BreakpointManager::instance().hit("cleanup_after_wal");

        std::error_code ec2;
        bool ok = true;
        // 注意：fs::is_directory 会跟随符号链接。备份的 .lpkg_bak 若本身是符号链接
        // （如 filesystem 包的 /usr/lib64 → lib 被 rename 成 .lpkg_bak），跟随它判断
        // 成目录会递归删除其指向的目录（/usr/lib 全树被删）。symlink 必须只删自身。
        if (fs::is_directory(p) && !fs::is_symlink(p)) {
            // 从里到外删除目录内容
            std::vector<fs::path> entries;
            for (const auto& entry : fs::recursive_directory_iterator(p, ec2))
                if (!ec2) entries.push_back(entry.path());
            if (!ec2) {
                std::ranges::reverse(entries);
                for (const auto& e : entries) {
                    if (!fs::remove(e, ec2)) ok = false;
                }
            }
            if (!fs::remove(p, ec2)) ok = false;
        } else {
            if (!fs::remove(p, ec2)) ok = false;
        }

        if (!ok) log_warning(string_format("warning.cleanup_failed", p.string()));
    }
}

/** 将缓存数据写回磁盘 */
void write_cache()
{
    Cache::instance().write();
}

/**
 * 安装包的主入口
 * 流程：解析参数 -> 初始化仓库和缓存 -> 解析依赖 -> 静态一致性检查 ->
 * 用户确认 -> 实际安装 -> 触发运行
 */
void install_packages(const std::vector<std::string>& pkg_args, const std::string& hash_file_path,
                      bool force_reinstall)
{
    Cache::instance().load();
    TmpDirManager tmp;
    Repository repo;
    try {
        repo.load_index();
    } catch (const std::exception& e) {
        log_warning(string_format("warning.repo_index_load_failed", e.what()));
    }

    std::map<std::string, InstallPlan> plan;
    std::vector<std::string> order;
    std::map<std::string, fs::path> locals;
    std::vector<std::pair<std::string, std::string>> targets;

    std::string provided_hash;
    if (!hash_file_path.empty()) {
        std::ifstream hf(hash_file_path);
        if (!(hf >> provided_hash)) throw LpkgException(get_string("error.read_hash_failed"));
    }

    // 安装参数解析
    for (const auto& arg : pkg_args) {
        const fs::path p(arg);
        if (p.extension() == constants::EXT_ZST || p.extension() == constants::EXT_LPKG ||
            arg.find('/') != std::string::npos) {
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

    InstallContext ctx{repo, plan, order, locals, targets, force_reinstall, /*top_level=*/true, {}};

    // 解析安装计划。真正的"元数据一致性重解析"发生在 run_batch_transaction 内部
    // 的 metadata verification 循环（见下），此处不再需要外层死循环。
    plan.clear();
    order.clear();
    ctx.successfully_installed.clear();
    ctx.installed_set.clear();

    detail::resolve_with_solver(ctx);

    if (!provided_hash.empty()) {
        if (locals.empty()) throw LpkgException(get_string("error.hash_requires_local"));
        // 单个 --hash 无法校验多个本地包（至多一个能通过哈希校验）
        if (locals.size() > 1) throw LpkgException(get_string("error.hash_requires_single_local"));
        for (auto& [n, p] : plan)
            if (!p.local_path.empty()) p.sha256 = provided_hash;
    }

    if (plan.empty()) {
        log_info(get_string("info.all_packages_already_installed"));
        return;
    }

    // 冲突/ABI 一致性与依赖拉入已由 libsolv solver 原生保证
    // （取代旧的手动 check_plan_consistency / check_needed_so_consistency /
    //  check_forward_soname_integrity）

    // 用户确认
    std::string prompt;
    for (const auto& n : order) {
        const auto& p = plan.at(n);
        prompt +=
            "  " +
            string_format(p.is_explicit ? "info.package_list_item" : "info.package_list_item_dep",
                          p.name, p.actual_version) +
            "\n";
    }
    if (!user_confirms(prompt + get_string("info.confirm_proceed"))) {
        log_info(get_string("info.installation_aborted"));
        return;
    }

    ctx.successfully_installed.clear();
    ctx.installed_set.clear();

    // 执行安装（WAL 2.0 批量事务）
    std::vector<std::pair<fs::path, fs::path>> all_backups;
    run_batch_transaction([&](std::vector<std::string>& success) {
        auto& cache = Cache::instance();

        size_t i = 0;
        while (i < order.size()) {
            if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

            const std::string& n = order[i];
            ++i;

            if (ctx.installed_set.contains(n)) continue;

            auto& p = plan.at(n);

            if (!p.metadata_verified) {
                InstallationTask check_task(p.name, p.actual_version, p.is_explicit,
                                            Cache::instance().get_installed_version(p.name),
                                            p.local_path, p.sha256, p.force_reinstall);
                ensure_dir_exists(check_task.tmp_pkg_dir());
                check_task.download_and_verify_package();

                json meta = detail::read_archive_metadata(check_task.archive_path());
                std::vector<std::string> dep_strs =
                    meta.value(std::string(constants::J_DEPS), std::vector<std::string>{});
                auto actual_deps = detail::parse_dep_strings(dep_strs);
                std::vector<std::string> actual_provides =
                    meta.value(std::string(constants::J_PROVIDES), std::vector<std::string>{});
                std::vector<std::string> actual_needed_so =
                    meta.value(std::string(constants::J_NEEDED_SO), std::vector<std::string>{});

                bool metadata_differs = (actual_deps.size() != p.dependencies.size()) ||
                                        (actual_provides != p.provides) ||
                                        (actual_needed_so != p.needed_so);
                if (!metadata_differs) {
                    for (size_t di = 0; di < actual_deps.size(); ++di) {
                        if (actual_deps[di].name != p.dependencies[di].name ||
                            actual_deps[di].constraints != p.dependencies[di].constraints) {
                            metadata_differs = true;
                            break;
                        }
                    }
                }

                if (metadata_differs) {
                    log_info(string_format("info.resolving_metadata", p.name));
                    ctx.repo.update_package_info(p.name, p.actual_version, actual_deps,
                                                 actual_provides, actual_needed_so);
                    ctx.local_candidates[p.name] = check_task.archive_path();

                    ctx.plan.clear();
                    ctx.install_order.clear();
                    detail::resolve_with_solver(ctx);
                    i = 0;
                    continue;
                }

                p.local_path = check_task.archive_path();
                p.metadata_verified = true;
            }

            InstallationTask task(p.name, p.actual_version, p.is_explicit,
                                  Cache::instance().get_installed_version(p.name), p.local_path,
                                  p.sha256, p.force_reinstall);
            task.run(&ctx);

            // 收集 .lpkg_bak 路径供批次成功后统一清理（升级/重装时产生）
            for (const auto& b : task.get_backups()) all_backups.emplace_back(b);

            cache.write(p.name + ":installed");
            success.push_back(p.name);
            ctx.installed_set.insert(p.name);
        }
    });

    // 清理批次产生的 .lpkg_bak 文件（post-commit：写 CLEANUP WAL，崩溃可续传）。
    // 清理失败（磁盘满等）不视为安装失败——批次已提交、DB 一致，残留 bak 的
    // CLEANUP 记录留在 WAL，由下次 recover 续传。
    try {
        cleanup_baks(all_backups);
    } catch (const std::exception& e) {
        log_warning(string_format("warning.cleanup_deferred", e.what()));
    }

    trim_completed();
    cleanup_db_backups();

    TriggerManager::instance().run_all();
    log_info(get_string("info.install_complete"));
}

namespace
{

/**
 * 单包移除核心（须在 run_batch_transaction 内调用）。
 *
 * remove_package 与 remove_package_recursive 共用此实现（原先是两处近乎逐字
 * 重复的移除逻辑，去重后差异只剩"单包 vs 多包"与是否做共享文件检查）。
 *
 * 文件备份（BACKUP + rename 到 .lpkg_bak）产出进入 backups，由调用方在合适时机
 * 统一走 cleanup_baks() 清理（CLEANUP 阶段，事务内、COMMIT_PKGS 前）。
 */
void do_remove_package(const std::string& pkg_name, bool force, const std::string& ver,
                       std::vector<std::pair<fs::path, fs::path>>& backups)
{
    auto& cache = Cache::instance();

    if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

    detail::run_hook(pkg_name, std::string(constants::PRERM_SH));

    // WAL: RM_BEGIN
    wal::log_wal_line("RM_BEGIN " + pkg_name + " " + ver);

    std::error_code ec;

    auto owned_entries = cache.get_package_files(pkg_name);

    // 共享文件检查
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

    // 备份阶段
    int file_count = 0;
    if (!owned_entries.empty()) {
        std::vector<fs::path> paths;
        for (const auto& e : owned_entries) paths.emplace_back(e);
        std::ranges::sort(paths, std::greater<>{});

        for (const auto& p : paths) {
            std::string path_str = p.string();
            if (path_str.ends_with('/')) continue;
            if (!force && path_str.starts_with(constants::DIR_ETC_PREFIX)) continue;
            const fs::path phys = p.is_absolute()
                                      ? Config::instance().root_dir() / fs::path(p).relative_path()
                                      : Config::instance().root_dir() / p;

            if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

            if (fs::exists(phys) || fs::is_symlink(phys)) {
                fs::path bak = unique_bak_path(phys, pkg_name);
                wal::log_wal_line("BACKUP " + phys.string() + " \xe2\x86\x92 " + bak.string());
                BreakpointManager::instance().hit("rm_backup_after_wal_" + pkg_name);
                safe_rename(phys, bak);
                backups.emplace_back(phys, bak);
                ++file_count;
            }
        }
    }

    if (file_count > 0) log_info(string_format("info.files_removed", file_count));

    if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

    // 断点：移除的 BACKUP 阶段完成后、文件删除前
    BreakpointManager::instance().hit("rm_before_file_removal_" + pkg_name);

    remove_package_files(pkg_name, force);

    if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

    // 目录处理：BACKUP 目录（仅最后持有者 + 安全检查）
    {
        std::vector<fs::path> dir_paths;
        for (const auto& e : owned_entries)
            if (e.ends_with('/')) dir_paths.emplace_back(fs::path(e));
        std::ranges::sort(dir_paths, std::greater<>{});

        for (const auto& p : dir_paths) {
            cache.remove_file_owner(p.string(), pkg_name);
            if (!cache.get_file_owners(p.string()).empty()) continue;

            const fs::path phys = p.is_absolute()
                                      ? Config::instance().root_dir() / p.relative_path()
                                      : Config::instance().root_dir() / p;
            if (!fs::exists(phys) || !fs::is_directory(phys)) continue;

            // 安全检查：目录中只能有本包的 .lpkg_bak 文件
            bool can_backup = true;
            std::error_code ec2;
            for (const auto& entry : fs::directory_iterator(phys, ec2)) {
                auto fname = entry.path().filename().string();
                if (fname.find(std::string(constants::SUFFIX_LPKG_BAK) + pkg_name + "_") !=
                    std::string::npos)
                    continue;
                can_backup = false;
                break;
            }
            if (!can_backup) continue;

            fs::path bak = unique_bak_path(phys, pkg_name);
            wal::log_wal_line("BACKUP " + phys.string() + " \xe2\x86\x92 " + bak.string());
            safe_rename(phys, bak);
            backups.emplace_back(phys, bak);
        }
    }

    // DBRM 清理
    auto cleanup_with_dbr = [&](const fs::path& fpath, const std::string& /*desc*/) {
        if (fs::exists(fpath)) {
            wal::log_wal_line("DBRM " + fpath.string() + " " + pkg_name + ":removed");
            safe_rename(fpath,
                        fs::path(fpath.string() + ".lpkg_db_bak_before:" + pkg_name + ":removed"));
        }
    };

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
    // needed_so 派生的反向依赖（register_package 按提供者加边）也要清理，
    // 否则同一进程内 get_reverse_deps(provider) 会返回已移除的包。
    {
        const fs::path nso_file = Config::instance().needed_so_dir() / pkg_name;
        if (fs::exists(nso_file)) {
            std::ifstream f(nso_file);
            std::string soname;
            while (std::getline(f, soname)) {
                if (soname.empty()) continue;
                for (const auto& prov_pkg : cache.get_providers(soname))
                    cache.remove_reverse_dep(prov_pkg, pkg_name);
            }
        }
    }
    cleanup_with_dbr(dep_file, "dep");
    cleanup_with_dbr(Config::instance().needed_so_dir() / pkg_name, "needed_so");
    cleanup_with_dbr(
        Config::instance().docs_dir() / (pkg_name + std::string(constants::SUFFIX_MAN)), "man");

    fs::remove_all(Config::instance().hooks_dir() / pkg_name, ec);
    cache.remove_installed(pkg_name);

    if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

    // DB 落盘（先于 RM_COMMIT：提交标记前 DB 已持久化，崩溃可恢复）
    cache.write(pkg_name + ":removed");

    // WAL: RM_COMMIT + RM_END
    wal::log_wal_line("RM_COMMIT " + pkg_name + " " + ver);
    wal::log_wal_line("RM_END " + pkg_name + " " + ver);
}

}  // anonymous namespace

/**
 * 移除已安装的包
 * 检查是否为 essential 包、是否有其他包依赖它、是否有包依赖其提供的虚拟包名
 * force 模式下跳过所有安全检查
 */
void remove_package(const std::string& pkg_name, bool force, bool /*wrap_in_txn*/)
{
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

    if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

    log_info(string_format("info.removing_package", pkg_name));

    // WAL 2.0 批量事务：单个包移除 = 一批一包
    run_batch_transaction([&](std::vector<std::string>& success) {
        std::vector<std::pair<fs::path, fs::path>> backups;
        do_remove_package(pkg_name, force, ver, backups);

        // ── CLEANUP 阶段（事务内、COMMIT_PKGS 前；write-ahead：先记日志再删）──
        cleanup_baks(backups);

        success.push_back(pkg_name);
    });

    trim_completed();
    cleanup_db_backups();

    log_info(string_format("info.package_removed_successfully", pkg_name));
}

void remove_package_files(const std::string& pkg_name, bool force)
{
    auto& cache = Cache::instance();
    auto owned_entries = cache.get_package_files(pkg_name);
    if (owned_entries.empty()) return;

    if (!force) {
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
            std::string msg = get_string("error.shared_file_header") + std::string(constants::NL);
            for (const auto& [file, owners] : shared) {
                msg += "  " + string_format("error.shared_file_entry", file, owners) +
                       std::string(constants::NL);
            }
            throw LpkgException(msg + get_string("error.removal_aborted"));
        }
    }

    std::vector<fs::path> paths;
    for (const auto& e : owned_entries) paths.emplace_back(e);
    std::ranges::sort(paths, std::greater<>{});

    int file_count = 0;
    for (const auto& p : paths) {
        if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

        std::string path_str = p.string();
        const fs::path phys = p.is_absolute()
                                  ? Config::instance().root_dir() / fs::path(p).relative_path()
                                  : Config::instance().root_dir() / p;

        if (path_str.ends_with('/')) {
            continue;
        }
        if (!force && path_str.starts_with(constants::DIR_ETC_PREFIX)) {
            cache.remove_file_owner(path_str, pkg_name);
            continue;
        }
        {
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
 */
void autoremove()
{
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
            try {
                remove_package(n, true);
            } catch (const std::exception& e) {
                log_warning(string_format("warning.autoremove_remove_failed", n, e.what()));
            }
        }
        log_info(string_format("info.autoremove_complete", to_rem.size()));
    }
}

/**
 * 升级所有已安装的包
 *
 * 和安装流程共享依赖解析机制（libsolv resolve_with_solver），
 * 确保新版本引入的新依赖被正确解析并安装。
 */
void upgrade_packages()
{
    log_info(get_string("info.checking_upgradable"));
    TmpDirManager tmp;
    Repository repo;
    try {
        repo.load_index();
    } catch (const std::exception& e) {
        log_warning(string_format("warning.repo_index_load_failed", e.what()));
        return;
    }

    // 快照已安装包列表
    std::vector<std::pair<std::string, std::string>> installed;
    {
        std::lock_guard lock(Cache::instance().get_mutex());
        for (const auto& [name, ver] : Cache::instance().get_all_installed()) {
            installed.emplace_back(name, ver);
        }
    }

    // 找出可升级的包，构造升级目标列表
    // 需要先收集完毕再统一解析，避免在遍历 installed 时修改 plan
    std::vector<std::pair<std::string, std::string>> upgrade_targets;
    for (const auto& [n, curr] : installed) {
        if (sigint_graceful.load()) {
            log_info(get_string("info.sigint_aborted"));
            return;
        }
        auto opt = repo.find_package(n);
        if (!opt) continue;
        if (!version_compare(curr, opt->version)) continue;
        upgrade_targets.emplace_back(n, std::string(constants::VER_LATEST));
    }

    if (upgrade_targets.empty()) {
        log_info(get_string("info.all_packages_latest"));
        return;
    }

    // ── 依赖解析（和 install_packages 使用同一套机制） ──────────────
    std::map<std::string, InstallPlan> plan;
    std::vector<std::string> order;
    std::map<std::string, fs::path> local_candidates;
    InstallContext ctx{repo,
                       plan,
                       order,
                       local_candidates,
                       upgrade_targets,
                       /*force_reinstall=*/false,
                       /*top_level=*/true,
                       {}};

    detail::resolve_with_solver(ctx);

    if (plan.empty()) {
        log_info(get_string("info.all_packages_latest"));
        return;
    }

    // 冲突/ABI 一致性已由 libsolv solver 原生保证（取代旧的手动三校验）

    // ── 用户确认 ────────────────────────────────────────────────────
    std::string prompt;
    for (const auto& n : order) {
        const auto& p = plan.at(n);
        const std::string old_ver = Cache::instance().get_installed_version(n);
        if (!old_ver.empty()) {
            if (old_ver != p.actual_version) {
                // 已有旧版本且版本不同 → 升级
                prompt += "  " + n + " " + old_ver + " \xe2\x86\x92 " + p.actual_version + "\n";
            } else {
                // 已是最新版本（可能是其他依赖引入的已满足依赖）→ 不显示
                continue;
            }
        } else {
            // 新增的依赖
            prompt += "  " +
                      string_format(
                          p.is_explicit ? "info.package_list_item" : "info.package_list_item_dep",
                          p.name, p.actual_version) +
                      "\n";
        }
    }
    if (!user_confirms(prompt + get_string("info.confirm_proceed"))) {
        log_info(get_string("info.installation_aborted"));
        return;
    }

    // ── 执行升级（WAL 2.0 批量事务） ────────────────────────────────
    // 处理顺序由 resolve_with_solver（libsolv transaction_order）产生的 order 决定
    // （依赖先处理），确保新依赖在依赖者之前安装
    ctx.successfully_installed.clear();
    ctx.installed_set.clear();

    std::vector<std::pair<fs::path, fs::path>> upgrade_backups;
    size_t upgraded_count = 0;
    run_batch_transaction([&](std::vector<std::string>& success) {
        auto& cache = Cache::instance();

        size_t i = 0;
        while (i < order.size()) {
            if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

            const std::string& n = order[i];
            ++i;

            if (ctx.installed_set.contains(n)) continue;

            auto& p = plan.at(n);
            const std::string old_ver = cache.get_installed_version(n);

            // 跳过已是最新版本的包（如依赖已满足的情况）
            if (!p.force_reinstall && !old_ver.empty() && old_ver == p.actual_version) {
                ctx.installed_set.insert(n);
                continue;
            }

            // ── 元数据验证：下载后比对真实 metadata 和索引是否一致 ──
            // （和 install_packages 中的逻辑一致）
            if (!p.metadata_verified) {
                InstallationTask check_task(p.name, p.actual_version, p.is_explicit,
                                            cache.get_installed_version(p.name), p.local_path,
                                            p.sha256, p.force_reinstall);
                ensure_dir_exists(check_task.tmp_pkg_dir());
                check_task.download_and_verify_package();

                json meta = detail::read_archive_metadata(check_task.archive_path());
                std::vector<std::string> dep_strs =
                    meta.value(std::string(constants::J_DEPS), std::vector<std::string>{});
                auto actual_deps = detail::parse_dep_strings(dep_strs);
                std::vector<std::string> actual_provides =
                    meta.value(std::string(constants::J_PROVIDES), std::vector<std::string>{});
                std::vector<std::string> actual_needed_so =
                    meta.value(std::string(constants::J_NEEDED_SO), std::vector<std::string>{});

                bool metadata_differs = (actual_deps.size() != p.dependencies.size()) ||
                                        (actual_provides != p.provides) ||
                                        (actual_needed_so != p.needed_so);
                if (!metadata_differs) {
                    for (size_t di = 0; di < actual_deps.size(); ++di) {
                        if (actual_deps[di].name != p.dependencies[di].name ||
                            actual_deps[di].constraints != p.dependencies[di].constraints) {
                            metadata_differs = true;
                            break;
                        }
                    }
                }

                if (metadata_differs) {
                    log_info(string_format("info.resolving_metadata", p.name));
                    ctx.repo.update_package_info(p.name, p.actual_version, actual_deps,
                                                 actual_provides, actual_needed_so);
                    ctx.local_candidates[p.name] = check_task.archive_path();

                    ctx.plan.clear();
                    ctx.install_order.clear();
                    detail::resolve_with_solver(ctx);
                    i = 0;
                    continue;
                }

                p.local_path = check_task.archive_path();
                p.metadata_verified = true;
            }

            // 确定 hold 标志：保留当前 hold 状态，新增依赖不 hold
            const bool hold_pkg = cache.is_held(n);

            if (!old_ver.empty()) {
                log_info(string_format("info.upgrading_package", n, old_ver, p.actual_version));
            } else {
                log_info(string_format("info.installing_package", n, p.actual_version));
            }

            InstallationTask task(p.name, p.actual_version, hold_pkg, old_ver, p.local_path,
                                  p.sha256, p.force_reinstall);
            task.run(&ctx);

            for (const auto& b : task.get_backups()) upgrade_backups.emplace_back(b);

            cache.write(n + ":installed");
            success.push_back(n);
            ctx.installed_set.insert(n);
            if (!old_ver.empty()) ++upgraded_count;
        }
    });

    // 清理批次产生的 .lpkg_bak 文件（post-commit：写 CLEANUP WAL，崩溃可续传）。
    // 清理失败不视为升级失败——批次已提交、DB 一致，残留 bak 由下次 recover 续传。
    try {
        cleanup_baks(upgrade_backups);
    } catch (const std::exception& e) {
        log_warning(string_format("warning.cleanup_deferred", e.what()));
    }

    trim_completed();
    cleanup_db_backups();

    log_info(string_format("info.upgraded_packages", upgraded_count));
}

/**
 * force-solve-conflict — 显式删除所有被当前仓库状态打破的已安装包。
 *
 * 判定"打破"：已安装包的 needed_so 中任一 SONAME 当前仓库无人提供（ABI 断裂），
 * 或其依赖版本约束在仓库中无法满足。列出冲突包后要求输入确认短语
 * `I understand that this may break my system.` 才真正删除（防误操作）。
 *
 * 设计（配合 rebuild 流程）：install/upgrade 遇冲突一律硬报错、不再自动卸载；
 * 本命令是唯一显式的冲突清理入口。farm 容器内用
 * `echo "I understand that this may break my system." | lpkg force-solve-conflict` 喂入短语，
 * 删除后 upgrade/rebuild 即可继续。
 */
void force_solve_conflict()
{
    constexpr const char* PHRASE = "I understand that this may break my system.";

    log_info(get_string("info.force_solve_start"));
    Repository repo;
    repo.load_index();

    std::set<std::string> broken;
    auto& cache = Cache::instance();
    {
        std::lock_guard lock(cache.get_mutex());
        for (const auto& [pkg, ver] : cache.get_all_installed()) {
            // needed_so：当前仓库无人提供 → 打破
            const fs::path nso_file = Config::instance().needed_so_dir() / pkg;
            if (fs::exists(nso_file)) {
                std::ifstream f(nso_file);
                std::string soname;
                while (std::getline(f, soname)) {
                    if (soname.empty()) continue;
                    if (!repo.find_provider(soname)) {
                        broken.insert(pkg);
                        break;
                    }
                }
            }
            if (broken.count(pkg)) continue;
            // deps：版本约束在仓库中无法满足 → 打破
            const fs::path dep_file = Config::instance().dep_dir() / pkg;
            if (fs::exists(dep_file)) {
                std::ifstream f(dep_file);
                std::string line;
                while (std::getline(f, line)) {
                    if (line.empty()) continue;
                    for (const auto& dep : detail::parse_dep_strings({line})) {
                        if (dep.constraints.empty()) continue;
                        if (!repo.find_best_matching_version(dep.name, dep.constraints)) {
                            broken.insert(pkg);
                            break;
                        }
                    }
                }
            }
        }
    }

    if (broken.empty()) {
        log_info(get_string("info.force_solve_none"));
        return;
    }

    log_warning(get_string("error.dependency_conflict_title"));
    for (const auto& p : broken) log_warning(string_format("warning.force_solve_pkg", p));

    // 必须输入确认短语——任何非交互模式都不绕过（显式破坏性操作）。
    // 非交互模式（-y/-n）直接报错而非阻塞读 stdin，避免脚本永久挂起。
    // 脚本/容器喂短语的正确姿势：`echo 'I understand...' | lpkg force-solve-conflict`
    // （不带 -y，stdin 即 TTY/管道，lpkg 从 stdin 读短语）。
    if (Config::instance().non_interactive_mode() != NonInteractiveMode::INTERACTIVE) {
        throw LpkgException(get_string("error.force_solve_requires_interactive"));
    }
    std::cout << string_format("info.force_solve_confirm", PHRASE);
    std::cout.flush();
    std::string input;
    std::getline(std::cin, input);
    if (!input.empty() && input.back() == '\r') input.pop_back();
    if (input != PHRASE) {
        throw LpkgException(get_string("error.force_solve_phrase_mismatch"));
    }

    for (const auto& p : broken) remove_package(p, true);
    cache.write();
    log_info(string_format("info.force_solve_removed", broken.size()));
}

/** 显示包的 man 页面内容 */
void show_man_page(const std::string& pkg_name)
{
    const fs::path p = Config::instance().docs_dir() / (pkg_name + ".man");
    if (!fs::exists(p)) throw LpkgException(string_format("error.no_man_page", pkg_name));
    std::ifstream f(p);
    if (!f.is_open()) throw LpkgException(string_format("error.open_man_page_failed", p.string()));
    std::cout << f.rdbuf();
}

/**
 * 重新安装一个包
 */
void reinstall_package(const std::string& arg)
{
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
    install_packages({arg}, "", true);
}

/** 查询指定包安装的所有文件列表 */
void query_package(const std::string& pkg_name)
{
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
void query_file(const std::string& filename)
{
    auto& cache = Cache::instance();
    std::string target = filename;
    auto owners = cache.get_file_owners(target);

    if (owners.empty()) {
        try {
            const fs::path p(filename);
            if (!fs::is_symlink(p)) {
                const fs::path abs_p = fs::absolute(p);
                // 前缀判断必须带目录边界：root=/lanke 时 /lankefoo 不应算在根内
                const std::string root = Config::instance().root_dir().string();
                const std::string abs_s = abs_p.string();
                const bool in_root = (abs_s == root) || abs_s.starts_with(root + "/");
                if (in_root) {
                    const std::string logical =
                        "/" + fs::relative(abs_p, Config::instance().root_dir()).string();
                    owners = cache.get_file_owners(logical);
                    if (!owners.empty()) target = logical;
                }
            }
        } catch (const std::exception& e) {
            log_warning(string_format("warning.query_path_resolve_failed", filename) + ": " +
                        e.what());
        }
    }

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

namespace
{

/** 生成 N 位随机大写字母数字验证码 */
std::string generate_code(size_t len = 6)
{
    static const char chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::random_device rd;
    std::string code;
    for (size_t i = 0; i < len; ++i) code += chars[rd() % (sizeof(chars) - 1)];
    return code;
}

/** 获取某包及其传递反向依赖的集合 */
std::unordered_set<std::string> collect_recursive_remove_set(const std::string& pkg_name)
{
    std::unordered_set<std::string> result;
    std::unordered_set<std::string> visited;
    std::vector<std::string> queue = {pkg_name};

    while (!queue.empty()) {
        auto current = std::move(queue.back());
        queue.pop_back();
        if (!visited.insert(current).second) continue;
        result.insert(current);

        auto rdeps = Cache::instance().get_reverse_deps(current);
        for (const auto& cap : Cache::instance().get_package_provides(current)) {
            auto cap_rdeps = Cache::instance().get_reverse_deps(cap);
            rdeps.insert(cap_rdeps.begin(), cap_rdeps.end());
        }
        for (const auto& rdep : rdeps) {
            if (rdep != current && !visited.contains(rdep)) queue.push_back(rdep);
        }
    }
    return result;
}

}  // anonymous namespace

/**
 * 递归移除包及其所有受影响的依赖者。
 */
void remove_package_recursive(const std::string& pkg_name, bool force)
{
    if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));
    Cache::instance().load();
    log_info(string_format("info.recursive_remove_start", pkg_name));

    const std::string ver = Cache::instance().get_installed_version(pkg_name);
    if (ver.empty()) {
        log_info(string_format("info.package_not_installed", pkg_name));
        return;
    }

    auto affected = collect_recursive_remove_set(pkg_name);
    if (affected.empty()) return;

    if (!force && Cache::instance().is_essential(pkg_name)) {
        log_error(string_format("error.skip_remove_essential", pkg_name));
        return;
    }

    std::vector<std::string> to_remove;
    std::vector<std::string> essential_pkgs;
    for (const auto& p : affected) {
        if (!force && Cache::instance().is_essential(p)) {
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
        for (const auto& p : essential_pkgs) msg += "  " + p + "\n";
        log_warning(msg);
    }

    log_info(get_string("info.recursive_remove_header"));
    for (const auto& p : to_remove) log_info(string_format("info.recursive_remove_item", p));

    // 按反向依赖数量升序排列（叶子先删）
    std::ranges::sort(to_remove, [](const std::string& a, const std::string& b) {
        return Cache::instance().get_reverse_deps(a).size() <
               Cache::instance().get_reverse_deps(b).size();
    });

    // 3 轮验证码确认
    bool confirmed = true;
    if (Config::instance().non_interactive_mode() == NonInteractiveMode::INTERACTIVE) {
        for (int i = 0; i < 3; ++i) {
            std::string code = generate_code();
            log_info(string_format("info.recursive_confirm_prompt", std::to_string(i + 1), code));
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

    // WAL 2.0: 整批原子移除
    // 目录通过 BACKUP WAL 原子化移除，.lpkg_bak 通过 CLEANUP WAL 在事务内清理
    run_batch_transaction([&](std::vector<std::string>& success) {
        auto& cache = Cache::instance();
        std::vector<std::pair<fs::path, fs::path>> all_backups;

        for (const auto& p : to_remove) {
            log_info(string_format("info.recursive_removing", p));

            if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

            std::string ver = cache.get_installed_version(p);
            do_remove_package(p, true, ver, all_backups);
            success.push_back(p);
        }

        // ── CLEANUP 阶段（事务内、COMMIT_PKGS 前；write-ahead：先记日志再删）──
        cleanup_baks(all_backups);
    });

    trim_completed();
    cleanup_db_backups();

    log_info(get_string("info.recursive_remove_done"));
}
