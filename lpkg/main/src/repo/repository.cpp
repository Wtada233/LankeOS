#include "repository.hpp"
#include "archive/downloader.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "config/config.hpp"
#include "i18n/localization.hpp"
#include "vercmp/dep_parser.hpp"
#include "vercmp/version.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <ranges>
#include <sstream>
#include <string_view>

/**
 * 加载仓库索引文件
 * 支持远程（http/https）和本地（file://）两种方式。
 * 远程索引会被下载到临时目录后解析。
 * 所有异常会被捕获并输出警告，不会影响程序运行。
 */
void Repository::load_index() {
  packages_.clear();
  providers_.clear();

  // 读取镜像地址（可能为本地路径或 http URL）
  std::string mirror;
  try {
    mirror = Config::instance().get_mirror_url();
  } catch (const std::exception &e) {
    log_warning(string_format("warning.repo_mirror_config", e.what()));
    return;
  }
  std::string arch = Config::instance().get_architecture();

  bool is_local =
      mirror.find(constants::PROTOCOL_FILE) == 0 || mirror.find("/") == 0;
  std::filesystem::path index_path;

  try {
    if (is_local) {
      std::string path_str = (mirror.find(constants::PROTOCOL_FILE) == 0)
                                 ? mirror.substr(7)
                                 : mirror;
      index_path =
          std::filesystem::path(path_str) / arch / constants::REPO_INDEX_FILE;
    } else {
      std::string url =
          mirror + arch + "/" + std::string(constants::REPO_INDEX_FILE);
      index_path = Config::get_tmp_dir() / constants::REPO_INDEX_TMP;
      download_file(url, index_path, false);
    }
    if (!std::filesystem::exists(index_path)) {
      log_warning(
          string_format("warning.repo_index_missing", index_path.string()));
      return;
    }
  } catch (const std::exception &e) {
    log_warning(string_format("warning.repo_index_download", e.what()));
    return;
  }

  // 逐个解析索引行，格式: 包名|版本:哈希:依赖;版本2:哈希2:依赖2|提供者
  std::ifstream file(index_path);
  std::string line;

  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::string_view sv = line;
    if (!sv.empty() && sv.back() == '\r')
      sv.remove_suffix(1);

    // 格式: 包名|版本:哈希:依赖:提供者:needed_so;版本2:...|
    auto parts = split_string_view(sv, constants::PIPE_CHAR);
    if (parts.size() < 2)
      continue;

    std::string pkg_name(parts[0]);
    std::string_view version_blocks_sv = parts[1];

    // 旧格式兼容（已移除）：provides/needed_so 在版本块内

    // 一个包可能对应多个版本，用 ';' 分隔
    for (auto version_info_sv :
         split_string_view(version_blocks_sv, constants::SEMICOLON_CHAR)) {
      if (version_info_sv.empty())
        continue;

      auto vh_parts = split_string_view(version_info_sv, constants::COLON_CHAR);
      if (vh_parts.empty())
        continue;

      std::string version(vh_parts[0]);
      std::string hash = (vh_parts.size() > 1) ? std::string(vh_parts[1]) : "";
      std::string_view deps_sv = (vh_parts.size() > 2) ? vh_parts[2] : "";

      // provides/needed_so 在版本块内（第 4、5 字段），每个版本独立
      std::string_view ver_prov_sv =
          (vh_parts.size() > 3) ? vh_parts[3] : std::string_view{};
      std::string_view ver_needed_so_sv =
          (vh_parts.size() > 4) ? vh_parts[4] : std::string_view{};

      // 解析依赖字符串（复用 vercmp/dep_parser 的统一实现）
      std::vector<DependencyInfo> deps;
      if (!deps_sv.empty()) {
        std::vector<std::string> dep_strs;
        for (auto ds : split_string_view(deps_sv, constants::COMMA_CHAR))
          dep_strs.push_back(std::string(ds));
        deps = detail::parse_dep_strings(dep_strs);
      }

      // 记录提供者（provides）— 版本级优先，回退到包级
      if (!ver_prov_sv.empty()) {
        for (auto prov :
             split_string_view(ver_prov_sv, constants::COMMA_CHAR)) {
          auto &pv = providers_[std::string(prov)];
          if (pv.empty() || pv.back() != pkg_name) {
            pv.push_back(pkg_name);
          }
        }
      }

      PackageInfo pkg;
      pkg.name = pkg_name;
      pkg.version = version;
      pkg.sha256 = hash;
      pkg.dependencies = std::move(deps);
      if (!ver_prov_sv.empty()) {
        for (auto prov :
             split_string_view(ver_prov_sv, constants::COMMA_CHAR)) {
          pkg.provides.push_back(std::string(prov));
        }
      }
      if (!ver_needed_so_sv.empty()) {
        for (auto needed :
             split_string_view(ver_needed_so_sv, constants::COMMA_CHAR)) {
          pkg.needed_so.push_back(std::string(needed));
        }
      }
      packages_[pkg.name].push_back(std::move(pkg));
    }
  }

  // 每个包的版本列表按版本号升序排列（最后一个就是最新版）
  for (auto &versions : packages_ | std::views::values) {
    std::ranges::sort(versions, [](const PackageInfo &a, const PackageInfo &b) {
      return version_compare(a.version, b.version);
    });
  }
}

/** 根据 capability 查找提供该能力的第一个包 */
std::optional<PackageInfo>
Repository::find_provider(const std::string &capability) {
  auto it = providers_.find(capability);
  if (it == providers_.end() || it->second.empty())
    return std::nullopt;
  return find_package(it->second[0]);
}

/** 更新（或新增）某包某版本的元数据 */
void Repository::update_package_info(
    const std::string &name, const std::string &version,
    const std::vector<DependencyInfo> &deps,
    const std::vector<std::string> &provides,
    const std::vector<std::string> &needed_so) {
  auto &versions = packages_[name];
  bool found = false;
  for (auto &pkg : versions) {
    if (pkg.version == version) {
      pkg.dependencies = deps;
      pkg.provides = provides;
      pkg.needed_so = needed_so;
      found = true;
      break;
    }
  }
  if (!found) {
    PackageInfo pkg;
    pkg.name = name;
    pkg.version = version;
    pkg.dependencies = deps;
    pkg.provides = provides;
    pkg.needed_so = needed_so;
    versions.push_back(std::move(pkg));
    std::ranges::sort(versions, [](const PackageInfo &a, const PackageInfo &b) {
      return version_compare(a.version, b.version);
    });
  }

  // 增量更新 providers 映射：只移除旧版本中不再提供的条目，添加新版本
  // 遍历所有版本的 provides，删除该包名下所有旧记录后重新插入
  for (auto it = providers_.begin(); it != providers_.end();) {
    auto &vec = it->second;
    for (auto vit = vec.begin(); vit != vec.end();) {
      if (*vit == name) {
        vit = vec.erase(vit);
      } else {
        ++vit;
      }
    }
    if (vec.empty()) {
      it = providers_.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto &[pkg_name, pkg_versions] : packages_) {
    for (const auto &pkg : pkg_versions) {
      for (const auto &prov : pkg.provides) {
        auto &pv = providers_[prov];
        if (pv.empty() || pv.back() != pkg_name) {
          pv.push_back(pkg_name);
        }
      }
    }
  }
}

/** 按包名查找最新版本 */
std::optional<PackageInfo> Repository::find_package(const std::string &name) {
  auto it = packages_.find(name);
  if (it == packages_.end() || it->second.empty())
    return std::nullopt;
  return it->second.back();
}

/** 按包名+版本精确查找 */
std::optional<PackageInfo>
Repository::find_package(const std::string &name, const std::string &version) {
  auto it = packages_.find(name);
  if (it == packages_.end())
    return std::nullopt;
  for (const auto &pkg : it->second) {
    if (pkg.version == version)
      return pkg;
  }
  return std::nullopt;
}

/** 按复合版本约束查找最匹配的版本（从高到低遍历，返回第一个满足全部约束的） */
std::optional<PackageInfo> Repository::find_best_matching_version(
    const std::string &name, const std::vector<Constraint> &constraints) {
  auto it = packages_.find(name);
  if (it == packages_.end() || it->second.empty())
    return std::nullopt;
  for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
    if (version_satisfies_all(rit->version, constraints)) {
      return *rit;
    }
  }
  return std::nullopt;
}

/** 按单一版本约束查找最匹配的版本（从高到低遍历，返回第一个满足条件的） */
std::optional<PackageInfo>
Repository::find_best_matching_version(const std::string &name,
                                       const std::string &op,
                                       const std::string &version_req) {
  auto it = packages_.find(name);
  if (it == packages_.end() || it->second.empty())
    return std::nullopt;
  for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
    if (version_satisfies(rit->version, op, version_req)) {
      return *rit;
    }
  }
  return std::nullopt;
}
