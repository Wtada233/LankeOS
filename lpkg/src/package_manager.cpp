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
#include <memory>
#include <tuple>
#include <unordered_set>
#include <vector>
#include <future>

namespace fs = std::filesystem;

namespace {

// --- Caching --- //
struct Cache {
    std::map<std::string, std::unordered_set<std::string>> file_db;
    std::unordered_set<std::string> pkgs;
    std::unordered_set<std::string> holdpkgs;
    bool dirty = false;

    void load() {
        file_db = read_file_db_uncached();
        pkgs = read_set_from_file(PKGS_FILE);
        holdpkgs = read_set_from_file(HOLDPKGS_FILE);
        dirty = false;
    }

    void write_pkgs() {
        write_set_to_file(PKGS_FILE, pkgs);
    }

    void write_holdpkgs() {
        write_set_to_file(HOLDPKGS_FILE, holdpkgs);
    }

    void write_file_db() {
        write_file_db_uncached(file_db);
    }

private:
    std::map<std::string, std::unordered_set<std::string>> read_file_db_uncached() {
        std::map<std::string, std::unordered_set<std::string>> db;
        std::ifstream db_file(FILES_DB);
        if (!db_file.is_open()) {
            throw LpkgException(string_format("error.open_file_failed", FILES_DB.string()));
        }
        std::string path, owner;
        while (db_file >> path >> owner) {
            db[path].insert(owner);
        }
        return db;
    }

    void write_file_db_uncached(const std::map<std::string, std::unordered_set<std::string>>& db) {
        std::ofstream db_file(FILES_DB, std::ios::trunc);
        if (!db_file.is_open()) {
            throw LpkgException(string_format("error.create_file_failed", FILES_DB.string()));
        }
        for (const auto& [path, owners] : db) {
            for (const auto& owner : owners) {
                db_file << path << " " << owner << "\n";
            }
        }
    }
};

Cache& get_cache() {
    static Cache cache;
    static bool loaded = false;
    if (!loaded) {
        cache.load();
        loaded = true;
    }
    return cache;
}


// Forward declarations
void do_install(const std::string& pkg_name, const std::string& version, bool explicit_install, std::vector<std::string>& install_path);
std::string get_installed_version(const std::string& pkg_name);
bool is_manually_installed(const std::string& pkg_name);

// --- Database Helper Functions ---
std::map<std::string, std::unordered_set<std::string>>& read_file_db() {
    return get_cache().file_db;
}

void mark_cache_dirty() {
    get_cache().dirty = true;
}

// --- Other Helper Functions ---
std::string get_installed_version(const std::string& pkg_name) {
    for (const auto& pkg : get_cache().pkgs) {
        if (pkg.starts_with(pkg_name + ":")) {
            return pkg.substr(pkg_name.length() + 1);
        }
    }
    return "";
}

bool is_manually_installed(const std::string& pkg_name) {
    return get_cache().holdpkgs.contains(pkg_name);
}

} // anonymous namespace

// --- InstallationTask Class Implementation ---

InstallationTask::InstallationTask(std::string pkg_name, std::string version, bool explicit_install, std::vector<std::string>& install_path)
    : pkg_name_(std::move(pkg_name)),
      version_(std::move(version)),
      explicit_install_(explicit_install),
      install_path_(install_path),
      tmp_pkg_dir_(get_tmp_dir() / pkg_name_) {}

void InstallationTask::run() {
    if (!get_installed_version(pkg_name_).empty()) {
        log_info(string_format("info.package_already_installed", pkg_name_));
        return;
    }

    log_info(string_format("info.installing_package", pkg_name_, version_));
    install_path_.push_back(pkg_name_);

    fs::remove_all(tmp_pkg_dir_);
    ensure_dir_exists(tmp_pkg_dir_);

    auto cleanup = [&](const void*) { fs::remove_all(tmp_pkg_dir_); };
    std::unique_ptr<const void, decltype(cleanup)> tmp_dir_guard(tmp_pkg_dir_.c_str(), cleanup);

    try {
        download_and_verify_package();
        extract_and_validate_package();
        resolve_dependencies();

        if (!get_installed_version(pkg_name_).empty()) {
            log_warning(string_format("warning.skip_already_installed", pkg_name_));
            install_path_.pop_back();
            return;
        }

        check_for_file_conflicts();
        copy_package_files();
        register_package();

    } catch (...) {
        install_path_.pop_back();
        throw;
    }

    install_path_.pop_back();
    log_info(string_format("info.package_installed_successfully", pkg_name_));
}

void InstallationTask::download_and_verify_package() {
    std::string mirror_url = get_mirror_url();
    std::string arch = get_architecture();

    actual_version_ = version_;
    if (version_ == "latest") {
        actual_version_ = get_latest_version(pkg_name_);
        log_info(string_format("info.latest_version", actual_version_));
    }

    const std::string download_url = mirror_url + arch + "/" + pkg_name_ + "/" + actual_version_ + "/app.tar.zst";
    archive_path_ = tmp_pkg_dir_ / "app.tar.zst";
    const std::string hash_url = mirror_url + arch + "/" + pkg_name_ + "/" + actual_version_ + "/hash.txt";
    const fs::path hash_path = tmp_pkg_dir_ / "hash.txt";

    log_info(string_format("info.downloading_from", download_url));

    download_with_retries(hash_url, hash_path, 5, false);
    std::ifstream hash_file(hash_path);
    std::string expected_hash;
    if (!(hash_file >> expected_hash)) {
        throw LpkgException(string_format("error.hash_download_failed", hash_url));
    }
    hash_file.close();

    download_with_retries(download_url, archive_path_, 5, true);

    std::string actual_hash = calculate_sha256(archive_path_);
    if (expected_hash != actual_hash) {
        throw LpkgException(string_format("error.hash_mismatch", pkg_name_));
    }
}

void InstallationTask::extract_and_validate_package() {
    log_info(get_string("info.extracting_to_tmp"));
    extract_tar_zst(archive_path_, tmp_pkg_dir_);

    std::vector<fs::path> required_files = {"man.txt", "deps.txt", "files.txt", "content/"};
    for (const auto& file : required_files) {
        if (!fs::exists(tmp_pkg_dir_ / file)) {
            throw LpkgException(string_format("error.incomplete_package", (tmp_pkg_dir_ / file).string()));
        }
    }
}

void InstallationTask::resolve_dependencies() {
    log_info(get_string("info.checking_deps"));
    std::ifstream deps_file(tmp_pkg_dir_ / "deps.txt");
    std::string dep;
    while (std::getline(deps_file, dep)) {
        if (dep.empty()) continue;

        if (std::find(install_path_.begin(), install_path_.end(), dep) != install_path_.end()) {
            log_warning(string_format("warning.circular_dependency", pkg_name_, dep));
            continue;
        }

        log_info(string_format("info.dep_found", dep));
        if (get_installed_version(dep).empty()) {
            log_info(string_format("info.dep_not_installed", dep));
            do_install(dep, "latest", false, install_path_);
        } else {
            log_info(string_format("info.dep_already_installed", dep));
        }
    }
}

void InstallationTask::check_for_file_conflicts() {
    log_info(get_string("info.checking_for_file_conflicts"));
    std::map<std::string, std::string> conflicts;
    std::ifstream files_list(tmp_pkg_dir_ / "files.txt");
    std::string src, dest;
    auto& db = read_file_db();

    while (files_list >> src >> dest) {
        fs::path dest_path = fs::path(dest) / src;
        auto it = db.find(dest_path.string());
        if (it != db.end()) {
            for (const auto& owner : it->second) {
                if (owner != pkg_name_) {
                    conflicts[dest_path.string()] = owner;
                    break;
                }
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

void InstallationTask::copy_package_files() {
    log_info(get_string("info.copying_files"));
    std::ifstream files_list(tmp_pkg_dir_ / "files.txt");
    std::string src, dest;
    int file_count = 0;
    std::vector<fs::path> installed_files;
    std::set<fs::path> created_dirs_for_rollback;

    auto rollback = [&](const void*) {
        log_error("Rolling back installation of " + pkg_name_);
        for (const auto& file : installed_files) {
            try {
                fs::remove(file);
            } catch (const fs::filesystem_error& e) {
                log_warning(string_format("warning.remove_file_failed", file.string(), e.what()));
            }
        }
        // Attempt to remove created directories, from deepest to shallowest
        std::vector<fs::path> sorted_dirs(created_dirs_for_rollback.begin(), created_dirs_for_rollback.end());
        std::sort(sorted_dirs.rbegin(), sorted_dirs.rend());
        for (const auto& dir : sorted_dirs) {
            if (fs::exists(dir) && fs::is_directory(dir) && fs::is_empty(dir)) {
                try {
                    fs::remove(dir);
                } catch (const fs::filesystem_error& e) {
                    log_warning(string_format("warning.remove_file_failed", dir.string(), e.what()));
                }
            }
        }
    };
    std::unique_ptr<const void, decltype(rollback)> rollback_guard(nullptr, rollback);

    while (files_list >> src >> dest) {
        const fs::path src_path = tmp_pkg_dir_ / "content" / src;
        const fs::path dest_path = fs::path(dest) / src;

        if (!fs::exists(src_path)) {
            log_warning(string_format("error.incomplete_package", src));
            continue;
        }

        fs::path current_dest_parent = dest_path.parent_path();
        while (!current_dest_parent.empty() && !fs::exists(current_dest_parent)) {
            created_dirs_for_rollback.insert(current_dest_parent);
            current_dest_parent = current_dest_parent.parent_path();
        }
        ensure_dir_exists(dest_path.parent_path());

        try {
            fs::copy(src_path, dest_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            file_count++;
            installed_files.push_back(dest_path);
        } catch (const fs::filesystem_error& e) {
            throw LpkgException(string_format("error.copy_failed_rollback", src_path.string(), dest_path.string(), e.what()));
        }
    }
    log_info(string_format("info.copy_complete", file_count));

    // Now register the copied files for this package
    std::ofstream pkg_files(FILES_DIR / (pkg_name_ + ".txt"));
    if (!pkg_files.is_open()) {
        throw LpkgException(string_format("error.create_file_failed", (FILES_DIR / (pkg_name_ + ".txt")).string()));
    }
    for(const auto& file : installed_files) {
        pkg_files << file.string() << "\n";
    }
    std::ofstream dirs_file(FILES_DIR / (pkg_name_ + ".dirs"));
    if (!dirs_file.is_open()) {
        throw LpkgException(string_format("error.create_file_failed", (FILES_DIR / (pkg_name_ + ".dirs")).string()));
    }
    for (const auto& dir : created_dirs_for_rollback) {
        dirs_file << dir.string() << "\n";
    }
}

void InstallationTask::register_package() {
    // Record dependencies
    std::ifstream deps_file_src(tmp_pkg_dir_ / "deps.txt");
    std::ofstream pkg_deps_dest(DEP_DIR / pkg_name_);
    std::string dep;
    while (std::getline(deps_file_src, dep)) {
        if (!dep.empty()) {
            pkg_deps_dest << dep << "\n";
        }
    }

    // Update file ownership database
    auto& db = read_file_db();
    std::ifstream files_list(FILES_DIR / (pkg_name_ + ".txt"));
    std::string file_path;
    while (std::getline(files_list, file_path)) {
        if (!file_path.empty()) {
            db[file_path].insert(pkg_name_);
        }
    }
    mark_cache_dirty();

    // Copy man page
    fs::copy(tmp_pkg_dir_ / "man.txt", DOCS_DIR / (pkg_name_ + ".man"), fs::copy_options::overwrite_existing);

    // Add to main package list
    get_cache().pkgs.insert(pkg_name_ + ":" + actual_version_);
    mark_cache_dirty();

    // Add to manually installed list if needed
    if (explicit_install_) {
        get_cache().holdpkgs.insert(pkg_name_);
        mark_cache_dirty();
    }
}


// --- Public API Functions ---

namespace {

void do_install(const std::string& pkg_name, const std::string& version, bool explicit_install, std::vector<std::string>& install_path) {
    InstallationTask task(pkg_name, version, explicit_install, install_path);
    task.run();
}

std::unordered_set<std::string> get_all_required_packages() {
    auto manually_installed = get_cache().holdpkgs;
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

void install_package(const std::string& pkg_name, const std::string& version) {
    std::vector<std::string> install_path;
    do_install(pkg_name, version, true, install_path);
}

void remove_package(const std::string& pkg_name, bool force) {
    std::string installed_version = get_installed_version(pkg_name);
    if (installed_version.empty()) {
        log_info(string_format("info.package_not_installed", pkg_name));
        return;
    }

    if (!force) {
        // Check if other packages depend on this one
        for (const auto& entry : fs::directory_iterator(DEP_DIR)) {
            if (entry.is_directory()) continue;
            std::string current_pkg_name = entry.path().stem().string();
            if (current_pkg_name == pkg_name) continue;

            std::ifstream file(entry.path());
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
        
        std::map<std::string, std::vector<std::string>> shared_files;
        auto& db = get_cache().file_db;

        std::ifstream files_to_check(files_list_path);
        if (!files_to_check.is_open()) {
            throw LpkgException(string_format("error.open_file_failed", files_list_path.string()));
        }
        std::string file_to_check;
        while (std::getline(files_to_check, file_to_check)) {
            if (file_to_check.empty()) continue;
            auto it = db.find(file_to_check);
            if (it != db.end()) {
                // If the file is owned by other packages, add to shared_files
                for (const auto& owner : it->second) {
                    if (owner != pkg_name) {
                        shared_files[file_to_check].push_back(owner);
                    }
                }
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

        // No shared files, proceed with removal
        std::ifstream files_list(files_list_path);
        if (!files_list.is_open()) {
            throw LpkgException(string_format("error.open_file_failed", files_list_path.string()));
        }
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

        // Update file ownership database
        auto& db_to_update = get_cache().file_db;
        for (const auto& path : file_paths) {
            auto it = db_to_update.find(path.string());
            if (it != db_to_update.end()) {
                it->second.erase(pkg_name);
                if (it->second.empty()) {
                    db_to_update.erase(it);
                }
            }
        }
        mark_cache_dirty();
    }

    const fs::path dirs_list_path = FILES_DIR / (pkg_name + ".dirs");
    if (fs::exists(dirs_list_path)) {
        std::ifstream dirs_list(dirs_list_path);
        if (!dirs_list.is_open()) {
            throw LpkgException(string_format("error.open_file_failed", dirs_list_path.string()));
        }
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

    get_cache().pkgs.erase(pkg_name + ":" + installed_version);
    mark_cache_dirty();

    if (get_cache().holdpkgs.contains(pkg_name)) {
        get_cache().holdpkgs.erase(pkg_name);
        mark_cache_dirty();
    }

    log_info(string_format("info.package_removed_successfully", pkg_name));
}

void autoremove() {
    log_info(get_string("info.checking_autoremove"));
    
    auto required_pkgs = get_all_required_packages();
    std::vector<std::string> packages_to_remove;

    for (const auto& record : get_cache().pkgs) {
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

    fs::remove_all(get_tmp_dir());
    ensure_dir_exists(get_tmp_dir());
}

void upgrade_packages() {
    log_info(get_string("info.checking_upgradable"));
    int upgraded_count = 0;
    std::vector<std::tuple<std::string, std::string, std::string>> upgradable_pkgs;
    std::vector<std::future<std::string>> futures;

    auto& pkgs = get_cache().pkgs;
    for (const auto& pkg : pkgs) {
        futures.push_back(std::async(std::launch::async, [pkg] {
            size_t pos = pkg.find(':');
            if (pos == std::string::npos) return std::string();

            std::string pkg_name = pkg.substr(0, pos);
            std::string current_version = pkg.substr(pos + 1);

            try {
                std::string latest_version = get_latest_version(pkg_name);
                if (version_compare(current_version, latest_version)) {
                    return pkg_name + ":" + current_version + ":" + latest_version;
                }
            } catch (...) {
                log_warning(string_format("warning.get_latest_version_failed", pkg_name));
            }
            return std::string();
        }));
    }

    for (auto& fut : futures) {
        std::string result = fut.get();
        if (!result.empty()) {
            size_t pos1 = result.find(':');
            size_t pos2 = result.rfind(':');
            std::string pkg_name = result.substr(0, pos1);
            std::string current_version = result.substr(pos1 + 1, pos2 - pos1 - 1);
            std::string latest_version = result.substr(pos2 + 1);
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

void write_cache() {
    auto& cache = get_cache();
    if (cache.dirty) {
        cache.write_file_db();
        cache.write_pkgs();
        cache.write_holdpkgs();
        cache.dirty = false;
    }
}