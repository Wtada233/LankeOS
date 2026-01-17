#pragma once

#include <string>
#include <filesystem>

// Global variables for paths (initially set to defaults, but can be modified)
extern std::filesystem::path ROOT_DIR;
extern std::filesystem::path CONFIG_DIR;
extern std::filesystem::path L10N_DIR;
extern std::filesystem::path DOCS_DIR;
extern std::filesystem::path LOCK_DIR;
extern std::filesystem::path HOOKS_DIR;

// Derived paths
extern std::filesystem::path DEP_DIR;
extern std::filesystem::path FILES_DIR;
extern std::filesystem::path PKGS_FILE;
extern std::filesystem::path HOLDPKGS_FILE;
extern std::filesystem::path ESSENTIAL_FILE;
extern std::filesystem::path MIRROR_CONF;
extern std::filesystem::path FILES_DB;
extern std::filesystem::path PROVIDES_DB;
std::filesystem::path get_tmp_dir();
extern std::filesystem::path LOCK_FILE;

// Functions
void set_root_path(const std::string& root_path);
void init_filesystem();
void set_architecture(const std::string& arch); // Manually override architecture
std::string get_architecture();
std::string get_mirror_url();

