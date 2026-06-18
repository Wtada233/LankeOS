#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <filesystem>

#include "repository.hpp"

// Install plan: a resolved package ready to be installed
struct InstallPlan {
    std::string name, actual_version, sha256;
    bool is_explicit = false;
    std::filesystem::path local_path;
    std::vector<DependencyInfo> dependencies;
    std::vector<std::string> provides;
    bool force_reinstall = false;
    bool metadata_verified = false;
};

// Shared context for recursive installation transactions
struct InstallContext {
    Repository& repo;
    std::map<std::string, InstallPlan>& plan;
    std::vector<std::string>& install_order;
    std::map<std::string, std::filesystem::path>& local_candidates;
    std::vector<std::pair<std::string, std::string>>& targets;
    bool force_reinstall;
    bool top_level;                     // false for recursive sub-calls
    std::vector<std::string> successfully_installed;  // within current transaction
};

class InstallationTask {
public:
    InstallationTask(std::string pkg_name, std::string version, bool explicit_install, std::string old_version_to_replace = "", std::filesystem::path local_package_path = "", std::string expected_hash = "", bool force_reinstall = false);
    void run(InstallContext* ctx = nullptr);  // ctx for recursive dep discovery

    // External callers (upgrade, etc.) still use the old interface
    void run_simple() { run(nullptr); }

    std::string pkg_name_;
    std::string version_;
    bool explicit_install_;
    std::filesystem::path tmp_pkg_dir_;
    std::string actual_version_;
    std::filesystem::path archive_path_;
    std::string old_version_to_replace_;
    std::filesystem::path local_package_path_;
    std::string expected_hash_;
    bool has_config_conflicts_ = false;
    bool force_reinstall_ = false;
    std::vector<std::string> deps_;
    std::vector<std::string> provides_;
    std::string man_content_;

    // Temporarily public for testing
    void prepare(InstallContext* ctx = nullptr);
    void download_and_verify_package();
    void extract_and_validate_package();
    void ensure_dependencies_satisfied(InstallContext& ctx);  // NEW: recursive dep discovery
    void check_for_file_conflicts();
    void commit();
    void copy_package_files();
    void register_package();
    void run_post_install_hook();
    void rollback_files();

private:
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> backups_;
    std::vector<std::filesystem::path> installed_files_;
    std::set<std::filesystem::path> created_dirs_;

    // Parse deps_ strings into DependencyInfo structs
    std::vector<DependencyInfo> parse_deps() const;
};

// Public API
void install_package(const std::string& pkg_name, const std::string& version);
void install_packages(const std::vector<std::string>& pkg_args, const std::string& hash_file = "", bool force_reinstall = false);

// Internal recursive engine
void install_packages_internal(InstallContext& ctx);

void remove_package(const std::string& pkg_name, bool force = false);
void autoremove();
void upgrade_packages();
void reinstall_package(const std::string& pkg_name);
void query_package(const std::string& pkg_name);
void query_file(const std::string& filename);
void show_man_page(const std::string& pkg_name);
void write_cache();
void remove_package_files(const std::string& pkg_name, bool force = false);
