#include "config.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "utils.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

/**
 * 获取 Config 单例实例
 */
Config &Config::instance() {
  static Config cfg;
  return cfg;
}

/**
 * 构造函数：只初始化基础路径成员，派生路径通过 rebase_paths() 统一计算
 *
 * 设计说明：
 *   hooks_dir_ / dep_dir_ / pkgs_file_ 等"派生路径"不应在本阶段硬编码赋值，
 *   而是全部交由 rebase_paths() 根据基础路径统一推导，避免两处逻辑不一致。
 *   当需要新增派生路径时，只需在 rebase_paths() 中添加，无需修改构造函数。
 */
Config::Config()
    : root_dir_("/"), config_dir_(fs::path{LPKG_CONF_DIR}),
      state_dir_("/var/lib/lpkg"), l10n_dir_(fs::path{LPKG_L10N_DIR}),
      docs_dir_(fs::path{LPKG_DOCS_DIR}), lock_dir_(fs::path{LPKG_LOCK_DIR}) {
  rebase_paths(); // 统一计算所有派生路径（hooks、dep、pkgs 等）
}

/**
 * 重新计算所有路径，将相对路径改为相对于 root_dir_ 的绝对路径
 * 在设置了新的根目录后调用此方法
 */
void Config::rebase_paths() {
  auto rebase = [&](const std::string &default_path) -> fs::path {
    fs::path p(default_path);
    if (p.is_absolute()) {
      return root_dir_ / p.relative_path();
    }
    return root_dir_ / p;
  };

  config_dir_ = rebase(LPKG_CONF_DIR);
  state_dir_ = rebase("/var/lib/lpkg");
  l10n_dir_ = rebase(LPKG_L10N_DIR);
  docs_dir_ = rebase(LPKG_DOCS_DIR);
  lock_dir_ = rebase(LPKG_LOCK_DIR);

  hooks_dir_ = config_dir_ / "hooks/";
  dep_dir_ = state_dir_ / "deps/";
  needed_so_dir_ = state_dir_ / "needed_so/";
  pkgs_file_ = state_dir_ / "pkgs";
  holdpkgs_file_ = state_dir_ / "holdpkgs";
  essential_file_ = config_dir_ / "essential";
  mirror_conf_ = config_dir_ / "mirror.conf";
  triggers_conf_ = config_dir_ / "triggers.conf";
  files_db_ = state_dir_ / "files.db";
  provides_db_ = state_dir_ / "provides.db";
  lock_file_ = lock_dir_ / "db.lck";
}

/**
 * 设置软件包安装根目录，并重新计算所有派生路径
 */
void Config::set_root_path(const std::string &root_path) {
  std::lock_guard<std::mutex> lock(config_mutex_);
  root_dir_ = fs::path(root_path).lexically_normal();
  if (root_dir_.empty())
    root_dir_ = "/";
  rebase_paths();
}

/**
 * 获取当前进程的临时目录，路径为 /tmp/lpkg_<PID>
 */
fs::path Config::get_tmp_dir() {
  static const fs::path tmp_dir = []() {
    // 使用 PID + 随机后缀降低 PID 复用冲突概率。
    // cleanup_tmp_dirs 通过 lpkg_ 前缀识别并 kill(pid,0) 检查存活性，
    // 随机后缀不会影响清理逻辑（stoi 在首个非数字处停止）。
    std::random_device rd;
    return fs::path("/tmp") / ("lpkg_" + std::to_string(getpid()) + "_" +
                               std::to_string(rd() % 10000));
  }();
  return tmp_dir;
}

void Config::set_non_interactive_mode(NonInteractiveMode m) noexcept {
  std::lock_guard<std::mutex> lock(config_mutex_);
  non_interactive_mode_ = m;
}

void Config::set_force_overwrite_mode(bool v) noexcept {
  std::lock_guard<std::mutex> lock(config_mutex_);
  force_overwrite_mode_ = v;
}

void Config::set_no_hooks_mode(bool v) noexcept {
  std::lock_guard<std::mutex> lock(config_mutex_);
  no_hooks_mode_ = v;
}

void Config::set_no_deps_mode(bool v) noexcept {
  std::lock_guard<std::mutex> lock(config_mutex_);
  no_deps_mode_ = v;
}

void Config::set_testing_mode(bool v) noexcept {
  std::lock_guard<std::mutex> lock(config_mutex_);
  testing_mode_ = v;
}

/**
 * 初始化配置所需的文件系统结构
 * 创建所有必要的目录和空文件（如包数据库、锁定文件等）
 */
void Config::init_filesystem() {
  ensure_dir_exists(config_dir_);
  ensure_dir_exists(state_dir_);
  ensure_dir_exists(dep_dir_);
  ensure_dir_exists(needed_so_dir_);
  ensure_dir_exists(l10n_dir_);
  ensure_dir_exists(docs_dir_);
  ensure_dir_exists(hooks_dir_);
  ensure_dir_exists(lock_dir_);
  ensure_file_exists(pkgs_file_);
  ensure_file_exists(holdpkgs_file_);
  ensure_file_exists(essential_file_);
  ensure_file_exists(files_db_);
  ensure_file_exists(provides_db_);
}

/**
 * 覆盖系统的架构检测结果，强制使用指定架构
 */
void Config::set_architecture(const std::string &arch) {
  std::lock_guard<std::mutex> lock(config_mutex_);
  architecture_override_ = arch;
}

/**
 * 获取当前系统的 CPU 架构
 * 如果未设置架构覆盖，通过 uname 系统调用获取
 */
std::string Config::get_architecture() {
  std::lock_guard<std::mutex> lock(config_mutex_);
  if (!architecture_override_.empty()) {
    return architecture_override_;
  }

  struct utsname buf;
  if (uname(&buf) != 0) {
    throw LpkgException(get_string("error.get_arch_failed"));
  }
  return std::string(buf.machine);
}

/**
 * 从镜像配置文件中读取镜像源 URL
 * 确保 URL 末尾包含斜杠
 */
std::string Config::get_mirror_url() {
  std::ifstream mirror_file(mirror_conf_);
  if (!mirror_file.is_open()) {
    throw LpkgException(
        string_format("error.open_file_failed", mirror_conf_.string()));
  }
  std::string mirror_url;
  if (!std::getline(mirror_file, mirror_url) || mirror_url.empty()) {
    throw LpkgException(get_string("error.invalid_mirror_config"));
  }
  if (mirror_url.back() != '/') {
    mirror_url += '/';
  }
  return mirror_url;
}
