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

#include <algorithm>
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
 * 执行完整的安装流程：下载验证 -> 解压提取 -> 依赖检查 -> 文件冲突检测 -> 复制文件 -> 注册包 -> 运行钩子
 * 安装过程中的任何异常都会触发回滚操作，清理已安装的文件
 */
void InstallationTask::run(InstallContext* ctx) {
    const std::string current_installed_version = Cache::instance().get_installed_version(pkg_name_);
    if (!force_reinstall_ && !current_installed_version.empty() && current_installed_version == actual_version_) {
        log_info(string_format("info.package_already_installed", pkg_name_));
        return;
    }

    log_info(string_format("info.installing_package", pkg_name_, version_));
    ensure_dir_exists(tmp_pkg_dir_);

    try {
        prepare(ctx);
        commit();
    } catch (const std::exception& e) {
        rollback_files();
        throw;
    }
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
 * 回滚机制：安装失败时清理已安装的文件
 * 按逆序执行：删除已复制的新文件 -> 恢复备份的旧文件 -> 删除新建的空目录
 */
void InstallationTask::rollback_files() {
    log_error(string_format("error.rollback_install", pkg_name_));
    for (const auto& file : installed_files_) {
        if (fs::exists(file) || fs::is_symlink(file)) {
            std::error_code ec;
            fs::remove_all(file, ec);
        }
    }
    for (const auto& [physical, backup] : backups_) {
        if (fs::exists(backup)) {
            std::error_code ec;
            fs::rename(backup, physical, ec);
        }
    }
    for (const auto& dir : created_dirs_ | std::views::reverse) {
        if (fs::exists(dir) && fs::is_directory(dir) && fs::is_empty(dir)) {
            std::error_code ec;
            fs::remove(dir, ec);
        }
    }
}

/**
 * 提交安装：复制文件 -> 注册包 -> 移除旧版本遗留的过时文件 -> 清理备份 -> 运行 post-install 钩子
 */
void InstallationTask::commit() {
    std::unordered_set<std::string> old_files;
    if (!old_version_to_replace_.empty()) {
        old_files = Cache::instance().get_package_files(pkg_name_);
    }

    copy_package_files();

    try {
        register_package();

        // 移除新版本中不再包含的旧文件（但不处理 /etc 下的配置文件）
        if (!old_files.empty()) {
            const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
            auto new_files = detail::scan_content_files(content_dir);
            std::unordered_set<std::string> new_set;
            for (const auto& f : new_files) new_set.insert((fs::path("/") / f).string());

            auto& cache = Cache::instance();
            for (const auto& old_file : old_files) {
                if (old_file.starts_with(std::string(constants::DIR_ETC_PREFIX))) continue;
                if (!new_set.contains(old_file)) {
                    auto owners = cache.get_file_owners(old_file);
                    if (owners.contains(pkg_name_)) {
                        cache.remove_file_owner(old_file, pkg_name_);
                        if (cache.get_file_owners(old_file).empty()) {
                            const fs::path phys = (fs::path(old_file).is_absolute())
                                ? Config::instance().root_dir() / fs::path(old_file).relative_path()
                                : Config::instance().root_dir() / old_file;
                            if (fs::exists(phys) || fs::is_symlink(phys)) {
                                log_info(string_format("info.removing_obsolete_file", old_file));
                                fs::remove(phys);
                            }
                        }
                    }
                }
            }
        }
    } catch (...) { throw; }

    // 清理备份文件
    for (const auto& [physical, backup] : backups_) {
        std::error_code ec;
        fs::remove(backup, ec);
    }
    backups_.clear();
    run_post_install_hook();
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
    detail::read_package_metadata(tmp_pkg_dir_, meta_name, meta_version, deps_, provides_, man_content_);
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
    auto actual_deps = detail::parse_dep_strings(deps_);
    if (actual_deps.empty()) return;

    log_info(string_format("info.checking_deps"));
    bool found_new = false;

    for (const auto& dep : actual_deps) {
        const std::string& dep_name = dep.name;
        const std::string installed_ver = Cache::instance().get_installed_version(dep_name);

        if (!installed_ver.empty()) {
            if (dep.op.empty() || installed_ver == "virtual" || version_satisfies(installed_ver, dep.op, dep.version_req)) {
                continue;
            }
        }

        if (ctx.plan.contains(dep_name)) continue;

        std::string req_ver = std::string(constants::VER_LATEST);
        if (!dep.op.empty()) {
            if (auto matching = ctx.repo.find_best_matching_version(dep_name, dep.op, dep.version_req))
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
                        } catch (...) {}
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
        if (fs::is_directory(content_dir / f)) continue;

        const std::string path_str = logical_path.string();

        // 快速路径：文件已完全属于当前包 → 无冲突，跳过全量集合拷贝
        if (cache.is_file_owned_by(path_str, pkg_name_)) continue;

        // 检查是否被其他包占用
        auto owners = cache.get_file_owners(path_str);
        if (!owners.empty()) {
            // 走到这里说明所有者不是当前包（快速路径已排除），直接报告冲突
            if (!Config::instance().force_overwrite_mode()) {
                conflicts[path_str] = *owners.begin();
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
            created_dirs_.insert(d);
        }

        if (fs::is_directory(src_path)) {
            ensure_dir_exists(physical_path);
            struct stat st;
            if (lstat(src_path.c_str(), &st) == 0) {
                (void)lchown(physical_path.c_str(), st.st_uid, st.st_gid);
                (void)chmod(physical_path.c_str(), st.st_mode & 07777);
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
            } else if (fs::exists(physical_path) || fs::is_symlink(physical_path)) {
                // 替换已存在文件前先创建备份
                if (!fs::is_directory(physical_path)) {
                    fs::path bak = physical_path;
                    bak += std::string(constants::SUFFIX_LPKG_BAK) + pkg_name_;
                    fs::rename(physical_path, bak);
                    backups_.emplace_back(physical_path, bak);
                }
            }

            if (fs::is_symlink(src_path)) {
                fs::path link_target = fs::read_symlink(src_path);
                if (fs::exists(final_dest) || fs::is_symlink(final_dest)) fs::remove(final_dest);
                fs::create_symlink(link_target, final_dest);
            } else {
                fs::copy(src_path, final_dest,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            }

            // 保留原始文件权限和所有者
            struct stat st;
            if (lstat(src_path.c_str(), &st) == 0) {
                (void)lchown(final_dest.c_str(), st.st_uid, st.st_gid);
                if (!S_ISLNK(st.st_mode)) {
                    (void)chmod(final_dest.c_str(), st.st_mode & 07777);
                }
            }

            installed_files_.push_back(final_dest);
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
    }

    std::ofstream deps_out(Config::instance().dep_dir() / pkg_name_);
    for (const auto& d : deps_) {
        deps_out << d << constants::NL;
        std::string name = d;
        if (const auto pos = d.find_first_of(" \t<>="); pos != std::string::npos) name = d.substr(0, pos);
        cache.add_reverse_dep(name, pkg_name_);
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
