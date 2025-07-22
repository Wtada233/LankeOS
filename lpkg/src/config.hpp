#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>

// Global constants
extern const std::string CONFIG_DIR;
extern const std::string DEP_DIR;
extern const std::string FILES_DIR;
extern const std::string PKGS_FILE;
extern const std::string HOLDPKGS_FILE;
extern const std::string MIRROR_CONF;
extern const std::string DOCS_DIR;
extern const std::string TMP_DIR;
extern const std::string LOCK_DIR;
extern const std::string LOCK_FILE;

// Functions
void init_filesystem();
std::string get_architecture();
std::string get_mirror_url();

#endif // CONFIG_HPP
