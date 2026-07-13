#include "install_common.hpp"
#include "archive.hpp"
#include "db/cache.hpp"
#include "trigger/trigger.hpp"
#include "config/config.hpp"
#include "downloader.hpp"
#include "crypto/hash.hpp"
#include "base/exception.hpp"
#include "i18n/localization.hpp"
#include "base/utils.hpp"
#include "vercmp/version.hpp"
#include "base/constants.hpp"
#include "transaction_log.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ===== InstallationTask 实现 =====

InstallationTask::InstallationTask(std::string pkg_name, std::string version, bool explicit_install,
                                   std::string old_version_to_replace, fs::path local_package_path,
                                   std::string expected_hash, bool force_reinstall)
    : pkg_name_(std::move(pkg_name)), version_(std::move(version)),
      explicit_install_(explicit_install),
      tmp_pkg_dir_(Config::get_tmp_dir() / pkg_name_),
      actual_version_(version_),
      old_version_to_replace_(std::move(old_version_to_replace)),
      local_package_path_(std::move(local_package_path)),
      expected_hash_(std::move(expected_hash)),
      force_reinstall_(force_reinstall) {}

/**
 * 执行原子化安装流程：
 *   预检 → 备份 → 复制 → 注册（最后一步）→ 清理备份
 *
 * 事务保证：复制中任意文件失败 → 全量回滚到安装前状态。
 * 冲突保证：不加 --force-overwrite 不覆盖任何已存在文件（不论是否被包管理）。
 * 回滚保证：回滚恢复所有 .lpkgbak 备份 + 删除本次新建的文件。
 */
void InstallationTask::run(InstallContext* ctx) {
    const std::string current_installed_version = Cache::instance().get_installed_version(pkg_name_);
    if (!force_reinstall_ && !current_installed_version.empty() && current_installed_version == actual_version_) {
        log_info(string_format("info.package_already_installed", pkg_name_));
        return;
    }

    log_info(string_format("info.installing_package", pkg_name_, version_));
    ensure_dir_exists(tmp_pkg_dir_);

    // 第一阶段：预检——不碰文件，只做检查
    prepare(ctx);

    // 第二阶段：备份 + 复制——任一失败则全量回滚
    TransactionLog log;
    log.begin(pkg_name_, actual_version_);
    try {
        backup_existing_files();
        copy_package_files();
    } catch (...) {
        log.rollback(pkg_name_, actual_version_);
        rollback();
        log.end(pkg_name_, actual_version_);
        throw;
    }

    // 第四阶段：注册——数据库修改是最后一步
    commit_without_file_ops();
    log.commit(pkg_name_, actual_version_);

    // 清理备份
    cleanup_backups();
    log.end(pkg_name_, actual_version_);

    log_info(string_format("info.package_installed_successfully", pkg_name_));
}

/** 准备阶段：下载并验证包、解压、检查依赖和文件冲突 */
void InstallationTask::prepare(InstallContext* ctx) {
    download_and_verify_package();
    extract_and_validate_package();
    if (ctx) {
        ensure_dependencies_satisfied(*ctx);
    }
    check_for_file_conflicts();
}

/**
 * 提交安装（无文件操作部分）：注册包 -> 移除旧版废弃文件 -> 运行 post-install 钩子
 */
void InstallationTask::commit_without_file_ops() {
    std::unordered_set<std::string> old_files;
    if (!old_version_to_replace_.empty()) {
        old_files = Cache::instance().get_package_files(pkg_name_);
        log_info(string_format("info.upgrade_old_files_check",
                               old_version_to_replace_, actual_version_, old_files.size()));
    }

    register_package();

    // 移除新版本中不再包含的旧文件（但不处理 /etc 下的配置文件）
    if (!old_files.empty()) {
        const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
        auto new_files = detail::scan_content_files(content_dir);
        std::unordered_set<std::string> new_set;
        for (const auto& f : new_files) new_set.insert((fs::path("/") / f).string());

        auto& cache = Cache::instance();
        for (const auto& old_file : old_files) {
            if (old_file.starts_with(std::string(constants::DIR_ETC_PREFIX))) {
                if (!new_set.contains(old_file))
                    cache.remove_file_owner(old_file, pkg_name_);
                continue;
            }
            if (!new_set.contains(old_file)) {
                auto owners = cache.get_file_owners(old_file);
                if (owners.contains(pkg_name_)) {
                    cache.remove_file_owner(old_file, pkg_name_);
                    if (cache.get_file_owners(old_file).empty()) {
                        const fs::path phys = (fs::path(old_file).is_absolute())
                            ? Config::instance().root_dir() / fs::path(old_file).relative_path()
                            : Config::instance().root_dir() / old_file;
                        if (fs::exists(phys) || fs::is_symlink(phys)) {
                            if (fs::is_directory(phys)) {
                                std::error_code ec;
                                if (fs::is_empty(phys, ec)) {
                                    log_info(string_format("info.removing_obsolete_file", old_file));
                                    fs::remove(phys, ec);
                                }
                                continue;
                            }
                            // ★ WAL 保护：REMOVE_OLD 明确记录"旧版本废弃文件被移除"
                            //   与 BACKUP（覆盖前临时备份）语义分离，确保 lpkg rec 能恢复
                            log_info(string_format("info.removing_obsolete_file", old_file));
                            fs::path bak = phys;
                            bak += std::string(constants::SUFFIX_LPKG_BAK) + pkg_name_;
                            TransactionLog::log_raw("REMOVE_OLD " + phys.string() + " → " + bak.string());
                            fs::rename(phys, bak);
                            backups_.emplace_back(phys, bak);
                        }
                    }
                }
            }
        }
    }

    run_post_install_hook();
}

/** 备份所有将被覆盖的文件为 .lpkgbak */
void InstallationTask::backup_existing_files() {
    const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
    auto files = detail::scan_content_files(content_dir);

    for (const auto& f : files) {
        // SIGINT 时立即中止，避免备份后 rollback 恢复
        extern std::atomic<bool> sigint_graceful;
        if (sigint_graceful.load())
            throw LpkgException(get_string("info.sigint_aborted"));

        fs::path rel_f = f;
        if (rel_f.is_absolute()) rel_f = rel_f.relative_path();
        const fs::path physical_path = Config::instance().root_dir() / rel_f;
        const fs::path phys_dir = physical_path.parent_path();

        // 确保目标父目录存在
        std::error_code ec;
        fs::create_directories(phys_dir, ec);

        // 配置文件跳过备份（复制阶段使用 .lpkg-new 后缀处理）
        const bool is_config = f.starts_with(std::string(constants::DIR_ETC));
        if (is_config) continue;

        if (fs::exists(physical_path) || fs::is_symlink(physical_path)) {
            if (!fs::is_directory(physical_path)) {
                // 只有普通文件和符号链接需要备份（目录被多包共享，不备份）
                fs::path bak = physical_path;
                bak += std::string(constants::SUFFIX_LPKG_BAK) + pkg_name_;
                // WAL 顺序：先写日志（确保崩溃后可恢复），再执行操作
                TransactionLog::log_raw("BACKUP " + physical_path.string() + " → " + bak.string());
                fs::rename(physical_path, bak);
                backups_.emplace_back(physical_path, bak);
            }
        } else {
            // 记录新文件（不存在于磁盘上），回滚时需要删除
            new_files_.push_back(physical_path);
            TransactionLog::log_raw("NEW " + physical_path.string());
        }
    }
}

/** 全量回滚：恢复备份 + 删除新文件 + 清理临时文件 */
void InstallationTask::rollback() {
    log_error(string_format("error.rollback_install", pkg_name_));

    // 清理可能残留的 .lpkgtmp 临时文件（来自原子写入）
    std::error_code ec_tmp;
    for (auto& entry : fs::recursive_directory_iterator(Config::instance().root_dir() / "usr", ec_tmp)) {
        if (entry.path().extension() == ".lpkgtmp")
            fs::remove(entry.path(), ec_tmp);
    }

    // 删除新创建的文件
    for (const auto& f : new_files_ | std::views::reverse) {
        if (fs::exists(f) || fs::is_symlink(f)) {
            std::error_code ec;
            fs::remove(f, ec);
            TransactionLog::log_raw("ROLLBACK_DEL " + f.string());
        }
    }
    new_files_.clear();

    // 恢复备份（用 fs::is_symlink 处理悬空软链接——备份阶段目标文件被重命名后原软链接变成悬空）
    for (const auto& [original, backup] : backups_ | std::views::reverse) {
        if (fs::exists(backup) || fs::is_symlink(backup)) {
            fs::rename(backup, original);
            TransactionLog::log_raw("RESTORE " + backup.string() + " → " + original.string());
        }
    }
    backups_.clear();
}

/** 安装成功后清理备份文件 */
void InstallationTask::cleanup_backups() {
    for (const auto& [original, backup] : backups_) {
        std::error_code ec;
        fs::remove(backup, ec);
    }
    backups_.clear();
    new_files_.clear();
}

/**
 * 下载并验证包文件
 * 如果是本地包文件，直接使用并校验 SHA256 哈希
 * 如果是从仓库安装，从镜像 URL 下载并校验
 */
void InstallationTask::download_and_verify_package() {
    if (!local_package_path_.empty()) {
        if (!fs::exists(local_package_path_))
            throw LpkgException(string_format("error.local_pkg_not_found", local_package_path_.string()));
        log_info(string_format("info.installing_local_file", local_package_path_.string()));
        archive_path_ = local_package_path_;
        if (!expected_hash_.empty() && calculate_sha256(archive_path_) != expected_hash_)
            throw LpkgException(string_format("error.hash_mismatch", pkg_name_));
        return;
    }

    const std::string mirror_url = Config::instance().get_mirror_url();
    const std::string arch = Config::instance().get_architecture();

    // 未指定版本时从仓库索引获取最新版本和哈希
    if (actual_version_.empty() || actual_version_ == constants::VER_LATEST) {
        Repository repo;
        repo.load_index();
        auto info = repo.find_package(pkg_name_);
        if (info) {
            actual_version_ = info->version;
            expected_hash_ = info->sha256;
        } else {
            throw LpkgException(string_format("warning.package_not_in_repo", pkg_name_));
        }
    }

    const std::string download_url = mirror_url + arch + "/" + pkg_name_ + "/"
                                     + actual_version_ + std::string(constants::EXT_LPKG);
    archive_path_ = tmp_pkg_dir_ / (actual_version_ + std::string(constants::EXT_LPKG));

    if (!fs::exists(archive_path_)) download_with_retries(download_url, archive_path_, 5, true);
    if (!expected_hash_.empty() && calculate_sha256(archive_path_) != expected_hash_)
        throw LpkgException(string_format("error.hash_mismatch", pkg_name_));
}

/** 解压包文件到临时目录，验证包结构完整性（metadata + content 目录），读取元数据 */
void InstallationTask::extract_and_validate_package() {
    log_info(get_string("info.extracting_to_tmp"));
    extract_tar_zst(archive_path_, tmp_pkg_dir_);

    for (const auto& meta : {constants::PKG_METADATA_FILE, constants::DIR_CONTENT}) {
        if (!fs::exists(tmp_pkg_dir_ / meta))
            throw LpkgException(string_format("error.incomplete_package", (tmp_pkg_dir_ / meta).string()));
    }

    std::string meta_name, meta_version;
    detail::read_package_metadata(tmp_pkg_dir_, meta_name, meta_version, deps_, provides_, needed_so_, man_content_);
    if (meta_name != pkg_name_) {
        log_warning(string_format("warning.package_name_mismatch", pkg_name_, meta_name));
    }
}

/**
 * 确保包的所有依赖已满足
 * 对缺失的依赖进行递归解析，检查本地已安装包和仓库中的包
 * 支持通过 providers 查找虚拟包提供者
 */
void InstallationTask::ensure_dependencies_satisfied(InstallContext& ctx) {
    if (Config::instance().no_deps_mode()) return;
    auto actual_deps = detail::parse_dep_strings(deps_);
    if (actual_deps.empty()) return;

    log_info(string_format("info.checking_deps"));
    bool found_new = false;

    for (const auto& dep : actual_deps) {
        const std::string& dep_name = dep.name;
        const std::string installed_ver = Cache::instance().get_installed_version(dep_name);

        if (!installed_ver.empty()) {
            if (dep.constraints.empty() || installed_ver == "virtual" || version_satisfies_all(installed_ver, dep.constraints)) {
                continue;
            }
        }

        if (ctx.plan.contains(dep_name)) continue;

        std::string req_ver = std::string(constants::VER_LATEST);
        if (!dep.constraints.empty()) {
            if (auto matching = ctx.repo.find_best_matching_version(dep_name, dep.constraints))
                req_ver = matching->version;
        }

        log_info(string_format("info.installing_discovered_dep", dep_name));

        std::set<std::string> vs;
        detail::resolve_package_dependencies(dep_name, req_ver, false, ctx, vs);

        // 检查虚拟包提供者和 targets 中的候选包
        if (!ctx.plan.contains(dep_name)) {
            auto providers = Cache::instance().get_providers(dep_name);
            if (!providers.empty()) continue;

            bool found_in_plan = false;
            for (const auto& [pkg_name, pplan] : ctx.plan) {
                for (const auto& prov : pplan.provides) {
                    if (prov == dep_name || prov.find(dep_name) != std::string::npos) {
                        found_in_plan = true;
                        break;
                    }
                }
                if (found_in_plan) break;
            }

            if (!found_in_plan) {
                for (const auto& [tn, tv] : ctx.targets) {
                    if (ctx.plan.contains(tn)) continue;
                    if (auto it = ctx.local_candidates.find(tn); it != ctx.local_candidates.end()) {
                        try {
                            std::string m_json = extract_file_from_archive(it->second,
                                std::string(constants::PKG_METADATA_FILE));
                            if (!m_json.empty()) {
                                json meta = json::parse(m_json);
                                for (const auto& prov : meta.value(std::string(constants::J_PROVIDES),
                                                                     std::vector<std::string>{})) {
                                    if (prov == dep_name) { found_in_plan = true; break; }
                                }
                            }
                        } catch (const std::exception& e) {
                            log_warning(string_format("warning.dep_scan_metadata_parse_failed", tn, e.what()));
                        }
                    }
                    if (found_in_plan) break;
                }
            }
            if (found_in_plan) continue;

            throw LpkgException(string_format("error.unresolvable_drift", pkg_name_,
                                string_format("error.package_not_in_repo", dep_name)));
        }
        found_new = true;
    }

    if (found_new) {
        if (auto broken = detail::check_plan_consistency(ctx.plan); !broken.empty()) {
            throw LpkgException(string_format("error.unresolvable_drift", pkg_name_,
                                "Dependency conflict detected in discovered dependencies"));
        }
    }

    // --- needed_so 完整性校验 ---
    // 每个声明的 SONAME 必须在 plan / 已安装缓存 / repo 中有提供者，
    // 否则是"唯一真相"断裂。采用版本级校验顺序避免包级缺口。
    if (!needed_so_.empty()) {
        for (const auto& soname : needed_so_) {
            bool provided = false;

            // 1) 检查当前 plan 中是否有包提供此 SONAME（版本精准）
            for (const auto& [pn, plan_pkg] : ctx.plan) {
                for (const auto& prov : plan_pkg.provides) {
                    if (prov == soname) { provided = true; break; }
                }
                if (provided) break;
            }

            // 2) 检查已安装包缓存
            if (!provided) {
                auto providers = Cache::instance().get_providers(soname);
                for (const auto& p : providers) {
                    if (Cache::instance().is_installed(p)) {
                        if (!ctx.plan.contains(p)) { provided = true; break; }
                    }
                }
            }

            // 3) 检查 repo index——验证返回的版本确实提供此 SONAME
            if (!provided) {
                if (auto prov_pkg = ctx.repo.find_provider(soname)) {
                    for (const auto& prov : prov_pkg->provides) {
                        if (prov == soname) { provided = true; break; }
                    }
                }
            }

            if (!provided) {
                throw LpkgException(string_format("error.unresolvable_drift", pkg_name_,
                    string_format("error.unresolved_soname", soname)));
            }
        }
    }
}

/**
 * 检查包文件与已安装文件的冲突
 * 检测未被任何包管理的文件（第三方手动安装）以及被其他包占用的文件
 * 配置文件冲突会在复制阶段使用 .lpkg-new 后缀处理
 */
void InstallationTask::check_for_file_conflicts() {
    std::map<std::string, std::string> conflicts;
    const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
    auto files = detail::scan_content_files(content_dir);
    auto& cache = Cache::instance();

    for (const auto& f : files) {
        fs::path rel_f = f;
        if (rel_f.is_absolute()) rel_f = rel_f.relative_path();
        const fs::path logical_path = fs::path("/") / rel_f;
        const std::string path_str = logical_path.string();

        // 目录条目（末尾 /）支持多包共同持有，不视为冲突
        if (path_str.ends_with('/')) continue;

        // 快速路径：文件已完全属于当前包 → 无冲突，跳过全量集合拷贝
        if (cache.is_file_owned_by(path_str, pkg_name_)) continue;

        // 检查是否被其他包占用
        auto owners = cache.get_file_owners(path_str);
        if (!owners.empty()) {
            if (!Config::instance().force_overwrite_mode()) {
                // 正常模式：报告冲突
                conflicts[path_str] = *owners.begin();
            } else {
                // --force-overwrite：转移所有权给新包（移除旧持有者）
                for (const auto& owner : owners) {
                    cache.remove_file_owner(path_str, owner);
                }
            }
            continue;
        }

        // 文件未被任何包管理，检查是否为磁盘上已存在的第三方手动安装文件
        if (old_version_to_replace_.empty()) {
            const fs::path phys = Config::instance().root_dir() / rel_f;
            if ((fs::exists(phys) || fs::is_symlink(phys)) && !Config::instance().force_overwrite_mode()) {
                conflicts[path_str] = get_string("error.unknown_manual_file");
            }
        }
    }

    if (!conflicts.empty()) {
        std::string msg = get_string("error.file_conflict_header") + "\n";
        for (const auto& [file, owner] : conflicts)
            msg += "  " + string_format("error.file_conflict_entry", file, owner) + "\n";
        throw LpkgException(msg + get_string("error.installation_aborted"));
    }
}

/**
 * 将包文件复制到系统根目录
 * 处理配置文件冲突（使用 .lpkg-new 后缀）、替换已存在文件时创建备份、
 * 保留文件权限和所有者信息
 */
void InstallationTask::copy_package_files() {
    log_info(get_string("info.copying_files"));
    const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
    auto files = detail::scan_content_files(content_dir);

    for (const auto& f : files) {
        // 测试钩子：在复制前调用（用于精确触发中断场景）
        if (on_before_file_copy) on_before_file_copy();

        // SIGINT 时立即中止复制，避免文件处于不一致状态
        extern std::atomic<bool> sigint_graceful;
        if (sigint_graceful.load())
            throw LpkgException(get_string("info.sigint_aborted"));

        fs::path rel_f = f;
        if (rel_f.is_absolute()) rel_f = rel_f.relative_path();
        const fs::path src_path = content_dir / f;
        const fs::path physical_path = Config::instance().root_dir() / rel_f;

        if (!fs::exists(src_path) && !fs::is_symlink(src_path)) continue;

        // 递归创建父目录
        fs::path parent = physical_path.parent_path();
        std::vector<fs::path> to_create;
        while (!parent.empty() && !fs::exists(parent)) {
            to_create.push_back(parent);
            if (parent == Config::instance().root_dir()) break;
            parent = parent.parent_path();
        }
        for (const auto& d : to_create | std::views::reverse) {
            ensure_dir_exists(d);
        }

        if (fs::is_symlink(src_path)) {
            // 符号链接（包括指向目录的）必须在此处理，不能走目录或普通文件分支
            fs::path link_target = fs::read_symlink(src_path);
            fs::path dest = physical_path;

            // 配置文件冲突：保留原文件，将符号链接安装到 .lpkgnew
            const bool is_config = f.starts_with(std::string(constants::DIR_ETC));
            if (is_config && fs::exists(physical_path) && !fs::is_directory(physical_path)) {
                dest += std::string(constants::SUFFIX_LPKG_NEW);
                log_warning(string_format("warning.config_conflict", physical_path.string(), dest.string()));
                has_config_conflicts_ = true;
            }

            if (fs::exists(dest) || fs::is_symlink(dest)) fs::remove(dest);
            fs::create_symlink(link_target, dest);
            struct stat st;
            if (lstat(src_path.c_str(), &st) == 0) {
                (void)lchown(dest.c_str(), st.st_uid, st.st_gid);
            }
            TriggerManager::instance().check_file((fs::path("/") / f).string());
            continue;
        }

        if (fs::is_directory(src_path)) {
            bool existed = fs::exists(physical_path);
            ensure_dir_exists(physical_path);
            struct stat st;
            if (lstat(src_path.c_str(), &st) == 0) {
                (void)lchown(physical_path.c_str(), st.st_uid, st.st_gid);
                mode_t pkg_mode = st.st_mode & 07777;
                if (existed) {
                    struct stat dst_st;
                    if (lstat(physical_path.c_str(), &dst_st) == 0) {
                        mode_t cur_mode = dst_st.st_mode & 07777;
                        if (cur_mode != pkg_mode) {
                            log_warning(string_format(
                                "warning.dir_perm_mismatch",
                                physical_path.string(),
                                static_cast<int>(cur_mode),
                                static_cast<int>(pkg_mode)));
                        }
                    }
                }
                (void)chmod(physical_path.c_str(), pkg_mode);
            }
            continue;
        }

        try {
            const bool is_config = f.starts_with(std::string(constants::DIR_ETC));
            fs::path final_dest = physical_path;

            // 配置文件冲突：使用 .lpkg-new 后缀
            if (is_config && fs::exists(physical_path) && !fs::is_directory(physical_path)) {
                final_dest += std::string(constants::SUFFIX_LPKG_NEW);
                if (fs::exists(final_dest) || fs::is_symlink(final_dest)) fs::remove(final_dest);
                log_warning(string_format("warning.config_conflict", physical_path.string(), final_dest.string()));
                has_config_conflicts_ = true;
                // 配置文件的 .lpkg-new 不需要原子写入，因为不会覆盖现有文件
                fs::copy(src_path, final_dest,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            } else {
                // 原子写入：先复制到临时路径，再 rename 到最终位置
                // rename 在同文件系统内是原子的，避免出现中间态文件
                // WAL 顺序：先写 COPY 日志（确保崩溃后可恢复），再 rename
                fs::path tmp_path = final_dest;
                tmp_path += ".lpkgtmp";
                fs::copy(src_path, tmp_path,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);

                // 保留原始文件权限和所有者（在 tmp 文件上操作）
                struct stat st;
                if (lstat(src_path.c_str(), &st) == 0) {
                    (void)lchown(tmp_path.c_str(), st.st_uid, st.st_gid);
                    if (!S_ISLNK(st.st_mode)) {
                        (void)chmod(tmp_path.c_str(), st.st_mode & 07777);
                    }
                }
                TransactionLog::log_raw("COPY " + tmp_path.string() + " → " + final_dest.string());
                fs::rename(tmp_path, final_dest);
            }

            TriggerManager::instance().check_file((fs::path("/") / f).string());
        } catch (const std::exception& e) {
            throw LpkgException(string_format("error.copy_failed_rollback", f, physical_path.string(), e.what()));
        }
    }

    if (has_config_conflicts_) log_warning(get_string("info.config_review_reminder"));
}

/**
 * 在包管理数据库中注册已安装的包
 * 记录包文件与所有者的映射、依赖关系、provides、man 页面和版本信息
 * 处理包升级时的旧数据清理（移除旧的逆向依赖和 provides）
 */
void InstallationTask::register_package() {
    auto& cache = Cache::instance();

    // 升级时清理旧的逆向依赖和 provides 记录
    if (!old_version_to_replace_.empty()) {
        const fs::path old_dep_file = Config::instance().dep_dir() / pkg_name_;
        if (fs::exists(old_dep_file)) {
            std::ifstream f(old_dep_file);
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty()) {
                    std::stringstream ss(line);
                    std::string dn;
                    if (ss >> dn) cache.remove_reverse_dep(dn, pkg_name_);
                }
            }
        }
        for (const auto& cap : cache.get_package_provides(pkg_name_)) {
            cache.remove_provider(cap, pkg_name_);
        }
        // 清理旧的 needed_so 文件（升级后重新写入）
        fs::remove(Config::instance().needed_so_dir() / pkg_name_);
    }

    // 写 deps（包名级别的依赖）— 合并两个来源：
    // 1. metadata.json 中的 deps 字段（显式声明的包依赖）
    // 2. needed_so 解析得到的提供者包（SONAME→提供者包映射）
    //
    // 这样 autoremove / remove 的逆向依赖检查能同时覆盖两类依赖。
    std::unordered_set<std::string> dep_entries;
    for (const auto& d : deps_) {
        dep_entries.insert(d);
        std::string name = d;
        if (const auto pos = d.find_first_of(" \t<>="); pos != std::string::npos) name = d.substr(0, pos);
        cache.add_reverse_dep(name, pkg_name_);
    }

    // 将 needed_so 也写入 deps 文件（解析 SONAME→包名后合并入同一个系统）
    for (const auto& soname : needed_so_) {
        auto providers = cache.get_providers(soname);
        for (const auto& prov_pkg : providers) {
            if (prov_pkg != pkg_name_ && cache.is_installed(prov_pkg)) {
                dep_entries.insert(prov_pkg);
                cache.add_reverse_dep(prov_pkg, pkg_name_);
            }
        }
    }

    // 从 set 写出（已去重），排序以保证每次生成的文件顺序一致
    std::vector<std::string> sorted_deps(dep_entries.begin(), dep_entries.end());
    std::sort(sorted_deps.begin(), sorted_deps.end());
    std::ofstream deps_out(Config::instance().dep_dir() / pkg_name_);
    for (const auto& entry : sorted_deps) {
        deps_out << entry << constants::NL;
    }

    // 写 needed_so 原始 SONAME 列表（保留原始真相供审计和重校验）
    std::ofstream nso_out(Config::instance().needed_so_dir() / pkg_name_);
    for (const auto& sn : needed_so_) {
        nso_out << sn << constants::NL;
    }

    const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
    for (const auto& f : detail::scan_content_files(content_dir)) {
        cache.add_file_owner((fs::path("/") / f).string(), pkg_name_);
    }

    const fs::path man_path = Config::instance().docs_dir() / (pkg_name_ + std::string(constants::SUFFIX_MAN));
    if (!man_content_.empty()) {
        std::ofstream man_out(man_path);
        man_out << man_content_;
    } else {
        std::error_code ec;
        fs::remove(man_path, ec);
    }

    for (const auto& cap : provides_) {
        cache.add_provider(cap, pkg_name_);
    }
    cache.add_installed(pkg_name_, actual_version_, explicit_install_);
}

/** 复制包的 hooks 目录到系统 hooks 目录，赋予执行权限，然后运行 post-install 钩子 */
void InstallationTask::run_post_install_hook() {
    const fs::path hook_src = tmp_pkg_dir_ / constants::DIR_HOOKS;
    if (!fs::exists(hook_src) || !fs::is_directory(hook_src)) return;

    const fs::path dest_dir = Config::instance().hooks_dir() / pkg_name_;
    ensure_dir_exists(dest_dir);
    for (const auto& entry : fs::directory_iterator(hook_src)) {
        if (entry.is_regular_file()) {
            const fs::path dest = dest_dir / entry.path().filename();
            fs::copy(entry.path(), dest, fs::copy_options::overwrite_existing);
            fs::permissions(dest, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                           fs::perm_options::add);
        }
    }
    detail::run_hook(pkg_name_, std::string(constants::POSTINST_SH));
}

std::vector<DependencyInfo> InstallationTask::parse_deps() const {
    return detail::parse_dep_strings(deps_);
}
