#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>

// Global constants
inline const std::string CONFIG_DIR = "/etc/lpkg/";
inline const std::string DEP_DIR = CONFIG_DIR + "deps/";
inline const std::string FILES_DIR = CONFIG_DIR + "files/";
inline const std::string PKGS_FILE = CONFIG_DIR + "pkgs";
inline const std::string HOLDPKGS_FILE = CONFIG_DIR + "holdpkgs";
inline const std::string MIRROR_CONF = CONFIG_DIR + "mirror.conf";
inline const std::string L10N_DIR = "/usr/share/lpkg/l10n/";
inline const std::string DOCS_DIR = "/usr/share/lpkg/docs/";
inline const std::string TMP_DIR = "/tmp/lpkg/";
inline const std::string LOCK_DIR = "/var/lpkg/";
inline const std::string LOCK_FILE = LOCK_DIR + "db.lck";

// Functions
void init_filesystem();
std::string get_architecture();
std::string get_mirror_url();

#endif // CONFIG_HPP