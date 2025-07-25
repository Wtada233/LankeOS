#include "config.hpp"
#include "utils.hpp"
#include "localization.hpp"
#include "exception.hpp"
#include <sys/utsname.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

const std::string CONFIG_DIR = "/etc/lpkg/";
const std::string DEP_DIR = CONFIG_DIR + "deps/";
const std::string FILES_DIR = CONFIG_DIR + "files/";
const std::string PKGS_FILE = CONFIG_DIR + "pkgs";
const std::string HOLDPKGS_FILE = CONFIG_DIR + "holdpkgs";
const std::string MIRROR_CONF = CONFIG_DIR + "mirror.conf";
const std::string L10N_DIR = "/usr/share/lpkg/l10n/";
const std::string DOCS_DIR = "/usr/share/lpkg/docs/";
const std::string TMP_DIR = "/tmp/lpkg/";
const std::string LOCK_DIR = "/var/lpkg/";
const std::string LOCK_FILE = LOCK_DIR + "db.lck";

void init_filesystem() {
    ensure_dir_exists(CONFIG_DIR);
    ensure_dir_exists(DEP_DIR);
    ensure_dir_exists(FILES_DIR);
    ensure_dir_exists(L10N_DIR);
    ensure_dir_exists(DOCS_DIR);
    ensure_dir_exists(TMP_DIR);
    ensure_file_exists(PKGS_FILE);
    ensure_file_exists(HOLDPKGS_FILE);
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
    std::string mirror_url;
    if (!std::getline(mirror_file, mirror_url) || mirror_url.empty()) {
        throw LpkgException(get_string("error.invalid_mirror_config"));
    }
    if (mirror_url.back() != '/') mirror_url += '/';
    return mirror_url;
}
