#include "package_manager.hpp"

#include "archive.hpp"
#include "config.hpp"
#include "downloader.hpp"
#include "exception.hpp"
#include "hash.hpp"
#include "localization.hpp"
#include "utils.hpp"
#include "version.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Forward declarations for internal functions
void do_install(const std::string& pkg_name, const std::string& version, bool explicit_install, std::vector<std::string>& install_path);
std::string get_installed_version(const std::string& pkg_name);
bool is_manually_installed(const std::string& pkg_name);
std::unordered_set<std::string> get_all_required_packages();
std::string find_file_owner(const fs::path& file_path, const std::string& current_pkg_name);
void download_and_verify_package(const std::string& pkg_name, const std::string& version, const fs::path& tmp_pkg_dir, fs::path& out_archive_path, std::string& out_actual_version);
void extract_and_validate_package(const std::string& pkg_name, const fs::path& archive_path, const fs::path& tmp_pkg_dir);
void resolve_dependencies(const std::string& pkg_name, const fs::path& tmp_pkg_dir, std::vector<std::string>& install_path);
void check_for_file_conflicts(const std::string& pkg_name, const fs::path& tmp_pkg_dir);
void copy_package_files(const std::string& pkg_name, const fs::path& tmp_pkg_dir, std::vector<std::string>& out_installed_files, std::set<std::string>& out_created_dirs);
void register_package(const std::string& pkg_name, const std::string& actual_version, bool explicit_install, const fs::path& tmp_pkg_dir, const std::vector<std::string>& installed_files, const std::set<std::string>& created_dirs);


//
// Core Helper Functions
//

std::string get_installed_version(const std::string& pkg_name) {
    auto pkgs = read_set_from_file(PKGS_FILE);
    for (const auto& pkg : pkgs) {
        if (pkg.starts_with(pkg_name + ":")) {
            return pkg.substr(pkg_name.length() + 1);
        }
    }
    return "";
}

bool is_manually_installed(const std::string& pkg_name) {
    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
    return holdpkgs.contains(pkg_name);
}

std::string find_file_owner(const fs::path& file_path, const std::string& current_pkg_name) {
    for (const auto& entry : fs::directory_iterator(FILES_DIR)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".txt") continue;
        
        std::string owner_pkg_name = entry.path().stem().string();
        if (owner_pkg_name == current_pkg_name) continue;

        std::ifstream other_pkg_files(entry.path());
        std::string line;
        while (std::getline(other_pkg_files, line)) {
            if (line == file_path.string()) {
                return owner_pkg_name;
            }
        }
    }
    return "";
}

void download_with_retries(const std::string& url, const fs::path& output_path, int max_retries = 5, bool show_progress = true) {
    for (int i = 0; i < max_retries; ++i) {
        try {
            download_file(url, output_path, show_progress);
            return; // Success
        } catch (const LpkgException& e) {
            fs::remove(output_path); // Clean up failed download
            if (i < max_retries - 1) {
                log_warning(std::string(e.what()) + ". Retrying...");
            } else {
                throw; // Rethrow on last attempt
            }
        }
    }
}

//
// Installation Step Functions
//

void download_and_verify_package(const std::string& pkg_name, const std::string& version, const fs::path& tmp_pkg_dir, fs::path& out_archive_path, std::string& out_actual_version) {
    std::string mirror_url = get_mirror_url();
    std::string arch = get_architecture();

    out_actual_version = version;
    if (version == "latest") {
        out_actual_version = get_latest_version(pkg_name);
        log_info(string_format("info.latest_version", out_actual_version));
    }

    const std::string download_url = mirror_url + arch + "/" + pkg_name + "/" + out_actual_version + "/app.tar.zst";
    out_archive_path = tmp_pkg_dir / "app.tar.zst";
    const std::string hash_url = mirror_url + arch + "/" + pkg_name + "/" + out_actual_version + "/hash.txt";
    const fs::path hash_path = tmp_pkg_dir / "hash.txt";

    log_info(string_format("info.downloading_from", download_url));

    download_with_retries(hash_url, hash_path, 5, false);
    std::ifstream hash_file(hash_path);
    std::string expected_hash;
    if (!(hash_file >> expected_hash)) {
        throw LpkgException(string_format("error.hash_download_failed", hash_url));
    }
    hash_file.close();

    download_with_retries(download_url, out_archive_path, 5, true);

    std::string actual_hash = calculate_sha256(out_archive_path);
    if (expected_hash != actual_hash) {
        throw LpkgException(string_format("error.hash_mismatch", pkg_name));
    }
}

void extract_and_validate_package([[maybe_unused]] const std::string& pkg_name, const fs::path& archive_path, const fs::path& tmp_pkg_dir) {
    log_info(get_string("info.extracting_to_tmp"));
    extract_tar_zst(archive_path, tmp_pkg_dir);

    std::vector<fs::path> required_files = {"man.txt", "deps.txt", "files.txt", "content/"};
    for (const auto& file : required_files) {
        if (!fs::exists(tmp_pkg_dir / file)) {
            throw LpkgException(string_format("error.incomplete_package", (tmp_pkg_dir / file).string()));
        }
    }
}

void resolve_dependencies(const std::string& pkg_name, const fs::path& tmp_pkg_dir, std::vector<std::string>& install_path) {
    log_info(get_string("info.checking_deps"));
    std::ifstream deps_file(tmp_pkg_dir / "deps.txt");
    std::string dep;
    while (std::getline(deps_file, dep)) {
        if (dep.empty()) continue;

        if (std::find(install_path.begin(), install_path.end(), dep) != install_path.end()) {
            log_warning(string_format("warning.circular_dependency", pkg_name, dep));
            continue;
        }

        log_info(string_format("info.dep_found", dep));
        if (get_installed_version(dep).empty()) {
            log_info(string_format("info.dep_not_installed", dep));
            do_install(dep, "latest", false, install_path);
        } else {
            log_info(string_format("info.dep_already_installed", dep));
        }
    }
}

void check_for_file_conflicts(const std::string& pkg_name, const fs::path& tmp_pkg_dir) {
    log_info(get_string("info.checking_for_file_conflicts"));
    std::map<std::string, std::string> conflicts;
    std::ifstream files_list(tmp_pkg_dir / "files.txt");
    std::string src, dest;
    while (files_list >> src >> dest) {
        fs::path dest_path = fs::path(dest) / src;
        if (fs::exists(dest_path)) {
            std::string owner = find_file_owner(dest_path, pkg_name);
            if (!owner.empty()) {
                conflicts[dest_path.string()] = owner;
            }
        }
    }

    if (!conflicts.empty()) {
        std::string error_msg = get_string("error.file_conflict_header") + "\n";
        for (const auto& [file, owner] : conflicts) {
            error_msg += "  " + string_format("error.file_conflict_entry", file, owner) + "\n";
        }
        error_msg += get_string("error.installation_aborted");
        throw LpkgException(error_msg);
    }
}

void copy_package_files([[maybe_unused]] const std::string& pkg_name, const fs::path& tmp_pkg_dir, std::vector<std::string>& out_installed_files, std::set<std::string>& out_created_dirs) {
    log_info(get_string("info.copying_files"));
    std::ifstream files_list(tmp_pkg_dir / "files.txt");
    std::string src, dest;
    int file_count = 0;

    while (files_list >> src >> dest) {
        const fs::path src_path = tmp_pkg_dir / "content" / src;
        const fs::path dest_path = fs::path(dest) / src;

        if (!fs::exists(src_path)) {
            log_warning(string_format("error.incomplete_package", src));
            continue;
        }

        fs::path dest_parent = dest_path.parent_path();
        if (!dest_parent.empty()) {
            ensure_dir_exists(dest_parent);
            out_created_dirs.insert(dest_parent.string());
        }

        try {
            fs::copy(src_path, dest_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            file_count++;
            out_installed_files.push_back(dest_path.string());
        } catch (const fs::filesystem_error& e) {
            throw LpkgException(string_format("error.copy_failed_rollback", src_path.string(), dest_path.string(), e.what()));
        }
    }
    log_info(string_format("info.copy_complete", file_count));
}

void register_package(const std::string& pkg_name, const std::string& actual_version, bool explicit_install, const fs::path& tmp_pkg_dir, const std::vector<std::string>& installed_files, const std::set<std::string>& created_dirs) {
    // Record installed files
    std::ofstream pkg_files(FILES_DIR / (pkg_name + ".txt"));
    for(const auto& file : installed_files) {
        pkg_files << file << "\n";
    }

    // Record created directories
    std::ofstream dirs_file(FILES_DIR / (pkg_name + ".dirs"));
    for (const auto& dir : created_dirs) {
        dirs_file << dir << "\n";
    }

    // Record dependencies
    std::ifstream deps_file_src(tmp_pkg_dir / "deps.txt");
    std::ofstream pkg_deps_dest(DEP_DIR / pkg_name);
    std::string dep;
    while (std::getline(deps_file_src, dep)) {
        if (!dep.empty()) {
            pkg_deps_dest << dep << "\n";
        }
    }

    // Copy man page
    fs::copy(tmp_pkg_dir / "man.txt", DOCS_DIR / (pkg_name + ".man"), fs::copy_options::overwrite_existing);

    // Add to main package list
    auto pkgs = read_set_from_file(PKGS_FILE);
    pkgs.insert(pkg_name + ":" + actual_version);
    write_set_to_file(PKGS_FILE, pkgs);

    // Add to manually installed list if needed
    if (explicit_install) {
        auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
        holdpkgs.insert(pkg_name);
        write_set_to_file(HOLDPKGS_FILE, holdpkgs);
    }
}


//
// Main Control Flow
//

void do_install(const std::string& pkg_name, const std::string& version, bool explicit_install, std::vector<std::string>& install_path) {
    if (!get_installed_version(pkg_name).empty()) {
        log_info(string_format("info.package_already_installed", pkg_name));
        return;
    }

    log_info(string_format("info.installing_package", pkg_name, version));
    install_path.push_back(pkg_name);

    const fs::path tmp_pkg_dir = fs::path(TMP_DIR) / pkg_name;
    fs::remove_all(tmp_pkg_dir);
    ensure_dir_exists(tmp_pkg_dir);

    // RAII cleanup for temp directory
    auto cleanup = [&](const void*) { fs::remove_all(tmp_pkg_dir); };
    std::unique_ptr<const void, decltype(cleanup)> tmp_dir_guard(tmp_pkg_dir.c_str(), cleanup);

    try {
        // Step 1: Download and Verify
        fs::path archive_path;
        std::string actual_version;
        download_and_verify_package(pkg_name, version, tmp_pkg_dir, archive_path, actual_version);

        // Step 2: Extract and Validate
        extract_and_validate_package(pkg_name, archive_path, tmp_pkg_dir);

        // Step 3: Resolve Dependencies
        resolve_dependencies(pkg_name, tmp_pkg_dir, install_path);

        // Re-check if installed during dependency resolution
        if (!get_installed_version(pkg_name).empty()) {
            log_warning(string_format("warning.skip_already_installed", pkg_name));
            install_path.pop_back();
            return;
        }

        // Step 4: Check for File Conflicts
        check_for_file_conflicts(pkg_name, tmp_pkg_dir);

        // Step 5: Copy Files
        std::vector<std::string> installed_files;
        std::set<std::string> created_dirs;
        copy_package_files(pkg_name, tmp_pkg_dir, installed_files, created_dirs);

        // Step 6: Register Package
        register_package(pkg_name, actual_version, explicit_install, tmp_pkg_dir, installed_files, created_dirs);

    } catch (...) {
        install_path.pop_back();
        throw; // Re-throw the exception after cleanup
    }

    install_path.pop_back();
    log_info(string_format("info.package_installed_successfully", pkg_name));
}

std::unordered_set<std::string> get_all_required_packages() {
    auto manually_installed = read_set_from_file(HOLDPKGS_FILE);
    std::unordered_set<std::string> required;
    std::vector<std::string> queue;

    for (const auto& pkg : manually_installed) {
        if (!required.contains(pkg)) {
            queue.push_back(pkg);
            required.insert(pkg);
        }
    }

    size_t head = 0;
    while (head < queue.size()) {
        std::string current_pkg = queue[head++];
        fs::path dep_file_path = DEP_DIR / current_pkg;

        if (fs::exists(dep_file_path)) {
            std::ifstream dep_file(dep_file_path);
            std::string dep;
            while (std::getline(dep_file, dep)) {
                if (!dep.empty() && !required.contains(dep)) {
                    required.insert(dep);
                    queue.push_back(dep);
                }
            }
        }
    }
    return required;
}

} // anonymous namespace

//
// Public API Functions
//

void install_package(const std::string& pkg_name, const std::string& version) {
    std::vector<std::string> install_path;
    do_install(pkg_name, version, true, install_path);
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

    const fs::path files_list_path = FILES_DIR / (pkg_name + ".txt");
    if (fs::exists(files_list_path)) {
        log_info(get_string("info.checking_for_shared_files"));
        std::map<std::string, std::vector<std::string>> shared_files;
        std::ifstream files_to_check(files_list_path);
        std::string file_to_check;
        while (std::getline(files_to_check, file_to_check)) {
            if (file_to_check.empty()) continue;
            std::string owner = find_file_owner(file_to_check, pkg_name);
            if (!owner.empty()) {
                shared_files[file_to_check].push_back(owner);
            }
        }

        if (!shared_files.empty()) {
            std::string error_msg = get_string("error.shared_file_header") + "\n";
            for (const auto& [file, owners] : shared_files) {
                std::string owners_str;
                for(size_t i = 0; i < owners.size(); ++i) {
                    owners_str += owners[i] + (i == owners.size() - 1 ? "" : ", ");
                }
                error_msg += "  " + string_format("error.shared_file_entry", file, owners_str) + "\n";
            }
            error_msg += get_string("error.removal_aborted");
            throw LpkgException(error_msg);
        }

        std::ifstream files_list(files_list_path);
        std::string file_path;
        std::vector<fs::path> file_paths;
        while (std::getline(files_list, file_path)) {
            if (!file_path.empty()) file_paths.emplace_back(file_path);
        }
        
        std::sort(file_paths.rbegin(), file_paths.rend());

        int removed_count = 0;
        for (const auto& path : file_paths) {
            if (fs::exists(path) || fs::is_symlink(path)) {
                try {
                    fs::remove(path);
                    removed_count++;
                } catch (const fs::filesystem_error& e) {
                    log_warning(string_format("warning.remove_file_failed", path.string(), e.what()));
                }
            }
        }
        log_info(string_format("info.files_removed", removed_count));
        fs::remove(files_list_path);
    }

    const fs::path dirs_list_path = FILES_DIR / (pkg_name + ".dirs");
    if (fs::exists(dirs_list_path)) {
        std::ifstream dirs_list(dirs_list_path);
        std::string dir_path;
        std::vector<fs::path> dir_paths;
        while (std::getline(dirs_list, dir_path)) {
            if (!dir_path.empty()) dir_paths.emplace_back(dir_path);
        }
        std::sort(dir_paths.rbegin(), dir_paths.rend());
        for (const auto& dir : dir_paths) {
            if (fs::exists(dir) && fs::is_directory(dir) && fs::is_empty(dir)) {
                try {
                    fs::remove(dir);
                } catch (const fs::filesystem_error&) {
                    // Ignore errors, maybe another package created it again
                }
            }
        }
        fs::remove(dirs_list_path);
    }

    fs::remove(DEP_DIR / pkg_name);
    fs::remove(DOCS_DIR / (pkg_name + ".man"));

    auto pkgs = read_set_from_file(PKGS_FILE);
    std::erase_if(pkgs, [&](const auto& record){ return record.starts_with(pkg_name + ":"); });
    write_set_to_file(PKGS_FILE, pkgs);

    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
    if (holdpkgs.contains(pkg_name)) {
        holdpkgs.erase(pkg_name);
        write_set_to_file(HOLDPKGS_FILE, holdpkgs);
    }

    log_info(string_format("info.package_removed_successfully", pkg_name));
}

void autoremove() {
    log_info(get_string("info.checking_autoremove"));
    
    auto required_pkgs = get_all_required_packages();
    auto all_pkgs_records = read_set_from_file(PKGS_FILE);
    std::vector<std::string> packages_to_remove;

    for (const auto& record : all_pkgs_records) {
        std::string pkg_name = record.substr(0, record.find(':'));
        if (!required_pkgs.contains(pkg_name)) {
            packages_to_remove.push_back(pkg_name);
        }
    }

    if (packages_to_remove.empty()) {
        log_info(get_string("info.no_autoremove_packages"));
    } else {
        log_info(string_format("info.autoremove_candidates", packages_to_remove.size()));
        for (const auto& pkg_name : packages_to_remove) {
            try {
                remove_package(pkg_name, true);
            } catch (const LpkgException& e) {
                log_warning(string_format("warning.autoremove_skipped", pkg_name, e.what()));
            }
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
    std::vector<std::tuple<std::string, std::string, std::string>> upgradable_pkgs;

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
            log_info(string_format("info.upgradable_found", pkg_name, current_version, latest_version));
            upgradable_pkgs.emplace_back(pkg_name, current_version, latest_version);
        }
    }

    if (upgradable_pkgs.empty()) {
        log_info(get_string("info.all_packages_latest"));
        return;
    }

    for (const auto& [pkg_name, current_version, latest_version] : upgradable_pkgs) {
        try {
            log_info(string_format("info.upgrading_package", pkg_name, current_version, latest_version));
            bool was_manually_installed = is_manually_installed(pkg_name);
            
            remove_package(pkg_name, true); 
            
            std::vector<std::string> install_path;
            do_install(pkg_name, latest_version, was_manually_installed, install_path);
            upgraded_count++;
        } catch (const LpkgException& e) {
            log_error(string_format("error.upgrade_failed", pkg_name, e.what()));
            log_info(string_format("info.restoring_package", pkg_name, current_version));
            try {
                std::vector<std::string> install_path;
                do_install(pkg_name, current_version, is_manually_installed(pkg_name), install_path);
            } catch (const LpkgException& e2) {
                log_error("Fatal: Failed to restore package " + pkg_name + ": " + e2.what());
            }
        }
    }

    if (upgraded_count > 0) {
        log_info(string_format("info.upgraded_packages", upgraded_count));
    }
}

void show_man_page(const std::string& pkg_name) {
    const fs::path man_file_path = DOCS_DIR / (pkg_name + ".man");
    if (!fs::exists(man_file_path)) {
        throw LpkgException(string_format("error.no_man_page", pkg_name));
    }

    std::ifstream man_file(man_file_path);
    if (!man_file.is_open()) {
        throw LpkgException(string_format("error.open_man_page_failed", man_file_path.string()));
    }

    std::cout << man_file.rdbuf();
}