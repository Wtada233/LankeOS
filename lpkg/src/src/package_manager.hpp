#pragma once

#include <string>
#include <vector>
#include <filesystem>

class InstallationTask {
public:
    InstallationTask(std::string pkg_name, std::string version, bool explicit_install, std::vector<std::string>& install_path, std::string old_version_to_replace = "", std::filesystem::path local_package_path = "");
    void run();

    void prepare();
    void download_and_verify_package();
    void extract_and_validate_package();
    void resolve_dependencies();
    void check_for_file_conflicts();

    void commit();
    void copy_package_files();
    void register_package();
    void run_post_install_hook();

    std::string pkg_name_;
    std::string version_;
    bool explicit_install_;
    std::vector<std::string>& install_path_;
    std::filesystem::path tmp_pkg_dir_;
    std::string actual_version_;
    std::filesystem::path archive_path_;
    std::string old_version_to_replace_;
    std::filesystem::path local_package_path_;
};

void install_package(const std::string& pkg_name, const std::string& version);
void install_packages(const std::vector<std::string>& pkg_args);
void remove_package(const std::string& pkg_name, bool force = false);
void autoremove();
void upgrade_packages();
void show_man_page(const std::string& pkg_name);
void write_cache();
void remove_package_files(const std::string& pkg_name, bool force = false);