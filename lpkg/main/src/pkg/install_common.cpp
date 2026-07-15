#include "install_common.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "i18n/localization.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace detail {

/** 从 lpkg 归档文件中读取 metadata.json 并解析为 JSON 对象 */
json read_archive_metadata(const fs::path &archive_path) {
  std::string meta_json = extract_file_from_archive(
      archive_path, std::string(constants::PKG_METADATA_FILE));
  if (meta_json.empty())
    throw LpkgException(string_format("error.local_pkg_missing_metadata",
                                      archive_path.string()));
  return json::parse(meta_json);
}

/**
 * 执行包的钩子脚本（如 post-install、pre-remove）
 * 支持 chroot 环境下运行，使用 mount namespace 隔离
 */
void run_hook(std::string_view pkg_name, std::string_view hook_name) {
  if (Config::instance().no_hooks_mode())
    return;

  const fs::path hook_path =
      Config::instance().hooks_dir() / pkg_name / hook_name;
  if (!fs::exists(hook_path) || !fs::is_regular_file(hook_path))
    return;

  log_info(string_format("info.running_hook", std::string(hook_name)));

  const bool use_chroot = (Config::instance().root_dir() != "/" &&
                           Config::instance().root_dir().string() != "/");
  std::vector<std::string> args = {std::string(constants::BIN_SH), "-c"};

  if (use_chroot) {
    if (!fs::exists(Config::instance().root_dir() / "bin/sh")) {
      log_warning(string_format("warning.hook_failed_setup",
                                std::string(hook_name),
                                get_string("error.sh_not_found")));
      return;
    }
    const fs::path hook_rel =
        fs::relative(hook_path, Config::instance().root_dir());
    args.push_back("/" + hook_rel.string());
  } else {
    args.push_back(hook_path.string());
  }

  pid_t pid = fork();
  if (pid == -1)
    return;
  if (pid == 0) {
    if (use_chroot) {
      // 创建独立的 mount namespace，避免影响主机挂载
      if (unshare(CLONE_NEWNS) != 0)
        _exit(1);
      mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);
      if (chroot(Config::instance().root_dir().c_str()) != 0)
        _exit(1);
      if (chdir("/") != 0)
        _exit(1);
    }

    std::vector<char *> c_args;
    for (const auto &arg : args)
      c_args.push_back(const_cast<char *>(arg.c_str()));
    c_args.push_back(nullptr);

    execv(c_args[0], c_args.data());
    _exit(1);
  }
  int status;
  waitpid(pid, &status, 0);
  int ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

  if (ret != 0) {
    log_warning(string_format("warning.hook_failed_exec",
                              std::string(hook_name), std::to_string(ret)));
  }
}

/** 从已解压的包目录读取 metadata.json，提取包名、版本、依赖等信息 */
void read_package_metadata(const fs::path &tmp_pkg_dir, std::string &name,
                           std::string &version, std::vector<std::string> &deps,
                           std::vector<std::string> &provides,
                           std::vector<std::string> &needed_so,
                           std::string &man) {
  fs::path meta_path = tmp_pkg_dir / constants::PKG_METADATA_FILE;
  json meta;
  {
    std::ifstream f(meta_path);
    if (!f.is_open())
      throw LpkgException(
          string_format("error.open_file_failed", meta_path.string()));
    f >> meta;
  }
  name = meta.at(std::string(constants::J_NAME)).get<std::string>();
  version = meta.at(std::string(constants::J_VERSION)).get<std::string>();
  deps = meta.value(std::string(constants::J_DEPS), std::vector<std::string>{});
  provides = meta.value(std::string(constants::J_PROVIDES),
                        std::vector<std::string>{});
  needed_so = meta.value(std::string(constants::J_NEEDED_SO),
                         std::vector<std::string>{});
  man = meta.value(std::string(constants::J_MAN), "");
}

/**
 * 扫描包内容目录，返回所有可注册路径的相对路径列表。
 *
 * 实现 pacman 风格：目录以斜杠结尾（如 "usr/bin/"），普通文件不带斜杠。
 * 目录支持多包共同持有，在移除时计数归零才删除。
 *
 * 包含：
 *  - 普通文件 → "usr/bin/bash"
 *  - 符号链接（含指向目录的，如 jvm/conf → /etc/java）→ "jvm/conf"
 *  - 普通目录 → "usr/bin/", "usr/"
 *
 * 不包含：
 *  - content/ 目录本身
 *
 * 这样做的原因：
 *   builder.cpp 清理 USR-Merge 符号链后，包内的目录就是包的真实内容。
 *   目录共享（如多包共享 /usr/bin/）通过引用计数管理，在最后持有者
 *   移除时删除目录。
 */
std::vector<std::string> scan_content_files(const fs::path &content_dir) {
  std::vector<std::string> entries;
  for (const auto &entry : fs::recursive_directory_iterator(content_dir)) {
    std::string rel = entry.path().lexically_relative(content_dir).string();
    if (entry.is_directory() && !entry.is_symlink()) {
      // 目录 → 末尾加 /，和普通文件区分
      entries.push_back(rel + "/");
    } else {
      // 文件或符号链接 → 原样保留
      entries.push_back(rel);
    }
  }
  return entries;
}

/** 解析依赖字符串列表为 DependencyInfo 结构体，支持复合约束 */
// 实现在 vercmp/dep_parser.cpp 中，此处仅为函数声明转发

/**
 * 递归解析包依赖关系，构建安装计划
 * 支持版本约束、虚拟包提供、循环依赖检测
 * 检查本地缓存、本地包文件和远程仓库中的包信息
 * depth 最大 64 层，超过则抛出异常防止栈溢出
 */
void resolve_package_dependencies(const std::string &pkg_name,
                                  const std::string &version_spec,
                                  bool is_explicit, InstallContext &ctx,
                                  std::set<std::string> &visited_stack,
                                  int depth) {
  constexpr int MAX_DEPTH = 64;
  if (depth > MAX_DEPTH)
    throw LpkgException(
        string_format("error.dependency_depth_exceeded", pkg_name, MAX_DEPTH));

  if (visited_stack.contains(pkg_name)) {
    log_warning(
        string_format("warning.circular_dependency", pkg_name, pkg_name));
    return;
  }
  if (ctx.plan.contains(pkg_name)) {
    if (is_explicit)
      ctx.plan.at(pkg_name).is_explicit = true;
    return;
  }

  const std::string installed_version =
      Cache::instance().get_installed_version(pkg_name);
  fs::path local_path;
  std::string latest_version, pkg_hash;
  std::vector<DependencyInfo> deps;
  std::vector<std::string> provides;
  std::vector<std::string> needed_so;

  // 优先检查本地包文件候选
  if (auto it = ctx.local_candidates.find(pkg_name);
      it != ctx.local_candidates.end()) {
    local_path = it->second;
    json meta = read_archive_metadata(local_path);
    latest_version =
        meta.at(std::string(constants::J_VERSION)).get<std::string>();

    deps = parse_dep_strings(
        meta.value(std::string(constants::J_DEPS), std::vector<std::string>{}));
    provides = meta.value(std::string(constants::J_PROVIDES),
                          std::vector<std::string>{});
    needed_so = meta.value(std::string(constants::J_NEEDED_SO),
                           std::vector<std::string>{});
  } else {
    // 从远程仓库查找
    auto pkg_info = (version_spec == constants::VER_LATEST)
                        ? ctx.repo.find_package(pkg_name)
                        : ctx.repo.find_package(pkg_name, version_spec);
    if (!pkg_info) {
      // 检查是否有其他包提供此虚拟包名
      if (auto prov = ctx.repo.find_provider(pkg_name)) {
        resolve_package_dependencies(prov->name, prov->version, is_explicit,
                                     ctx, visited_stack, depth + 1);
        return;
      }
      // 包在仓库中不存在 → 跳过，安装阶段会通过 ensure_dependencies_satisfied
      // 检查 ctx.plan 中的提供者和已安装包
      if (installed_version.empty())
        log_warning(string_format("warning.package_not_in_repo", pkg_name));
      return;
    }
    latest_version = pkg_info->version;
    pkg_hash = pkg_info->sha256;
    deps = pkg_info->dependencies;
    provides = pkg_info->provides;
    needed_so = pkg_info->needed_so;
  }

  if (latest_version.empty())
    latest_version = std::string(constants::VER_DEFAULT);

  // 检查是否需要安装/升级（非强制重装时跳过已安装的包）
  if (!ctx.force_reinstall || !is_explicit) {
    if (!is_explicit && !installed_version.empty() &&
        !version_compare(installed_version, latest_version))
      return;
    if (is_explicit && !installed_version.empty() &&
        !version_compare(installed_version, latest_version))
      return;
  }

  visited_stack.insert(pkg_name);
  InstallPlan p{.name = pkg_name,
                .actual_version = latest_version,
                .sha256 = pkg_hash,
                .is_explicit = is_explicit,
                .local_path = local_path,
                .dependencies = deps,
                .provides = provides,
                .needed_so = needed_so,
                .force_reinstall = (ctx.force_reinstall && is_explicit)};

  // 递归解析子依赖
  if (!Config::instance().no_deps_mode()) {
    for (const auto &dep : deps) {
      const std::string idv = Cache::instance().get_installed_version(dep.name);
      bool needs_resolution = idv.empty();
      if (!needs_resolution && !dep.constraints.empty() && idv != "virtual" &&
          !version_satisfies_all(idv, dep.constraints)) {
        if (!ctx.plan.contains(dep.name)) {
          log_info(string_format("info.adding_upgrade_to_plan", dep.name));
          needs_resolution = true;
        }
      }
      if (needs_resolution) {
        std::string req_ver = std::string(constants::VER_LATEST);
        if (!dep.constraints.empty()) {
          if (auto matching = ctx.repo.find_best_matching_version(
                  dep.name, dep.constraints))
            req_ver = matching->version;
        }
        resolve_package_dependencies(dep.name, req_ver, false, ctx,
                                     visited_stack, depth + 1);
      }

      // 验证候选版本满足依赖版本约束
      std::string cand_v =
          ctx.plan.contains(dep.name)
              ? ctx.plan[dep.name].actual_version
              : Cache::instance().get_installed_version(dep.name);
      if (!dep.constraints.empty() && !cand_v.empty() && cand_v != "virtual" &&
          !version_satisfies_all(cand_v, dep.constraints))
        throw LpkgException(string_format(
            "error.candidate_dep_version_mismatch", dep.name, cand_v,
            dep.constraints[0].op, dep.constraints[0].version));
    }
  }
  ctx.plan[pkg_name] = std::move(p);
  ctx.install_order.push_back(pkg_name);
  visited_stack.erase(pkg_name);
}

/**
 * 检查安装计划与已安装包的兼容性
 * 检测是否有已安装的包依赖于即将被升级/替换的包版本，且新版本不满足其版本约束
 * 返回被破坏的包名集合
 */
std::set<std::string>
check_plan_consistency(const std::map<std::string, InstallPlan> &plan) {
  std::set<std::string> broken;
  auto &cache = Cache::instance();
  std::lock_guard lock(cache.get_mutex());
  for (const auto &[pkg, ver] : cache.get_all_installed()) {
    if (plan.contains(pkg))
      continue;
    const fs::path dep_file = Config::instance().dep_dir() / pkg;
    if (!fs::exists(dep_file))
      continue;
    std::ifstream f(dep_file);
    std::string line;
    while (std::getline(f, line)) {
      auto parsed = parse_dep_strings({line});
      if (parsed.empty() || !plan.contains(parsed[0].name))
        continue;
      const auto &dep = parsed[0];
      const std::string &new_v = plan.at(dep.name).actual_version;
      if (!dep.constraints.empty() &&
          !version_satisfies_all(new_v, dep.constraints)) {
        log_error(string_format("error.conflict_breaks_existing", dep.name,
                                new_v, pkg, dep.constraints[0].op,
                                dep.constraints[0].version));
        broken.insert(pkg);
      }
    }
  }
  return broken;
}

/**
 * 检查计划升级是否会破坏已安装包的 needed_so
 * 当计划升级/替换某个包时，遍历已安装包的 needed_so 文件，
 * 若某个 SONAME 由被升级包提供但新版不再提供，则将该已安装包
 * 标记为 broken。
 */
std::set<std::string>
check_needed_so_consistency(const std::map<std::string, InstallPlan> &plan) {
  std::set<std::string> broken;
  auto &cache = Cache::instance();
  auto &config = Config::instance();
  const fs::path nso_dir = config.needed_so_dir();

  for (const auto &[pkg, ver] : cache.get_all_installed()) {
    if (plan.contains(pkg))
      continue;

    const fs::path nso_file = nso_dir / pkg;
    if (!fs::exists(nso_file))
      continue;

    std::ifstream f(nso_file);
    std::string soname;
    while (std::getline(f, soname)) {
      if (soname.empty())
        continue;

      // 查找此 SONAME 的已安装提供者
      auto providers = cache.get_providers(soname);
      for (const auto &prov_pkg : providers) {
        // 若提供者在 plan 中（正在被升级），验证新版仍提供此 SONAME
        if (!plan.contains(prov_pkg))
          continue;

        const auto &plan_entry = plan.at(prov_pkg);
        bool still_provides = false;
        for (const auto &prov : plan_entry.provides) {
          if (prov == soname) {
            still_provides = true;
            break;
          }
        }
        if (!still_provides) {
          log_warning(string_format("warning.needed_so_dropped_in_upgrade",
                                    prov_pkg, plan_entry.actual_version, soname,
                                    pkg));
          broken.insert(pkg);
        }
      }
    }
  }
  return broken;
}

/**
 * 获取所有必需包的集合（被明确标记为 held 的包及其传递依赖）
 * 用于 autoremove 判断哪些包可以安全移除
 */
std::unordered_set<std::string> get_all_required_packages() {
  auto &cache = Cache::instance();
  std::unordered_set<std::string> req;
  {
    std::lock_guard lock(cache.get_mutex());
    req = cache.get_all_held();
  }
  std::vector q(req.begin(), req.end());
  size_t head = 0;
  while (head < q.size()) {
    const std::string curr = q[head++];
    const fs::path p = Config::instance().dep_dir() / curr;
    if (!fs::exists(p))
      continue;
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) {
      std::string d_name = line;
      if (const auto pos = line.find_first_of(" \t<>=");
          pos != std::string::npos)
        d_name = line.substr(0, pos);
      auto check_and_add = [&](const std::string &name) {
        if (cache.is_installed(name) && !req.contains(name)) {
          req.insert(name);
          q.push_back(name);
        }
      };
      if (cache.is_installed(d_name))
        check_and_add(d_name);
      else
        for (const auto &prov : cache.get_providers(d_name))
          check_and_add(prov);
    }
  }
  return req;
}

/**
 * 向前 needed_so 完整性校验。
 *
 * 对计划中的每个包，检查其每个 SONAME 的提供链：
 *   plan（版本精准）→ 已安装缓存 → repo（版本精准）
 *
 * 版本精准的含义：
 *   - plan 中同时升级的包以新版本计算 provides
 *   - 缓存中的包以当前安装版本计算
 *   - repo 中只取实际提供该 SONAME 的版本（find_provider 返回的版本必须提供该
 * SONAME）
 */
void check_forward_soname_integrity(
    const std::map<std::string, InstallPlan> &plan, Repository &repo) {
  bool all_ok = true;
  std::string errors;
  for (const auto &[pname, pplan] : plan) {
    if (pplan.needed_so.empty())
      continue;
    for (const auto &soname : pplan.needed_so) {
      bool provided = false;

      // 1) plan — 同批次升级的包以新版本 provides 为准
      for (const auto &[pn2, pp2] : plan) {
        for (const auto &prov : pp2.provides) {
          if (prov == soname) {
            provided = true;
            break;
          }
        }
        if (provided)
          break;
      }

      // 2) 已安装缓存
      if (!provided) {
        auto providers = Cache::instance().get_providers(soname);
        for (const auto &p : providers) {
          if (Cache::instance().is_installed(p) && !plan.contains(p)) {
            provided = true;
            break;
          }
        }
      }

      // 3) repo — 必须确认返回的版本确实提供此 SONAME
      if (!provided) {
        if (auto prov_pkg = repo.find_provider(soname)) {
          for (const auto &prov : prov_pkg->provides) {
            if (prov == soname) {
              provided = true;
              break;
            }
          }
        }
      }

      if (!provided) {
        all_ok = false;
        errors +=
            "  " + string_format("error.unresolved_soname", soname) + "\n";
      }
    }
  }
  if (!all_ok) {
    log_error(errors);
    throw LpkgException(errors);
  }
}

} // namespace detail
