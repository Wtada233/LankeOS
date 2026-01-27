#include "config.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "utils.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

fs::path ROOT_DIR = "/";
fs::path CONFIG_DIR = LPKG_CONF_DIR;
fs::path STATE_DIR = "/var/lib/lpkg";
fs::path L10N_DIR = LPKG_L10N_DIR;
fs::path DOCS_DIR = LPKG_DOCS_DIR;
fs::path LOCK_DIR = LPKG_LOCK_DIR;
fs::path HOOKS_DIR = fs::path(LPKG_CONF_DIR) / "hooks/";

// Derived paths
fs::path DEP_DIR = fs::path("/var/lib/lpkg") / "deps/";
fs::path FILES_DIR = fs::path("/var/lib/lpkg") / "files/";
fs::path PKGS_FILE = fs::path("/var/lib/lpkg") / "pkgs";
fs::path HOLDPKGS_FILE = fs::path("/var/lib/lpkg") / "holdpkgs";
fs::path ESSENTIAL_FILE = fs::path(LPKG_CONF_DIR) / "essential";
fs::path MIRROR_CONF = fs::path(LPKG_CONF_DIR) / "mirror.conf";
fs::path TRIGGERS_CONF = fs::path(LPKG_CONF_DIR) / "triggers.conf";
fs::path FILES_DB = fs::path("/var/lib/lpkg") / "files.db";
fs::path PROVIDES_DB = fs::path("/var/lib/lpkg") / "provides.db";
fs::path LOCK_FILE = fs::path(LPKG_LOCK_DIR) / "db.lck";

void set_root_path(const std::string& root_path) {
    ROOT_DIR = fs::path(root_path).lexically_normal();
    if (ROOT_DIR.empty()) ROOT_DIR = "/";

    auto rebase = [&](const std::string& default_path) {
        fs::path p(default_path);
        if (p.is_absolute()) {
            return ROOT_DIR / p.relative_path();
        }
        return ROOT_DIR / p;
    };

    CONFIG_DIR = rebase(LPKG_CONF_DIR);
    STATE_DIR = rebase("/var/lib/lpkg");
    L10N_DIR = rebase(LPKG_L10N_DIR);
    DOCS_DIR = rebase(LPKG_DOCS_DIR);
    LOCK_DIR = rebase(LPKG_LOCK_DIR);
    
    HOOKS_DIR = CONFIG_DIR / "hooks/";
    DEP_DIR = STATE_DIR / "deps/";
    FILES_DIR = STATE_DIR / "files/";
    PKGS_FILE = STATE_DIR / "pkgs";
    HOLDPKGS_FILE = STATE_DIR / "holdpkgs";
    ESSENTIAL_FILE = CONFIG_DIR / "essential";
    MIRROR_CONF = CONFIG_DIR / "mirror.conf";
    TRIGGERS_CONF = CONFIG_DIR / "triggers.conf";
    FILES_DB = STATE_DIR / "files.db";
    PROVIDES_DB = STATE_DIR / "provides.db";
    LOCK_FILE = LOCK_DIR / "db.lck";
}

fs::path get_tmp_dir() {
    static const fs::path tmp_dir = fs::path("/tmp") / ("lpkg_" + std::to_string(getpid()));
    return tmp_dir;
}

void init_filesystem() {
    ensure_dir_exists(CONFIG_DIR);
    ensure_dir_exists(STATE_DIR);
    ensure_dir_exists(DEP_DIR);
    ensure_dir_exists(FILES_DIR);
    ensure_dir_exists(L10N_DIR);
    ensure_dir_exists(DOCS_DIR);
    ensure_dir_exists(HOOKS_DIR);
    ensure_dir_exists(LOCK_DIR);
    ensure_file_exists(PKGS_FILE);
    ensure_file_exists(HOLDPKGS_FILE);
    ensure_file_exists(ESSENTIAL_FILE);
    ensure_file_exists(FILES_DB);
    ensure_file_exists(PROVIDES_DB);
}

static std::string g_architecture_override;

void set_architecture(const std::string& arch) {
    g_architecture_override = arch;
}

std::string get_architecture() {
    if (!g_architecture_override.empty()) {
        return g_architecture_override;
    }

    struct utsname buf;
    if (uname(&buf) != 0) {
        throw LpkgException(get_string("error.get_arch_failed"));
    }
    std::string arch(buf.machine);
    if (arch != "x86_64" && arch != "aarch64") {
        throw LpkgException(string_format("error.unsupported_arch", arch));
    }
    return (arch == "x86_64") ? "amd64" : "arm64";
}

std::string get_mirror_url() {
    std::ifstream mirror_file(MIRROR_CONF);
    if (!mirror_file.is_open()) {
        throw LpkgException(string_format("error.open_file_failed", MIRROR_CONF.string()));
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
