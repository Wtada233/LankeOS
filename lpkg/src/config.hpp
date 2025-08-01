#pragma once

#include <string>
#include <filesystem>

namespace fs = std::filesystem;

// These paths are now set by the compiler from the Makefile
// This allows for easy customization of installation directories.
#ifndef LPKG_CONF_DIR
#define LPKG_CONF_DIR "/etc/lpkg"
#endif
#ifndef LPKG_L10N_DIR
#define LPKG_L10N_DIR "/usr/share/lpkg/l10n"
#endif
#ifndef LPKG_DOCS_DIR
#define LPKG_DOCS_DIR "/usr/share/lpkg/docs"
#endif
#ifndef LPKG_LOCK_DIR
#define LPKG_LOCK_DIR "/var/lpkg"
#endif

// Global constants based on build-time configuration
inline const fs::path CONFIG_DIR = LPKG_CONF_DIR;
inline const fs::path L10N_DIR = LPKG_L10N_DIR;
inline const fs::path DOCS_DIR = LPKG_DOCS_DIR;
inline const fs::path LOCK_DIR = LPKG_LOCK_DIR;
inline const fs::path HOOKS_DIR = CONFIG_DIR / "hooks/";

// Derived paths
inline const fs::path DEP_DIR = CONFIG_DIR / "deps/";
inline const fs::path FILES_DIR = CONFIG_DIR / "files/";
inline const fs::path PKGS_FILE = CONFIG_DIR / "pkgs";
inline const fs::path HOLDPKGS_FILE = CONFIG_DIR / "holdpkgs";
inline const fs::path MIRROR_CONF = CONFIG_DIR / "mirror.conf";
inline const fs::path FILES_DB = FILES_DIR / "files.db"; // Changed from main.db
fs::path get_tmp_dir();
inline const fs::path LOCK_FILE = LOCK_DIR / "db.lck";

// Functions
void init_filesystem();
std::string get_architecture();
std::string get_mirror_url();
