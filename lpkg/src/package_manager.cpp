#include "package_manager.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "downloader.hpp"
#include "archive.hpp"
#include "version.hpp"
#include "localization.hpp"
#include "exception.hpp"
#include "hash.hpp"

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
        log_info(string_format("info.package_already_installed", pkg_name));
        return;
    }

    log_info(string_format("info.installing_package", pkg_name, version));
    install_path.push_back(pkg_name);

    std::string mirror_url = get_mirror_url();
    std::string arch = get_architecture();

    std::string actual_version = version;
    if (version == "latest") {
        actual_version = get_latest_version(pkg_name);
        log_info(string_format("info.latest_version", actual_version));
    }

    std::string tmp_pkg_dir = TMP_DIR + pkg_name;
    fs::remove_all(tmp_pkg_dir);
    ensure_dir_exists(tmp_pkg_dir);

    std::string download_url = mirror_url + arch + "/" + pkg_name + "/" + actual_version + "/app.tar.zst";
    std::string archive_path = tmp_pkg_dir + "/app.tar.zst";
    std::string hash_url = mirror_url + arch + "/" + pkg_name + "/" + actual_version + "/hash.txt";
    std::string hash_path = tmp_pkg_dir + "/hash.txt";

    log_info(string_format("info.downloading_from", download_url));

    int max_retries = 5;
    for (int i = 0; i < max_retries; ++i) {
        if (download_file(hash_url, hash_path, false)) {
            break;
        }
        fs::remove(hash_path);
        if (i == max_retries - 1) {
            throw LpkgException(string_format("error.hash_download_failed", hash_url));
        }
    }

    std::ifstream hash_file(hash_path);
    std::string expected_hash;
    hash_file >> expected_hash;

    for (int i = 0; i < max_retries; ++i) {
        if (!download_file(download_url, archive_path)) {
            fs::remove(archive_path);
            if (i == max_retries - 1) {
                throw LpkgException(string_format("error.download_failed", download_url));
            }
            continue;
        }

        std::string actual_hash = calculate_sha256(archive_path);
        if (expected_hash == actual_hash) {
            break; // Success
        }

        fs::remove(archive_path);
        if (i < max_retries - 1) {
            log_warning(string_format("error.hash_mismatch", pkg_name));
        } else {
            throw LpkgException(string_format("error.hash_mismatch", pkg_name));
        }
    }

    log_info(get_string("info.extracting_to_tmp"));
    if (!extract_tar_zst(archive_path, tmp_pkg_dir)) {
        fs::remove_all(tmp_pkg_dir);
        throw LpkgException(string_format("error.extract_failed", archive_path));
    }

    std::vector<std::string> required_files = {"man.txt", "deps.txt", "files.txt", "content/"};
    for (const auto& file : required_files) {
        if (!fs::exists(tmp_pkg_dir + "/" + file)) {
            fs::remove_all(tmp_pkg_dir);
            throw LpkgException(string_format("error.incomplete_package", file));
        }
    }

    log_info(get_string("info.checking_deps"));
    std::ifstream deps_file(tmp_pkg_dir + "/deps.txt");
    std::string dep;
    while (std::getline(deps_file, dep)) {
        if (dep.empty()) continue;

        if (std::find(install_path.begin(), install_path.end(), dep) != install_path.end()) {
            log_warning(string_format("warning.circular_dependency", pkg_name, dep));
            continue;
        }

        log_sync(string_format("info.dep_found", dep));
        std::string installed_version = get_installed_version(dep);

        if (installed_version.empty()) {
            log_sync(string_format("info.dep_not_installed", dep));
            do_install(dep, "latest", false, install_path);
        } else {
            log_sync(string_format("info.dep_already_installed", dep));
        }
    }

    if (!get_installed_version(pkg_name).empty()) {
        fs::remove_all(tmp_pkg_dir);
        log_warning(string_format("warning.skip_already_installed", pkg_name));
        install_path.pop_back();
        return;
    }

    log_info(get_string("info.copying_files"));
    std::ifstream files_list(tmp_pkg_dir + "/files.txt");
    std::string src, dest;
    int file_count = 0;
    std::vector<std::string> installed_files;
    std::set<std::string> created_dirs;

    while (files_list >> src >> dest) {
        std::string src_path = tmp_pkg_dir + "/content/" + src;
        std::string dest_path = dest + "/" + src;

        if (!fs::exists(src_path)) {
            log_warning(string_format("error.incomplete_package", src));
            continue;
        }

        [&]{
            if (fs::exists(dest_path)) {
                bool owned = false;
                for (const auto& entry : fs::directory_iterator(FILES_DIR)) {
                    if (entry.path().stem().string() == pkg_name) continue;
                    std::ifstream other_pkg_files(entry.path());
                    std::string line;
                    while (std::getline(other_pkg_files, line)) {
                        if (line == dest_path) {
                            log_warning(string_format("warning.overwrite_file", dest_path, entry.path().stem().string()));
                            if (!user_confirms("")) {
                                log_info(string_format("info.skipped_overwrite", dest_path));
                                return;
                            }
                            owned = true;
                            break;
                        }
                    }
                    if (owned) break;
                }
            }

            {
                fs::path dest_parent = fs::path(dest_path).parent_path();
                if (!dest_parent.empty()) {
                    ensure_dir_exists(dest_parent.string());
                    created_dirs.insert(dest_parent.string());
                }
            }

            try {
                fs::copy(src_path, dest_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                file_count++;
                installed_files.push_back(dest_path);
            } catch (const fs::filesystem_error& e) {
                log_warning(string_format("warning.copy_file_failed", src_path, dest_path, e.what()));
            }
        }();
    }
    log_sync(string_format("info.copy_complete", file_count));

    std::ofstream pkg_files(FILES_DIR + pkg_name + ".txt");
    for(const auto& file : installed_files) {
        pkg_files << file << "\n";
    }

    std::ofstream dirs_file(FILES_DIR + pkg_name + ".dirs");
    for (const auto& dir : created_dirs) {
        dirs_file << dir << "\n";
    }

    std::ofstream pkg_deps(DEP_DIR + pkg_name);
    deps_file.clear();
    deps_file.seekg(0);
    while (std::getline(deps_file, dep)) {        if (!dep.empty()) pkg_deps << dep << "\n";    }

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
    log_info(string_format("info.package_installed_successfully", pkg_name));
}

void remove_package(const std::string& pkg_name, bool force) {
    if (get_installed_version(pkg_name).empty()) {
        log_info(string_format("info.package_not_installed", pkg_name));
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
                    log_info(string_format("info.skip_remove_dependency", pkg_name, current_pkg_name));
                    return;
                }
            }
        }
    }

    log_info(string_format("info.removing_package", pkg_name));

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
            [&]{
                if (fs::exists(path) || fs::is_symlink(path)) {
                    bool is_shared = false;
                    for (const auto& entry : fs::directory_iterator(FILES_DIR)) {
                        if (entry.path().stem().string() == pkg_name || !entry.is_regular_file() || entry.path().extension() != ".txt") continue;
                        std::ifstream other_pkg_files(entry.path());
                        std::string line;
                        while (std::getline(other_pkg_files, line)) {
                            if (line == path) {
                                log_warning(string_format("warning.remove_shared_file", path, entry.path().stem().string()));
                                if (!user_confirms("")) {
                                    log_info(string_format("info.skipped_remove", path));
                                    return;
                                }
                                is_shared = true;
                                break;
                            }
                        }
                        if (is_shared) break;
                    }

                    try {
                        fs::remove(path);
                        removed_count++;
                    } catch (const fs::filesystem_error& e) {
                        log_warning(string_format("warning.remove_file_failed", path, e.what()));
                    }
                }
            }();
        }
        log_sync(string_format("info.files_removed", removed_count));
        fs::remove(files_list_path);
    }

    std::string dirs_list_path = FILES_DIR + pkg_name + ".dirs";
    if (fs::exists(dirs_list_path)) {
        std::ifstream dirs_list(dirs_list_path);
        std::string dir_path;
        std::vector<std::string> dir_paths;
        while (std::getline(dirs_list, dir_path)) {
            dir_paths.push_back(dir_path);
        }
        std::sort(dir_paths.rbegin(), dir_paths.rend());
        for (const auto& dir : dir_paths) {
            if (fs::exists(dir) && fs::is_directory(dir) && fs::is_empty(dir)) {
                try {
                    fs::remove(dir);
                } catch (const fs::filesystem_error& e) {
                    // Ignore errors, maybe another package created it again
                }
            }
        }
        fs::remove(dirs_list_path);
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

    log_info(string_format("info.package_removed_successfully", pkg_name));
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
        log_info(string_format("info.autoremove_complete", packages_to_remove.size()));
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
            log_warning(string_format("warning.get_latest_version_failed", pkg_name));
            continue;
        }

        if (version_compare(current_version, latest_version)) {
            log_sync(string_format("info.upgradable_found", pkg_name, current_version, latest_version));
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
        log_info(string_format("info.upgraded_packages", upgraded_count));
    }
}

void show_man_page(const std::string& pkg_name) {
    std::string man_file_path = DOCS_DIR + pkg_name + ".man";
    if (!fs::exists(man_file_path)) {
        throw LpkgException(string_format("error.no_man_page", pkg_name));
    }

    std::ifstream man_file(man_file_path);
    if (!man_file.is_open()) {
        throw LpkgException(string_format("error.open_man_page_failed", man_file_path));
    }

    std::cout << man_file.rdbuf();
}
