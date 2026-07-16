#include "builder.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "builder_config.hpp"
#include "builder_executor.hpp"
#include "i18n/localization.hpp"
#include "lib_utils.hpp"
#include "packer.hpp"

#include <array>
#include <ctime>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

// =====================================================================
// 内部辅助函数
// =====================================================================

namespace {

/**
 * 设置构建目录结构
 * 创建工作目录、staging 目录和 hooks 目录，初始化 UsrMerge 符号链接
 * 创建默认的 post-install 脚本占位符
 */
fs::path setup_build_directories(const fs::path &build_dir) {
  fs::path work_root = build_dir / constants::DIR_WORK;
  fs::path staging_root = build_dir / constants::DIR_CONTENT;
  fs::path staging_hooks = build_dir / constants::DIR_HOOKS;

  fs::remove_all(work_root);
  fs::remove_all(staging_root);
  fs::remove_all(staging_hooks);

  ensure_dir_exists(work_root);
  ensure_dir_exists(staging_root);

  // 路径规范化（UsrMerge 合并）
  log_info(get_string("info.path_normalization"));
  for (const auto &d : {constants::BIN, constants::LIB, constants::INCLUDE,
                        constants::SHARE_MAN, constants::LOCAL_BIN}) {
    ensure_dir_exists(staging_root / constants::USR / d);
  }
  fs::create_directory_symlink(constants::USR_BIN,
                               staging_root / constants::BIN);
  fs::create_directory_symlink(constants::USR_BIN,
                               staging_root / constants::SBIN);
  fs::create_directory_symlink(constants::USR_LIB,
                               staging_root / constants::LIB);
  fs::create_directory_symlink(constants::USR_LIB,
                               staging_root / constants::LIB64);
  fs::create_directory_symlink(constants::BIN,
                               staging_root / constants::USR_SBIN);
  fs::create_directory_symlink(constants::LIB,
                               staging_root / constants::USR_LIB64);

  ensure_dir_exists(staging_hooks);
  {
    std::ofstream h(staging_hooks / constants::POSTINST_SH);
    h << "#!/bin/sh" << constants::NL;
  }
  fs::permissions(staging_hooks / constants::POSTINST_SH,
                  fs::perms::owner_exec | fs::perms::group_exec |
                      fs::perms::others_exec,
                  fs::perm_options::add);

  return staging_hooks;
}

/** 构建模板变量映射表，将占位符替换为实际的路径和配置值 */
std::map<std::string, std::string>
build_variable_map(const BuildConfig &cfg, const fs::path &work_root,
                   const fs::path &actual_work_dir,
                   const fs::path &staging_root, const fs::path &staging_hooks,
                   const std::string &effective_version) {
  return {
      {"{PKG_NAME}", cfg.name},
      {"{PKG_VER}", effective_version},
      {"{WORK_DIR}", fs::absolute(work_root).string()},
      {"{SRC_DIR}", fs::absolute(actual_work_dir).string()},
      {"{STAGING_ROOT}", fs::absolute(staging_root).string()},
      {"{STAGING_HOOKS}", fs::absolute(staging_hooks).string()},
      {"{PREFIX}", "/usr"},
      {"{BINDIR}", "/usr/bin"},
      {"{SBINDIR}", "/usr/bin"},
      {"{LIBDIR}", "/usr/lib"},
      {"{INCLUDEDIR}", "/usr/include"},
      {"{MANDIR}", "/usr/share/man"},
      {"{LOCALSTATEDIR}", "/var"},
      {"{DATADIR}", "/usr/share"},
      {"{ORIG_PKG_VER}", cfg.version},
      {"{NO_STRIP}", cfg.no_strip ? "1" : "0"},
  };
}

/**
 * 构建后处理：清理 libtool 文件、对 ELF 二进制文件进行 strip、
 * 生成 SONAME 符号链接
 */
void finalize_staging(const fs::path &staging_root, bool no_strip) {
  log_info(get_string("info.finalizing_staging"));

  log_info(get_string("info.cleaning_libtool_files"));
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(staging_root, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec)
      break;
    if (it->is_regular_file() && it->path().extension() == constants::EXT_LA) {
      fs::remove(it->path(), ec);
    }
  }

  if (!no_strip) {
    log_info(get_string("info.stripping_binaries"));
    fs::path usr_root = staging_root / constants::USR;
    if (fs::exists(usr_root) && fs::is_directory(usr_root)) {
      for (const auto &entry : fs::recursive_directory_iterator(usr_root)) {
        if (!entry.is_regular_file() || entry.is_symlink())
          continue;

        // 仅对 ELF 文件进行 strip
        bool is_elf = false;
        std::ifstream f(entry.path(), std::ios::binary);
        if (f) {
          std::array<char, 4> magic{};
          f.read(magic.data(), magic.size());
          if (f.gcount() == 4 && magic[0] == 0x7f && magic[1] == 'E' &&
              magic[2] == 'L' && magic[3] == 'F') {
            is_elf = true;
          }
        }
        if (!is_elf)
          continue;

        strip_binary(entry.path());
      }
    }
  }

  fs::remove(staging_root / "usr/share/info/dir", ec);

  if (fs::exists(staging_root / constants::USR / constants::LIB)) {
    log_info(get_string("info.generating_soname_links"));
    apply_soname_links(staging_root / constants::USR / constants::LIB);
  }
}

/** 清理构建过程中产生的临时文件和工作目录 */
void cleanup_build([[maybe_unused]] const fs::path &build_dir,
                   const fs::path &work_root, const fs::path &staging_root,
                   const fs::path &staging_hooks,
                   const std::vector<fs::path> &downloaded_files) {
  log_info(get_string("info.cleaning_up_build"));
  fs::remove_all(work_root);
  fs::remove_all(staging_root);
  fs::remove_all(staging_hooks);
  for (const auto &f : downloaded_files) {
    fs::remove(f);
  }
}

} // anonymous namespace

// =====================================================================
// 公开 API
// =====================================================================

/**
 * 执行完整的包构建流程：
 * 1. 解析 LankeBUILD.json 配置
 * 2. 准备构建目录结构和 UsrMerge 符号链接
 * 3. 下载并解压源码
 * 4. 检测源码树结构
 * 5. 处理构建脚本并执行各构建阶段（prepare/build/package）
 * 6. 后处理：strip、清理 libtool 文件、生成 SONAME 链接
 * 7. 打包为 .lpkg 文件
 * 8. 清理临时文件
 */
void run_build(const fs::path &build_dir) {
  // 1. 解析元数据
  fs::path json_path = build_dir / constants::LANK_BUILD_JSON;
  fs::path script_path = build_dir / constants::LANK_BUILD_SCRIPT;

  if (!fs::exists(json_path))
    throw LpkgException(
        string_format("error.missing_lankebuild_json", build_dir.string()));
  if (!fs::exists(script_path))
    throw LpkgException(
        string_format("error.missing_lankebuild", build_dir.string()));

  log_info(get_string("info.parsing_lankebuild"));
  auto cfg = parse_build_config(json_path);

  // 计算有效版本号：如有 release 修订号，附加到版本号后
  std::string effective_version = cfg.version;
  if (cfg.release > 0) {
    effective_version = cfg.version + "+" + std::to_string(cfg.release);
  }
  log_info(string_format("info.building_package", cfg.name, effective_version));

  // 如果 LankeBUILD.json 中未指定 man 页面内容，则自动生成
  if (cfg.man_content.empty()) {
    std::time_t t = std::time(nullptr);
    std::tm *tm = std::localtime(&t);
    char date_buf[32];
    std::strftime(date_buf, sizeof(date_buf), "%Y%m%d", tm);
    cfg.man_content =
        get_string("man.info_name") + ": " + cfg.name + "\n" +
        get_string("man.info_version") + ": " + effective_version + "\n" +
        get_string("man.info_build_date") + ": " + date_buf + "\n";
    log_info(string_format("info.auto_generated_man", cfg.name));
  }

  // 2. 准备目录
  fs::path work_root = build_dir / constants::DIR_WORK;
  fs::path staging_root = build_dir / constants::DIR_CONTENT;
  auto staging_hooks = setup_build_directories(build_dir);

  // 3. 下载并解压源码
  auto downloaded = download_and_prepare_sources(cfg.sources, cfg.work_sources,
                                                 build_dir, work_root);

  // 4. 检测源码树
  auto actual_work_dir = detect_source_tree(work_root);

  // 5. 处理脚本并执行构建阶段
  auto vars = build_variable_map(cfg, work_root, actual_work_dir, staging_root,
                                 staging_hooks, effective_version);
  fs::path processed_script = build_dir / constants::LANK_BUILD_PROCESSED;
  {
    std::string content = process_build_script(script_path, vars);
    std::ofstream f(processed_script);
    f << content;
  }

  try {
    execute_build_phase("lankebuild_prepare", actual_work_dir,
                        processed_script);
    execute_build_phase("lankebuild_build", actual_work_dir, processed_script);
    execute_build_phase("lankebuild_package", actual_work_dir,
                        processed_script);
  } catch (...) {
    fs::remove(processed_script);
    throw;
  }
  fs::remove(processed_script);

  // 6. 后处理
  finalize_staging(staging_root, cfg.no_strip);

  // 6.5. 移除 USR-Merge
  // 兼容符号链接（它们是构建阶段的辅助设施，不应打包入包）。
  //       Builder 在 setup_build_directories() 中创建这些链接使构建脚本能够
  //       向 bin/、lib/ 等路径安装文件（实际写入 usr/bin/、usr/lib/）。
  //       现在打包前清理它们，避免每个包都声称"拥有"这些系统级符号链接，
  //       从而导致卸载时因"文件被其他包共享"而拒绝移除。
  for (const auto &link :
       {staging_root / constants::BIN, staging_root / constants::SBIN,
        staging_root / constants::LIB, staging_root / constants::LIB64,
        staging_root / constants::USR_SBIN,
        staging_root / constants::USR_LIB64}) {
    std::error_code ec;
    if (fs::is_symlink(link, ec) || (!ec && fs::exists(link))) {
      fs::remove(link, ec);
    }
  }

  // 7. 打包
  log_info(get_string("info.packing_built_pkg"));
  std::string output_filename =
      cfg.name + "-" + effective_version + std::string(constants::EXT_LPKG);
  pack_package(output_filename, build_dir.string(), cfg.name, effective_version,
               cfg.deps, cfg.provides, cfg.man_content, cfg.needed_so);
  log_info(string_format("info.build_success", output_filename));

  // 8. 清理
  cleanup_build(build_dir, work_root, staging_root, staging_hooks, downloaded);
}
