#include "config.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "utils.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

fs::path get_tmp_dir() {
    return fs::path("/tmp") / ("lpkg_" + std::to_string(getpid()));
}

void init_filesystem() {
    ensure_dir_exists(CONFIG_DIR);
    ensure_dir_exists(DEP_DIR);
    ensure_dir_exists(FILES_DIR);
    ensure_dir_exists(L10N_DIR);
    ensure_dir_exists(DOCS_DIR);
    ensure_dir_exists(get_tmp_dir());
    ensure_dir_exists(LOCK_DIR);
    ensure_file_exists(PKGS_FILE);
    ensure_file_exists(HOLDPKGS_FILE);
    ensure_file_exists(FILES_DB);
}

std::string get_architecture() {
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
