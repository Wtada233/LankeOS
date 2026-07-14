#include "archive.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "config/config.hpp"
#include "crypto/hash.hpp"
#include "db/cache.hpp"
#include "db/transaction_log.hpp"
#include "db/wal_op.hpp"
#include "downloader.hpp"
#include "i18n/localization.hpp"
#include "install_common.hpp"
#include "trigger/trigger.hpp"
#include "vercmp/version.hpp"

#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ===== InstallationTask 实现 =====

InstallationTask::InstallationTask(std::string pkg_name, std::string version,
                                   bool explicit_install,
                                   std::string old_version_to_replace,
                                   fs::path local_package_path,
                                   std::string expected_hash,
                                   bool force_reinstall)
    : pkg_name_(std::move(pkg_name)), version_(std::move(version)),
      explicit_install_(explicit_install),
      tmp_pkg_dir_(Config::get_tmp_dir() / pkg_name_),
      actual_version_(version_),
      old_version_to_replace_(std::move(old_version_to_replace)),
      local_package_path_(std::move(local_package_path)),
      expected_hash_(std::move(expected_hash)),
      force_reinstall_(force_reinstall) {}

/**
 * 执行安装流程（WAL 2.0 原子事务）：
 *   WAL: BEGIN → 预检 → 备份(BACKUP/NEW/NEW_DIR) → 复制(COPY) →
 *        注册 → COMMIT → END
 *
 * 如果 copy_package_files() 抛出异常，执行包级文件回滚(RESTORE_x/REMOVE_x)
 * 并写 ROLLBACK/END。
 *
 * .lpkg_bak 文件延迟到 COMMIT_PKGS 后的统一清理阶段才删除，
 * 确保批量回滚时可以恢复每个已安装包的文件。
 */
void InstallationTask::run(InstallContext *ctx) {
  const std::string current_installed_version =
      Cache::instance().get_installed_version(pkg_name_);
  if (!force_reinstall_ && !current_installed_version.empty() &&
      current_installed_version == actual_version_) {
    log_info(string_format("info.package_already_installed", pkg_name_));
    return;
  }

  log_info(string_format("info.installing_package", pkg_name_, version_));
  ensure_dir_exists(tmp_pkg_dir_);

  // 第一阶段：预检——不碰文件，只做检查
  prepare(ctx);

  // WAL: BEGIN <pkg> <ver> + fsync
  wal::log_wal_line("BEGIN " + pkg_name_ + " " + actual_version_);

  try {
    // 第二阶段：备份 + 复制（含 WAL 条目）
    backup_existing_files();
    copy_package_files();

    // 第三阶段：注册——数据库修改
    commit_without_file_ops();

    // WAL: COMMIT <pkg> <ver> + fsync
    wal::log_wal_line("COMMIT " + pkg_name_ + " " + actual_version_);

    // WAL: END <pkg> <ver> + fsync
    wal::log_wal_line("END " + pkg_name_ + " " + actual_version_);

    // 注意：不在此处清理 .lpkg_bak！
    // 所有备份文件延迟到 COMMIT_PKGS 后统一清理（见 batch_transaction.hpp）

    log_info(string_format("info.package_installed_successfully", pkg_name_));
  } catch (...) {
    // 包级文件回滚
    rollback_files();
    throw;
  }
}

/** 准备阶段：下载并验证包、解压、检查依赖和文件冲突 */
void InstallationTask::prepare(InstallContext *ctx) {
  download_and_verify_package();
  extract_and_validate_package();
  if (ctx) {
    ensure_dependencies_satisfied(*ctx);
  }
  check_for_file_conflicts();
}

/**
 * 提交安装（无文件操作部分）：注册包 -> 移除旧版废弃文件 -> 运行 post-install
 * 钩子
 */
void InstallationTask::commit_without_file_ops() {
  std::unordered_set<std::string> old_files;
  if (!old_version_to_replace_.empty()) {
    old_files = Cache::instance().get_package_files(pkg_name_);
    log_info(string_format("info.upgrade_old_files_check",
                           old_version_to_replace_, actual_version_,
                           old_files.size()));
  }

  register_package();

  // 移除新版本中不再包含的旧文件
  if (!old_files.empty()) {
    const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
    auto new_files = detail::scan_content_files(content_dir);
    std::unordered_set<std::string> new_set;
    for (const auto &f : new_files)
      new_set.insert((fs::path("/") / f).string());

    auto &cache = Cache::instance();
    for (const auto &old_file : old_files) {
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
            const fs::path phys =
                (fs::path(old_file).is_absolute())
                    ? Config::instance().root_dir() /
                          fs::path(old_file).relative_path()
                    : Config::instance().root_dir() / old_file;
            if (fs::exists(phys) || fs::is_symlink(phys)) {
              if (fs::is_directory(phys)) {
                std::error_code ec;
                if (fs::is_empty(phys, ec)) {
                  log_info(
                      string_format("info.removing_obsolete_file", old_file));
                  fs::remove(phys, ec);
                }
                continue;
              }
              log_info(string_format("info.removing_obsolete_file", old_file));
              fs::path bak = phys;
              bak += std::string(constants::SUFFIX_LPKG_BAK) + pkg_name_;
              fs::rename(phys, bak);
              fsync_parent_dir(bak);
              backups_.emplace_back(phys, bak);
            }
          }
        }
      }
    }
  }

  run_post_install_hook();
}

/** 备份所有将被覆盖的文件为 .lpkgbak，同时写入 WAL 条目 */
void InstallationTask::backup_existing_files() {
  const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
  auto files = detail::scan_content_files(content_dir);

  for (const auto &f : files) {
    extern std::atomic<bool> sigint_graceful;
    if (sigint_graceful.load())
      throw LpkgException(get_string("info.sigint_aborted"));

    fs::path rel_f = f;
    if (rel_f.is_absolute())
      rel_f = rel_f.relative_path();
    const fs::path physical_path = Config::instance().root_dir() / rel_f;
    const fs::path phys_dir = physical_path.parent_path();

    const bool is_config = f.starts_with(std::string(constants::DIR_ETC));
    if (is_config)
      continue;

    std::error_code ec;
    if (f.ends_with('/')) {
      bool is_new_dir = !fs::exists(physical_path);
      if (is_new_dir) {
        new_dirs_.push_back(physical_path);
        // WAL: NEW_DIR <path>  (write-ahead: 先写 WAL 再做实际操作)
        wal::log_wal_line("NEW_DIR " + physical_path.string());
      }
      fs::create_directories(phys_dir, ec);
      if (is_new_dir) {
        fs::create_directories(physical_path, ec);
        std::string dir_rel = f;
        if (dir_rel.ends_with('/'))
          dir_rel.pop_back();
        const fs::path src_dir = content_dir / dir_rel;
        struct stat st;
        if (lstat(src_dir.c_str(), &st) == 0) {
          (void)lchown(physical_path.c_str(), st.st_uid, st.st_gid);
          (void)chmod(physical_path.c_str(), st.st_mode & 07777);
        }
      }
      continue;
    }

    fs::create_directories(phys_dir, ec);
    if (fs::exists(physical_path) || fs::is_symlink(physical_path)) {
      if (!fs::is_directory(physical_path)) {
        fs::path bak = physical_path;
        bak += std::string(constants::SUFFIX_LPKG_BAK) + pkg_name_;
        // write-ahead: 先写 WAL 再 rename
        wal::log_wal_line("BACKUP " + physical_path.string() +
                          " \xe2\x86\x92 " + bak.string());
        fs::rename(physical_path, bak);
        fsync_parent_dir(bak);
        backups_.emplace_back(physical_path, bak);
      }
    } else {
      new_files_.push_back(physical_path);
      // WAL: NEW <path>
      wal::log_wal_line("NEW " + physical_path.string());
    }
  }
}

/**
 * 清理备份文件。
 * 在批次模式下（有 WAL 上下文时），此方法为空操作——
 * 所有 .lpkg_bak 延迟到 COMMIT_PKGS 后统一清理。
 * 在非批次模式（旧版调用路径）中，立即清理备份以保持向后兼容。
 */
void InstallationTask::cleanup_backups() {
  for (const auto &[orig, bak] : backups_) {
    std::error_code ec;
    fs::remove(bak, ec);
  }
  backups_.clear();
  new_files_.clear();
  new_dirs_.clear();
}

/**
 * 包级文件回滚（WAL 2.0）。
 *
 * 在 copy_package_files() 的 catch 中触发。
 * 每步逆向操作后写入 RESTORE_* / REMOVE_* 审计 WAL 行，
 * 确保回滚过程中断电能通过 rec 的幂等续传完成。
 */
void InstallationTask::rollback_files() {
  // 1. 恢复备份文件
  for (const auto &[orig, bak] : backups_) {
    if (fs::exists(bak)) {
      fs::rename(bak, orig);
      fsync_parent_dir(orig);
      wal::log_wal_line("RESTORE_FILE " + bak.string() + " \xe2\x86\x92 " +
                        orig.string());
    }
  }

  // 2. 删除新文件
  for (const auto &f : new_files_) {
    if (fs::exists(f) || fs::is_symlink(f)) {
      std::error_code ec;
      fs::remove(f, ec);
      if (!ec) {
        wal::log_wal_line("REMOVE_FILE " + f.string());
      }
    }
  }

  // 3. 删除新目录（仅当为空）
  for (const auto &d : new_dirs_) {
    if (fs::exists(d) && fs::is_directory(d)) {
      std::error_code ec;
      if (fs::is_empty(d, ec)) {
        fs::remove(d, ec);
        if (!ec) {
          wal::log_wal_line("REMOVE_DIR " + d.string());
        }
      }
    }
  }

  // 4. WAL: ROLLBACK + END
  wal::log_wal_line("ROLLBACK " + pkg_name_ + " " + actual_version_);
  wal::log_wal_line("END " + pkg_name_ + " " + actual_version_);

  // 5. 清空内部追踪
  backups_.clear();
  new_files_.clear();
  new_dirs_.clear();
}

/**
 * 下载并验证包文件
 * 如果是本地包文件，直接使用并校验 SHA256 哈希
 * 如果是从仓库安装，从镜像 URL 下载并校验
 */
void InstallationTask::download_and_verify_package() {
  if (!local_package_path_.empty()) {
    if (!fs::exists(local_package_path_))
      throw LpkgException(string_format("error.local_pkg_not_found",
                                        local_package_path_.string()));
    log_info(string_format("info.installing_local_file",
                           local_package_path_.string()));
    archive_path_ = local_package_path_;
    if (!expected_hash_.empty() &&
        calculate_sha256(archive_path_) != expected_hash_)
      throw LpkgException(string_format("error.hash_mismatch", pkg_name_));
    return;
  }

  const std::string mirror_url = Config::instance().get_mirror_url();
  const std::string arch = Config::instance().get_architecture();

  if (actual_version_.empty() || actual_version_ == constants::VER_LATEST) {
    Repository repo;
    repo.load_index();
    auto info = repo.find_package(pkg_name_);
    if (info) {
      actual_version_ = info->version;
      expected_hash_ = info->sha256;
    } else {
      throw LpkgException(
          string_format("warning.package_not_in_repo", pkg_name_));
    }
  }

  const std::string download_url = mirror_url + arch + "/" + pkg_name_ + "/" +
                                   actual_version_ +
                                   std::string(constants::EXT_LPKG);
  archive_path_ =
      tmp_pkg_dir_ / (actual_version_ + std::string(constants::EXT_LPKG));

  if (!fs::exists(archive_path_))
    download_with_retries(download_url, archive_path_, 5, true);
  if (!expected_hash_.empty() &&
      calculate_sha256(archive_path_) != expected_hash_)
    throw LpkgException(string_format("error.hash_mismatch", pkg_name_));
}

/** 解压包文件到临时目录，验证包结构完整性 */
void InstallationTask::extract_and_validate_package() {
  log_info(get_string("info.extracting_to_tmp"));
  extract_tar_zst(archive_path_, tmp_pkg_dir_);

  for (const auto &meta :
       {constants::PKG_METADATA_FILE, constants::DIR_CONTENT}) {
    if (!fs::exists(tmp_pkg_dir_ / meta))
      throw LpkgException(string_format("error.incomplete_package",
                                        (tmp_pkg_dir_ / meta).string()));
  }

  std::string meta_name, meta_version;
  detail::read_package_metadata(tmp_pkg_dir_, meta_name, meta_version, deps_,
                                provides_, needed_so_, man_content_);
  if (meta_name != pkg_name_) {
    log_warning(
        string_format("warning.package_name_mismatch", pkg_name_, meta_name));
  }
}

void InstallationTask::ensure_dependencies_satisfied(InstallContext &ctx) {
  if (Config::instance().no_deps_mode())
    return;
  auto actual_deps = detail::parse_dep_strings(deps_);
  if (actual_deps.empty())
    return;

  log_info(string_format("info.checking_deps"));
  bool found_new = false;

  for (const auto &dep : actual_deps) {
    const std::string &dep_name = dep.name;
    const std::string installed_ver =
        Cache::instance().get_installed_version(dep_name);

    if (!installed_ver.empty()) {
      if (dep.constraints.empty() || installed_ver == "virtual" ||
          version_satisfies_all(installed_ver, dep.constraints)) {
        continue;
      }
    }

    if (ctx.plan.contains(dep_name))
      continue;

    std::string req_ver = std::string(constants::VER_LATEST);
    if (!dep.constraints.empty()) {
      if (auto matching =
              ctx.repo.find_best_matching_version(dep_name, dep.constraints))
        req_ver = matching->version;
    }

    log_info(string_format("info.installing_discovered_dep", dep_name));

    std::set<std::string> vs;
    detail::resolve_package_dependencies(dep_name, req_ver, false, ctx, vs);

    if (!ctx.plan.contains(dep_name)) {
      auto providers = Cache::instance().get_providers(dep_name);
      if (!providers.empty())
        continue;

      bool found_in_plan = false;
      for (const auto &[pkg_name, pplan] : ctx.plan) {
        for (const auto &prov : pplan.provides) {
          if (prov == dep_name || prov.find(dep_name) != std::string::npos) {
            found_in_plan = true;
            break;
          }
        }
        if (found_in_plan)
          break;
      }

      if (!found_in_plan) {
        for (const auto &[tn, tv] : ctx.targets) {
          if (ctx.plan.contains(tn))
            continue;
          if (auto it = ctx.local_candidates.find(tn);
              it != ctx.local_candidates.end()) {
            try {
              std::string m_json = extract_file_from_archive(
                  it->second, std::string(constants::PKG_METADATA_FILE));
              if (!m_json.empty()) {
                json meta = json::parse(m_json);
                for (const auto &prov :
                     meta.value(std::string(constants::J_PROVIDES),
                                std::vector<std::string>{})) {
                  if (prov == dep_name) {
                    found_in_plan = true;
                    break;
                  }
                }
              }
            } catch (const std::exception &e) {
              log_warning(string_format(
                  "warning.dep_scan_metadata_parse_failed", tn, e.what()));
            }
          }
          if (found_in_plan)
            break;
        }
      }
      if (found_in_plan)
        continue;

      throw LpkgException(
          string_format("error.unresolvable_drift", pkg_name_,
                        string_format("error.package_not_in_repo", dep_name)));
    }
    found_new = true;
  }

  if (found_new) {
    if (auto broken = detail::check_plan_consistency(ctx.plan);
        !broken.empty()) {
      throw LpkgException(string_format(
          "error.unresolvable_drift", pkg_name_,
          "Dependency conflict detected in discovered dependencies"));
    }
  }

  if (!needed_so_.empty()) {
    for (const auto &soname : needed_so_) {
      bool provided = false;

      for (const auto &[pn, plan_pkg] : ctx.plan) {
        for (const auto &prov : plan_pkg.provides) {
          if (prov == soname) {
            provided = true;
            break;
          }
        }
        if (provided)
          break;
      }

      if (!provided) {
        auto providers = Cache::instance().get_providers(soname);
        for (const auto &p : providers) {
          if (Cache::instance().is_installed(p)) {
            if (!ctx.plan.contains(p)) {
              provided = true;
              break;
            }
          }
        }
      }

      if (!provided) {
        if (auto prov_pkg = ctx.repo.find_provider(soname)) {
          for (const auto &prov : prov_pkg->provides) {
            if (prov == soname) {
              provided = true;
              break;
            }
          }
        }
      }

      if (!provided) {
        throw LpkgException(
            string_format("error.unresolvable_drift", pkg_name_,
                          string_format("error.unresolved_soname", soname)));
      }
    }
  }
}

void InstallationTask::check_for_file_conflicts() {
  std::map<std::string, std::string> conflicts;
  const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
  auto files = detail::scan_content_files(content_dir);
  auto &cache = Cache::instance();

  for (const auto &f : files) {
    fs::path rel_f = f;
    if (rel_f.is_absolute())
      rel_f = rel_f.relative_path();
    const fs::path logical_path = fs::path("/") / rel_f;
    const std::string path_str = logical_path.string();

    if (path_str.ends_with('/'))
      continue;

    if (f.starts_with(std::string(constants::DIR_ETC)))
      continue;

    if (cache.is_file_owned_by(path_str, pkg_name_))
      continue;

    auto owners = cache.get_file_owners(path_str);
    if (!owners.empty()) {
      if (!Config::instance().force_overwrite_mode()) {
        conflicts[path_str] = *owners.begin();
      } else {
        for (const auto &owner : owners) {
          cache.remove_file_owner(path_str, owner);
        }
      }
      continue;
    }

    if (old_version_to_replace_.empty()) {
      const fs::path phys = Config::instance().root_dir() / rel_f;
      if ((fs::exists(phys) || fs::is_symlink(phys)) &&
          !Config::instance().force_overwrite_mode()) {
        conflicts[path_str] = get_string("error.unknown_manual_file");
      }
    }
  }

  if (!conflicts.empty()) {
    std::string msg = get_string("error.file_conflict_header") + "\n";
    for (const auto &[file, owner] : conflicts)
      msg +=
          "  " + string_format("error.file_conflict_entry", file, owner) + "\n";
    throw LpkgException(msg + get_string("error.installation_aborted"));
  }
}

void InstallationTask::copy_package_files() {
  log_info(get_string("info.copying_files"));
  const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
  auto files = detail::scan_content_files(content_dir);

  for (const auto &f : files) {
    if (on_before_file_copy)
      on_before_file_copy();

    extern std::atomic<bool> sigint_graceful;
    if (sigint_graceful.load())
      throw LpkgException(get_string("info.sigint_aborted"));

    fs::path rel_f = f;
    if (rel_f.is_absolute())
      rel_f = rel_f.relative_path();
    const fs::path src_path = content_dir / f;
    const fs::path physical_path = Config::instance().root_dir() / rel_f;

    if (!fs::exists(src_path) && !fs::is_symlink(src_path))
      continue;

    fs::path parent = physical_path.parent_path();
    std::vector<fs::path> to_create;
    while (!parent.empty() && !fs::exists(parent)) {
      to_create.push_back(parent);
      if (parent == Config::instance().root_dir())
        break;
      parent = parent.parent_path();
    }
    for (const auto &d : to_create | std::views::reverse) {
      ensure_dir_exists(d);
    }

    if (fs::is_symlink(src_path)) {
      fs::path link_target = fs::read_symlink(src_path);
      fs::path dest = physical_path;

      const bool is_config = f.starts_with(std::string(constants::DIR_ETC));
      if (is_config && fs::exists(physical_path) &&
          !fs::is_directory(physical_path)) {
        dest += std::string(constants::SUFFIX_LPKG_NEW);
        log_warning(string_format("warning.config_conflict",
                                  physical_path.string(), dest.string()));
        has_config_conflicts_ = true;
      }

      if (fs::exists(dest) || fs::is_symlink(dest))
        fs::remove(dest);
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
                  "warning.dir_perm_mismatch", physical_path.string(),
                  static_cast<int>(cur_mode), static_cast<int>(pkg_mode)));
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

      if (is_config && fs::exists(physical_path) &&
          !fs::is_directory(physical_path)) {
        final_dest += std::string(constants::SUFFIX_LPKG_NEW);
        if (fs::exists(final_dest) || fs::is_symlink(final_dest))
          fs::remove(final_dest);
        log_warning(string_format("warning.config_conflict",
                                  physical_path.string(), final_dest.string()));
        has_config_conflicts_ = true;
        fs::copy(src_path, final_dest,
                 fs::copy_options::recursive |
                     fs::copy_options::overwrite_existing);
      } else {
        fs::path tmp_path = final_dest;
        tmp_path += ".lpkgtmp";
        fs::copy(src_path, tmp_path,
                 fs::copy_options::recursive |
                     fs::copy_options::overwrite_existing);

        struct stat st;
        if (lstat(src_path.c_str(), &st) == 0) {
          (void)lchown(tmp_path.c_str(), st.st_uid, st.st_gid);
          if (!S_ISLNK(st.st_mode)) {
            (void)chmod(tmp_path.c_str(), st.st_mode & 07777);
          }
        }
        // fsync .lpkgtmp 后再写 WAL
        int fd = ::open(tmp_path.c_str(), O_RDONLY);
        if (fd >= 0) {
          ::fsync(fd);
          ::close(fd);
        }
        // WAL: COPY <tmp> → <dst> (write-ahead: WAL 先于 rename)
        wal::log_wal_line("COPY " + tmp_path.string() + " \xe2\x86\x92 " +
                          final_dest.string());
        fs::rename(tmp_path, final_dest);
        fsync_parent_dir(final_dest);
      }

      TriggerManager::instance().check_file((fs::path("/") / f).string());
    } catch (const std::exception &e) {
      throw LpkgException(string_format("error.copy_failed_rollback", f,
                                        physical_path.string(), e.what()));
    }
  }

  if (has_config_conflicts_)
    log_warning(get_string("info.config_review_reminder"));
}

void InstallationTask::register_package() {
  auto &cache = Cache::instance();

  if (!old_version_to_replace_.empty()) {
    const fs::path old_dep_file = Config::instance().dep_dir() / pkg_name_;
    if (fs::exists(old_dep_file)) {
      std::ifstream f(old_dep_file);
      std::string line;
      while (std::getline(f, line)) {
        if (!line.empty()) {
          std::stringstream ss(line);
          std::string dn;
          if (ss >> dn)
            cache.remove_reverse_dep(dn, pkg_name_);
        }
      }
    }
    for (const auto &cap : cache.get_package_provides(pkg_name_)) {
      cache.remove_provider(cap, pkg_name_);
    }
    fs::remove(Config::instance().needed_so_dir() / pkg_name_);
  }

  std::unordered_set<std::string> dep_entries;
  for (const auto &d : deps_) {
    dep_entries.insert(d);
    std::string name = d;
    if (const auto pos = d.find_first_of(" \t<>="); pos != std::string::npos)
      name = d.substr(0, pos);
    cache.add_reverse_dep(name, pkg_name_);
  }

  for (const auto &soname : needed_so_) {
    auto providers = cache.get_providers(soname);
    for (const auto &prov_pkg : providers) {
      if (prov_pkg != pkg_name_ && cache.is_installed(prov_pkg)) {
        dep_entries.insert(prov_pkg);
        cache.add_reverse_dep(prov_pkg, pkg_name_);
      }
    }
  }

  std::vector<std::string> sorted_deps(dep_entries.begin(), dep_entries.end());
  std::sort(sorted_deps.begin(), sorted_deps.end());
  std::ofstream deps_out(Config::instance().dep_dir() / pkg_name_);
  for (const auto &entry : sorted_deps) {
    deps_out << entry << constants::NL;
  }

  std::ofstream nso_out(Config::instance().needed_so_dir() / pkg_name_);
  for (const auto &sn : needed_so_) {
    nso_out << sn << constants::NL;
  }

  const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
  for (const auto &f : detail::scan_content_files(content_dir)) {
    cache.add_file_owner((fs::path("/") / f).string(), pkg_name_);
  }

  const fs::path man_path = Config::instance().docs_dir() /
                            (pkg_name_ + std::string(constants::SUFFIX_MAN));
  if (!man_content_.empty()) {
    std::ofstream man_out(man_path);
    man_out << man_content_;
  } else {
    std::error_code ec;
    fs::remove(man_path, ec);
  }

  for (const auto &cap : provides_) {
    cache.add_provider(cap, pkg_name_);
  }
  cache.add_installed(pkg_name_, actual_version_, explicit_install_);
}

void InstallationTask::run_post_install_hook() {
  const fs::path hook_src = tmp_pkg_dir_ / constants::DIR_HOOKS;
  if (!fs::exists(hook_src) || !fs::is_directory(hook_src))
    return;

  const fs::path dest_dir = Config::instance().hooks_dir() / pkg_name_;
  ensure_dir_exists(dest_dir);
  for (const auto &entry : fs::directory_iterator(hook_src)) {
    if (entry.is_regular_file()) {
      const fs::path dest = dest_dir / entry.path().filename();
      fs::copy(entry.path(), dest, fs::copy_options::overwrite_existing);
      fs::permissions(dest,
                      fs::perms::owner_exec | fs::perms::group_exec |
                          fs::perms::others_exec,
                      fs::perm_options::add);
    }
  }
  detail::run_hook(pkg_name_, std::string(constants::POSTINST_SH));
}

std::vector<DependencyInfo> InstallationTask::parse_deps() const {
  return detail::parse_dep_strings(deps_);
}
