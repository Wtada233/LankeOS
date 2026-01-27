#pragma once

#include <string>
#include <vector>
#include <set>
#include <filesystem>

class InstallationTask {
public:
    InstallationTask(std::string pkg_name, std::string version, bool explicit_install, std::string old_version_to_replace = "", std::filesystem::path local_package_path = "", std::string expected_hash = "");
    void run();

    std::string pkg_name_;
    std::string version_;
    bool explicit_install_;
    std::filesystem::path tmp_pkg_dir_;
    std::string actual_version_;
    std::filesystem::path archive_path_;
    std::string old_version_to_replace_;
    std::filesystem::path local_package_path_;
    std::string expected_hash_;

private:
    void prepare();
    void download_and_verify_package();
    void extract_and_validate_package();
    void check_for_file_conflicts();

    void commit();
    void copy_package_files();
    void register_package();
    void run_post_install_hook();
    void rollback_files();

    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> backups_;
    std::vector<std::filesystem::path> installed_files_;
    std::set<std::filesystem::path> created_dirs_;
};

void install_package(const std::string& pkg_name, const std::string& version);
void install_packages(const std::vector<std::string>& pkg_args, const std::string& hash_file = "");
void remove_package(const std::string& pkg_name, bool force = false);
void autoremove();
void upgrade_packages();
void show_man_page(const std::string& pkg_name);
void write_cache();
void remove_package_files(const std::string& pkg_name, bool force = false);