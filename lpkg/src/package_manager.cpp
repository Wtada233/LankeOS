#include "package_manager.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "downloader.hpp"
#include "archive.hpp"
#include "version.hpp"
#include "localization.hpp"

#include <iostream>
#include <vector>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <set>
#include <algorithm>

namespace fs = std::filesystem;

// Forward declarations for internal functions
std::string get_installed_version(const std::string& pkg_name);
bool is_manually_installed(const std::string& pkg_name);
void do_install(const std::string& pkg_name, const std::string& version, bool explicit_install, std::vector<std::string>& install_path);

// Implementation
std::string get_installed_version(const std::string& pkg_name) {
    auto pkgs = read_set_from_file(PKGS_FILE);
    for (const auto& pkg : pkgs) {
        if (pkg.find(pkg_name + ":") == 0) {
            return pkg.substr(pkg_name.length() + 1);
        }
    }
    return "";
}

bool is_manually_installed(const std::string& pkg_name) {
    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
    return holdpkgs.count(pkg_name) > 0;
}

void install_package(const std::string& pkg_name, const std::string& version) {
    std::vector<std::string> install_path;
    do_install(pkg_name, version, true, install_path);
}

void do_install(const std::string& pkg_name, const std::string& version, bool explicit_install, std::vector<std::string>& install_path) {
    if (!get_installed_version(pkg_name).empty()) {
        log_info(string_format("info.package_already_installed", pkg_name.c_str()));
        return;
    }

    log_info(string_format("info.installing_package", pkg_name.c_str(), version.c_str()));
    install_path.push_back(pkg_name);

    std::string mirror_url = get_mirror_url();
    std::string arch = get_architecture();

    std::string actual_version = version;
    if (version == "latest") {
        actual_version = get_latest_version(pkg_name);
        log_info(string_format("info.latest_version", actual_version.c_str()));
    }

    std::string tmp_pkg_dir = TMP_DIR + pkg_name;
    fs::remove_all(tmp_pkg_dir);
    ensure_dir_exists(tmp_pkg_dir);

    std::string download_url = mirror_url + arch + "/" + pkg_name + "/" + actual_version + "/app.tar.zst";
    std::string archive_path = tmp_pkg_dir + "/app.tar.zst";

    log_info(string_format("info.downloading_from", download_url.c_str()));
    if (!download_file(download_url, archive_path)) {
        fs::remove_all(tmp_pkg_dir);
        exit_with_error(string_format("error.download_failed", download_url.c_str()));
    }

    log_info(get_string("info.extracting_to_tmp"));
    if (!extract_tar_zst(archive_path, tmp_pkg_dir)) {
        fs::remove_all(tmp_pkg_dir);
        exit_with_error(string_format("error.extract_failed", archive_path.c_str()));
    }

    std::vector<std::string> required_files = {"man.txt", "deps.txt", "files.txt", "content/"};
    for (const auto& file : required_files) {
        if (!fs::exists(tmp_pkg_dir + "/" + file)) {
            fs::remove_all(tmp_pkg_dir);
            exit_with_error(string_format("error.incomplete_package", file.c_str()));
        }
    }

    log_info(get_string("info.checking_deps"));
    std::ifstream deps_file(tmp_pkg_dir + "/deps.txt");
    std::string dep;
    while (std::getline(deps_file, dep)) {
        if (dep.empty()) continue;

        if (std::find(install_path.begin(), install_path.end(), dep) != install_path.end()) {
            log_warning(string_format("error.circular_dependency", pkg_name.c_str(), dep.c_str()));
            continue;
        }

        log_sync(string_format("info.dep_found", dep.c_str()));
        std::string installed_version = get_installed_version(dep);

        if (installed_version.empty()) {
            log_sync(string_format("info.dep_not_installed", dep.c_str()));
            do_install(dep, "latest", false, install_path);
        } else {
            log_sync(string_format("info.dep_already_installed", dep.c_str()));
        }
    }

    if (!get_installed_version(pkg_name).empty()) {
        fs::remove_all(tmp_pkg_dir);
        log_info(string_format("info.skip_already_installed", pkg_name.c_str()));
        install_path.pop_back();
        return;
    }

    log_info(get_string("info.copying_files"));
    std::ifstream files_list(tmp_pkg_dir + "/files.txt");
    std::string src, dest;
    int file_count = 0;
    std::vector<std::string> installed_files;
    while (files_list >> src >> dest) {
        std::string src_path = tmp_pkg_dir + "/content/" + src;
        std::string dest_path = dest + "/" + src;
        installed_files.push_back(dest_path);

        if (!fs::exists(src_path)) {
            log_warning(string_format("error.incomplete_package", src.c_str()));
            continue;
        }

        fs::path dest_parent = fs::path(dest_path).parent_path();
        if (!dest_parent.empty()) {
            ensure_dir_exists(dest_parent.string());
        }

        try {
            fs::copy(src_path, dest_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            file_count++;
        } catch (const fs::filesystem_error& e) {
            log_warning(string_format("error.copy_file_failed", src_path.c_str(), dest_path.c_str(), e.what()));
        }
    }
    log_sync(string_format("info.copy_complete", std::to_string(file_count).c_str()));

    std::ofstream pkg_files(FILES_DIR + pkg_name + ".txt");
    for(const auto& file : installed_files) {
        pkg_files << file << "\n";
    }

    std::ofstream pkg_deps(DEP_DIR + pkg_name);
    deps_file.clear();
    deps_file.seekg(0);
    while (std::getline(deps_file, dep)) {
        if (!dep.empty()) pkg_deps << dep << "\n";
    }

    fs::copy(tmp_pkg_dir + "/man.txt", DOCS_DIR + pkg_name + ".man", fs::copy_options::overwrite_existing);

    auto pkgs = read_set_from_file(PKGS_FILE);
    pkgs.insert(pkg_name + ":" + actual_version);
    write_set_to_file(PKGS_FILE, pkgs);

    if (explicit_install) {
        auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
        holdpkgs.insert(pkg_name);
        write_set_to_file(HOLDPKGS_FILE, holdpkgs);
    }

    fs::remove_all(tmp_pkg_dir);
    install_path.pop_back();
    log_info(string_format("info.package_installed_successfully", pkg_name.c_str()));
}

void remove_package(const std::string& pkg_name, bool force) {
    if (get_installed_version(pkg_name).empty()) {
        log_info(string_format("info.package_not_installed", pkg_name.c_str()));
        return;
    }

    if (!force) {
        for (const auto& dep_file : fs::directory_iterator(DEP_DIR)) {
            std::string current_pkg_name = dep_file.path().stem().string();
            if (current_pkg_name == pkg_name) continue;

            std::ifstream file(dep_file.path());
            std::string dep;
            while (std::getline(file, dep)) {
                if (dep == pkg_name) {
                    exit_with_error(string_format("error.skip_remove_dependency", pkg_name.c_str(), current_pkg_name.c_str()));
                }
            }
        }
    }

    log_info(string_format("info.removing_package", pkg_name.c_str()));

    std::string files_list_path = FILES_DIR + pkg_name + ".txt";
    if (fs::exists(files_list_path)) {
        std::ifstream files_list(files_list_path);
        std::string file_path;
        std::vector<std::string> file_paths;
        while (std::getline(files_list, file_path)) {
            file_paths.push_back(file_path);
        }
        
        std::sort(file_paths.rbegin(), file_paths.rend());

        int removed_count = 0;
        for (const auto& path : file_paths) {
            if (fs::exists(path) || fs::is_symlink(path)) {
                try {
                    fs::remove(path);
                    removed_count++;
                } catch (const fs::filesystem_error& e) {
                    log_warning(string_format("error.remove_file_failed", path.c_str(), e.what()));
                }
            }
        }
        log_sync(string_format("info.files_removed", std::to_string(removed_count).c_str()));
        fs::remove(files_list_path);
    }

    fs::remove(DEP_DIR + pkg_name);
    fs::remove(DOCS_DIR + pkg_name + ".man");

    auto pkgs = read_set_from_file(PKGS_FILE);
    std::string pkg_record;
    for (const auto& pkg : pkgs) {
        if (pkg.find(pkg_name + ":") == 0) {
            pkg_record = pkg;
            break;
        }
    }
    if (!pkg_record.empty()) {
        pkgs.erase(pkg_record);
        write_set_to_file(PKGS_FILE, pkgs);
    }

    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
    if (holdpkgs.count(pkg_name)) {
        holdpkgs.erase(pkg_name);
        write_set_to_file(HOLDPKGS_FILE, holdpkgs);
    }

    log_info(string_format("info.package_removed_successfully", pkg_name.c_str()));
}

// Helper function for autoremove to perform a full dependency graph traversal
std::unordered_set<std::string> get_all_required_packages() {
    auto manually_installed = read_set_from_file(HOLDPKGS_FILE);
    std::unordered_set<std::string> required;
    std::vector<std::string> queue;

    for (const auto& pkg : manually_installed) {
        if (required.find(pkg) == required.end()) {
            queue.push_back(pkg);
            required.insert(pkg);
        }
    }

    size_t head = 0;
    while (head < queue.size()) {
        std::string current_pkg = queue[head++];
        std::string dep_file_path = DEP_DIR + current_pkg;

        if (fs::exists(dep_file_path)) {
            std::ifstream dep_file(dep_file_path);
            std::string dep;
            while (std::getline(dep_file, dep)) {
                if (!dep.empty() && required.find(dep) == required.end()) {
                    required.insert(dep);
                    queue.push_back(dep);
                }
            }
        }
    }
    return required;
}

void autoremove() {
    log_info(get_string("info.checking_autoremove"));
    
    auto required_pkgs = get_all_required_packages();
    auto all_pkgs_records = read_set_from_file(PKGS_FILE);
    std::vector<std::string> packages_to_remove;

    for (const auto& record : all_pkgs_records) {
        std::string pkg_name = record.substr(0, record.find(':'));
        if (required_pkgs.find(pkg_name) == required_pkgs.end()) {
            packages_to_remove.push_back(pkg_name);
        }
    }

    if (packages_to_remove.empty()) {
        log_info(get_string("info.no_autoremove_packages"));
    } else {
        for (const auto& pkg_name : packages_to_remove) {
            // The 'force' flag is true because we've already determined it's safe to remove.
            remove_package(pkg_name, true);
        }
        log_info(string_format("info.autoremove_complete", std::to_string(packages_to_remove.size()).c_str()));
    }

    fs::remove_all(TMP_DIR);
    ensure_dir_exists(TMP_DIR);
}

void upgrade_packages() {
    log_info(get_string("info.checking_upgradable"));
    auto pkgs = read_set_from_file(PKGS_FILE);
    int upgraded_count = 0;

    for (const auto& pkg : pkgs) {
        size_t pos = pkg.find(':');
        if (pos == std::string::npos) continue;

        std::string pkg_name = pkg.substr(0, pos);
        std::string current_version = pkg.substr(pos + 1);

        std::string latest_version;
        try {
            latest_version = get_latest_version(pkg_name);
        } catch (...) {
            log_error(string_format("error.get_latest_version_failed", pkg_name.c_str()));
            continue;
        }

        if (version_compare(current_version, latest_version)) {
            log_sync(string_format("info.upgradable_found", pkg_name.c_str(), current_version.c_str(), latest_version.c_str()));
            bool was_manually_installed = is_manually_installed(pkg_name);
            remove_package(pkg_name, true);
            std::vector<std::string> install_path;
            do_install(pkg_name, latest_version, was_manually_installed, install_path);
            upgraded_count++;
        }
    }

    if (upgraded_count == 0) {
        log_info(get_string("info.all_packages_latest"));
    } else {
        log_info(string_format("info.upgraded_packages", std::to_string(upgraded_count).c_str()));
    }
}

void show_man_page(const std::string& pkg_name) {
    std::string man_file_path = DOCS_DIR + pkg_name + ".man";
    if (!fs::exists(man_file_path)) {
        exit_with_error(string_format("error.no_man_page", pkg_name.c_str()));
    }

    std::ifstream man_file(man_file_path);
    if (!man_file.is_open()) {
        exit_with_error(string_format("error.open_man_page_failed", man_file_path.c_str()));
    }

    std::cout << man_file.rdbuf();
}
