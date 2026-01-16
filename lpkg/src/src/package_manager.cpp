#include "package_manager.hpp"

#include "archive.hpp"
#include "config.hpp"
#include "downloader.hpp"
#include "exception.hpp"
#include "hash.hpp"
#include "localization.hpp"
#include "utils.hpp"
#include "version.hpp"
#include "repository.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_set>
#include <vector>
#include <future>
#include <cstdlib>
#include <cstring>
#include <cerrno>

namespace fs = std::filesystem;

namespace {

// --- Caching --- //
struct Cache {
    std::map<std::string, std::unordered_set<std::string>> file_db;
    std::map<std::string, std::unordered_set<std::string>> providers; // virtual_pkg -> {real_pkgs}
    std::unordered_set<std::string> pkgs;
    std::unordered_set<std::string> holdpkgs;
    bool dirty = false;
    std::mutex mtx;

    void load() {
        file_db = read_db_uncached(FILES_DB);
        providers = read_db_uncached(PROVIDES_DB);
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
        write_db_uncached(FILES_DB, file_db);
    }

    void write_providers() {
        write_db_uncached(PROVIDES_DB, providers);
    }

private:
    std::map<std::string, std::unordered_set<std::string>> read_db_uncached(const fs::path& path) {
        std::map<std::string, std::unordered_set<std::string>> db;
        std::ifstream db_file(path);
        if (!db_file.is_open()) {
            throw LpkgException(string_format("error.open_file_failed", path.string()));
        }
        std::string key, value;
        while (db_file >> key >> value) {
            db[key].insert(value);
        }
        return db;
    }

    void write_db_uncached(const fs::path& path, const std::map<std::string, std::unordered_set<std::string>>& db) {
        std::ofstream db_file(path, std::ios::trunc);
        if (!db_file.is_open()) {
            throw LpkgException(string_format("error.create_file_failed", path.string()));
        }
        for (const auto& [key, values] : db) {
            for (const auto& value : values) {
                db_file << key << " " << value << "\n";
            }
        }
    }
};

Cache& get_cache() {
    static Cache cache;
    static bool loaded = false;

    if (!loaded) {
        std::lock_guard<std::mutex> lock(cache.mtx);
        if (!loaded) { // Double-check after acquiring the lock
            try {
                cache.load();
                loaded = true;
            } catch (const LpkgException& e) {
                log_error(string_format("error.lpkg_error", e.what()));
                throw;
            }
        }
    }
    return cache;
}


// Forward declarations
void do_install(const std::string& pkg_name, const std::string& version, bool explicit_install, std::vector<std::string>& install_path, const std::string& old_version);
std::string get_installed_version(const std::string& pkg_name);
bool is_manually_installed(const std::string& pkg_name);
void run_hook(const std::string& pkg_name, const std::string& hook_name);

// --- Helper Functions ---

// In get_cache, we might need a way to force reload for tests or ensure consistency
void force_reload_cache() {
    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);
    cache.load();
}

// --- Other Helper Functions ---
std::string get_installed_version(const std::string& pkg_name) {
    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);
    // Check real packages
    for (const auto& pkg : cache.pkgs) {
        if (pkg.starts_with(pkg_name + ":")) {
            return pkg.substr(pkg_name.length() + 1);
        }
    }
    // Check virtual packages
    if (cache.providers.count(pkg_name)) {
        return "virtual";
    }
    return "";
}

bool is_manually_installed(const std::string& pkg_name) {
    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);
    return cache.holdpkgs.contains(pkg_name);
}

} // anonymous namespace

// --- InstallationTask Class Implementation ---

InstallationTask::InstallationTask(std::string pkg_name, std::string version, bool explicit_install, std::vector<std::string>& install_path, std::string old_version_to_replace, fs::path local_package_path, std::string expected_hash)
    : pkg_name_(std::move(pkg_name)),
      version_(std::move(version)),
      explicit_install_(explicit_install),
      install_path_(install_path),
      tmp_pkg_dir_(get_tmp_dir() / pkg_name_),
      actual_version_(version_), // Initialize actual_version_ with version_
      old_version_to_replace_(std::move(old_version_to_replace)),
      local_package_path_(std::move(local_package_path)),
      expected_hash_(std::move(expected_hash)) {}

void InstallationTask::run() {
    std::string current_installed_version = get_installed_version(pkg_name_);
    if (!current_installed_version.empty() && current_installed_version == actual_version_) {
        log_info(string_format("info.package_already_installed", pkg_name_.c_str()));
        return;
    }

    log_info(string_format("info.installing_package", pkg_name_.c_str(), version_.c_str()));
    install_path_.push_back(pkg_name_);

    ensure_dir_exists(tmp_pkg_dir_);

    try {
        prepare(); // Phase 1: Prepare everything without modifying the system

        // Re-check if package was installed during dependency resolution
        if (!get_installed_version(pkg_name_).empty() && get_installed_version(pkg_name_) == actual_version_) {
            log_warning(string_format("warning.skip_already_installed", pkg_name_.c_str()));
            install_path_.pop_back();
            return;
        }

        commit(); // Phase 2: Apply changes to the system

    } catch (const LpkgException& e) {
        install_path_.pop_back();
        // If an old version was being replaced, attempt to restore it
        if (!old_version_to_replace_.empty()) {
            log_info(string_format("info.restoring_package", pkg_name_.c_str(), old_version_to_replace_.c_str()));
            try {
                std::vector<std::string> restore_path; // Use a new path for restoration to avoid circularity
                do_install(pkg_name_, old_version_to_replace_, explicit_install_, restore_path, "");
            } catch (const LpkgException& e2) {
                log_error(string_format("error.fatal_restore_failed", pkg_name_.c_str(), e2.what()));
            }
        }
        throw; // Re-throw the original exception
    } catch (...) {
        install_path_.pop_back();
        throw;
    }

    install_path_.pop_back();
    log_info(string_format("info.package_installed_successfully", pkg_name_.c_str()));
}

void InstallationTask::prepare() {
    download_and_verify_package();
    extract_and_validate_package();
    resolve_dependencies();
    check_for_file_conflicts();
}

void InstallationTask::commit() {
    // Logic for safe upgrade:
    // 1. Identify files from old version (if exists)
    // 2. Copy new files (InstallationTask::copy_package_files handles backups of overwrites)
    // 3. Register new package
    // 4. Remove files that were in old version but NOT in new version
    
    std::unordered_set<std::string> old_files;
    if (!old_version_to_replace_.empty()) {
        fs::path old_files_list_path = FILES_DIR / (pkg_name_ + ".txt");
        if (fs::exists(old_files_list_path)) {
            std::ifstream f(old_files_list_path);
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty()) old_files.insert(line);
            }
        }
    }

    // This will overwrite files and create backups.
    // If it fails, it rolls back (restores backups and deletes new files).
    // Note: The old files list file (pkg.txt) will be overwritten here.
    copy_package_files();
    
    register_package();
    
    // Prune obsolete files
    if (!old_version_to_replace_.empty()) {
        std::unordered_set<std::string> new_files;
        fs::path new_files_list_path = FILES_DIR / (pkg_name_ + ".txt");
        if (fs::exists(new_files_list_path)) {
            std::ifstream f(new_files_list_path);
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty()) new_files.insert(line);
            }
        }

        for (const auto& old_file : old_files) {
            if (new_files.find(old_file) == new_files.end()) {
                // File was in old version but not in new version. Remove it.
                // We should check if it's owned by another package? 
                // remove_package_files logic does that.
                // But here we are just pruning specific files.
                // Let's assume strict ownership for now or check DB?
                // The DB has already been updated in register_package()!
                // So if we check DB for this file, it won't point to us anymore.
                // If no one owns it, delete it.
                
                auto& cache = get_cache();
                std::lock_guard<std::mutex> lock(cache.mtx);
                auto& db = cache.file_db;
                if (db.find(old_file) == db.end()) {
                     fs::path physical_path = ROOT_DIR / old_file;
                     if (fs::exists(physical_path)) {
                         log_info("Removing obsolete file: " + old_file);
                         fs::remove(physical_path);
                     }
                }
            }
        }
    }

    run_post_install_hook();
}

void InstallationTask::download_and_verify_package() {
    actual_version_ = version_;

    if (!local_package_path_.empty()) {
        if (!fs::exists(local_package_path_)) {
            throw LpkgException("Local package file not found: " + local_package_path_.string());
        }
        log_info("Installing from local file: " + local_package_path_.string());
        archive_path_ = local_package_path_;
        
        // Skip hash verification for local files as we trust the user provided file
        // or we could calculate it but we have no expected hash to compare against.
        return;
    }

    std::string mirror_url = get_mirror_url();
    std::string arch = get_architecture();

    if (version_ == "latest") {
        actual_version_ = get_latest_version(pkg_name_);
        log_info(string_format("info.latest_version", actual_version_.c_str()));
    }

    const std::string download_url = mirror_url + arch + "/" + pkg_name_ + "/" + actual_version_ + "/app.tar.zst";
    archive_path_ = tmp_pkg_dir_ / "app.tar.zst";
    
    std::string expected_hash;
    if (!expected_hash_.empty()) {
        expected_hash = expected_hash_;
        log_info(get_string("info.verifying_provided_hash"));
    } else {
        const std::string hash_url = mirror_url + arch + "/" + pkg_name_ + "/" + actual_version_ + "/hash.txt";
        const fs::path hash_path = tmp_pkg_dir_ / "hash.txt";
        log_info(string_format("info.downloading_from", download_url.c_str()));

        download_with_retries(hash_url, hash_path, 5, false);
        std::ifstream hash_file(hash_path);
        if (!(hash_file >> expected_hash)) {
            throw LpkgException(string_format("error.hash_download_failed", hash_url.c_str()));
        }
        hash_file.close();
    }

    if (!fs::exists(archive_path_)) { // Check if already downloaded (by plan preparation maybe? No, plan doesn't download now)
        // If expected_hash was provided, we might still need to download
        if (expected_hash_.empty()) {
             // If we just downloaded hash, we still need to download app.
             // Wait, the original code downloaded hash then app.
             // If expected_hash_ is provided, we skip hash download, but we MUST download app.
        }
        log_info(string_format("info.downloading_from", download_url.c_str()));
        download_with_retries(download_url, archive_path_, 5, true);
    }

    std::string actual_hash = calculate_sha256(archive_path_);
    if (expected_hash != actual_hash) {
        throw LpkgException(string_format("error.hash_mismatch", pkg_name_.c_str()));
    }
}

void InstallationTask::extract_and_validate_package() {
    log_info(get_string("info.extracting_to_tmp"));
    extract_tar_zst(archive_path_, tmp_pkg_dir_);

    std::vector<fs::path> required_files = {"man.txt", "deps.txt", "files.txt", "content/"};
    for (const auto& file : required_files) {
        if (!fs::exists(tmp_pkg_dir_ / file)) {
            throw LpkgException(string_format("error.incomplete_package", (tmp_pkg_dir_ / file).string().c_str()));
        }
    }
    // provides.txt is optional
}

void InstallationTask::resolve_dependencies() {
    log_info(get_string("info.checking_deps"));
    std::ifstream deps_file(tmp_pkg_dir_ / "deps.txt");
    std::string line;
    while (std::getline(deps_file, line)) {
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string dep_name, op, req_ver;
        ss >> dep_name;
        
        // Check for version constraints
        bool has_constraint = false;
        if (ss >> op >> req_ver) {
            has_constraint = true;
        }
        
        if (std::find(install_path_.begin(), install_path_.end(), dep_name) != install_path_.end()) {
            log_warning(string_format("warning.circular_dependency", pkg_name_.c_str(), dep_name.c_str()));
            continue;
        }

        log_info(string_format("info.dep_found", dep_name.c_str()));
        std::string installed_ver = get_installed_version(dep_name);
        
        if (installed_ver.empty()) {
            log_info(string_format("info.dep_not_installed", dep_name.c_str()));
            // TODO: If remote, we could try to find a version matching requirement.
            // For now, install latest and check later (or here if we had metadata).
            do_install(dep_name, "latest", false, install_path_, "");
            
            // Re-check after install
            installed_ver = get_installed_version(dep_name);
            if (has_constraint && !installed_ver.empty() && installed_ver != "virtual") {
                 if (!version_satisfies(installed_ver, op, req_ver)) {
                     throw LpkgException("Dependency " + dep_name + " installed version " + installed_ver + " does not satisfy constraint " + op + " " + req_ver);
                 }
            }
        } else {
            log_info(string_format("info.dep_already_installed", dep_name.c_str()));
            if (has_constraint && installed_ver != "virtual") {
                 bool sat = version_satisfies(installed_ver, op, req_ver);
                 if (!sat) {
                     // TODO: Trigger upgrade if possible? For now, error out.
                     throw LpkgException("Installed dependency " + dep_name + " version " + installed_ver + " does not satisfy constraint " + op + " " + req_ver);
                 }
            }
        }
    }
}

void InstallationTask::check_for_file_conflicts() {
    log_info(get_string("info.checking_for_file_conflicts"));
    std::map<std::string, std::string> conflicts;
    std::ifstream files_list(tmp_pkg_dir_ / "files.txt");
    std::string src, dest;
    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);
    auto& db = cache.file_db;

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
            error_msg += "  " + string_format("error.file_conflict_entry", file.c_str(), owner.c_str()) + "\n";
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
    std::vector<std::pair<fs::path, fs::path>> backups; // <installed_path, backup_path>

    auto rollback = [&](const void*) {
        log_error(string_format("error.rollback_install", pkg_name_.c_str()));
        
        // 1. Remove new files
        for (const auto& file : installed_files) {
            try {
                if (fs::exists(file)) fs::remove(file);
            } catch (const fs::filesystem_error& e) {
                log_warning(string_format("warning.remove_file_failed", file.string().c_str(), e.what()));
            }
        }
        
        // 2. Restore backups
        for (const auto& backup : backups) {
            try {
                fs::rename(backup.second, backup.first);
            } catch (const fs::filesystem_error& e) {
                log_error(string_format("error.restore_backup_failed", backup.second.string().c_str(), backup.first.string().c_str(), e.what()));
            }
        }
        // Cleanup remaining backups (if any restore failed, we might still have them, but we try to restore all)
        // Note: The loop above tries to restore. 

        // 3. Remove created directories
        std::vector<fs::path> sorted_dirs(created_dirs_for_rollback.begin(), created_dirs_for_rollback.end());
        std::sort(sorted_dirs.rbegin(), sorted_dirs.rend());
        for (const auto& dir : sorted_dirs) {
            if (fs::exists(dir) && fs::is_directory(dir) && fs::is_empty(dir)) {
                try {
                    fs::remove(dir);
                } catch (const fs::filesystem_error& e) {
                    log_warning(string_format("warning.remove_file_failed", dir.string().c_str(), e.what()));
                }
            }
        }
    };
    std::unique_ptr<const void, decltype(rollback)> rollback_guard(this, rollback);

    while (files_list >> src >> dest) {
        const fs::path src_path = tmp_pkg_dir_ / "content" / src;
        const fs::path logical_dest_path = fs::path(dest) / src;
        
        // Handle Sysroot: If ROOT_DIR is set, rebase the destination
        fs::path physical_dest_path;
        if (logical_dest_path.is_absolute()) {
             physical_dest_path = ROOT_DIR / logical_dest_path.relative_path();
        } else {
             physical_dest_path = ROOT_DIR / logical_dest_path;
        }

        if (!fs::exists(src_path)) {
            log_warning(string_format("error.incomplete_package", src.c_str()));
            continue;
        }

        fs::path current_dest_parent = physical_dest_path.parent_path();
        std::vector<fs::path> dirs_to_create;
        while (!current_dest_parent.empty() && !fs::exists(current_dest_parent)) {
            dirs_to_create.push_back(current_dest_parent);
            current_dest_parent = current_dest_parent.parent_path();
        }
        std::reverse(dirs_to_create.begin(), dirs_to_create.end()); // Create from shallowest to deepest
        for (const auto& dir : dirs_to_create) {
            ensure_dir_exists(dir);
            created_dirs_for_rollback.insert(dir);
        }

        try {
            // Backup if exists
            if (fs::exists(physical_dest_path) && !fs::is_directory(physical_dest_path)) {
                fs::path backup_path = physical_dest_path;
                backup_path += ".lpkg_bak";
                fs::copy_file(physical_dest_path, backup_path, fs::copy_options::overwrite_existing); // copy first to be safe, or rename?
                // Rename is atomic on same FS. Copy+Delete is safer if rename fails?
                // Let's use rename if possible, but fallback to copy.
                // Actually, standard upgrade safety usually involves: 
                // 1. Write new file to .tmp
                // 2. fsync
                // 3. Rename old to .bak
                // 4. Rename .tmp to new
                // For now, simpler: Rename existing to .bak
                fs::rename(physical_dest_path, backup_path);
                backups.emplace_back(physical_dest_path, backup_path);
            }
            
            fs::copy(src_path, physical_dest_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks);
            file_count++;
            installed_files.push_back(physical_dest_path); // For rollback
            // We need to keep track of logical paths for the database
        } catch (const fs::filesystem_error& e) {
            throw LpkgException(string_format("error.copy_failed_rollback", src_path.string().c_str(), physical_dest_path.string().c_str(), e.what()));
        }
    }
    
    // Success: Remove backups
    for (const auto& backup : backups) {
        std::error_code ec;
        fs::remove(backup.second, ec);
    }
    rollback_guard.release(); // Cancel rollback

    log_info(string_format("info.copy_complete", file_count));

    // Now register the copied files for this package
    std::ofstream pkg_files(FILES_DIR / (pkg_name_ + ".txt"));
    if (!pkg_files.is_open()) {
        throw LpkgException(string_format("error.create_file_failed", (FILES_DIR / (pkg_name_ + ".txt")).string().c_str()));
    }
    
    // Re-iterate to save logical paths, or we should have saved them.
    // Iterating files_list again is safe since we just read it.
    {
        std::ifstream files_list_again(tmp_pkg_dir_ / "files.txt");
        std::string s, d;
        while (files_list_again >> s >> d) {
             fs::path logical = fs::path(d) / s;
             pkg_files << logical.string() << "\n";
        }
    }

    std::ofstream dirs_file(FILES_DIR / (pkg_name_ + ".dirs"));
    if (!dirs_file.is_open()) {
        throw LpkgException(string_format("error.create_file_failed", (FILES_DIR / (pkg_name_ + ".dirs")).string().c_str()));
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

    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);

    // Update file ownership database
    auto& db = cache.file_db;
    std::ifstream files_list(FILES_DIR / (pkg_name_ + ".txt"));
    std::string file_path;
    while (std::getline(files_list, file_path)) {
        if (!file_path.empty()) {
            db[file_path].insert(pkg_name_);
        }
    }

    // Copy man page
    fs::copy(tmp_pkg_dir_ / "man.txt", DOCS_DIR / (pkg_name_ + ".man"), fs::copy_options::overwrite_existing);

    // Add to main package list
    cache.pkgs.insert(pkg_name_ + ":" + actual_version_);
    if (!old_version_to_replace_.empty()) {
        cache.pkgs.erase(pkg_name_ + ":" + old_version_to_replace_);
    }

    // Handle provides
    std::ifstream provides_file(tmp_pkg_dir_ / "provides.txt");
    std::ofstream pkg_provides_dest(FILES_DIR / (pkg_name_ + ".provides"));
    if (provides_file.is_open()) {
        std::string capability;
        while (std::getline(provides_file, capability)) {
            if (!capability.empty()) {
                cache.providers[capability].insert(pkg_name_);
                pkg_provides_dest << capability << "\n";
            }
        }
    }

    // Add to manually installed list if needed
    if (explicit_install_) {
        cache.holdpkgs.insert(pkg_name_);
    }

    cache.dirty = true;
}

void InstallationTask::run_post_install_hook() {
    fs::path hook_src_dir = tmp_pkg_dir_ / "hooks";
    if (!fs::exists(hook_src_dir) || !fs::is_directory(hook_src_dir)) {
        return; // No hooks directory, nothing to do.
    }

    fs::path pkg_hook_dest_dir = HOOKS_DIR / pkg_name_;
    ensure_dir_exists(pkg_hook_dest_dir);

    for (const auto& entry : fs::directory_iterator(hook_src_dir)) {
        const auto& path = entry.path();
        if (entry.is_regular_file()) {
            fs::path dest_path = pkg_hook_dest_dir / path.filename();
            try {
                fs::copy(path, dest_path, fs::copy_options::overwrite_existing);
                fs::permissions(dest_path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add);
            } catch (const fs::filesystem_error& e) {
                log_warning(string_format("warning.hook_failed_setup", path.filename().string().c_str(), e.what()));
            }
        }
    }

    run_hook(pkg_name_, "postinst.sh");
}


// --- Public API Functions ---

namespace {

void do_install(const std::string& pkg_name, const std::string& version, bool explicit_install, std::vector<std::string>& install_path, const std::string& old_version = "") {
    InstallationTask task(pkg_name, version, explicit_install, install_path, old_version);
    task.run();
}

std::unordered_set<std::string> get_all_required_packages() {
    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);
    auto manually_installed = cache.holdpkgs;
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

#include <sys/wait.h> // for waitpid

void run_hook(const std::string& pkg_name, const std::string& hook_name) {
    fs::path hook_path = HOOKS_DIR / pkg_name / hook_name;
    if (fs::exists(hook_path) && fs::is_regular_file(hook_path)) {
        log_info(string_format("info.running_hook", hook_name.c_str()));

        // Check if we need chroot
        bool use_chroot = false;
        if (ROOT_DIR != "/" && !ROOT_DIR.empty()) {
            use_chroot = true;
        }

        if (use_chroot) {
            // Need to fork to chroot safely
            pid_t pid = fork();
            if (pid == -1) {
                log_warning(string_format("error.hook_fork_failed", std::string(strerror(errno))));
                return;
            } else if (pid == 0) {
                // Child
                if (chroot(ROOT_DIR.c_str()) != 0) {
                    std::cerr << string_format("error.hook_chroot_failed", ROOT_DIR.string().c_str(), strerror(errno)) << std::endl;
                    _exit(1);
                }
                if (chdir("/") != 0) {
                     std::cerr << string_format("error.hook_chdir_failed", strerror(errno)) << std::endl;
                     _exit(1);
                }
                // Hook path inside chroot (HOOKS_DIR is logical path relative to config, but config is rebased in set_root_path)
                // Wait, HOOKS_DIR variable in config.cpp is absolute path including ROOT_DIR.
                // We need the path relative to ROOT_DIR for execution inside chroot.
                fs::path hook_rel = fs::relative(hook_path, ROOT_DIR);
                // Prepend / to make it absolute in chroot
                std::string hook_cmd = "/" + hook_rel.string();
                
                execl("/bin/sh", "sh", "-c", hook_cmd.c_str(), (char*)NULL);
                std::cerr << string_format("error.hook_exec_failed", strerror(errno)) << std::endl;
                _exit(1);
            } else {
                // Parent
                int status;
                waitpid(pid, &status, 0);
                if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                     log_warning(string_format("warning.hook_failed_exec", hook_name.c_str(), std::to_string(WEXITSTATUS(status))));
                }
            }
        } else {
            // Normal execution
            int ret = std::system(hook_path.c_str());
            if (ret != 0) {
                log_warning(string_format("warning.hook_failed_exec", hook_name.c_str(), std::to_string(ret)));
            }
        }
    }
}


// --- New installation logic helpers ---

struct InstallPlan {
    std::string name;
    std::string version_spec;
    std::string actual_version;
    bool is_explicit = false;
    fs::path tmp_dir;
    fs::path local_path;
    std::string sha256;
    std::vector<DependencyInfo> dependencies; // Pre-resolved dependencies from repo
};

void resolve_package_dependencies(
    const std::string& pkg_name, const std::string& version_spec, bool is_explicit,
    std::map<std::string, InstallPlan>& plan,
    std::vector<std::string>& install_order,
    std::set<std::string>& visited_stack,
    const std::map<std::string, fs::path>& local_candidates,
    Repository& repo) // Added repo param
{
    if (visited_stack.count(pkg_name)) {
        log_warning(string_format("warning.circular_dependency", visited_stack.rbegin()->c_str(), pkg_name.c_str()));
        return;
    }
    
    if (plan.count(pkg_name)) {
        if (is_explicit) plan.at(pkg_name).is_explicit = true;
        return;
    }

    std::string installed_version = get_installed_version(pkg_name);
    
    // Check local candidates
    fs::path local_path;
    std::string latest_version;
    std::vector<DependencyInfo> deps;
    std::string pkg_hash;
    
    auto it = local_candidates.find(pkg_name);
    if (it != local_candidates.end()) {
        local_path = it->second;
        auto name_ver = parse_package_filename(local_path.filename().string());
        latest_version = name_ver.second;
        
        // For local files, we MUST extract to find dependencies (no index)
        // We create a temp task just to extract deps.txt
        // Or we just extract deps.txt manually.
        // Let's use InstallationTask but stop after extract.
        std::vector<std::string> dummy;
        InstallationTask task(pkg_name, latest_version, false, dummy, "", local_path);
        // task.prepare() downloads and extracts. We only need extract.
        // Local path logic in prepare() handles local file.
        // But prepare() calls resolve_dependencies() which we are currently IN.
        // So we should only call task.extract_and_validate_package().
        // But constructor initializes tmp_pkg_dir.
        task.download_and_verify_package(); // copies/verifies local file
        task.extract_and_validate_package();
        
        // Read deps.txt
        std::ifstream deps_file(task.tmp_pkg_dir_ / "deps.txt");
        std::string line;
        while (std::getline(deps_file, line)) {
            if (line.empty()) continue;
            if (line.back() == '\r') line.pop_back();
            std::stringstream ss(line);
            std::string dep_name, op, req_ver;
            if (ss >> dep_name) {
                DependencyInfo d;
                d.name = dep_name;
                if (ss >> op >> req_ver) {
                    d.op = op;
                    d.version_req = req_ver;
                }
                deps.push_back(d);
            }
        }
        
    } else {
        // Remote resolution using Repository
        std::optional<PackageInfo> pkg_info;
        if (version_spec == "latest") {
            pkg_info = repo.find_package(pkg_name);
        } else {
            pkg_info = repo.find_package(pkg_name, version_spec);
        }
        
        if (!pkg_info) {
             log_warning("Package not found in repository: " + pkg_name);
             return; // Or throw?
        }
        
        latest_version = pkg_info->version;
        pkg_hash = pkg_info->sha256;
        deps = pkg_info->dependencies;
    }

    if (!installed_version.empty() && !version_compare(installed_version, latest_version)) {
        return; 
    }

    visited_stack.insert(pkg_name);

    InstallPlan pkg_plan;
    pkg_plan.name = pkg_name;
    pkg_plan.version_spec = version_spec;
    pkg_plan.actual_version = latest_version;
    pkg_plan.is_explicit = is_explicit;
    pkg_plan.tmp_dir = get_tmp_dir() / pkg_name; // Note: this path might change if we don't download yet
    pkg_plan.local_path = local_path;
    pkg_plan.sha256 = pkg_hash;
    pkg_plan.dependencies = deps;

    // Recursive resolution
    for (const auto& dep : deps) {
        // Check constraints
        bool has_constraint = !dep.op.empty();
        
        if (has_constraint) {
                std::string installed_dep_ver = get_installed_version(dep.name);
                if (!installed_dep_ver.empty() && installed_dep_ver != "virtual") {
                    bool sat = version_satisfies(installed_dep_ver, dep.op, dep.version_req);
                    if (!sat) {
                        throw LpkgException("Installed dependency " + dep.name + " version " + installed_dep_ver + " does not satisfy constraint " + dep.op + " " + dep.version_req);
                    }
                }
        }

        resolve_package_dependencies(dep.name, "latest", false, plan, install_order, visited_stack, local_candidates, repo);
        
        if (has_constraint && plan.count(dep.name)) {
                if (!version_satisfies(plan[dep.name].actual_version, dep.op, dep.version_req)) {
                    throw LpkgException("Candidate dependency " + dep.name + " version " + plan[dep.name].actual_version + " does not satisfy constraint " + dep.op + " " + dep.version_req);
                }
        }
    }

    plan[pkg_name] = pkg_plan;
    install_order.push_back(pkg_name);
    visited_stack.erase(pkg_name);
}

void commit_package_installation(const InstallPlan& plan) {
    std::vector<std::string> dummy_path;
    InstallationTask task(plan.name, plan.actual_version, plan.is_explicit, dummy_path, get_installed_version(plan.name), plan.local_path, plan.sha256);
    
    // Check if we need to download/prepare (if not local)
    // resolve_package_dependencies didn't download if it was remote.
    // So we need to call prepare() or parts of it.
    // InstallationTask::run() calls prepare().
    // prepare() downloads and extracts.
    
    task.prepare(); // This will download (using hash if provided) and extract.
    // Note: resolve_dependencies() inside prepare() might re-check things, but since we already planned, maybe we should skip it?
    // task.prepare() calls:
    // download_and_verify_package(); -> uses hash
    // extract_and_validate_package();
    // resolve_dependencies(); -> checks deps.txt. Safe to run again, just verifies.
    // check_for_file_conflicts();
    
    task.commit();
}


void rollback_installed_package(const std::string& pkg_name, const std::string& version) {
    log_info(string_format("info.rolling_back", pkg_name.c_str()));

    remove_package_files(pkg_name, true); // force remove

    fs::remove(DEP_DIR / pkg_name);
    fs::remove(DOCS_DIR / (pkg_name + ".man"));
    fs::remove_all(HOOKS_DIR / pkg_name);

    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);
    cache.pkgs.erase(pkg_name + ":" + version);
    if (cache.holdpkgs.contains(pkg_name)) {
        cache.holdpkgs.erase(pkg_name);
    }
    cache.dirty = true;
}

} // anonymous namespace

void install_package(const std::string& pkg_name, const std::string& version) {
    TmpDirManager tmp_manager;
    std::vector<std::string> install_path;
    do_install(pkg_name, version, true, install_path, "");
}

// ...

void install_packages(const std::vector<std::string>& pkg_args) {
    // Force cache reload to handle consecutive calls in the same process (e.g. tests)
    force_reload_cache();

    TmpDirManager tmp_manager;
    log_info(get_string("info.resolving_dependencies"));

    // Initialize Repository
    Repository repo;
    try {
        repo.load_index();
    } catch (const std::exception& e) {
        log_warning(string_format("warning.repo_index_load_failed", e.what()));
    }

    std::map<std::string, InstallPlan> plan;
    std::vector<std::string> install_order;
    std::map<std::string, fs::path> local_candidates;

    // Pre-scan for local files
    std::vector<std::pair<std::string, std::string>> targets;

    for (const auto& pkg_arg : pkg_args) {
        fs::path p(pkg_arg);
        if (p.extension() == ".zst" || p.extension() == ".lpkg" || pkg_arg.find('/') != std::string::npos) {
            // Treat as local file
            if (fs::exists(p)) {
                 try {
                     auto name_ver = parse_package_filename(p.filename().string());
                     local_candidates[name_ver.first] = fs::absolute(p);
                     targets.emplace_back(name_ver.first, name_ver.second);
                 } catch (const std::exception& e) {
                     log_error(string_format("warning.skip_invalid_local_pkg", pkg_arg.c_str(), e.what()));
                 }
            } else {
                log_error(string_format("error.local_pkg_not_found", pkg_arg.c_str()));
            }
        } else {
            // Regular package spec
            std::string pkg_name = pkg_arg;
            std::string version = "latest";
            size_t pos = pkg_arg.find(':');
            if (pos != std::string::npos) {
                pkg_name = pkg_arg.substr(0, pos);
                version = pkg_arg.substr(pos + 1);
            }
            targets.emplace_back(pkg_name, version);
        }
    }

    for (const auto& target : targets) {
        std::set<std::string> visited_stack;
        resolve_package_dependencies(target.first, target.second, true, plan, install_order, visited_stack, local_candidates, repo);
    }


    if (plan.empty()) {
        log_info(get_string("info.all_packages_already_installed"));
        return;
    }

    log_info(get_string("info.packages_to_install"));
    std::string confirmation_prompt;
    for (const auto& pkg_name : install_order) {
        const auto& p = plan.at(pkg_name);
        if (p.is_explicit) {
            confirmation_prompt += "  " + string_format("info.package_list_item", p.name.c_str(), p.actual_version.c_str()) + "\n";
        } else {
            confirmation_prompt += "  " + string_format("info.package_list_item_dep", p.name.c_str(), p.actual_version.c_str()) + "\n";
        }
    }
    
    if (!user_confirms(confirmation_prompt + get_string("info.confirm_proceed"))) {
        log_info(get_string("info.installation_aborted"));
        return;
    }

    std::vector<std::string> installed_this_run;
    try {
        for (const auto& pkg_name : install_order) {
            const auto& p = plan.at(pkg_name);
            log_info(string_format("info.installing_package", p.name.c_str(), p.actual_version.c_str()));
            commit_package_installation(p);
            installed_this_run.push_back(p.name);
            log_info(string_format("info.package_installed_successfully", p.name.c_str()));
        }
        write_cache();
    } catch (const LpkgException& e) {
        log_error(get_string("error.installation_failed_rolling_back"));
        std::reverse(installed_this_run.begin(), installed_this_run.end());
        for (const auto& pkg_to_remove_name : installed_this_run) {
            try {
                const auto& p = plan.at(pkg_to_remove_name);
                rollback_installed_package(p.name, p.actual_version);
            } catch (const LpkgException& e2) {
                log_error(string_format("error.rollback_failed", pkg_to_remove_name.c_str(), e2.what()));
            }
        }
        write_cache();
        throw;
    }
}

void remove_package(const std::string& pkg_name, bool force) {
    std::string installed_version = get_installed_version(pkg_name);
    if (installed_version.empty()) {
        log_info(string_format("info.package_not_installed", pkg_name.c_str()));
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
                    log_info(string_format("info.skip_remove_dependency", pkg_name.c_str(), current_pkg_name.c_str()));
                    return;
                }
            }
        }
    }

    log_info(string_format("info.removing_package", pkg_name.c_str()));

    run_hook(pkg_name, "prerm.sh");

    remove_package_files(pkg_name, force);

    fs::remove(DEP_DIR / pkg_name);
    fs::remove(DOCS_DIR / (pkg_name + ".man"));
    fs::remove_all(HOOKS_DIR / pkg_name);

    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);
    cache.pkgs.erase(pkg_name + ":" + installed_version);
    if (cache.holdpkgs.contains(pkg_name)) {
        cache.holdpkgs.erase(pkg_name);
    }
    cache.dirty = true;

    log_info(string_format("info.package_removed_successfully", pkg_name.c_str()));
}

void remove_package_files(const std::string& pkg_name, bool force) {
    const fs::path files_list_path = FILES_DIR / (pkg_name + ".txt");
    if (fs::exists(files_list_path)) {
        
        std::map<std::string, std::vector<std::string>> shared_files;
        auto& cache = get_cache();
        std::unique_lock<std::mutex> lock(cache.mtx);
        auto& db = cache.file_db;

        std::ifstream files_to_check(files_list_path);
        if (!files_to_check.is_open()) {
            throw LpkgException(string_format("error.open_file_failed", files_list_path.string().c_str()));
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

        if (!shared_files.empty() && !force) { // Only abort if not forced
            std::string error_msg = get_string("error.shared_file_header") + "\n";
            for (const auto& [file, owners] : shared_files) {
                std::string owners_str;
                for(size_t i = 0; i < owners.size(); ++i) {
                    owners_str += owners[i] + (i == owners.size() - 1 ? "" : ", ");
                }
                error_msg += "  " + string_format("error.shared_file_entry", file.c_str(), owners_str.c_str()) + "\n";
            }
            error_msg += get_string("error.removal_aborted");
            throw LpkgException(error_msg);
        }

        lock.unlock(); // Unlock before performing file operations

        // No shared files, proceed with removal
        std::ifstream files_list(files_list_path);
        if (!files_list.is_open()) {
            throw LpkgException(string_format("error.open_file_failed", files_list_path.string().c_str()));
        }
        std::string file_path;
        std::vector<fs::path> file_paths;
        while (std::getline(files_list, file_path)) {
            if (!file_path.empty()) file_paths.emplace_back(file_path);
        }
        
        std::sort(file_paths.rbegin(), file_paths.rend());

        int removed_count = 0;
        for (const auto& path : file_paths) {
            fs::path physical_path;
            if (path.is_absolute()) {
                physical_path = ROOT_DIR / path.relative_path();
            } else {
                physical_path = ROOT_DIR / path;
            }

            if (fs::exists(physical_path) || fs::is_symlink(physical_path)) {
                try {
                    // Check if the file is still owned by this package before removing
                    lock.lock();
                    auto it = db.find(path.string()); // DB uses logical path
                    bool can_remove = (it != db.end() && it->second.count(pkg_name));
                    lock.unlock();

                    if (can_remove) {
                        fs::remove(physical_path);
                        removed_count++;
                    } else {
                        log_info(string_format("info.skipped_remove", physical_path.string().c_str()));
                    }
                } catch (const fs::filesystem_error& e) {
                    log_warning(string_format("warning.remove_file_failed", physical_path.string().c_str(), e.what()));
                }
            }
        }
        log_info(string_format("info.files_removed", removed_count));
        fs::remove(files_list_path);

        // Update file ownership database
        lock.lock();
        for (const auto& path : file_paths) {
            auto it = db.find(path.string());
            if (it != db.end()) {
                it->second.erase(pkg_name);
                if (it->second.empty()) {
                    db.erase(it);
                }
            }
        }
        cache.dirty = true;
    }

    const fs::path dirs_list_path = FILES_DIR / (pkg_name + ".dirs");
    if (fs::exists(dirs_list_path)) {
        std::ifstream dirs_list(dirs_list_path);
        if (!dirs_list.is_open()) {
            throw LpkgException(string_format("error.open_file_failed", dirs_list_path.string().c_str()));
        }
        std::string dir_path;
        std::vector<fs::path> dir_paths;
        while (std::getline(dirs_list, dir_path)) {
            if (!dir_path.empty()) dir_paths.emplace_back(dir_path);
        }
        std::sort(dir_paths.rbegin(), dir_paths.rend());
        for (const auto& dir : dir_paths) {
            fs::path physical_dir;
            if (dir.is_absolute()) {
                physical_dir = ROOT_DIR / dir.relative_path();
            } else {
                physical_dir = ROOT_DIR / dir;
            }

            if (fs::exists(physical_dir) && fs::is_directory(physical_dir) && fs::is_empty(physical_dir)) {
                try {
                    fs::remove(physical_dir);
                } catch (const fs::filesystem_error&) {
                    // Ignore errors, maybe another package created it again
                }
            }
        }
        fs::remove(dirs_list_path);
    }

    // Remove provides
    const fs::path provides_list_path = FILES_DIR / (pkg_name + ".provides");
    if (fs::exists(provides_list_path)) {
        auto& cache = get_cache();
        std::unique_lock<std::mutex> lock(cache.mtx);
        
        std::ifstream provides_list(provides_list_path);
        std::string capability;
        while (std::getline(provides_list, capability)) {
            if (!capability.empty()) {
                cache.providers[capability].erase(pkg_name);
                if (cache.providers[capability].empty()) {
                    cache.providers.erase(capability);
                }
            }
        }
        lock.unlock();
        fs::remove(provides_list_path);
        
        lock.lock();
        cache.dirty = true;
        lock.unlock();
    }
}


void autoremove() {
    log_info(get_string("info.checking_autoremove"));
    
    auto required_pkgs = get_all_required_packages();
    std::vector<std::string> packages_to_remove;

    auto& cache = get_cache();
    std::unique_lock<std::mutex> lock(cache.mtx);
    for (const auto& record : cache.pkgs) {
        std::string pkg_name = record.substr(0, record.find(':'));
        if (!required_pkgs.contains(pkg_name)) {
            packages_to_remove.push_back(pkg_name);
        }
    }
    lock.unlock();

    if (packages_to_remove.empty()) {
        log_info(get_string("info.no_autoremove_packages"));
    } else {
        log_info(string_format("info.autoremove_candidates", packages_to_remove.size()));
        for (const auto& pkg_name : packages_to_remove) {
            try {
                remove_package(pkg_name, true);
            } catch (const LpkgException& e) {
                log_warning(string_format("warning.autoremove_skipped", pkg_name.c_str(), e.what()));
            }
        }
        log_info(string_format("info.autoremove_complete", packages_to_remove.size()));
    }

    
}

void upgrade_packages() {
    log_info(get_string("info.checking_upgradable"));
    TmpDirManager tmp_manager;
    int upgraded_count = 0;
    std::vector<std::tuple<std::string, std::string, std::string>> upgradable_pkgs; // name, current, latest

    // Initialize Repository
    Repository repo;
    try {
        repo.load_index();
    } catch (const std::exception& e) {
        log_warning("Failed to load repository index: " + std::string(e.what()) + ". Upgrade check might require individual requests or fail.");
    }

    std::vector<std::pair<std::string, std::string>> installed_packages_info;
    auto& cache = get_cache();
    std::unique_lock<std::mutex> lock(cache.mtx);
    for (const auto& pkg_record : cache.pkgs) {
        size_t pos = pkg_record.find(':');
        if (pos != std::string::npos) {
            installed_packages_info.emplace_back(pkg_record.substr(0, pos), pkg_record.substr(pos + 1));
        }
    }
    lock.unlock();

    for (const auto& pkg_info : installed_packages_info) {
        const std::string& pkg_name = pkg_info.first;
        const std::string& current_version = pkg_info.second;
        
        std::string latest_version;
        // Try repository first
        auto pkg_opt = repo.find_package(pkg_name);
        if (pkg_opt) {
            latest_version = pkg_opt->version;
        } else {
            // Fallback to old method (slow)
             try {
                latest_version = get_latest_version(pkg_name);
            } catch (const std::exception& e) {
                log_warning(string_format("warning.get_latest_version_failed", pkg_name.c_str(), e.what()));
                continue;
            }
        }

        if (version_compare(current_version, latest_version)) {
             log_info(string_format("info.upgradable_found", pkg_name.c_str(), current_version.c_str(), latest_version.c_str()));
             upgradable_pkgs.emplace_back(pkg_name, current_version, latest_version);
        }
    }

    if (upgradable_pkgs.empty()) {
        log_info(get_string("info.all_packages_latest"));
        return;
    }

    for (const auto& [pkg_name, current_version, latest_version] : upgradable_pkgs) {
        try {
            log_info(string_format("info.upgrading_package", pkg_name.c_str(), current_version.c_str(), latest_version.c_str()));
            bool was_manually_installed = is_manually_installed(pkg_name);

            // Use resolve_package_dependencies logic (InstallPlan) instead of direct do_install 
            // to ensure deps are updated/checked too.
            // But for now, sticking to do_install but passing hash if known from repo?
            // do_install calls InstallationTask.
            // We can pass expected hash if we have it.
            std::string expected_hash;
            auto pkg_opt = repo.find_package(pkg_name);
            if (pkg_opt && pkg_opt->version == latest_version) {
                expected_hash = pkg_opt->sha256;
            }

            std::vector<std::string> install_path;
            InstallationTask task(pkg_name, latest_version, was_manually_installed, install_path, current_version, "", expected_hash);
            task.run();
            
            write_cache();
            upgraded_count++;
        } catch (const LpkgException& e) {
            log_error(string_format("error.upgrade_failed", pkg_name.c_str(), e.what()));
        }
    }

    if (upgraded_count > 0) {
        log_info(string_format("info.upgraded_packages", upgraded_count));
    }
}

void show_man_page(const std::string& pkg_name) {
    const fs::path man_file_path = DOCS_DIR / (pkg_name + ".man");
    if (!fs::exists(man_file_path)) {
        throw LpkgException(string_format("error.no_man_page", pkg_name.c_str()));
    }

    std::ifstream man_file(man_file_path);
    if (!man_file.is_open()) {
        throw LpkgException(string_format("error.open_man_page_failed", man_file_path.string().c_str()));
    }

    std::cout << man_file.rdbuf();
}

void write_cache() {
    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);
    if (cache.dirty) {
        cache.write_file_db();
        cache.write_providers();
        cache.write_pkgs();
        cache.write_holdpkgs();
        cache.dirty = false;
    }
}
