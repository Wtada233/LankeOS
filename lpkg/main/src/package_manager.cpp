#include "package_manager.hpp"

#include "archive.hpp"
#include "cache.hpp"
#include "trigger.hpp"
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
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sys/mount.h> 
#include <sys/wait.h> 
#include <unistd.h> 
#include <ranges>
#include <concepts>

namespace fs = std::filesystem;

namespace {

// Forward declarations for internal logic
void run_hook(std::string_view pkg_name, std::string_view hook_name);

void force_reload_cache() {
    Cache::instance().load();
}

bool is_essential_package(std::string_view pkg_name) {
    return Cache::instance().is_essential(pkg_name);
}

std::string get_installed_version(std::string_view pkg_name) {
    return Cache::instance().get_installed_version(pkg_name);
}

bool is_manually_installed(std::string_view pkg_name) {
    return Cache::instance().is_held(pkg_name);
}

// Modern helper for executing processes
int execute_process(const std::vector<std::string>& args, bool use_chroot = false) {
    pid_t pid = fork();
    if (pid == -1) return -1;
    if (pid == 0) {
        if (use_chroot) {
            if (unshare(CLONE_NEWNS) != 0) _exit(1);
            mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
            if (chroot(ROOT_DIR.c_str()) != 0) _exit(1);
            if (chdir("/") != 0) _exit(1);
        }
        
        std::vector<char*> c_args;
        for (const auto& arg : args) c_args.push_back(const_cast<char*>(arg.c_str()));
        c_args.push_back(nullptr);
        
        execv(c_args[0], c_args.data());
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

class ChrootMountGuard {
public:
    explicit ChrootMountGuard(const fs::path& root) : root_(root) {
        if (root_ == "/" || root_.empty()) return;
        mount_one("proc", "proc", "proc", 0, nullptr);
        mount_one("sys", "sys", "sysfs", 0, nullptr);
        mount_one("/dev", "dev", "", MS_BIND | MS_REC, nullptr);
        mount_one("devpts", "dev/pts", "devpts", 0, nullptr);
        mount_one("/run", "run", "", MS_BIND | MS_REC, nullptr);
        if (fs::exists("/etc/resolv.conf")) {
            fs::path target = root_ / "etc/resolv.conf";
            ensure_dir_exists(target.parent_path());
            try {
                if (!fs::exists(target)) { std::ofstream f(target); }
                mount_one("/etc/resolv.conf", "etc/resolv.conf", "", MS_BIND, nullptr);
            } catch (...) {}
        }
    }
    ~ChrootMountGuard() {
        if (root_ == "/" || root_.empty()) return;
        std::reverse(mounted_paths_.begin(), mounted_paths_.end());
        for (const auto& path : mounted_paths_) {
            umount2(path.c_str(), MNT_DETACH);
        }
    }
private:
    void mount_one(const std::string& source, const std::string& target_rel, const std::string& type, unsigned long flags, const void* data) {
        fs::path target = root_ / target_rel;
        ensure_dir_exists(target);
        if (mount(source.c_str(), target.c_str(), type.empty() ? nullptr : type.c_str(), flags, data) == 0) {
            mounted_paths_.push_back(target);
        }
    }
    fs::path root_;
    std::vector<fs::path> mounted_paths_;
};

void run_hook(std::string_view pkg_name, std::string_view hook_name) {
    if (get_no_hooks_mode()) return;
    fs::path hook_path = HOOKS_DIR / pkg_name / hook_name;
    if (fs::exists(hook_path) && fs::is_regular_file(hook_path)) {
        log_info(string_format("info.running_hook", std::string(hook_name).c_str()));
        bool use_chroot = (ROOT_DIR != "/" && !ROOT_DIR.empty());
        if (use_chroot) {
            if (!fs::exists(ROOT_DIR / "bin/sh")) {
                log_warning(string_format("warning.hook_failed_setup", std::string(hook_name).c_str(), " /bin/sh not found in target root. Skipping."));
                return;
            }
            ChrootMountGuard mount_guard(ROOT_DIR);
            fs::path hook_rel = fs::relative(hook_path, ROOT_DIR);
            std::string hook_cmd = "/" + hook_rel.string();
            int ret = execute_process({"/bin/sh", "-c", hook_cmd}, true);
            if (ret != 0) {
                log_warning(string_format("warning.hook_failed_exec", std::string(hook_name).c_str(), std::to_string(ret).c_str()));
            }
        } else {
            int ret = execute_process({"/bin/sh", "-c", hook_path.string()});
            if (ret != 0) {
                log_warning(string_format("warning.hook_failed_exec", std::string(hook_name).c_str(), std::to_string(ret).c_str()));
            }
        }
    }
}

} // anonymous namespace

InstallationTask::InstallationTask(std::string pkg_name, std::string version, bool explicit_install, std::string old_version_to_replace, fs::path local_package_path, std::string expected_hash, bool force_reinstall)
    : pkg_name_(std::move(pkg_name)), version_(std::move(version)), explicit_install_(explicit_install), 
      tmp_pkg_dir_(get_tmp_dir() / pkg_name_), actual_version_(version_), old_version_to_replace_(std::move(old_version_to_replace)),
      local_package_path_(std::move(local_package_path)), expected_hash_(std::move(expected_hash)), has_config_conflicts_(false),
      force_reinstall_(force_reinstall) {}

void InstallationTask::run() {
    std::string current_installed_version = get_installed_version(pkg_name_);
    if (!force_reinstall_ && !current_installed_version.empty() && current_installed_version == actual_version_) {
        log_info(string_format("info.package_already_installed", pkg_name_.c_str()));
        return;
    }
    log_info(string_format("info.installing_package", pkg_name_.c_str(), version_.c_str()));
    ensure_dir_exists(tmp_pkg_dir_);
    try {
        prepare();
        commit();
    } catch (const std::exception& e) {
        rollback_files();
        throw;
    }
    log_info(string_format("info.package_installed_successfully", pkg_name_.c_str()));
}

void InstallationTask::prepare() {
    download_and_verify_package();
    extract_and_validate_package();
    check_for_file_conflicts();
}

void InstallationTask::rollback_files() {
    log_error(string_format("error.rollback_install", pkg_name_.c_str()));
    for (const auto& file : installed_files_) {
        try { if (fs::exists(file) || fs::is_symlink(file)) fs::remove(file); } catch (...) {}
    }
    for (const auto& [physical, backup] : backups_) {
        try { if (fs::exists(backup)) fs::rename(backup, physical); } catch (...) {}
    }
    
    for (const auto& dir : created_dirs_ | std::views::reverse) {
        if (fs::exists(dir) && fs::is_directory(dir) && fs::is_empty(dir)) {
            try { fs::remove(dir); } catch (...) {}
        }
    }
}

void InstallationTask::commit() {
    std::unordered_set<std::string> old_files;
    if (!old_version_to_replace_.empty()) {
        fs::path old_files_list_path = FILES_DIR / (pkg_name_ + ".txt");
        if (fs::exists(old_files_list_path)) {
            std::ifstream f(old_files_list_path);
            std::string line;
            while (std::getline(f, line)) if (!line.empty()) old_files.insert(line);
        }
    }
    copy_package_files();
    try {
        register_package();
        if (!old_files.empty()) {
            std::unordered_set<std::string> new_files;
            fs::path new_files_list_path = FILES_DIR / (pkg_name_ + ".txt");
            if (fs::exists(new_files_list_path)) {
                std::ifstream f(new_files_list_path);
                std::string line;
                while (std::getline(f, line)) if (!line.empty()) new_files.insert(line);
            }
            
            for (const auto& old_file : old_files) {
                if (old_file.find("/etc/") == 0) continue; // Protective rule
                if (!new_files.contains(old_file)) {
                    auto& cache = Cache::instance();
                    auto owners = cache.get_file_owners(old_file);
                    if (!owners.empty() && owners.contains(pkg_name_)) {
                        cache.remove_file_owner(old_file, pkg_name_);
                        if (cache.get_file_owners(old_file).empty()) {
                            fs::path physical_path = (fs::path(old_file).is_absolute()) ? ROOT_DIR / fs::path(old_file).relative_path() : ROOT_DIR / old_file;
                            if (fs::exists(physical_path) || fs::is_symlink(physical_path)) {
                                log_info(string_format("info.removing_obsolete_file", old_file.c_str()));
                                try { fs::remove(physical_path); } catch (...) {}
                            }
                        }
                    }
                }
            }
            
            fs::path old_dirs_list_path = FILES_DIR / (pkg_name_ + ".dirs");
            if (fs::exists(old_dirs_list_path)) {
                std::ifstream f(old_dirs_list_path);
                std::vector<fs::path> old_dirs;
                std::string line;
                while (std::getline(f, line)) if (!line.empty()) old_dirs.emplace_back(line);
                
                std::ranges::sort(old_dirs, std::greater<>{});
                for (const auto& d : old_dirs) {
                    fs::path physical_path = (d.is_absolute()) ? ROOT_DIR / d.relative_path() : ROOT_DIR / d;
                    if (fs::exists(physical_path) && fs::is_directory(physical_path) && fs::is_empty(physical_path)) {
                        try { fs::remove(physical_path); } catch (...) {}
                    }
                }
            }
        }
    } catch (...) { throw; }
    for (const auto& [physical, backup] : backups_) { try { fs::remove(backup); } catch (...) {} }
    backups_.clear();
    run_post_install_hook();
}

void InstallationTask::download_and_verify_package() {
    if (!local_package_path_.empty()) {
        if (!fs::exists(local_package_path_)) throw LpkgException(string_format("error.local_pkg_not_found", local_package_path_.string().c_str()));
        log_info(string_format("info.installing_local_file", local_package_path_.string().c_str()));
        archive_path_ = local_package_path_;
        if (!expected_hash_.empty()) {
            std::string actual_hash = calculate_sha256(archive_path_);
            if (expected_hash_ != actual_hash) throw LpkgException(string_format("error.hash_mismatch", pkg_name_.c_str()));
        }
        return;
    }
    std::string mirror_url = get_mirror_url(), arch = get_architecture();
    if (version_ == "latest") actual_version_ = get_latest_version(pkg_name_);
    const std::string download_url = mirror_url + arch + "/" + pkg_name_ + "/" + actual_version_ + "/app.tar.zst";
    archive_path_ = tmp_pkg_dir_ / "app.tar.zst";
    std::string expected_hash;
    if (!expected_hash_.empty()) { expected_hash = expected_hash_; } else {
        const std::string hash_url = mirror_url + arch + "/" + pkg_name_ + "/" + actual_version_ + "/hash.txt";
        const fs::path hash_path = tmp_pkg_dir_ / "hash.txt";
        download_with_retries(hash_url, hash_path, 5, false);
        std::ifstream hash_file(hash_path);
        if (!(hash_file >> expected_hash)) throw LpkgException(string_format("error.hash_download_failed", hash_url.c_str()));
    }
    if (!fs::exists(archive_path_)) download_with_retries(download_url, archive_path_, 5, true);
    if (calculate_sha256(archive_path_) != expected_hash) throw LpkgException(string_format("error.hash_mismatch", pkg_name_.c_str()));
}

void InstallationTask::extract_and_validate_package() {
    log_info(get_string("info.extracting_to_tmp"));
    extract_tar_zst(archive_path_, tmp_pkg_dir_);
    static const std::vector<std::string> required_metadata = {"man.txt", "deps.txt", "files.txt", "content/"};
    for (const auto& file : required_metadata) {
        if (!fs::exists(tmp_pkg_dir_ / file)) {
            throw LpkgException(string_format("error.incomplete_package", (tmp_pkg_dir_ / file).string().c_str()));
        }
    }
}

void InstallationTask::check_for_file_conflicts() {
    std::map<std::string, std::string> conflicts;
    std::ifstream files_list(tmp_pkg_dir_ / "files.txt");
    std::string line;
    auto& cache = Cache::instance();
    bool force_overwrite = get_force_overwrite_mode();
    
    while (std::getline(files_list, line)) {
        if (line.empty()) continue;
        size_t tab_pos = line.find('\t');
        if (tab_pos == std::string::npos) continue;
        
        std::string src = line.substr(0, tab_pos);
        std::string dest = line.substr(tab_pos + 1);
        if (!dest.empty() && dest.back() == '\r') dest.pop_back();

        fs::path logical_dest_path = fs::path(dest) / src;
        fs::path physical_dest_path = (logical_dest_path.is_absolute()) ? ROOT_DIR / logical_dest_path.relative_path() : ROOT_DIR / logical_dest_path;

        // Skip conflict check if it's a directory
        if (fs::is_directory(tmp_pkg_dir_ / "content" / src)) continue;

        std::string path_str = logical_dest_path.string();
        
        auto owners = cache.get_file_owners(path_str);
        if (!owners.empty()) {
            bool owned_by_others = false;
            for (const auto& owner : owners) {
                if (owner != pkg_name_) {
                    if (!force_overwrite) conflicts[path_str] = owner;
                    owned_by_others = true;
                    break;
                }
            }
            if (owned_by_others) continue;
        }
        
        if (old_version_to_replace_.empty()) {
            try {
                fs::path physical_path = (logical_dest_path.is_absolute()) ? ROOT_DIR / logical_dest_path.relative_path() : ROOT_DIR / logical_dest_path;
                if (fs::exists(physical_path) || fs::is_symlink(physical_path)) {
                    if (owners.empty() && !force_overwrite) {
                        conflicts[path_str] = "unknown (manual file)";
                    }
                }
            } catch (...) {}
        }
    }
    
    if (!conflicts.empty()) {
        std::string msg = get_string("error.file_conflict_header") + "\n";
        for (const auto& [file, owner] : conflicts) {
            msg += "  " + string_format("error.file_conflict_entry", file.c_str(), owner.c_str()) + "\n";
        }
        throw LpkgException(msg + get_string("error.installation_aborted"));
    }
}

void InstallationTask::copy_package_files() {
    log_info(get_string("info.copying_files"));
    std::ifstream files_list(tmp_pkg_dir_ / "files.txt");
    std::string line;
    while (std::getline(files_list, line)) {
        if (line.empty()) continue;
        size_t tab_pos = line.find('\t');
        if (tab_pos == std::string::npos) continue;

        std::string src = line.substr(0, tab_pos);
        std::string dest = line.substr(tab_pos + 1);
        if (!dest.empty() && dest.back() == '\r') dest.pop_back();

        const fs::path src_path = tmp_pkg_dir_ / "content" / src;
        const fs::path logical_dest_path = fs::path(dest) / src;
        fs::path physical_dest_path = (logical_dest_path.is_absolute()) ? ROOT_DIR / logical_dest_path.relative_path() : ROOT_DIR / logical_dest_path;
        
        if (!fs::exists(src_path) && !fs::is_symlink(src_path)) continue;
        
        fs::path parent = physical_dest_path.parent_path();
        std::vector<fs::path> to_create;
        while (!parent.empty() && parent != ROOT_DIR && !fs::exists(parent)) { 
            try { to_create.push_back(parent); parent = parent.parent_path(); } catch (...) { break; }
        }
        for (const auto& d : to_create | std::views::reverse) { ensure_dir_exists(d); created_dirs_.insert(d); }
        
        if (fs::is_directory(src_path)) {
            ensure_dir_exists(physical_dest_path);
            continue;
        }

        try {
            bool is_config = (src.size() >= 4 && src.substr(0, 4) == "etc/");
            fs::path final_dest_path = physical_dest_path;
            if (is_config && fs::exists(physical_dest_path) && !fs::is_directory(physical_dest_path)) {
                final_dest_path += ".lpkgnew";
                log_warning(string_format("warning.config_conflict", physical_dest_path.c_str(), final_dest_path.c_str()));
                has_config_conflicts_ = true;
            } else {
                 if (fs::exists(physical_dest_path) || fs::is_symlink(physical_dest_path)) {
                    if (!fs::is_directory(physical_dest_path)) {
                        fs::path bak = physical_dest_path; bak += ".lpkg_bak_" + pkg_name_; 
                        fs::rename(physical_dest_path, bak); backups_.emplace_back(physical_dest_path, bak);
                    }
                }
            }
            fs::copy(src_path, final_dest_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks);
            installed_files_.push_back(final_dest_path);
            TriggerManager::instance().check_file(logical_dest_path.string());
        } catch (const std::exception& e) {
            throw LpkgException(string_format("error.copy_failed_rollback", src.c_str(), physical_dest_path.c_str(), e.what()));
        }
    }
    if (has_config_conflicts_) {
        log_warning(get_string("info.config_review_reminder"));
    }
    std::ofstream pkg_f(FILES_DIR / (pkg_name_ + ".txt"));
    std::ifstream fl2(tmp_pkg_dir_ / "files.txt");
    std::string fl2_line;
    while (std::getline(fl2, fl2_line)) {
        if (fl2_line.empty()) continue;
        size_t tab_pos = fl2_line.find('\t');
        if (tab_pos != std::string::npos) {
            std::string src_reg = fl2_line.substr(0, tab_pos);
            std::string dest_reg = fl2_line.substr(tab_pos + 1);
            if (!dest_reg.empty() && dest_reg.back() == '\r') dest_reg.pop_back();
            pkg_f << (fs::path(dest_reg) / src_reg).string() << "\n";
        }
    }
    std::ofstream dir_f(FILES_DIR / (pkg_name_ + ".dirs"));
    for (const auto& d : created_dirs_) dir_f << d.string() << "\n";
}


void InstallationTask::register_package() {
    std::ifstream deps_in(tmp_pkg_dir_ / "deps.txt");
    std::ofstream deps_out(DEP_DIR / pkg_name_);
    std::string d;
    auto& cache = Cache::instance();
    if (!old_version_to_replace_.empty()) {
        fs::path p = DEP_DIR / pkg_name_;
        if (fs::exists(p)) {
            std::ifstream f(p); std::string l;
            while(std::getline(f, l)) {
                if (!l.empty()) {
                    std::string_view sv = l;
                    if (sv.back() == '\r') sv.remove_suffix(1);
                    if (auto pos = sv.find_first_of(" \t<>="); pos != std::string_view::npos) sv = sv.substr(0, pos);
                    if (!sv.empty()) cache.remove_reverse_dep(sv, pkg_name_);
                }
            }
        }
    }
    if (!old_version_to_replace_.empty()) {
        fs::path p = FILES_DIR / (pkg_name_ + ".provides");
        if (fs::exists(p)) {
            std::ifstream f(p); std::string c;
            while (std::getline(f, c)) {
                if (!c.empty()) {
                    if (c.back() == '\r') c.pop_back();
                    cache.remove_provider(c, pkg_name_);
                }
            }
        }
    }
    while (std::getline(deps_in, d)) {
        if (!d.empty()) {
            std::string_view sv = d;
            if (sv.back() == '\r') sv.remove_suffix(1);
            deps_out << sv << "\n";
            if (auto pos = sv.find_first_of(" \t<>="); pos != std::string_view::npos) sv = sv.substr(0, pos);
            if (!sv.empty()) cache.add_reverse_dep(sv, pkg_name_);
        }
    }
    std::ifstream fl(FILES_DIR / (pkg_name_ + ".txt"));
    std::string fp;
    while (std::getline(fl, fp)) if (!fp.empty()) cache.add_file_owner(fp, pkg_name_);
    fs::copy(tmp_pkg_dir_ / "man.txt", DOCS_DIR / (pkg_name_ + ".man"), fs::copy_options::overwrite_existing);
    
    std::ifstream prov_in(tmp_pkg_dir_ / "provides.txt");
    if (prov_in.is_open()) {
        std::ofstream prov_out(FILES_DIR / (pkg_name_ + ".provides"));
        std::string cap;
        while (std::getline(prov_in, cap)) if (!cap.empty()) { 
            cache.add_provider(cap, pkg_name_); 
            prov_out << cap << "\n"; 
        }
    }
    cache.add_installed(pkg_name_, actual_version_, explicit_install_);
}

void InstallationTask::run_post_install_hook() {
    fs::path hook_src = tmp_pkg_dir_ / "hooks";
    if (!fs::exists(hook_src) || !fs::is_directory(hook_src)) return;
    fs::path dest_dir = HOOKS_DIR / pkg_name_;
    ensure_dir_exists(dest_dir);
    for (const auto& entry : fs::directory_iterator(hook_src)) {
        if (entry.is_regular_file()) {
            fs::path dest = dest_dir / entry.path().filename();
            fs::copy(entry.path(), dest, fs::copy_options::overwrite_existing);
            fs::permissions(dest, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add);
        }
    }
    run_hook(pkg_name_, "postinst.sh");
}

namespace {

struct InstallPlan {
    std::string name, version_spec, actual_version, sha256;
    bool is_explicit = false;
    fs::path local_path;
    std::vector<DependencyInfo> dependencies;
    bool force_reinstall = false;
};

struct ResolutionContext {
    std::map<fs::path, std::vector<DependencyInfo>> local_deps_cache;
    std::map<fs::path, std::string> local_version_cache;
    Repository& repo;
    const std::map<std::string, fs::path>& local_candidates;
    std::map<std::string, InstallPlan>& plan;
    std::vector<std::string>& install_order;
    bool force_reinstall = false;
};

void resolve_package_dependencies(const std::string& pkg_name, const std::string& version_spec, bool is_explicit, 
    ResolutionContext& ctx, std::set<std::string>& visited_stack) {
    if (visited_stack.count(pkg_name)) { 
        log_warning(string_format("warning.circular_dependency", pkg_name.c_str(), pkg_name.c_str())); 
        return; 
    }
    if (ctx.plan.count(pkg_name)) { 
        if (is_explicit) ctx.plan.at(pkg_name).is_explicit = true; 
        return; 
    }
    std::string installed_version = get_installed_version(pkg_name);
    fs::path local_path; std::string latest_version, pkg_hash; std::vector<DependencyInfo> deps;
    auto it = ctx.local_candidates.find(pkg_name);
    if (it != ctx.local_candidates.end()) {
        local_path = it->second;
        if (ctx.local_version_cache.count(local_path)) {
            latest_version = ctx.local_version_cache[local_path];
            deps = ctx.local_deps_cache[local_path];
        } else {
            latest_version = parse_package_filename(local_path.filename().string()).second;
            std::stringstream deps_ss(extract_file_from_archive(local_path, "deps.txt"));
            std::string line;
            while (std::getline(deps_ss, line)) {
                if (line.empty()) continue;
                std::string_view sv = line;
                if (sv.back() == '\r') sv.remove_suffix(1);
                std::stringstream ss{std::string(sv)}; std::string dn, op, rv;
                if (ss >> dn) { DependencyInfo d; d.name = dn; if (ss >> op >> rv) { d.op = op; d.version_req = rv; } deps.push_back(d); }
            }
            ctx.local_version_cache[local_path] = latest_version;
            ctx.local_deps_cache[local_path] = deps;
        }
    } else {
        auto opt = ctx.repo.find_package(pkg_name); 
        std::optional<PackageInfo> pkg_info = (version_spec == "latest") ? opt : ctx.repo.find_package(pkg_name, version_spec);
        
        // VIRTUAL PROVIDER REDIRECTION
        if (!pkg_info) {
            auto prov = ctx.repo.find_provider(pkg_name);
            if (prov) { resolve_package_dependencies(prov->name, prov->version, is_explicit, ctx, visited_stack); return; }
        }

        if (!pkg_info) { 
            if (installed_version.empty()) log_warning(string_format("warning.package_not_in_repo", pkg_name.c_str())); 
            return; 
        }
        if (pkg_info->name != pkg_name) { 
            resolve_package_dependencies(pkg_info->name, pkg_info->version, is_explicit, ctx, visited_stack); 
            return; 
        }
        latest_version = pkg_info->version; pkg_hash = pkg_info->sha256; deps = pkg_info->dependencies;
    }

    if (latest_version.empty()) latest_version = "0.0.0";

    if (!ctx.force_reinstall || !is_explicit) {
        if (!is_explicit && !installed_version.empty() && !version_compare(installed_version, latest_version)) return;
        if (is_explicit && !installed_version.empty() && installed_version == latest_version) return;
    }

    visited_stack.insert(pkg_name);
    InstallPlan p; p.name = pkg_name; p.version_spec = version_spec; p.actual_version = latest_version; p.is_explicit = is_explicit; p.local_path = local_path; p.sha256 = pkg_hash; p.dependencies = deps; p.force_reinstall = (ctx.force_reinstall && is_explicit);
    if (!get_no_deps_mode()) {
        for (const auto& dep : deps) {
            std::string idv = get_installed_version(dep.name);
            bool needs_resolution = false;
            if (idv.empty()) {
                needs_resolution = true;
            } else if (!dep.op.empty() && idv != "virtual" && !version_satisfies(idv, dep.op, dep.version_req)) {
                if (!ctx.plan.count(dep.name)) {
                    log_info(string_format("info.adding_upgrade_to_plan", dep.name.c_str(), dep.version_req.c_str()));
                    needs_resolution = true;
                }
            }
            if (needs_resolution) {
                std::string req_ver = "latest";
                if (!dep.op.empty()) {
                    auto matching = ctx.repo.find_best_matching_version(dep.name, dep.op, dep.version_req);
                    if (matching) req_ver = matching->version;
                }
                resolve_package_dependencies(dep.name, req_ver, false, ctx, visited_stack);
            }
            std::string candidate_version;
            if (ctx.plan.count(dep.name)) candidate_version = ctx.plan[dep.name].actual_version;
            else candidate_version = get_installed_version(dep.name);
            if (!dep.op.empty() && !candidate_version.empty() && candidate_version != "virtual" && !version_satisfies(candidate_version, dep.op, dep.version_req))
                throw LpkgException(string_format("error.candidate_dep_version_mismatch", dep.name.c_str(), candidate_version.c_str(), dep.op.c_str(), dep.version_req.c_str()));
        }
    }
    ctx.plan[pkg_name] = p; ctx.install_order.push_back(pkg_name); visited_stack.erase(pkg_name);
}

std::set<std::string> check_plan_consistency(const std::map<std::string, InstallPlan>& plan) {
    std::set<std::string> broken_pkgs;
    auto& cache = Cache::instance();
    std::lock_guard<std::mutex> lock(cache.get_mutex());
    for (const auto& [pkg_to_be_mod, installed_ver] : cache.get_all_installed()) {
        if (plan.count(pkg_to_be_mod)) continue;
        fs::path dep_file = DEP_DIR / pkg_to_be_mod;
        if (fs::exists(dep_file)) {
            std::ifstream f(dep_file);
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty()) continue;
                std::string_view sv = line;
                if (sv.back() == '\r') sv.remove_suffix(1);
                std::stringstream ss{std::string(sv)};
                std::string dep_name, op, req_ver;
                if (ss >> dep_name) {
                    if (plan.count(dep_name)) {
                        const std::string& new_ver = plan.at(dep_name).actual_version;
                        if (ss >> op >> req_ver) {
                            if (!version_satisfies(new_ver, op, req_ver)) {
                                log_error(string_format("error.conflict_breaks_existing", dep_name.c_str(), new_ver.c_str(), pkg_to_be_mod.c_str(), op.c_str(), req_ver.c_str()));
                                broken_pkgs.insert(pkg_to_be_mod);
                            }
                        }
                    }
                }
            }
        }
    }
    return broken_pkgs;
}

void commit_package_installation(const InstallPlan& plan) {
    InstallationTask task(plan.name, plan.actual_version, plan.is_explicit, get_installed_version(plan.name), plan.local_path, plan.sha256, plan.force_reinstall);
    task.run();
}

std::unordered_set<std::string> get_all_required_packages() {
    auto& cache = Cache::instance();
    std::unordered_set<std::string> req;
    {
        std::lock_guard<std::mutex> lock(cache.get_mutex());
        req = cache.get_all_held();
    }
    std::vector<std::string> q(req.begin(), req.end());
    size_t head = 0;
    while (head < q.size()) {
        std::string curr = q[head++]; 
        fs::path p = DEP_DIR / curr;
        if (fs::exists(p)) {
            std::ifstream f(p);
            std::string d_line;
            while (std::getline(f, d_line)) {
                if (d_line.empty()) continue;
                std::string_view sv = d_line;
                if (sv.back() == '\r') sv.remove_suffix(1);
                if (auto space_pos = sv.find_first_of(" \t<>="); space_pos != std::string_view::npos) {
                    sv = sv.substr(0, space_pos);
                }
                
                std::string d_name(sv);
                if (cache.is_installed(d_name)) {
                    if (!req.contains(d_name)) { req.insert(d_name); q.push_back(d_name); }
                } else {
                    auto providers = cache.get_providers(d_name);
                    if (!providers.empty()) {
                        for (const auto& provider : providers) {
                            if (cache.is_installed(provider) && !req.contains(provider)) {
                                req.insert(provider); q.push_back(provider);
                            }
                        }
                    }
                }
            }
        }
    }
    return req;
}

} // anonymous namespace

void install_package(const std::string& pkg_name, const std::string& version) {
    std::string arg = pkg_name;
    if (version != "latest") arg += ":" + version;
    install_packages({arg});
}

void install_packages(const std::vector<std::string>& pkg_args, const std::string& hash_file_path, bool force_reinstall) {
    force_reload_cache(); TmpDirManager tmp; Repository repo;
    try { repo.load_index(); } catch (const std::exception& e) { log_warning(string_format("warning.repo_index_load_failed", e.what())); }
    std::map<std::string, InstallPlan> plan; std::vector<std::string> order; std::map<std::string, fs::path> locals;
    std::vector<std::pair<std::string, std::string>> targets;

    std::string provided_hash;
    if (!hash_file_path.empty()) {
        if (!fs::exists(hash_file_path)) throw LpkgException(string_format("error.open_file_failed", hash_file_path.c_str()));
        std::ifstream hf(hash_file_path);
        if (!(hf >> provided_hash)) throw LpkgException("Failed to read hash from provided file.");
    }

    for (const auto& arg : pkg_args) {
        fs::path p(arg);
        if (p.extension() == ".zst" || p.extension() == ".lpkg" || arg.find('/') != std::string::npos) {
            if (fs::exists(p)) { 
                try { 
                    auto nv = parse_package_filename(p.filename().string()); 
                    locals[nv.first] = fs::absolute(p); 
                    targets.emplace_back(nv.first, nv.second); 
                } 
                catch (const std::exception& e) { log_error(string_format("warning.skip_invalid_local_pkg", arg.c_str(), e.what())); }
            } else log_error(string_format("error.local_pkg_not_found", arg.c_str()));
        } else {
            std::string n = arg, v = "latest"; size_t pos = arg.find(':');
            if (pos != std::string::npos) { n = arg.substr(0, pos); v = arg.substr(pos+1); }
            targets.emplace_back(n, v);
        }
    }

    if (!provided_hash.empty() && locals.empty()) {
        throw LpkgException("--hash can only be used with local package installations.");
    }

    ResolutionContext ctx{{}, {}, repo, locals, plan, order, force_reinstall};
    for (const auto& t : targets) { 
        std::set<std::string> vs; 
        resolve_package_dependencies(t.first, t.second, true, ctx, vs); 
    }

    // Assign provided hash to the local packages in the plan
    if (!provided_hash.empty()) {
        for (auto& [name, p] : plan) {
            if (!p.local_path.empty()) {
                p.sha256 = provided_hash;
            }
        }
    }

    if (plan.empty()) { log_info(get_string("info.all_packages_already_installed")); return; }
    std::set<std::string> broken = check_plan_consistency(plan);
    if (!broken.empty()) {
        log_error(get_string("error.dependency_conflict_title"));
        if (user_confirms(get_string("prompt.remove_conflict_pkgs"))) {
            for (const auto& pkg : broken) {
                remove_package(pkg, true);
            }
            write_cache();
            install_packages(pkg_args);
            return;
        } else {
            log_info(get_string("info.installation_aborted"));
            return;
        }
    }
    // Global conflict check within transaction
    std::map<std::string, std::string> transaction_files;
    for (const auto& n : order) {
        const auto& p = plan.at(n);
        // If it's a local package, we need to extract files.txt
        std::string files_content;
        if (!p.local_path.empty()) {
            files_content = extract_file_from_archive(p.local_path, "files.txt");
        } else {
            // For remote packages, we'd need to download metadata. 
            // For simplicity in this implementation, we focus on what's available.
            // In a real PM, repo index should contain file lists.
        }
        
        if (!files_content.empty()) {
            std::stringstream ss(files_content);
            std::string line;
            while (std::getline(ss, line)) {
                if (line.empty()) continue;
                size_t tab_pos = line.find('\t');
                if (tab_pos != std::string::npos) {
                    std::string src = line.substr(0, tab_pos);
                    std::string dest = line.substr(tab_pos + 1);
                    if (!dest.empty() && dest.back() == '\r') dest.pop_back();
                    
                    std::string full = (fs::path(dest) / src).string();
                    if (transaction_files.contains(full)) {
                        throw LpkgException("Transaction Conflict: File " + full + " is provided by both " + transaction_files[full] + " and " + p.name);
                    }
                    transaction_files[full] = p.name;
                }
            }
        }
    }

    std::string prompt;
    for (const auto& n : order) {
        const auto& p = plan.at(n);
        prompt += "  " + string_format(p.is_explicit ? "info.package_list_item" : "info.package_list_item_dep", p.name.c_str(), p.actual_version.c_str()) + "\n";
    }
    if (!user_confirms(prompt + get_string("info.confirm_proceed"))) { log_info(get_string("info.installation_aborted")); return; }
    std::vector<std::string> installed;
    bool any_failed = false;
    for (const auto& n : order) {
        const auto& p = plan.at(n); 
        try {
            commit_package_installation(p); 
            installed.push_back(p.name);
        } catch (const std::exception& e) {
            log_error(string_format("error.upgrade_failed", p.name.c_str(), e.what()));
            any_failed = true;
            break; // Stop on first failure to allow rollback
        }
    }

    if (any_failed) {
        log_error(get_string("error.installation_failed_rolling_back"));
        for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
            try {
                log_info(string_format("info.rolling_back", it->c_str()));
                remove_package(*it, true);
            } catch (const std::exception& e) {
                log_error(string_format("error.rollback_failed", it->c_str(), e.what()));
            }
        }
        write_cache();
        throw LpkgException(get_string("error.some_failed"));
    }

    write_cache();
    TriggerManager::instance().run_all();
    log_info(get_string("info.install_complete"));
}

void remove_package(const std::string& pkg_name, bool force) {
    std::string ver = get_installed_version(pkg_name);
    if (ver.empty()) { log_info(string_format("info.package_not_installed", pkg_name.c_str())); return; }
    if (!force) {
        if (is_essential_package(pkg_name)) { log_error(string_format("error.skip_remove_essential", pkg_name.c_str())); return; }
        auto& cache = Cache::instance();
        auto rdeps = cache.get_reverse_deps(pkg_name);
        if (!rdeps.empty()) {
            std::string deps; for (const auto& d : rdeps) deps += d + " ";
            log_info(string_format("info.skip_remove_dependency", pkg_name.c_str(), deps.c_str())); return;
        }
        const fs::path plist = FILES_DIR / (pkg_name + ".provides");
        if (fs::exists(plist)) {
            std::ifstream f(plist); std::string cap;
            while (std::getline(f, cap)) {
                if (cap.empty()) continue;
                if (cap.back() == '\r') cap.pop_back();
                auto cap_rdeps = cache.get_reverse_deps(cap);
                if (!cap_rdeps.empty()) {
                    std::string deps; for (const auto& d : cap_rdeps) deps += d + " ";
                    log_info(string_format("info.skip_remove_dependency", cap.c_str(), deps.c_str())); return;
                }
            }
        }
    }
    log_info(string_format("info.removing_package", pkg_name.c_str()));
    run_hook(pkg_name, "prerm.sh");
    remove_package_files(pkg_name, force);
    {
        auto& cache = Cache::instance();
        fs::path p = DEP_DIR / pkg_name;
        if (fs::exists(p)) {
            std::ifstream f(p); std::string l;
            while(std::getline(f, l)) {
                if (!l.empty()) { 
                    std::string_view sv = l;
                    if (sv.back() == '\r') sv.remove_suffix(1);
                    if (auto pos = sv.find_first_of(" \t<>="); pos != std::string_view::npos) sv = sv.substr(0, pos);
                    if (!sv.empty()) cache.remove_reverse_dep(sv, pkg_name); 
                }
            }
        }
    }
    fs::remove(DEP_DIR / pkg_name); fs::remove(DOCS_DIR / (pkg_name + ".man")); fs::remove_all(HOOKS_DIR / pkg_name);
    Cache::instance().remove_installed(pkg_name);
    log_info(string_format("info.package_removed_successfully", pkg_name.c_str()));
}

void remove_package_files(const std::string& pkg_name, bool force) {
    const fs::path list = FILES_DIR / (pkg_name + ".txt");
    if (fs::exists(list)) {
        std::map<std::string, std::vector<std::string>> shared;
        auto& cache = Cache::instance();
        std::ifstream f(list);
        std::string l;
        std::vector<fs::path> paths;
        while (std::getline(f, l)) {
            if (l.empty()) continue;
            paths.emplace_back(l);
            auto owners = cache.get_file_owners(l);
            if (!owners.empty()) {
                for (const auto& owner : owners) if (owner != pkg_name) shared[l].push_back(owner);
            }
        }
        if (!shared.empty() && !force) {
            std::string msg = get_string("error.shared_file_header") + "\n";
            for (const auto& [file, owners] : shared) {
                std::string os; 
                for(size_t i=0; i<owners.size(); ++i) os += owners[i] + (i==owners.size()-1 ? "" : ", ");
                msg += "  " + string_format("error.shared_file_entry", file.c_str(), os.c_str()) + "\n";
            }
            throw LpkgException(msg + get_string("error.removal_aborted"));
        }
        
        std::ranges::sort(paths, std::greater<>{});
        int count = 0;
        for (const auto& p : paths) {
            fs::path phys = (p.is_absolute()) ? ROOT_DIR / p.relative_path() : ROOT_DIR / p;
            if (fs::exists(phys) || fs::is_symlink(phys)) {
                auto owners = cache.get_file_owners(p.string());
                if (!owners.empty() && owners.contains(pkg_name)) {
                    if (owners.size() == 1) {
                        try { fs::remove(phys); } catch (...) {}
                        count++; 
                    } else {
                        log_info(string_format("info.skipped_remove", p.string().c_str()));
                    }
                }
            }
        }
        log_info(string_format("info.files_removed", count)); 
        fs::remove(list);
        for (const auto& p : paths) {
            cache.remove_file_owner(p.string(), pkg_name);
        }
    }
    
    const fs::path dlist = FILES_DIR / (pkg_name + ".dirs");
    if (fs::exists(dlist)) {
        std::ifstream f(dlist);
        std::vector<fs::path> ps;
        std::string l;
        while (std::getline(f, l)) if (!l.empty()) ps.emplace_back(l);
        std::ranges::sort(ps, std::greater<>{});
        for (const auto& d : ps) {
            fs::path phys = (d.is_absolute()) ? ROOT_DIR / d.relative_path() : ROOT_DIR / d;
            if (fs::exists(phys) && fs::is_directory(phys) && fs::is_empty(phys)) try { fs::remove(phys); } catch (...) {}
        }
        fs::remove(dlist);
    }
    
    const fs::path plist = FILES_DIR / (pkg_name + ".provides");
    if (fs::exists(plist)) {
        auto& cache = Cache::instance();
        std::ifstream f(plist);
        std::string c;
        while (std::getline(f, c)) if (!c.empty()) {
            cache.remove_provider(c, pkg_name);
        }
        fs::remove(plist);
    }
}

void autoremove() {
    log_info(get_string("info.checking_autoremove"));
    auto req = get_all_required_packages(); std::vector<std::string> to_rem;
    auto& cache = Cache::instance();
    std::lock_guard<std::mutex> lock(cache.get_mutex());
    for (const auto& [name, ver] : cache.get_all_installed()) { if (!req.contains(name)) to_rem.push_back(name); }
    if (to_rem.empty()) log_info(get_string("info.no_autoremove_packages"));
    else {
        log_info(string_format("info.autoremove_candidates", to_rem.size()));
        for (const auto& n : to_rem) try { remove_package(n, true); } catch (...) {}
        log_info(string_format("info.autoremove_complete", to_rem.size()));
    }
}

void upgrade_packages() {
    log_info(get_string("info.checking_upgradable")); Repository repo;
    try { repo.load_index(); } catch (...) {}
    std::vector<std::pair<std::string, std::string>> installed;
    auto& cache = Cache::instance();
    {
        std::lock_guard<std::mutex> lock(cache.get_mutex());
        for (const auto& [name, ver] : cache.get_all_installed()) { installed.emplace_back(name, ver); }
    }
    std::vector<std::tuple<std::string, std::string, std::string>> up;
    for (const auto& i : installed) {
        auto opt = repo.find_package(i.first); std::string lat;
        if (opt) lat = opt->version; else try { lat = get_latest_version(i.first); } catch (...) { continue; }
        if (version_compare(i.second, lat)) { log_info(string_format("info.upgradable_found", i.first.c_str(), i.second.c_str(), lat.c_str())); up.emplace_back(i.first, i.second, lat); }
    }
    if (up.empty()) { log_info(get_string("info.all_packages_latest")); return; }
    int count = 0;
    for (const auto& [n, curr, lat] : up) {
        try {
            log_info(string_format("info.upgrading_package", n.c_str(), curr.c_str(), lat.c_str()));
            std::string hash; auto opt = repo.find_package(n); if (opt && opt->version == lat) hash = opt->sha256;
            InstallationTask t(n, lat, is_manually_installed(n), curr, "", hash, false);
            t.run(); write_cache(); count++;
        } catch (const std::exception& e) { log_error(string_format("error.upgrade_failed", n.c_str(), e.what())); }
    }
    if (count > 0) log_info(string_format("info.upgraded_packages", count));
}

void show_man_page(const std::string& pkg_name) {
    const fs::path p = DOCS_DIR / (pkg_name + ".man");
    if (!fs::exists(p)) throw LpkgException(string_format("error.no_man_page", pkg_name.c_str()));
    std::ifstream f(p); if (!f.is_open()) throw LpkgException(string_format("error.open_man_page_failed", p.string().c_str()));
    std::cout << f.rdbuf();
}

void reinstall_package(const std::string& arg) {
    std::string pkg_name = arg;
    if (arg.find('/') != std::string::npos || arg.ends_with(".lpkg") || arg.ends_with(".tar.zst")) {
        try {
            fs::path p(arg);
            pkg_name = parse_package_filename(p.filename().string()).first;
        } catch (...) {}
    }

    std::string ver = get_installed_version(pkg_name);
    if (ver.empty()) {
        install_package(arg, "latest");
        return;
    }

    log_info(string_format("info.reinstalling_package", pkg_name.c_str()));
    
    bool old_overwrite = get_force_overwrite_mode();
    set_force_overwrite_mode(true); // Automatically force overwrite for reinstallation
    try {
        install_packages({arg}, "", true); 
    } catch (...) {
        set_force_overwrite_mode(old_overwrite);
        throw;
    }
    set_force_overwrite_mode(old_overwrite);
}

void query_package(const std::string& pkg_name) {
    std::string ver = get_installed_version(pkg_name);
    if (ver.empty()) {
        log_info(string_format("info.package_not_installed", pkg_name.c_str()));
        return;
    }
    log_info(string_format("info.package_files", pkg_name.c_str()));
    const fs::path list = FILES_DIR / (pkg_name + ".txt");
    if (fs::exists(list)) {
        std::ifstream f(list);
        std::string l;
        while (std::getline(f, l)) {
            if (!l.empty()) std::cout << "  " << l << "\n";
        }
    }
}

void query_file(const std::string& filename) {
    auto& cache = Cache::instance();
    std::string target = filename;
    
    // 1. Try exact match as provided
    auto owners = cache.get_file_owners(target);

    // 2. Try resolving relative to current working directory and then relative to ROOT_DIR
    if (owners.empty()) {
        try {
            fs::path abs_p = fs::absolute(filename);
            // If the absolute path is inside our ROOT_DIR, resolve the logical path
            if (abs_p.string().starts_with(ROOT_DIR.string())) {
                std::string rel_to_root = fs::relative(abs_p, ROOT_DIR).string();
                std::string logical_path = "/" + rel_to_root;
                owners = cache.get_file_owners(logical_path);
                if (!owners.empty()) target = logical_path;
            }
        } catch (...) {
            // Ignore filesystem errors during resolution
        }
    }

    // 3. Fallback: original logic of prepending / if not absolute
    if (owners.empty() && !fs::path(filename).is_absolute()) {
        std::string fallback = (fs::path("/") / filename).string();
        owners = cache.get_file_owners(fallback);
        if (!owners.empty()) target = fallback;
    }

    if (owners.empty()) {
        log_info(string_format("info.file_not_owned", filename.c_str()));
    } else {
        std::string os;
        for (auto it = owners.begin(); it != owners.end(); ++it) {
            os += *it + (std::next(it) == owners.end() ? "" : ", ");
        }
        log_info(string_format("info.file_owned_by", target.c_str(), os.c_str()));
    }
}

void write_cache() {
    Cache::instance().write();
}