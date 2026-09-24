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
#include "op_sink.hpp"
#include "repo/repository.hpp"
#include "trigger/trigger.hpp"
#include "vercmp/version.hpp"

namespace fs = std::filesystem;

/** 在 main.cpp 中声明，由 SIGINT 信号处理函数设置 */
extern std::atomic<bool> sigint_graceful;

namespace
{
/// 批次**提交后**收尾（清理本批 stash → trim → 清 DB 备份）。定义在文件下部的匿名
/// namespace 里，这里前置声明以便 install/upgrade/remove 三条路径共用。
void finish_committed_batch(
    std::vector<fs::path>& stashes, const std::vector<std::string>& removed_pkgs = {},
    const std::vector<std::pair<std::string, std::vector<std::string>>>& hook_sets = {});
}  // namespace

// =====================================================================
// 公开 API
// =====================================================================

/**
 * 清理一批 stash 目录（CLEANUP 阶段，不可回滚）。TODO：备份现存放于每文件系统的
 * 隔离 stash（<fsroot>/.lpkg_bak_<pkg>_<pid>），清理 = 对每个 stash 根 remove_all。
 *
 * **write-ahead 顺序：先写 CLEANUP WAL 行（一行 = 一个 stash 根），再物理删除。**
 * 崩溃语义与旧 cleanup_baks 相同：
 *   - 崩溃在"日志后、删除前"→ 恢复看到 CLEANUP → continue_post_commit_cleanup 续删 → 一致
 *   - 崩溃在"删除后、下一条日志前"→ 已有 CLEANUP 行 → 同上续删 → 一致
 *   - 崩溃在首条 CLEANUP 前 → 清理尚未开始 → stash 仍在（recover 的 post-commit 阶段会收掉）
 * stash 是隔离根（只装本批次的备份），remove_all 不会碰到任何活文件/其他包内容，
 * 因此不再需要"逐 bak 递归删除/按路径长度排序/symlink 守卫"。
 *
 * **调用时机（install/upgrade/remove 三条路径统一）**：**批次提交之后**。
 * stash 是回滚的唯一来源（install 是"被覆盖的旧文件"、remove 是"被删掉的文件"），
 * 所以必须活到"批次已不可能回滚"= COMMIT_PKGS 之后才清。CLEANUP 行因而位于事务之外
 * （trailing 记录），由 trim_completed 保留（清理未完成时）+ recover_packages 续传，
 * 完成后随下一次 trim 一并清掉。
 *
 * （历史：remove 曾在**批次内**清理，等于把"删 stash"当成批次内的不可逆点——异常路径
 *   见到 CLEANUP 就不回滚，导致"所有包都删完、尚未提交"这个窗口里 Ctrl+C 不会恢复已删
 *   的包。现在与 install 同款：只要批次未提交，中途中断一律整批回滚。）
 */
void cleanup_stashes(std::vector<fs::path>& stashes)
{
    if (stashes.empty()) return;

    std::vector<fs::path> paths;
    for (auto& s : stashes) paths.push_back(std::move(s));
    std::ranges::sort(paths);
    auto last = std::unique(paths.begin(), paths.end());
    paths.erase(last, paths.end());

    for (const auto& p : paths) {
        if (!fs::exists(p) && !fs::is_symlink(p)) continue;

        // write-ahead：先记日志再删除（见函数注释）
        wal::log_wal_line("CLEANUP " + p.string());

        // 断点：CLEANUP 日志写入后、物理删除前 —— 测试 write-ahead 崩溃窗口
        // （此刻 stash 仍在磁盘，异常/崩溃可由 batch_rollback/rec 完整恢复）
        BreakpointManager::instance().hit("cleanup_after_wal");

        detail::remove_stash_dir(p);  // 隔离根 remove_all（失败仅告警语义在上层？此处静默）
    }
}

/** 将缓存数据写回磁盘 */
void write_cache()
{
    Cache::instance().write();
}

/**
 * 找出第一个"请求了但没落到计划里"的目标；全部落实则返回空串。
 *
 * 兜底用途：畸形依赖（空名 → libsolv ID_EMPTY）等原因会让 solver 把请求静默丢掉、
 * 产出**空事务**，落点是 plan.empty() 的"所有包都已安装"分支——用户看到成功、
 * 实际什么都没装（TODO.md D3）。能力目标（SONAME 等）与真实包名不同，故必须
 * 同时匹配 provides 与"已装包提供的能力"。
 */
static std::string first_unreached_target(
    const std::vector<std::pair<std::string, std::string>>& targets,
    const std::map<std::string, InstallPlan>& plan)
{
    auto& cache = Cache::instance();
    const auto provided_by_plan = [&](const std::string& t) {
        for (const auto& p : plan | std::views::values)
            if (std::ranges::find(p.provides, t) != p.provides.end()) return true;
        return false;
    };
    const auto provided_by_installed = [&](const std::string& t) {
        for (const auto& prov : cache.get_providers(t))
            if (cache.is_installed(prov)) return true;
        return false;
    };
    for (const auto& [tn, tv] : targets) {
        if (plan.contains(tn) || cache.is_installed(tn) || provided_by_plan(tn) ||
            provided_by_installed(tn))
            continue;
        return tn;
    }
    return "";
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

    // 请求的目标必须真的进了计划（或已装/已由计划包或已装包提供该能力）。
    // 否则是"求解成功但什么都没装"——绝不能报"所有包都已安装"（TODO.md D3）。
    if (const std::string unreached = first_unreached_target(targets, plan); !unreached.empty()) {
        throw LpkgException(string_format("error.target_missing_from_plan", unreached));
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

    // **整批文件冲突预检**：在进入事务之前（任何 BEGIN_PKGS 之前）对整批一次性判定。
    // 逐包的 check_for_file_conflicts 保留为第二道防线（见 check_batch_file_conflicts 的
    // 实现说明：为什么冲突判定必须整批做）。
    check_batch_file_conflicts(plan, order);

    // 执行安装（WAL 2.0 批量事务）
    std::vector<fs::path> all_stashes;
    std::vector<std::pair<std::string, std::vector<std::string>>> hook_sets;
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
            task.set_content_ready(p.content_ready);
            task.run(&ctx);

            // 收集备份 stash 供批次成功后统一清理（升级/重装时产生）
            for (const auto& s : task.get_stashes()) all_stashes.emplace_back(s);
            // **只对真被处理过的包记 hook 账**：求解器会把"已装同版本"的包以 REINSTALL
            // 步骤带进计划（如它是批次里某个新包的唯一依赖提供者、而盘上那份已装记录的
            // 能力集过期时）。那种包在 run() 里早退，hook_files_ 为空 —— 那是"本包没被
            // 处理"，不是"新版本没有 hooks"；记进去就会让 finish_committed_batch 把它的
            // hooks_dir/<pkg>/ 整个 remove_all 掉（静默删 hook，包却仍装着）。
            // upgrade_packages 对同一件事有显式 skip 分支，这里靠 did_process() 统一。
            if (task.did_process()) hook_sets.emplace_back(p.name, task.get_hook_files());

            cache.write(p.name + ":installed");
            success.push_back(p.name);
            ctx.installed_set.insert(p.name);
        }
    });

    // post-commit 收尾（写 CLEANUP → 清理 stash → trim → 清 DB 备份）
    finish_committed_batch(all_stashes, {}, hook_sets);

    TriggerManager::instance().run_all();
    log_info(get_string("info.install_complete"));
}

namespace
{

/**
 * 单包移除核心（须在 run_batch_transaction 内调用）。
 *
 * **唯一调用点是 `remove_packages_in_one_batch`**（`remove_package` / `remove a b c` /
 * remove_package_recursive / autoremove / force-solve 最终都汇到那个批次，
 * 原先是两处近乎逐字重复的移除逻辑）。安全检查不在这里：共享文件与"陈旧文件键撞实体
 * 目录"两项已整体前移到批次入口 `check_removal_preconditions()`。
 *
 * 文件备份（BACKUP + rename 到每文件系统 stash）产出进入 stashes；目录删除走
 * DIR_RM（rmdir + 元数据记录，不再整目录实体备份）。stashes 由调用方在合适时机
 * 文件备份进 stash；stash 在**批次提交后**统一清理（见 cleanup_stashes 的调用时机）。
 *
 * 本函数**不接收 force**：安全检查已整体前移到批次入口 `check_removal_preconditions()`，
 * 批次内没有任何"跳过检查"的分支了（`--force` 在入口处就决定了要不要跑那些检查）。
 * `purge_config`（真删配置文件）与它正交：任何移除路径、无论 force 与否，配置文件默认都
 * 改名成 `<路径>.lpkgsave` 保留。
 */
void do_remove_package(const std::string& pkg_name, bool purge_config, const std::string& ver,
                       std::vector<fs::path>& stashes)
{
    auto& cache = Cache::instance();
    // 写入层原语：BACKUP/DIR_RM 的 WAL 行与物理操作成对发生（见 op_sink.hpp）
    detail::OpSink sink(pkg_name, &stashes);

    if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

    // prerm：**文件被删之前**跑（按定义如此，不能挪到提交后 —— 那等于静默变成 postrm）。
    // 因此"整批的安全检查"必须在此之前跑完，见 check_removal_preconditions()。
    detail::run_hook(pkg_name, std::string(constants::PRERM_SH));

    // WAL: RM_BEGIN
    wal::log_wal_line("RM_BEGIN " + pkg_name + " " + ver);

    auto owned_entries = cache.get_package_files(pkg_name);

    // 注：共享文件检查与"陈旧文件键撞实体目录"检查**不在这里** —— 它们已整体前移到
    // check_removal_preconditions()（批次级、在**任何 prerm 之前**执行）。原先逐包检查时，
    // 批次里后面的包被拒绝会让前面的包"prerm 已跑、文件/DB 却整批回滚"。

    // 阶段 A：owned 文件（含符号链接）的处置分两类：
    //   · **配置文件**（判据是路径前缀 `/etc/`，不是包的声明）→ **改名保留**成
    //     `<路径>.lpkgsave`（SAVE_CONF；dst 不进 stash，提交后不会被清理掉）。
    //     只有显式 `--purge-config` 才走下面那条真删路径。
    //   · 其余文件 → rename 进每文件系统 stash（BACKUP WAL），提交后随 stash 清理真删。
    //   注意这里与 force 无关：force 只影响批次入口的安全检查（反向依赖/共享文件/陈旧
    //   文件键），与"丢配置"无关。`remove -r` 与 autoremove 入口也硬编 force=true ——
    //   旧行为下它们**连 --force 都不用给**就会把配置文件当普通文件删掉，那是本阶段
    //   要修掉的静默数据丢失。
    int file_count = 0;
    for (const auto& path_str : owned_entries) {
        if (path_str.ends_with('/')) continue;  // 目录 → 阶段 B
        const bool is_conf = path_str.starts_with(std::string(constants::DIR_ETC_PREFIX));
        const fs::path phys = strip_trailing_slash(Config::instance().root_dir() /
                                                   fs::path(path_str).relative_path());

        if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

        // 盘上是**实体目录**而 DB 记的是文件键：非 force 时上面已经拒绝整批（走不到这里）；
        // 走到这里说明是 `--force`，此时也只**跳过**，绝不 rename 进 stash —— stash 在批次提交后
        // 会被 remove_all，等于连带删掉目录里的全部内容（ARCH §3.6：无主内容一律不删）。
        // 配置文件同理：`<dir>.lpkgsave` 会把不知名的目录整个搬走，宁可不碰（只告警）。
        std::error_code ec;
        if (fs::is_directory(phys, ec) && !fs::is_symlink(phys)) {
            log_warning(string_format("warning.remove_path_is_dir", phys.string()));
            continue;
        }

        if (fs::exists(phys) || fs::is_symlink(phys)) {
            if (is_conf && !purge_config) {
                // WAL: SAVE_CONF + rename 到兄弟名（断点位于 write-ahead 窗口内）
                const fs::path kept = sink.save_config(phys, "rm_save_conf_after_wal_" + pkg_name);
                log_info(string_format("info.config_saved_as", kept.string()));
            } else {
                // WAL: BACKUP + rename 进 stash（一次调用；断点位于 write-ahead 窗口内）
                sink.backup(phys, "rm_backup_after_wal_" + pkg_name);
            }
            ++file_count;
            // 登记触发器（与安装侧 copy_package_files 对称）：删除也是"这个路径变了"，
            // 删掉 /usr/lib/libfoo.so.1 必须让 ldconfig 规则入队，否则提交后无人清理
            // 它留下的悬空 SONAME 链接（apply_soname_links 会在提交后 flush）。
            TriggerManager::instance().check_file((fs::path("/") / path_str).string());
        }
    }

    if (file_count > 0) log_info(string_format("info.files_removed", file_count));

    if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

    // 断点：移除的 BACKUP 阶段完成后、文件删除前
    BreakpointManager::instance().hit("rm_before_file_removal_" + pkg_name);

    remove_package_files(pkg_name);

    if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

    // 阶段 B：owned 目录（最深优先，仅最后持有者）→ 空则 DIR_RM（rmdir + 元数据记录）。
    //   文件已全部搬进 stash，目录此刻只剩"无主内容"才会非空 → 非空即保留（安全边界：
    //   无主文件/状态目录/conffile/其他包内容一律不碰）。
    {
        std::vector<fs::path> dir_paths;
        for (const auto& e : owned_entries)
            if (e.ends_with('/')) dir_paths.emplace_back(fs::path(e));
        std::ranges::sort(dir_paths, std::greater<>{});

        for (const auto& p : dir_paths) {
            cache.remove_file_owner(p.string(), pkg_name);
            if (!cache.get_file_owners(p.string()).empty()) continue;

            // 目录键带尾斜杠，必须先规范化：否则 is_symlink 恒假（尾斜杠会解引用末尾的链接），
            // "别动 symlink→目录"的守卫形同虚设 —— is_empty 看的是链接目标、rmdir 也落在目标上
            // （实测：移除一个在 `/var/run -> ../run` 上落了目录条目的包，真实 `/run` 被 rmdir）。
            const fs::path key = strip_trailing_slash(p);
            const fs::path phys = key.is_absolute()
                                      ? Config::instance().root_dir() / key.relative_path()
                                      : Config::instance().root_dir() / key;
            std::error_code ec;
            if (!fs::is_directory(phys, ec) || fs::is_symlink(phys)) continue;
            if (!fs::is_empty(phys, ec)) continue;  // 含无主内容 → 整树保留
            // WAL: DIR_RM（含 mode/uid/gid） + rmdir（一次调用）
            // 目录型**挂载点**由 remove_empty_dir 自己挡下（rmdir 恒 EBUSY，写行就成了
            // "行说删了、盘面还在"）→ 跳过 + 告警，目录保留。
            if (sink.remove_empty_dir(phys) == detail::DirRemoval::SkippedMountPoint)
                log_warning(string_format("warning.remove_mount_point", phys.string()));
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

    // 注：hooks_dir/<pkg> 的删除**不在这里**——它无 WAL 记录，放在可回滚的批次内会让
    // 批次回滚后钩子永久丢失（之后 remove/upgrade 静默跳过钩子）。改由提交后的
    // finish_committed_batch() 删除（TODO.md Z7）。
    cache.remove_installed(pkg_name);

    if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

    // DB 落盘（先于 RM_COMMIT：提交标记前 DB 已持久化，崩溃可恢复）
    cache.write(pkg_name + ":removed");

    // WAL: RM_COMMIT + RM_END
    wal::log_wal_line("RM_COMMIT " + pkg_name + " " + ver);
    wal::log_wal_line("RM_END " + pkg_name + " " + ver);
}

/**
 * 批次**提交后**收尾：清理本批 stash（写 CLEANUP → 物理删除）→ 剪枝 hooks → **执行 postinst**
 * → trim → 清 DB 备份。
 *
 * 清理失败**不算批次失败**（批次已提交、DB 一致）：只告警并保留 CLEANUP 记录，
 * 由下次 recover_packages 续传。install / upgrade / remove 三条路径共用同一收尾。
 *
 * **postinst 的执行时机就在这里，全仓唯一**（原先在 InstallationTask::commit_without_file_ops
 * 末尾 = 批次内）：批次是"全或无"，回滚能撤销文件与 DB，却撤不回钩子的副作用（钩子以 root
 * 跑 systemd-sysusers / tmpfiles --create / useradd，改的是系统状态）—— 留在批次内等于让
 * 一个已经整批回滚的事务在系统上留下撤不掉的痕迹。上游 libalpm 同理：POST hook 整段在
 * "提交 / 中断"判定之后，提交失败一个都不跑。
 * 放在剪枝之后：此刻 hooks_dir/<pkg>/ 里就是本版本最终的那一份脚本（回滚过的批次根本走不到
 * 这里，它的旧脚本由事务回滚原样还原）。
 */
void finish_committed_batch(
    std::vector<fs::path>& stashes, const std::vector<std::string>& removed_pkgs,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& hook_sets)
{
    try {
        cleanup_stashes(stashes);
    } catch (const std::exception& e) {
        log_warning(string_format("warning.cleanup_deferred", e.what()));
    }
    // 被移除包的 hooks 在**提交后**删除：移除已是最终态；批次若回滚则钩子完好无损（Z7）
    for (const auto& p : removed_pkgs) {
        std::error_code ec;
        fs::remove_all(Config::instance().hooks_dir() / p, ec);
    }
    // 安装/升级：剪枝新版本**不再提供**的 hook 文件。同样放在提交后——批次回滚时
    // 旧 hook 必须完好（与 Z7 同一理由）。若新版本完全没有 hooks，整目录清掉。
    for (const auto& [pkg, files] : hook_sets) {
        const fs::path dir = Config::instance().hooks_dir() / pkg;
        std::error_code ec;
        if (files.empty()) {
            fs::remove_all(dir, ec);
            continue;
        }
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string name = e.path().filename().string();
            if (std::ranges::find(files, name) == files.end()) fs::remove(e.path(), ec);
        }
    }
    // postinst：只在**此处**执行（理由见函数注释）。run_hook 自己判 no_hooks_mode 与脚本
    // 是否存在，执行失败只告警不抛（见 install_common.cpp）—— 批次已提交，包确实装上了，
    // 在这里抛异常只会把"装好了"报成"失败"。
    for (const auto& hs : hook_sets) {
        detail::run_hook(hs.first, std::string(constants::POSTINST_SH));
    }
    trim_completed();
    cleanup_db_backups();
}

/**
 * 移除前的安全检查（essential / 反向依赖 / 能力反向依赖）。
 * 任一项不通过 → 返回 false 并已打印原因：这是**拒绝**（log + return）而不是报错，
 * 保持既有 CLI 语义（`lpkg remove <essential>` 不抛异常）。
 */
static bool removal_allowed(const std::string& pkg_name, bool force)
{
    if (force) return true;
    auto& cache = Cache::instance();
    if (cache.is_essential(pkg_name)) {
        log_error(string_format("error.skip_remove_essential", pkg_name));
        return false;
    }
    const auto refused = [&](const std::string& what) {
        auto rdeps = cache.get_reverse_deps(what);
        if (rdeps.empty()) return false;
        std::string list;
        for (const auto& d : rdeps) list += d + " ";
        log_info(string_format("info.skip_remove_dependency", what, list));
        return true;
    };
    if (refused(pkg_name)) return false;
    for (const auto& cap : cache.get_package_provides(pkg_name))
        if (refused(cap)) return false;
    return true;
}

/**
 * 移除前的**批次级**安全检查：共享文件 + "DB 文件键所指路径在盘上已是实体目录"。
 *
 * **为什么整体前移到任何 prerm 之前**：这两项原先在 do_remove_package 里**逐包、批次内**
 * 检查 —— 那时批次里前面的包已经跑过 prerm 了。后面的包一旦被检查拒绝，整批回滚能撤销文件
 * 与 DB，却撤不回 prerm 的副作用（停服务、摘掉共享配置里的登记项）。前移到批次入口后，
 * 拒绝时一个 prerm 都还没跑，整批"什么都没发生"（而且**根本不开启事务**，WAL 里不留
 * ROLLBACK 痕迹）。判据本身与原先逐字一致，只是检查时机提前。
 *
 * **prerm 为什么不干脆也挪到提交后**：prerm 按定义要在文件被删之前跑（"停掉依赖这些文件的
 * 服务、把包在共享配置里登记的条目摘掉"必须先于文件消失），挪到提交后等于把它静默变成
 * postrm —— 那是语义变化，不是修 bug。代价与上游一致：**检查**保证都在 prerm 之前跑完，
 * 但 prerm 之后仍可能因 I/O 错误或 Ctrl+C 失败 —— 那种窗口里文件/DB 由整批回滚还原，
 * prerm 的副作用撤不回来（libalpm 的 pre_remove 同样在事务内、删除之前跑，提交失败时
 * 它的副作用同样撤不回）。要彻底消除这个窗口只能牺牲 prerm 的语义，取舍如此。
 */
static void check_removal_preconditions(const std::vector<std::string>& pkgs, bool force)
{
    if (force) return;  // --force 的语义就是"无视这些拒绝"（阶段 A 里仍只跳过、绝不搬目录）
    auto& cache = Cache::instance();

    for (const auto& pkg_name : pkgs) {
        auto owned_entries = cache.get_package_files(pkg_name);
        if (owned_entries.empty()) continue;

        // 共享文件检查
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

        // 陈旧文件键检查（先检后动，拒绝即整批中止，什么都不改）。
        //   DB 把某个路径记成本包的**文件**、盘上却已经是**实体目录** —— 这个路径早就不属于本包这个
        //   "文件"了（被别的包用目录接管，或被外部改成了目录）。移除时若把它 rename 进 stash，
        //   批次提交后的 remove_all 会连带删掉目录里的全部内容（实测：先装带目录 `usr/share/foo/`
        //   的包、再 remove 曾拥有文件 `usr/share/foo` 的旧包 → 前者内容永久丢失，全程无报错）。
        //   不确定就不动：默认**拒绝**并列出目录当前持有者，交给使用者决定；`--force` 才继续，
        //   且那时也只跳过（见阶段 A），绝不把别人的目录搬进 stash。
        //   注：安装侧接管路径时会清掉旧属主记录，所以这里正常只在"外部改动 / 老版本记录"时触发。
        std::vector<std::pair<std::string, std::string>> stale_dirs;
        for (const auto& entry : owned_entries) {
            if (entry.ends_with('/')) continue;
            if (entry.starts_with(std::string(constants::DIR_ETC_PREFIX))) continue;
            const fs::path phys = strip_trailing_slash(Config::instance().root_dir() /
                                                       fs::path(entry).relative_path());
            std::error_code ec;
            if (!fs::is_directory(phys, ec) || fs::is_symlink(phys)) continue;
            std::string holders;
            for (const auto& owner : cache.get_file_owners(entry + "/")) {
                if (!holders.empty()) holders += ", ";
                holders += owner;
            }
            stale_dirs.emplace_back(entry, holders);
        }
        if (!stale_dirs.empty()) {
            std::string msg =
                get_string("error.remove_path_is_dir_header") + std::string(constants::NL);
            for (const auto& [path, holders] : stale_dirs) {
                msg += "  " + string_format("error.remove_path_is_dir_entry", path, holders) +
                       std::string(constants::NL);
            }
            throw LpkgException(msg + get_string("error.removal_aborted"));
        }
    }
}

/**
 * 在**一个批次**里依次移除若干包（调用方必须已用 run_batch_transaction 之外的检查筛过）。
 *
 * 全部包都删完（各自 RM_COMMIT + DB 落盘）之后才写一次 CLEANUP —— 那是批次内的
 * 不可逆点（stash 是移除的唯一回滚来源），因此在那之前任何中断（Ctrl+C/失败）
 * 都能**整批**回滚。
 *
 * 逐包的安全检查（共享文件 / 陈旧文件键）在**进入事务之前**、对整批一次性跑完：它们是
 * "批次内后面的包才失败"的唯一确定性来源，必须在任何 prerm 之前出结果（见
 * check_removal_preconditions）。prerm 之后仍可能失败的只剩 I/O 错误与 Ctrl+C。
 *
 * `purge_config` 一路透传到 `do_remove_package`：**所有**移除路径（单包/多包/递归闭包/
 * autoremove/force-solve）共用这一个批次实现，因此配置文件"改名保留"的语义只有一处。
 */
static void remove_packages_in_one_batch(const std::vector<std::string>& pkgs, bool force,
                                         bool purge_config, std::vector<fs::path>& stashes_out)
{
    check_removal_preconditions(pkgs, force);

    run_batch_transaction([&](std::vector<std::string>& success) {
        auto& cache = Cache::instance();

        for (const auto& p : pkgs) {
            if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

            log_info(string_format("info.removing_package", p));
            do_remove_package(p, purge_config, cache.get_installed_version(p), stashes_out);
            success.push_back(p);

            // 断点：本包已删完、下一包尚未开始 —— 模拟"多包移除中途 Ctrl+C"
            BreakpointManager::instance().hit("remove_after_package_" + p);
        }

        // 断点：全部包都已删完、批次尚未提交 —— 这是"清理前最后可回滚点"
        BreakpointManager::instance().hit("remove_batch_before_commit");
    });
    // **不在此清理 stash**：stash 是回滚的唯一来源，必须活到批次提交之后
    // （与 install/upgrade 同款；调用方在提交后用 finish_committed_batch 收尾）。
}

/**
 * 移除一组包的统一入口：检查 → 单批次原子移除 → 收尾。
 *
 * **所有多包移除都必须走这里**（`remove a b c` / autoremove / 递归闭包）：
 * 曾逐包各自 `remove_package()`，等于每包一批，中途 Ctrl+C 只回滚当前包那个批次
 * ——用户实测到"删掉几个包、其余不恢复"。
 *
 * **筛选整体前置，拒绝即全或无**：只要有任何一个包被安全检查拒绝，就一个包都不删
 * （见下面 refused_any 处的注释）。这与"多包一个批次"是同一条不变量的两个面：
 * 批次的语义是"全部成功或全部什么都没发生"，那么"部分包被拒"也必须落到
 * "什么都没发生"，而不是"把能删的删掉、再用非零退出码表示被拒绝"。
 *
 * @return 实际移除的包数（被拒绝/未安装的不计）
 */
static size_t remove_packages_checked(const std::vector<std::string>& pkgs, bool force,
                                      bool purge_config, bool* refused_out = nullptr)
{
    std::vector<std::string> to_remove;
    bool refused_any = false;
    for (const auto& p : pkgs) {
        if (Cache::instance().get_installed_version(p).empty()) {
            log_info(string_format("info.package_not_installed", p));
            continue;
        }
        if (!removal_allowed(p, force)) {
            refused_any = true;
            continue;
        }
        to_remove.push_back(p);
    }
    // **全或无**：整个列表先检完，任一不通过即中止、一个都不删（pacman 的 `-R a b` 同义）。
    // 旧实现是"被拒的 continue 掉、其余照删，最后才因 refused 抛错" → 落点"部分包已删 +
    // 非零退出码"，而调用方（脚本/farm）拿非零退出码判断的是"什么都没发生"（TODO G4 正是
    // 为这个语义加的），盘面却已经少了几个包。拒绝必须发生在**任何文件操作之前** ——
    // 走到这里时连事务都还没开（remove_packages_in_one_batch 都没被调用），WAL 里不留
    // RM_BEGIN，也就不存在"删一半"的中间态。
    if (refused_any) {
        if (refused_out) *refused_out = true;
        return 0;
    }
    if (to_remove.empty()) return 0;
    if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

    std::vector<fs::path> stashes;
    remove_packages_in_one_batch(to_remove, force, purge_config, stashes);
    finish_committed_batch(stashes, to_remove);

    // 与 install/upgrade 一致：**提交后**flush 一次待执行触发器。删除路径此前一次都不跑，
    // 于是删掉 /usr/lib 下的库之后，它的 SONAME 链接留在原地悬空（依赖它的二进制报
    // cannot open shared object file）。apply_soname_links 幂等（正确的链接不动）。
    TriggerManager::instance().run_all();

    for (const auto& p : to_remove) log_info(string_format("info.package_removed_successfully", p));
    return to_remove.size();
}

}  // anonymous namespace

/**
 * 移除已安装的包
 * 检查是否为 essential 包、是否有其他包依赖它、是否有包依赖其提供的虚拟包名
 * force 模式下跳过所有安全检查（purge_config 与 force 正交：见头文件）
 */
void remove_package(const std::string& pkg_name, bool force, bool /*wrap_in_txn*/,
                    bool purge_config)
{
    remove_packages_checked({pkg_name}, force, purge_config);
}

/** 移除多个包：**一个批次**内原子完成（中途中断整批回滚） */
void remove_packages(const std::vector<std::string>& pkg_names, bool force, bool purge_config)
{
    if (pkg_names.empty()) return;
    // CLI 边界（main 的 `remove a b c` 走这里）：**被安全检查拒绝 → 报错**，
    // 让脚本/farm 凭退出码区分"删掉了"与"被拒绝"（TODO G4）。库层 remove_package
    // 保持"打印原因后返回"的友好语义（测试与内部调用依赖它）。
    bool refused = false;
    remove_packages_checked(pkg_names, force, purge_config, &refused);
    if (refused) throw LpkgException(get_string("error.removal_refused"));
}

void remove_package_files(const std::string& pkg_name)
{
    auto& cache = Cache::instance();
    auto owned_entries = cache.get_package_files(pkg_name);
    if (owned_entries.empty()) return;

    // **共享文件检查不在这里重复**：它已经在整批入口 `check_removal_preconditions()`
    // 对整批判过一条**逐字相同**的判据，且判在**任何 prerm / 任何文件操作之前**。
    // 从这里（`do_remove_package` 的批次内）看，那条检查的结论不可能被推翻：
    // 批次内属主集合只会**缩小** —— 移除路径只做 `remove_file_owner` /
    // `remove_provider`，不注册任何新属主（`add_file_owner` 只在安装侧），而"存在
    // 别的属主"这件事不会因为任何一个属主被摘掉而重新成立。所以"入口放行"蕴含
    // "这里也放行"，这段代码自我调用的那天起就不可达；留着它只会让人以为
    // 这里还有第二道防线（真去改那一条时容易改错地方）。
    //
    // force 语义不受影响：`--force` 本来就跳过这些安全检查（入口处直接 return）。

    // 文件本体已在 do_remove_package 阶段 A 处置完（普通文件搬进 stash；配置文件改名成
    // `<路径>.lpkgsave`），此处只做 DB 收尾：清文件归属 + 清 provides。目录归属由阶段 B
    // 的目录循环清。
    for (const auto& path_str : owned_entries) {
        if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));
        if (path_str.ends_with('/')) continue;
        cache.remove_file_owner(path_str, pkg_name);
        // 配置文件的哈希记录**随包走**：本包对 /etc 的归属一撤销，我们记的"往这里装进去过
        // 什么"也必须消失。留着它的后果不是"过期数据无害"：一个已不在册的包留下的记录会被
        // 重新装回来的那个包当成旧记录，从而把一份它并不拥有的同名文件按"用户没改过"
        // **静默覆盖**（盘上恰好 == 旧记录时）—— 那正是 .lpkgnew 要防的事。
        // 与文件处置方式无关：`--purge-config`（真删）与 .lpkgsave（改名保留）都不影响这里，
        // 记录的内容从来不是"盘上有什么"，而是"我们装过什么"。
        if (path_str.starts_with(std::string(constants::DIR_ETC_PREFIX)))
            cache.remove_conf_hash(path_str, pkg_name);
    }

    for (const auto& cap : cache.get_package_provides(pkg_name)) {
        cache.remove_provider(cap, pkg_name);
    }
}

/**
 * 自动移除不再被任何包依赖的孤立包
 */
void autoremove(bool purge_config)
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
    // 核心包永不自动移除：下面走的是 force 的批量移除，会跳过
    // is_essential 检查（force 的语义是"无视反向依赖"，不该顺带无视核心包保护）。
    // 必须在锁外调用（is_essential 自己会加同一把锁，TODO.md E3）。
    std::erase_if(to_rem, [&](const std::string& n) {
        if (!cache.is_essential(n)) return false;
        log_info(string_format("info.autoremove_skip_essential", n));
        return true;
    });

    if (to_rem.empty()) {
        log_info(get_string("info.no_autoremove_packages"));
    } else {
        log_info(string_format("info.autoremove_candidates", to_rem.size()));
        // **整批一次**：逐包各自 remove_package() 等于每包一批，中途中断会留下
        // "删了几个、其余还在"的状态。这里与 `remove a b c` 共用同一批次语义。
        // force=true 是**内部**的（孤儿本就没有反向依赖者，无需再查），它不代表"可以丢配置"：
        // 配置文件按 purge_config（CLI 的 --purge-config，默认 false）改名保留。
        try {
            remove_packages_checked(to_rem, /*force=*/true, purge_config);
            log_info(string_format("info.autoremove_complete", to_rem.size()));
        } catch (const std::exception& e) {
            // 整批已回滚 → **不得**再报"完成"（否则脚本无法区分成功与回滚，TODO.md Z8）
            log_warning(string_format("warning.autoremove_batch_failed", to_rem.size(), e.what()) +
                        " [not removed: batch rolled back]");
        }
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

    // **整批文件冲突预检**：与 install_packages 同一道闸门（升级批次同样可能几百个包，
    // "前面若干包已落地之后才发现后面某包的冲突"在这里同样成立）。见
    // check_batch_file_conflicts 的实现说明。
    check_batch_file_conflicts(plan, order);

    std::vector<fs::path> upgrade_stashes;
    std::vector<std::pair<std::string, std::vector<std::string>>> upgrade_hook_sets;
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
            task.set_content_ready(p.content_ready);
            task.run(&ctx);

            for (const auto& s : task.get_stashes()) upgrade_stashes.emplace_back(s);
            // 同上（install 侧）：早退的包（已装同版本）不记账 —— 它的空 hook_files_
            // 含义是"没被处理"，不是"新版本没有 hooks"。本循环上面已有显式 skip 分支，
            // 这里让不变量落在**记账处**而不依赖各循环各自记得加 guard。
            if (task.did_process()) upgrade_hook_sets.emplace_back(n, task.get_hook_files());

            cache.write(n + ":installed");
            success.push_back(n);
            ctx.installed_set.insert(n);
            if (!old_ver.empty()) ++upgraded_count;
        }
    });

    // post-commit 收尾（与 install/remove 同一实现）
    finish_committed_batch(upgrade_stashes, {}, upgrade_hook_sets);

    // 与 install_packages 对称：升级同样会产生待执行触发器（copy_package_files 里
    // check_file() 照常累积），漏掉这一行会让 glib-compile-schemas /
    // systemctl daemon-reload / gtk-update-icon-cache 在升级后**一次都不跑**
    // （schema 不可见、unit 不生效、图标缓存陈旧，TODO.md F1）。
    TriggerManager::instance().run_all();

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
void force_solve_conflict(bool purge_config)
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

    // 同样**整批一次**：逐包调用会失去跨包原子性（与 remove/autoremove 同一理由）
    remove_packages({broken.begin(), broken.end()}, /*force=*/true, purge_config);
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
 * 重装一组包 —— **整组一个批次**。
 *
 * 为什么必须整组一个批次：CLI 的 `reinstall a b` 曾**逐参数各调一次** install_packages，
 * 等于每参数一个批次、跨参数不原子 —— 后面的成员失败时，前面那个已经装完并提交，而退出码
 * 非零又让脚本/farm 以为"什么都没发生"。与 `install a b` / `remove a b c` 同一条不变量
 * （install/remove 早已是整批，reinstall 漏了）。
 *
 * 逐参数的既有语义保持不变：本地归档路径（含 '/' 或 .lpkg 后缀）先读 metadata 拿真实包名
 * （读不出来只告警、仍按原参数交给 install_packages）；已安装的记 info.reinstalling_package。
 *
 * `force_reinstall=true` 对"本来就没安装"的成员是**无害**的：它只影响"同版本已装也要进
 * 计划"这一条（solver 的 job 入队与 InstallationTask::run 的同版本短路），且只作用于
 * 显式目标（`p.force_reinstall = ctx.force_reinstall && p.is_explicit`），不波及被拉进来的
 * 依赖。所以一个批次可以同时容纳"已装要重装"与"没装要安装"两种成员 —— 旧实现为此把两种
 * 成员分到两个 install_packages 调用里，那正是跨参数不原子的来源。
 */
void reinstall_packages(const std::vector<std::string>& pkg_args)
{
    if (pkg_args.empty()) return;

    for (const auto& arg : pkg_args) {
        std::string name = arg;
        if (arg.find('/') != std::string::npos || arg.ends_with(".lpkg")) {
            try {
                json meta = detail::read_archive_metadata(fs::absolute(arg));
                name = meta.at(std::string(constants::J_NAME)).get<std::string>();
            } catch (const std::exception& e) {
                log_warning(string_format("warning.reinstall_metadata_read_failed", arg, e.what()));
            }
        }
        if (!Cache::instance().get_installed_version(name).empty())
            log_info(string_format("info.reinstalling_package", name));
    }

    install_packages(pkg_args, "", /*force_reinstall=*/true);
}

/** 单包重装：与多参数版**同一实现、同一批次语义**（见上）。 */
void reinstall_package(const std::string& arg)
{
    reinstall_packages({arg});
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
                // 前缀判断必须带目录边界（root=/lanke 时 /lankefoo 不算根内），
                // 且 root=="/" 必须成立——见 path_within
                const bool in_root = path_within(abs_p, Config::instance().root_dir());
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

    // 目录是以**尾斜杠**注册的（scan_content_files 的约定：`/usr/bin/` 才是目录键，
    // 普通文件不带斜杠），所以查询目录时要再试一次带斜杠的形式：
    // 否则 `lpkg query /usr/bin` 报"不属于任何包"，而 `/usr/bin/` 才查得到（TODO.md G1）。
    if (owners.empty() && !target.ends_with('/')) {
        std::string with_slash = target + "/";
        auto dir_owners = cache.get_file_owners(with_slash);
        if (!dir_owners.empty()) {
            owners = std::move(dir_owners);
            target = std::move(with_slash);
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
 * 递归移除一组包及其所有受影响的依赖者 —— **整组一个批次**。
 *
 * 为什么必须整组一个批次：CLI 的 `remove -r a b` 曾**逐参数各调一次**本函数，等于每参数
 * 一个批次、跨参数不原子 —— b 失败时 a 已经删完并提交，而退出码非零又让脚本/farm 以为
 * "什么都没发生"（与 `remove a b c` 曾经踩的是同一个坑，那条已由 remove_packages_checked
 * 修掉）。多参数一律走这里：先把每个参数的**受影响闭包并成一份**，再一次性交给
 * `remove_packages_in_one_batch`。
 *
 * 逐参数的既有语义保持不变：未安装 → 跳该参数（info.package_not_installed）；参数本身是
 * essential 且非 force → 报错并跳该参数（旧实现是 `return`，但那时每个参数各自一次调用，
 * 所以"return"的影响范围也只到该参数自己）——闭包里出现的 essential 包一律进
 * essential_pkgs 并从移除集合里剔除（info.recursive_protected_header 告警）。
 */
void remove_packages_recursive(const std::vector<std::string>& pkg_names, bool force,
                               bool purge_config)
{
    if (pkg_names.empty()) return;
    if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));
    Cache::instance().load();

    // 各参数的受影响闭包**并集**（顺序无关，下面统一排序）
    std::vector<std::string> affected_all;
    for (const auto& pkg_name : pkg_names) {
        log_info(string_format("info.recursive_remove_start", pkg_name));

        const std::string ver = Cache::instance().get_installed_version(pkg_name);
        if (ver.empty()) {
            log_info(string_format("info.package_not_installed", pkg_name));
            continue;
        }

        auto affected = collect_recursive_remove_set(pkg_name);
        if (affected.empty()) continue;

        if (!force && Cache::instance().is_essential(pkg_name)) {
            log_error(string_format("error.skip_remove_essential", pkg_name));
            continue;
        }

        affected_all.insert(affected_all.end(), affected.begin(), affected.end());
    }

    std::ranges::sort(affected_all);
    affected_all.erase(std::unique(affected_all.begin(), affected_all.end()), affected_all.end());

    std::vector<std::string> to_remove;
    std::vector<std::string> essential_pkgs;
    for (const auto& p : affected_all) {
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

    // 整批原子移除（与 remove_packages_checked 共用同一实现：闭包内所有包一个批次），
    // stash 活到批次提交之后才清（install/upgrade 同款）。
    // force=true 同为内部硬编（闭包是被显式点名的，反向依赖检查无意义），**与配置保留无关**：
    // 配置文件照 CLI 的 --purge-config 走（默认改名成 .lpkgsave 保留）——旧行为下这条路径
    // 连 --force 都不用给就把配置删了。
    std::vector<fs::path> stashes;
    remove_packages_in_one_batch(to_remove, /*force=*/true, purge_config, stashes);
    finish_committed_batch(stashes, to_remove);

    // 同 remove_packages_checked：提交后 flush 触发器（否则被删库的 SONAME 链接悬空）
    TriggerManager::instance().run_all();

    log_info(get_string("info.recursive_remove_done"));
}

/** 单包递归移除：与多参数版**同一实现、同一批次语义**（见上）。 */
void remove_package_recursive(const std::string& pkg_name, bool force, bool purge_config)
{
    remove_packages_recursive({pkg_name}, force, purge_config);
}
