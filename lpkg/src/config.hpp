#pragma once

#include <string>
#include <filesystem>

namespace fs = std::filesystem;

// Global constants
inline const fs::path CONFIG_DIR = "/etc/lpkg/";
inline const fs::path DEP_DIR = CONFIG_DIR / "deps/";
inline const fs::path FILES_DIR = CONFIG_DIR / "files/";
inline const fs::path PKGS_FILE = CONFIG_DIR / "pkgs";
inline const fs::path HOLDPKGS_FILE = CONFIG_DIR / "holdpkgs";
inline const fs::path MIRROR_CONF = CONFIG_DIR / "mirror.conf";
inline const fs::path L10N_DIR = "/usr/share/lpkg/l10n/";
inline const fs::path DOCS_DIR = "/usr/share/lpkg/docs/";
inline const fs::path TMP_DIR = "/tmp/lpkg/";
inline const fs::path LOCK_DIR = "/var/lpkg/";
inline const fs::path LOCK_FILE = LOCK_DIR / "db.lck";

// Functions
void init_filesystem();
std::string get_architecture();
std::string get_mirror_url();