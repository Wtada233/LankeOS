#include "config.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "utils.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

/**
 * 获取 Config 单例实例
 */
Config& Config::instance() {
    static Config cfg;
    return cfg;
}

/**
 * 构造函数：使用编译期宏定义初始化所有路径
 * 包括配置目录、状态目录、本地化目录等系统路径
 */
Config::Config()
    : root_dir_("/")
    , config_dir_(fs::path{LPKG_CONF_DIR})
    , state_dir_("/var/lib/lpkg")
    , l10n_dir_(fs::path{LPKG_L10N_DIR})
    , docs_dir_(fs::path{LPKG_DOCS_DIR})
    , lock_dir_(fs::path{LPKG_LOCK_DIR})
    , hooks_dir_(fs::path{LPKG_CONF_DIR} / "hooks/")

    , dep_dir_(fs::path{"/var/lib/lpkg"} / "deps/")
    , pkgs_file_(fs::path{"/var/lib/lpkg"} / "pkgs")
    , holdpkgs_file_(fs::path{"/var/lib/lpkg"} / "holdpkgs")
    , essential_file_(fs::path{LPKG_CONF_DIR} / "essential")
    , mirror_conf_(fs::path{LPKG_CONF_DIR} / "mirror.conf")
    , triggers_conf_(fs::path{LPKG_CONF_DIR} / "triggers.conf")
    , files_db_(fs::path{"/var/lib/lpkg"} / "files.db")
    , provides_db_(fs::path{"/var/lib/lpkg"} / "provides.db")
    , lock_file_(fs::path{LPKG_LOCK_DIR} / "db.lck")
{}

/**
 * 重新计算所有路径，将相对路径改为相对于 root_dir_ 的绝对路径
 * 在设置了新的根目录后调用此方法
 */
void Config::rebase_paths() {
    auto rebase = [&](const std::string& default_path) -> fs::path {
        fs::path p(default_path);
        if (p.is_absolute()) {
            return root_dir_ / p.relative_path();
        }
        return root_dir_ / p;
    };

    config_dir_ = rebase(LPKG_CONF_DIR);
    state_dir_  = rebase("/var/lib/lpkg");
    l10n_dir_   = rebase(LPKG_L10N_DIR);
    docs_dir_   = rebase(LPKG_DOCS_DIR);
    lock_dir_   = rebase(LPKG_LOCK_DIR);

    hooks_dir_        = config_dir_ / "hooks/";
    dep_dir_          = state_dir_ / "deps/";
    pkgs_file_        = state_dir_ / "pkgs";
    holdpkgs_file_    = state_dir_ / "holdpkgs";
    essential_file_   = config_dir_ / "essential";
    mirror_conf_      = config_dir_ / "mirror.conf";
    triggers_conf_    = config_dir_ / "triggers.conf";
    files_db_         = state_dir_ / "files.db";
    provides_db_      = state_dir_ / "provides.db";
    lock_file_        = lock_dir_ / "db.lck";
}

/**
 * 设置软件包安装根目录，并重新计算所有派生路径
 */
void Config::set_root_path(const std::string& root_path) {
    root_dir_ = fs::path(root_path).lexically_normal();
    if (root_dir_.empty()) root_dir_ = "/";
    rebase_paths();
}

/**
 * 获取当前进程的临时目录，路径为 /tmp/lpkg_<PID>
 */
fs::path Config::get_tmp_dir() {
    static const fs::path tmp_dir = fs::path("/tmp") / ("lpkg_" + std::to_string(getpid()));
    return tmp_dir;
}

/**
 * 初始化配置所需的文件系统结构
 * 创建所有必要的目录和空文件（如包数据库、锁定文件等）
 */
void Config::init_filesystem() {
    ensure_dir_exists(config_dir_);
    ensure_dir_exists(state_dir_);
    ensure_dir_exists(dep_dir_);
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
void Config::set_architecture(const std::string& arch) {
    architecture_override_ = arch;
}

/**
 * 获取当前系统的 CPU 架构
 * 如果未设置架构覆盖，通过 uname 系统调用获取
 */
std::string Config::get_architecture() {
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
        throw LpkgException(string_format("error.open_file_failed", mirror_conf_.string()));
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
