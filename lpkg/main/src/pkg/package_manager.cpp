#include "package_manager.hpp"

#include "install_common.hpp"

#include "archive.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "config/config.hpp"
#include "crypto/hash.hpp"
#include "db/cache.hpp"
#include "downloader.hpp"
#include "i18n/localization.hpp"
#include "repo/repository.hpp"
#include "trigger/trigger.hpp"
#include "vercmp/version.hpp"

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

#include <random>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

// =====================================================================
// 公开 API
// =====================================================================

/** 将缓存数据写回磁盘 */
void write_cache() { Cache::instance().write(); }

/**
 * 安装包的主入口
 * 流程：解析参数 -> 初始化仓库和缓存 -> 解析依赖 -> 静态一致性检查 ->
 * 用户确认 -> 实际安装 -> 触发运行
 */
void install_packages(const std::vector<std::string> &pkg_args,
                      const std::string &hash_file_path, bool force_reinstall) {
  Cache::instance().load();
  TmpDirManager tmp;
  Repository repo;
  try {
    repo.load_index();
  } catch (const std::exception &e) {
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

  // 安装参数解析
  for (const auto &arg : pkg_args) {
    const fs::path p(arg);
    if (p.extension() == constants::EXT_ZST ||
        p.extension() == constants::EXT_LPKG ||
        arg.find('/') != std::string::npos) {
      if (fs::exists(p)) {
        try {
          json meta = detail::read_archive_metadata(fs::absolute(p));
          std::string n = meta.at(std::string(constants::J_NAME));
          std::string v = meta.at(std::string(constants::J_VERSION));
          locals[n] = fs::absolute(p);
          targets.emplace_back(n, v);
        } catch (const std::exception &e) {
          log_error(
              string_format("warning.skip_invalid_local_pkg", arg, e.what()));
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

  InstallContext ctx{
      repo, plan, order, locals, targets, force_reinstall, /*top_level=*/true,
      {}};

  // 一致性重试循环
  while (true) {
    plan.clear();
    order.clear();
    ctx.plan = plan;
    ctx.install_order = order;
    ctx.successfully_installed.clear();
    ctx.installed_set.clear();

    for (const auto &[n, v] : targets) {
      std::set<std::string> vs;
      detail::resolve_package_dependencies(n, v, true, ctx, vs);
    }

    if (!provided_hash.empty()) {
      if (locals.empty())
        throw LpkgException(get_string("error.hash_requires_local"));
      for (auto &[n, p] : plan)
        if (!p.local_path.empty())
          p.sha256 = provided_hash;
    }

    if (plan.empty()) {
      log_info(get_string("info.all_packages_already_installed"));
      return;
    }

    if (auto broken = detail::check_plan_consistency(plan); !broken.empty()) {
      log_error(get_string("error.dependency_conflict_title"));
      if (user_confirms(get_string("prompt.remove_conflict_pkgs"))) {
        for (const auto &pkg : broken)
          remove_package(pkg, true);
        Cache::instance().write();
        continue;
      }
      log_info(get_string("info.installation_aborted"));
      return;
    }

    if (auto nso_broken = detail::check_needed_so_consistency(plan);
        !nso_broken.empty()) {
      log_error(get_string("error.dependency_conflict_title"));
      std::string nso_msg;
      for (const auto &pkg : nso_broken)
        nso_msg += "  " + pkg + "\n";
      log_error(nso_msg);
      if (user_confirms(get_string("prompt.remove_conflict_pkgs"))) {
        for (const auto &pkg : nso_broken)
          remove_package(pkg, true);
        Cache::instance().write();
        continue;
      }
      log_info(get_string("info.installation_aborted"));
      return;
    }

    break;
  }

  detail::check_forward_soname_integrity(plan, repo);

  // 用户确认
  std::string prompt;
  for (const auto &n : order) {
    const auto &p = plan.at(n);
    prompt += "  " +
              string_format(p.is_explicit ? "info.package_list_item"
                                          : "info.package_list_item_dep",
                            p.name, p.actual_version) +
              "\n";
  }
  if (!user_confirms(prompt + get_string("info.confirm_proceed"))) {
    log_info(get_string("info.installation_aborted"));
    return;
  }

  ctx.successfully_installed.clear();
  ctx.installed_set.clear();

  // 执行安装
  install_packages_internal(ctx);
  Cache::instance().write();


  TriggerManager::instance().run_all();
  log_info(get_string("info.install_complete"));
}

/** 在 main.cpp 中声明，由 SIGINT 信号处理函数设置 */
extern std::atomic<bool> sigint_graceful;

void install_packages_internal(InstallContext &ctx) {
  size_t i = 0;
  while (i < ctx.install_order.size()) {
    if (sigint_graceful.load())
      throw LpkgException(get_string("info.sigint_aborted"));

    const std::string &n = ctx.install_order[i];
    ++i;

    if (ctx.installed_set.contains(n)) {
      continue;
    }

    auto &p = ctx.plan.at(n);


    if (!p.metadata_verified) {
      InstallationTask check_task(
          p.name, p.actual_version, p.is_explicit,
          Cache::instance().get_installed_version(p.name), p.local_path,
          p.sha256, p.force_reinstall);
      ensure_dir_exists(check_task.tmp_pkg_dir());
      check_task.download_and_verify_package();

      json meta = detail::read_archive_metadata(check_task.archive_path());
      std::vector<std::string> dep_strs = meta.value(
          std::string(constants::J_DEPS), std::vector<std::string>{});
      auto actual_deps = detail::parse_dep_strings(dep_strs);
      std::vector<std::string> actual_provides = meta.value(
          std::string(constants::J_PROVIDES), std::vector<std::string>{});
      std::vector<std::string> actual_needed_so = meta.value(
          std::string(constants::J_NEEDED_SO), std::vector<std::string>{});

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
        for (const auto &[tn, tv] : ctx.targets) {
          std::set<std::string> vs;
          detail::resolve_package_dependencies(tn, tv, true, ctx, vs);
        }
        i = 0;
        continue;
      }

      p.local_path = check_task.archive_path();
      p.metadata_verified = true;
    }

    InstallationTask task(p.name, p.actual_version, p.is_explicit,
                          Cache::instance().get_installed_version(p.name),
                          p.local_path, p.sha256, p.force_reinstall);
    task.run(&ctx);
    ctx.successfully_installed.push_back(p.name);
    ctx.installed_set.insert(p.name);

  }
}

/**
 * 移除已安装的包
 * 检查是否为 essential 包、是否有其他包依赖它、是否有包依赖其提供的虚拟包名
 * force 模式下跳过所有安全检查
 */
void remove_package(const std::string &pkg_name, bool force, bool) {
  extern std::atomic<bool> sigint_graceful;
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
    if (auto rdeps = Cache::instance().get_reverse_deps(pkg_name);
        !rdeps.empty()) {
      std::string list;
      for (const auto &d : rdeps)
        list += d + " ";
      log_info(string_format("info.skip_remove_dependency", pkg_name, list));
      return;
    }
    for (const auto &cap : Cache::instance().get_package_provides(pkg_name)) {
      if (auto rdeps = Cache::instance().get_reverse_deps(cap);
          !rdeps.empty()) {
        std::string list;
        for (const auto &d : rdeps)
          list += d + " ";
        log_info(string_format("info.skip_remove_dependency", cap, list));
        return;
      }
    }
  }


  log_info(string_format("info.removing_package", pkg_name));

  if (sigint_graceful.load())
    throw LpkgException(get_string("info.sigint_aborted"));

  detail::run_hook(pkg_name, std::string(constants::PRERM_SH));

  std::vector<std::pair<fs::path, fs::path>> backups;
  std::error_code ec;

  try {

    auto &cache = Cache::instance();
    auto owned_entries = cache.get_package_files(pkg_name);

    // 共享文件检查
    if (!force && !owned_entries.empty()) {
      std::vector<std::pair<std::string, std::string>> shared;
      for (const auto &entry : owned_entries) {
        if (entry.ends_with('/'))
          continue;
        auto owners = cache.get_file_owners(entry);
        std::string others;
        for (const auto &owner : owners) {
          if (owner != pkg_name) {
            if (!others.empty())
              others += ", ";
            others += owner;
          }
        }
        if (!others.empty())
          shared.emplace_back(entry, others);
      }
      if (!shared.empty()) {
        std::string msg = get_string("error.shared_file_header") + "\n";
        for (const auto &[file, owners] : shared)
          msg += "  " + string_format("error.shared_file_entry", file, owners) +
                 "\n";
        throw LpkgException(msg + get_string("error.removal_aborted"));
      }
    }

    // 备份阶段：将每个包文件 rename 到 .lpkg_bak
    int file_count = 0;
    if (!owned_entries.empty()) {
      std::vector<fs::path> paths;
      for (const auto &e : owned_entries)
        paths.emplace_back(e);
      std::ranges::sort(paths, std::greater<>{});

      for (const auto &p : paths) {
        std::string path_str = p.string();
        if (path_str.ends_with('/'))
          continue;
        const fs::path phys =
            p.is_absolute()
                ? Config::instance().root_dir() / fs::path(p).relative_path()
                : Config::instance().root_dir() / p;

        if (sigint_graceful.load())
          throw LpkgException(get_string("info.sigint_aborted"));

        if (fs::exists(phys) || fs::is_symlink(phys)) {
          fs::path bak = phys;
          bak += std::string(constants::SUFFIX_LPKG_BAK) + pkg_name;
          fs::rename(phys, bak);
          backups.emplace_back(phys, bak);
          ++file_count;

        }
      }
    }

    if (file_count > 0)
      log_info(string_format("info.files_removed", file_count));


    if (sigint_graceful.load())
      throw LpkgException(get_string("info.sigint_aborted"));

    // 从磁盘移除文件
    remove_package_files(pkg_name, force);

    if (sigint_graceful.load())
      throw LpkgException(get_string("info.sigint_aborted"));

    // 释放目录所有权 → 删目录
    std::vector<fs::path> dir_paths;
    for (const auto &e : owned_entries)
      if (e.ends_with('/'))
        dir_paths.emplace_back(fs::path(e));
    std::ranges::sort(dir_paths, std::greater<>{});


    int dir_count = 0;
    for (const auto &p : dir_paths) {
      cache.remove_file_owner(p.string(), pkg_name);
      if (!cache.get_file_owners(p.string()).empty())
        continue;

      const fs::path phys =
          p.is_absolute() ? Config::instance().root_dir() / p.relative_path()
                          : Config::instance().root_dir() / p;
      if (!fs::exists(phys) || !fs::is_directory(phys))
        continue;

      const std::string bak_suffix =
          std::string(constants::SUFFIX_LPKG_BAK) + pkg_name;
      for (auto &entry : fs::recursive_directory_iterator(phys, ec)) {
        if (entry.path().filename().string().ends_with(bak_suffix)) {
          fs::remove(entry.path(), ec);
        }
      }

      std::error_code ec2;
      fs::remove(phys, ec2);
      if (!ec2) {
        ++dir_count;
      }
    }
    if (dir_count > 0)
      log_info(string_format("info.dirs_removed", dir_count));


    // 清理依赖、文档和钩子文件
    const fs::path dep_file = Config::instance().dep_dir() / pkg_name;
    if (fs::exists(dep_file)) {
      std::ifstream f(dep_file);
      std::string l;
      while (std::getline(f, l)) {
        std::stringstream ss(l);
        std::string dn;
        if (ss >> dn)
          cache.remove_reverse_dep(dn, pkg_name);
      }
    }
    fs::remove(dep_file, ec);
    fs::remove(Config::instance().needed_so_dir() / pkg_name, ec);
    fs::remove(Config::instance().docs_dir() /
                   (pkg_name + std::string(constants::SUFFIX_MAN)),
               ec);
    fs::remove_all(Config::instance().hooks_dir() / pkg_name, ec);
    cache.remove_installed(pkg_name);


    if (sigint_graceful.load())
      throw LpkgException(get_string("info.sigint_aborted"));

    // 数据库落盘
    Cache::instance().write(pkg_name);


    if (sigint_graceful.load())
      throw LpkgException(get_string("info.sigint_aborted"));

  } catch (...) {
    // 注意：当前版本不提供移除回滚。原子事务和回滚将在新架构中重新实现。
    throw;
  }

  // 清理全部 .lpkg_bak
  for (const auto &[orig, bak] : backups) {
    fs::remove(bak, ec);
  }

  Cache::instance().cleanup_db_backups();


  log_info(string_format("info.package_removed_successfully", pkg_name));
}

void remove_package_files(const std::string &pkg_name, bool force) {
  extern std::atomic<bool> sigint_graceful;
  auto &cache = Cache::instance();
  auto owned_entries = cache.get_package_files(pkg_name);
  if (owned_entries.empty())
    return;

  if (!force) {
    std::vector<std::pair<std::string, std::string>> shared;
    for (const auto &entry : owned_entries) {
      if (entry.ends_with('/'))
        continue;
      auto owners = cache.get_file_owners(entry);
      std::string others;
      for (const auto &owner : owners) {
        if (owner != pkg_name) {
          if (!others.empty())
            others += ", ";
          others += owner;
        }
      }
      if (!others.empty())
        shared.emplace_back(entry, others);
    }
    if (!shared.empty()) {
      std::string msg =
          get_string("error.shared_file_header") + std::string(constants::NL);
      for (const auto &[file, owners] : shared) {
        msg += "  " + string_format("error.shared_file_entry", file, owners) +
               std::string(constants::NL);
      }
      throw LpkgException(msg + get_string("error.removal_aborted"));
    }
  }

  std::vector<fs::path> paths;
  for (const auto &e : owned_entries)
    paths.emplace_back(e);
  std::ranges::sort(paths, std::greater<>{});

  int file_count = 0;
  for (const auto &p : paths) {
    if (sigint_graceful.load())
      throw LpkgException(get_string("info.sigint_aborted"));

    std::string path_str = p.string();
    const fs::path phys = p.is_absolute() ? Config::instance().root_dir() /
                                                fs::path(p).relative_path()
                                          : Config::instance().root_dir() / p;

    if (path_str.ends_with('/')) {
      continue;
    } else {
      if (fs::exists(phys) || fs::is_symlink(phys)) {
        std::error_code ec;
        fs::remove(phys, ec);
        if (!ec)
          ++file_count;
      }
      cache.remove_file_owner(path_str, pkg_name);
    }
  }

  if (file_count > 0) {
    log_info(string_format("info.files_removed", file_count));
  }

  for (const auto &cap : cache.get_package_provides(pkg_name)) {
    cache.remove_provider(cap, pkg_name);
  }
}

/**
 * 自动移除不再被任何包依赖的孤立包
 */
void autoremove() {
  log_info(get_string("info.checking_autoremove"));
  const auto req = detail::get_all_required_packages();
  std::vector<std::string> to_rem;
  auto &cache = Cache::instance();
  {
    std::lock_guard lock(cache.get_mutex());
    for (const auto &name : cache.get_all_installed() | std::views::keys) {
      if (!req.contains(name))
        to_rem.push_back(name);
    }
  }

  if (to_rem.empty()) {
    log_info(get_string("info.no_autoremove_packages"));
  } else {
    log_info(string_format("info.autoremove_candidates", to_rem.size()));
    for (const auto &n : to_rem) {
      try {
        remove_package(n, true);
      } catch (const std::exception &e) {
        log_warning(
            string_format("warning.autoremove_remove_failed", n, e.what()));
      }
    }
    log_info(string_format("info.autoremove_complete", to_rem.size()));
  }
}

/**
 * 升级所有已安装的包
 */
void upgrade_packages() {
  extern std::atomic<bool> sigint_graceful;
  log_info(get_string("info.checking_upgradable"));
  TmpDirManager tmp;
  Repository repo;
  try {
    repo.load_index();
  } catch (const std::exception &e) {
    log_warning(string_format("warning.repo_index_load_failed", e.what()));
    return;
  }

  // 快照已安装包列表
  std::vector<std::pair<std::string, std::string>> installed;
  {
    std::lock_guard lock(Cache::instance().get_mutex());
    for (const auto &[name, ver] : Cache::instance().get_all_installed()) {
      installed.emplace_back(name, ver);
    }
  }

  // 构建升级计划
  struct UpgradeEntry {
    std::string name;
    std::string old_ver;
    std::string new_ver;
    std::string hash;
    bool held;
  };
  std::vector<UpgradeEntry> plan;
  std::map<std::string, InstallPlan> consistency_plan;
  for (const auto &[n, curr] : installed) {
    if (sigint_graceful.load()) {
      log_info(get_string("info.sigint_aborted"));
      return;
    }
    auto opt = repo.find_package(n);
    if (!opt)
      continue;
    if (!version_compare(curr, opt->version))
      continue;
    const bool held = Cache::instance().is_held(n);
    plan.push_back({n, curr, opt->version, opt->sha256, held});

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

  // 一致性检查
  detail::check_forward_soname_integrity(consistency_plan, repo);

  if (auto broken = detail::check_plan_consistency(consistency_plan);
      !broken.empty()) {
    log_error(get_string("error.dependency_conflict_title"));
    std::string msg;
    for (const auto &p : broken)
      msg += "  " + p + "\n";
    log_error(msg);
    throw LpkgException(msg);
  }

  if (auto nso_broken = detail::check_needed_so_consistency(consistency_plan);
      !nso_broken.empty()) {
    log_error(get_string("error.dependency_conflict_title"));
    std::string msg;
    for (const auto &p : nso_broken)
      msg += "  " + p + "\n";
    log_error(msg);
    throw LpkgException(msg);
  }

  // 用户确认
  std::string prompt;
  for (const auto &e : plan) {
    prompt += "  " + e.name + " " + e.old_ver + " \xe2\x86\x92 " + e.new_ver + "\n";
  }
  if (!user_confirms(prompt + get_string("info.confirm_proceed"))) {
    log_info(get_string("info.installation_aborted"));
    return;
  }

  // 执行升级
  for (const auto &e : plan) {
    if (sigint_graceful.load())
      throw LpkgException(get_string("info.sigint_aborted"));


    log_info(string_format("info.upgrading_package", e.name, e.old_ver,
                           e.new_ver));
    InstallationTask t(e.name, e.new_ver, e.held, e.old_ver, "", e.hash,
                       false);
    t.run();

  }

  Cache::instance().write();


  log_info(string_format("info.upgraded_packages", plan.size()));
  Cache::instance().cleanup_db_backups();
}

/** 显示包的 man 页面内容 */
void show_man_page(const std::string &pkg_name) {
  const fs::path p = Config::instance().docs_dir() / (pkg_name + ".man");
  if (!fs::exists(p))
    throw LpkgException(string_format("error.no_man_page", pkg_name));
  std::ifstream f(p);
  if (!f.is_open())
    throw LpkgException(
        string_format("error.open_man_page_failed", p.string()));
  std::cout << f.rdbuf();
}

/**
 * 重新安装一个包
 */
void reinstall_package(const std::string &arg) {
  std::string name = arg;
  if (arg.find('/') != std::string::npos || arg.ends_with(".lpkg")) {
    try {
      json meta = detail::read_archive_metadata(fs::absolute(arg));
      name = meta.at(std::string(constants::J_NAME)).get<std::string>();
    } catch (const std::exception &e) {
      log_warning(string_format("warning.reinstall_metadata_read_failed", arg,
                                e.what()));
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
void query_package(const std::string &pkg_name) {
  if (Cache::instance().get_installed_version(pkg_name).empty()) {
    log_info(string_format("info.package_not_installed", pkg_name));
    return;
  }
  log_info(string_format("info.package_files", pkg_name));
  auto files = Cache::instance().get_package_files(pkg_name);
  for (const auto &f : files) {
    std::cout << "  " << f << "\n";
  }
}

/** 查询指定文件属于哪个包 */
void query_file(const std::string &filename) {
  auto &cache = Cache::instance();
  std::string target = filename;
  auto owners = cache.get_file_owners(target);

  if (owners.empty()) {
    try {
      const fs::path abs_p = fs::absolute(filename);
      if (abs_p.string().starts_with(Config::instance().root_dir().string())) {
        const std::string logical =
            "/" + fs::relative(abs_p, Config::instance().root_dir()).string();
        owners = cache.get_file_owners(logical);
        if (!owners.empty())
          target = logical;
      }
    } catch (const std::exception &e) {
      log_warning(string_format("warning.query_path_resolve_failed", filename) +
                  ": " + e.what());
    }
  }

  if (owners.empty() && !fs::path(filename).is_absolute()) {
    const std::string fallback = (fs::path("/") / filename).string();
    owners = cache.get_file_owners(fallback);
    if (!owners.empty())
      target = fallback;
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
std::unordered_set<std::string>
collect_recursive_remove_set(const std::string &pkg_name) {
  std::unordered_set<std::string> result;
  std::unordered_set<std::string> visited;
  std::vector<std::string> queue = {pkg_name};

  while (!queue.empty()) {
    auto current = std::move(queue.back());
    queue.pop_back();
    if (!visited.insert(current).second)
      continue;
    result.insert(current);

    auto rdeps = Cache::instance().get_reverse_deps(current);
    for (const auto &cap : Cache::instance().get_package_provides(current)) {
      auto cap_rdeps = Cache::instance().get_reverse_deps(cap);
      rdeps.insert(cap_rdeps.begin(), cap_rdeps.end());
    }
    for (const auto &rdep : rdeps) {
      if (rdep != current && !visited.contains(rdep))
        queue.push_back(rdep);
    }
  }
  return result;
}

} // anonymous namespace

extern std::atomic<bool> sigint_graceful;

/**
 * 递归移除包及其所有受影响的依赖者。
 */
void remove_package_recursive(const std::string &pkg_name, bool force) {
  if (sigint_graceful.load())
    throw LpkgException(get_string("info.sigint_aborted"));
  Cache::instance().load();
  log_info(string_format("info.recursive_remove_start", pkg_name));

  const std::string ver = Cache::instance().get_installed_version(pkg_name);
  if (ver.empty()) {
    log_info(string_format("info.package_not_installed", pkg_name));
    return;
  }

  auto affected = collect_recursive_remove_set(pkg_name);
  if (affected.empty())
    return;

  if (!force && Cache::instance().is_essential(pkg_name)) {
    log_error(string_format("error.skip_remove_essential", pkg_name));
    return;
  }

  std::vector<std::string> to_remove;
  std::vector<std::string> essential_pkgs;
  for (const auto &p : affected) {
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
    for (const auto &p : essential_pkgs)
      msg += "  " + p + "\n";
    log_warning(msg);
  }

  log_info(get_string("info.recursive_remove_header"));
  for (const auto &p : to_remove)
    log_info(string_format("info.recursive_remove_item", p));

  // 按反向依赖数量升序排列（叶子先删）
  std::ranges::sort(to_remove, [](const std::string &a, const std::string &b) {
    return Cache::instance().get_reverse_deps(a).size() <
           Cache::instance().get_reverse_deps(b).size();
  });

  // 3 轮验证码确认
  bool confirmed = true;
  if (Config::instance().non_interactive_mode() ==
      NonInteractiveMode::INTERACTIVE) {
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

  for (const auto &p : to_remove) {
    log_info(string_format("info.recursive_removing", p));
    remove_package(p, true, false);
  }

  Cache::instance().write("recursive-remove");
  Cache::instance().cleanup_db_backups();

  log_info(get_string("info.recursive_remove_done"));
}
