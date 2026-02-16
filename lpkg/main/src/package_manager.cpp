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
#include <sys/stat.h>
#include <unistd.h> 
#include <ranges>
#include <concepts>

namespace fs = std::filesystem;

namespace {

// Internal helper to run hooks
void run_hook(std::string_view pkg_name, std::string_view hook_name) {
    if (get_no_hooks_mode()) return;
    
    const fs::path hook_path = HOOKS_DIR / pkg_name / hook_name;
    if (!fs::exists(hook_path) || !fs::is_regular_file(hook_path)) return;

    log_info(string_format("info.running_hook", std::string(hook_name)));
    
    const bool use_chroot = (ROOT_DIR != "/" && !ROOT_DIR.empty());
    std::vector<std::string> args = {"/bin/sh", "-c"};
    
    if (use_chroot) {
        if (!fs::exists(ROOT_DIR / "bin/sh")) {
            log_warning(string_format("warning.hook_failed_setup", std::string(hook_name), get_string("error.sh_not_found")));
            return;
        }
        // Re-execute inside chroot via helper
        const fs::path hook_rel = fs::relative(hook_path, ROOT_DIR);
        args.push_back("/" + hook_rel.string());
    } else {
        args.push_back(hook_path.string());
    }

    pid_t pid = fork();
    if (pid == -1) return;
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
    int ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    
    if (ret != 0) {
        log_warning(string_format("warning.hook_failed_exec", std::string(hook_name), std::to_string(ret)));
    }
}

// Strictly parses Tab-separated line: key\tvalue
std::optional<std::pair<std::string, std::string>> parse_tab_line(const std::string& line) {
    if (line.empty()) return std::nullopt;
    if (const auto pos = line.find('\t'); pos != std::string::npos) {
        std::string key = line.substr(0, pos);
        std::string val = line.substr(pos + 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();
        return std::make_pair(std::move(key), std::move(val));
    }
    return std::nullopt;
}

} // anonymous namespace

InstallationTask::InstallationTask(std::string pkg_name, std::string version, bool explicit_install, std::string old_version_to_replace, fs::path local_package_path, std::string expected_hash, bool force_reinstall)
    : pkg_name_(std::move(pkg_name)), version_(std::move(version)), explicit_install_(explicit_install), 
      tmp_pkg_dir_(get_tmp_dir() / pkg_name_), actual_version_(version_), old_version_to_replace_(std::move(old_version_to_replace)),
      local_package_path_(std::move(local_package_path)), expected_hash_(std::move(expected_hash)),
      force_reinstall_(force_reinstall) {}

void InstallationTask::run() {
    const std::string current_installed_version = Cache::instance().get_installed_version(pkg_name_);
    if (!force_reinstall_ && !current_installed_version.empty() && current_installed_version == actual_version_) {
        log_info(string_format("info.package_already_installed", pkg_name_));
        return;
    }
    
    log_info(string_format("info.installing_package", pkg_name_, version_));
    ensure_dir_exists(tmp_pkg_dir_);
    
    try {
        prepare();
        commit();
    } catch (const std::exception& e) {
        rollback_files();
        throw;
    }
    log_info(string_format("info.package_installed_successfully", pkg_name_));
}

void InstallationTask::prepare() {
    download_and_verify_package();
    extract_and_validate_package();
    check_for_file_conflicts();
}

void InstallationTask::rollback_files() {
    log_error(string_format("error.rollback_install", pkg_name_));
    for (const auto& file : installed_files_) {
        if (fs::exists(file) || fs::is_symlink(file)) {
            std::error_code ec;
            fs::remove_all(file, ec);
            if (ec) {
                // Brute force for root
                std::string cmd = "sudo rm -rf \"" + file.string() + "\" 2>/dev/null";
                (void)std::system(cmd.c_str());
            }
        }
    }
    for (const auto& [physical, backup] : backups_) {
        if (fs::exists(backup)) {
            std::error_code ec;
            fs::rename(backup, physical, ec);
            if (ec) {
                std::string cmd = "sudo mv \"" + backup.string() + "\" \"" + physical.string() + "\" 2>/dev/null";
                (void)std::system(cmd.c_str());
            }
        }
    }
    
    // Clean up empty dirs in reverse
    for (const auto& dir : created_dirs_ | std::views::reverse) {
        if (fs::exists(dir) && fs::is_directory(dir) && fs::is_empty(dir)) {
            std::error_code ec;
            fs::remove(dir, ec);
        }
    }
}

void InstallationTask::commit() {
    std::unordered_set<std::string> old_files;
    if (!old_version_to_replace_.empty()) {
        const fs::path old_files_list = FILES_DIR / (pkg_name_ + ".txt");
        if (fs::exists(old_files_list)) {
            std::ifstream f(old_files_list);
            std::string line;
            while (std::getline(f, line)) if (!line.empty()) old_files.insert(line);
        }
    }

    copy_package_files();
    
    try {
        register_package();
        
        if (!old_files.empty()) {
            std::unordered_set<std::string> new_files;
            const fs::path new_files_list = FILES_DIR / (pkg_name_ + ".txt");
            std::ifstream f(new_files_list);
            std::string line;
            while (std::getline(f, line)) if (!line.empty()) new_files.insert(line);
            
            auto& cache = Cache::instance();
            for (const auto& old_file : old_files) {
                if (old_file.starts_with("/etc/")) continue; // Protective rule
                if (!new_files.contains(old_file)) {
                    auto owners = cache.get_file_owners(old_file);
                    if (owners.contains(pkg_name_)) {
                        cache.remove_file_owner(old_file, pkg_name_);
                        if (cache.get_file_owners(old_file).empty()) {
                            const fs::path phys = (fs::path(old_file).is_absolute()) ? 
                                ROOT_DIR / fs::path(old_file).relative_path() : ROOT_DIR / old_file;
                            if (fs::exists(phys) || fs::is_symlink(phys)) {
                                log_info(string_format("info.removing_obsolete_file", old_file));
                                fs::remove(phys);
                            }
                        }
                    }
                }
            }
            
            // Cleanup obsolete directories
            const fs::path old_dirs_list = FILES_DIR / (pkg_name_ + ".dirs");
            if (fs::exists(old_dirs_list)) {
                std::ifstream df(old_dirs_list);
                std::vector<fs::path> old_dirs;
                while (std::getline(df, line)) if (!line.empty()) old_dirs.emplace_back(line);
                
                std::ranges::sort(old_dirs, std::greater<>{});
                for (const auto& d : old_dirs) {
                    const fs::path phys = (d.is_absolute()) ? ROOT_DIR / d.relative_path() : ROOT_DIR / d;
                    if (fs::exists(phys) && fs::is_directory(phys) && fs::is_empty(phys)) {
                        fs::remove(phys);
                    }
                }
            }
        }
    } catch (...) { throw; }

    for (const auto& [physical, backup] : backups_) { 
        std::error_code ec;
        fs::remove(backup, ec); 
    }
    backups_.clear();
    run_post_install_hook();
}

void InstallationTask::download_and_verify_package() {
    if (!local_package_path_.empty()) {
        if (!fs::exists(local_package_path_)) 
            throw LpkgException(string_format("error.local_pkg_not_found", local_package_path_.string()));
        
        log_info(string_format("info.installing_local_file", local_package_path_.string()));
        archive_path_ = local_package_path_;
        if (!expected_hash_.empty() && calculate_sha256(archive_path_) != expected_hash_)
            throw LpkgException(string_format("error.hash_mismatch", pkg_name_));
        return;
    }

    const std::string mirror_url = get_mirror_url();
    const std::string arch = get_architecture();
    
    // actual_version_ and expected_hash_ should have been resolved from Repository index in the plan phase
    if (actual_version_.empty() || actual_version_ == "latest") {
         // Fallback/safety: if plan didn't resolve version, try to use Repository directly
         Repository repo;
         repo.load_index();
         auto info = repo.find_package(pkg_name_);
         if (info) {
             actual_version_ = info->version;
             expected_hash_ = info->sha256;
         } else {
             throw LpkgException(string_format("warning.package_not_in_repo", pkg_name_));
         }
    }
    
    // New URL format: mirror/arch/pkg/version.lpkg
    const std::string download_url = mirror_url + arch + "/" + pkg_name_ + "/" + actual_version_ + ".lpkg";
    archive_path_ = tmp_pkg_dir_ / (actual_version_ + ".lpkg");
    
    if (!fs::exists(archive_path_)) download_with_retries(download_url, archive_path_, 5, true);
    if (!expected_hash_.empty() && calculate_sha256(archive_path_) != expected_hash_) 
        throw LpkgException(string_format("error.hash_mismatch", pkg_name_));
}

void InstallationTask::extract_and_validate_package() {
    log_info(get_string("info.extracting_to_tmp"));
    extract_tar_zst(archive_path_, tmp_pkg_dir_);
    for (const auto& meta : {"man.txt", "deps.txt", "files.txt", "content/"}) {
        if (!fs::exists(tmp_pkg_dir_ / meta))
            throw LpkgException(string_format("error.incomplete_package", (tmp_pkg_dir_ / meta).string()));
    }
}

void InstallationTask::check_for_file_conflicts() {
    std::map<std::string, std::string> conflicts;
    std::ifstream files_list(tmp_pkg_dir_ / "files.txt");
    std::string line;
    auto& cache = Cache::instance();
    const bool force_overwrite = get_force_overwrite_mode();
    
    while (std::getline(files_list, line)) {
        auto res = parse_tab_line(line);
        if (!res) continue;
        
        const auto& [src, dest] = *res;
        const fs::path logical_path = fs::path(dest) / src;
        
        if (fs::is_directory(tmp_pkg_dir_ / "content" / src)) continue;

        const std::string path_str = logical_path.string();
        auto owners = cache.get_file_owners(path_str);
        
        if (!owners.empty()) {
            for (const auto& owner : owners) {
                if (owner != pkg_name_ && !force_overwrite) conflicts[path_str] = owner;
            }
            continue;
        }
        
        // Manual file check
        if (old_version_to_replace_.empty()) {
            const fs::path phys = (logical_path.is_absolute()) ? ROOT_DIR / logical_path.relative_path() : ROOT_DIR / logical_path;
            if ((fs::exists(phys) || fs::is_symlink(phys)) && !force_overwrite) {
                conflicts[path_str] = "unknown (manual file)";
            }
        }
    }
    
    if (!conflicts.empty()) {
        std::string msg = get_string("error.file_conflict_header") + "\n";
        for (const auto& [file, owner] : conflicts)
            msg += "  " + string_format("error.file_conflict_entry", file, owner) + "\n";
        throw LpkgException(msg + get_string("error.installation_aborted"));
    }
}

void InstallationTask::copy_package_files() {
    log_info(get_string("info.copying_files"));
    std::ifstream files_list(tmp_pkg_dir_ / "files.txt");
    std::string line;
    while (std::getline(files_list, line)) {
        auto res = parse_tab_line(line);
        if (!res) continue;

        const auto& [src, dest] = *res;
        const fs::path src_path = tmp_pkg_dir_ / "content" / src;
        const fs::path logical_path = fs::path(dest) / src;
        const fs::path physical_path = (logical_path.is_absolute()) ? ROOT_DIR / logical_path.relative_path() : ROOT_DIR / logical_path;
        
        if (!fs::exists(src_path) && !fs::is_symlink(src_path)) continue;
        
        // Create parent directories
        fs::path parent = physical_path.parent_path();
        std::vector<fs::path> to_create;
        while (!parent.empty() && parent != ROOT_DIR && !fs::exists(parent)) { 
            to_create.push_back(parent); 
            parent = parent.parent_path(); 
        }
        for (const auto& d : to_create | std::views::reverse) { 
            ensure_dir_exists(d); 
            created_dirs_.insert(d); 
        }
        
        if (fs::is_directory(src_path)) {
            ensure_dir_exists(physical_path);
            struct stat st;
            if (lstat(src_path.c_str(), &st) == 0) {
                (void)lchown(physical_path.c_str(), st.st_uid, st.st_gid);
                (void)chmod(physical_path.c_str(), st.st_mode & 07777);
            }
            continue;
        }

        try {
            const bool is_config = src.starts_with("etc/");
            fs::path final_dest = physical_path;
            
            if (is_config && fs::exists(physical_path) && !fs::is_directory(physical_path)) {
                final_dest += ".lpkgnew";
                if (fs::exists(final_dest) || fs::is_symlink(final_dest)) fs::remove(final_dest);
                log_warning(string_format("warning.config_conflict", physical_path.string(), final_dest.string()));
                has_config_conflicts_ = true;
            } else if (fs::exists(physical_path) || fs::is_symlink(physical_path)) {
                if (!fs::is_directory(physical_path)) {
                    fs::path bak = physical_path; bak += ".lpkg_bak_" + pkg_name_; 
                    fs::rename(physical_path, bak); 
                    backups_.emplace_back(physical_path, bak);
                }
            }

            if (fs::is_symlink(src_path)) {
                fs::path link_target = fs::read_symlink(src_path);
                if (fs::exists(final_dest) || fs::is_symlink(final_dest)) fs::remove(final_dest);
                fs::create_symlink(link_target, final_dest);
            } else {
                fs::copy(src_path, final_dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            }
            
            struct stat st;
            if (lstat(src_path.c_str(), &st) == 0) {
                (void)lchown(final_dest.c_str(), st.st_uid, st.st_gid);
                // Linux has no lchmod, calling chmod on a symlink will follow it.
                // We must skip chmod for symlinks to prevent target corruption.
                if (!S_ISLNK(st.st_mode)) {
                    (void)chmod(final_dest.c_str(), st.st_mode & 07777);
                }
            }

            installed_files_.push_back(final_dest);
            TriggerManager::instance().check_file(logical_path.string());
        } catch (const std::exception& e) {
            throw LpkgException(string_format("error.copy_failed_rollback", src, physical_path.string(), e.what()));
        }
    }
    
    if (has_config_conflicts_) log_warning(get_string("info.config_review_reminder"));

    // Update metadata files
    std::ofstream pkg_f(FILES_DIR / (pkg_name_ + ".txt"));
    std::ifstream fl2(tmp_pkg_dir_ / "files.txt");
    std::string fl2_line;
    while (std::getline(fl2, fl2_line)) {
        if (auto r = parse_tab_line(fl2_line)) {
            pkg_f << (fs::path(r->second) / r->first).string() << "\n";
        }
    }
    std::ofstream dir_f(FILES_DIR / (pkg_name_ + ".dirs"));
    for (const auto& d : created_dirs_) dir_f << d.string() << "\n";
}

void InstallationTask::register_package() {
    auto& cache = Cache::instance();
    
    // Helper to cleanup old records if upgrading
    auto cleanup_old = [&](const fs::path& path, auto remover) {
        if (!old_version_to_replace_.empty() && fs::exists(path)) {
            std::ifstream f(path);
            std::string line;
            while (std::getline(f, line)) if (!line.empty()) remover(line);
        }
    };

    cleanup_old(DEP_DIR / pkg_name_, [&](const std::string& l) {
        std::stringstream ss(l); std::string dn; if (ss >> dn) cache.remove_reverse_dep(dn, pkg_name_);
    });
    cleanup_old(FILES_DIR / (pkg_name_ + ".provides"), [&](const std::string& c) {
        cache.remove_provider(c, pkg_name_);
    });

    std::ifstream deps_in(tmp_pkg_dir_ / "deps.txt");
    std::ofstream deps_out(DEP_DIR / pkg_name_);
    std::string d;
    while (std::getline(deps_in, d)) {
        if (d.empty()) continue;
        deps_out << d << "\n";
        std::string name = d;
        if (const auto pos = d.find_first_of(" \t<>="); pos != std::string::npos) name = d.substr(0, pos);
        cache.add_reverse_dep(name, pkg_name_);
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
    const fs::path hook_src = tmp_pkg_dir_ / "hooks";
    if (!fs::exists(hook_src) || !fs::is_directory(hook_src)) return;
    
    const fs::path dest_dir = HOOKS_DIR / pkg_name_;
    ensure_dir_exists(dest_dir);
    for (const auto& entry : fs::directory_iterator(hook_src)) {
        if (entry.is_regular_file()) {
            const fs::path dest = dest_dir / entry.path().filename();
            fs::copy(entry.path(), dest, fs::copy_options::overwrite_existing);
            fs::permissions(dest, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add);
        }
    }
    run_hook(pkg_name_, "postinst.sh");
}

namespace {

struct InstallPlan {
    std::string name, actual_version, sha256;
    bool is_explicit = false;
    fs::path local_path;
    std::vector<DependencyInfo> dependencies;
    bool force_reinstall = false;
};

struct ResolutionContext {
    Repository& repo;
    const std::map<std::string, fs::path>& local_candidates;
    std::map<std::string, InstallPlan>& plan;
    std::vector<std::string>& install_order;
    bool force_reinstall = false;
};

void resolve_package_dependencies(const std::string& pkg_name, const std::string& version_spec, bool is_explicit, 
    ResolutionContext& ctx, std::set<std::string>& visited_stack) {
    
    if (visited_stack.contains(pkg_name)) { 
        log_warning(string_format("warning.circular_dependency", pkg_name, pkg_name)); 
        return; 
    }
    if (ctx.plan.contains(pkg_name)) { 
        if (is_explicit) ctx.plan.at(pkg_name).is_explicit = true; 
        return; 
    }

    const std::string installed_version = Cache::instance().get_installed_version(pkg_name);
    fs::path local_path; 
    std::string latest_version, pkg_hash; 
    std::vector<DependencyInfo> deps;

    if (auto it = ctx.local_candidates.find(pkg_name); it != ctx.local_candidates.end()) {
        local_path = it->second;
        latest_version = parse_package_filename(local_path.filename().string()).second;
        std::stringstream ss(extract_file_from_archive(local_path, "deps.txt"));
        std::string line;
        while (std::getline(ss, line)) {
            std::stringstream line_ss(line);
            std::string dn, op, rv;
            if (line_ss >> dn) {
                DependencyInfo d{.name = dn, .op = "", .version_req = ""};
                if (line_ss >> op >> rv) { d.op = op; d.version_req = rv; }
                deps.push_back(std::move(d));
            }
        }
    } else {
        auto pkg_info = (version_spec == "latest") ? ctx.repo.find_package(pkg_name) : ctx.repo.find_package(pkg_name, version_spec);
        if (!pkg_info) {
            if (auto prov = ctx.repo.find_provider(pkg_name)) {
                resolve_package_dependencies(prov->name, prov->version, is_explicit, ctx, visited_stack);
                return;
            }
            if (installed_version.empty()) log_warning(string_format("warning.package_not_in_repo", pkg_name));
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
    InstallPlan p{
        .name = pkg_name, .actual_version = latest_version, .sha256 = pkg_hash,
        .is_explicit = is_explicit, .local_path = local_path, .dependencies = deps,
        .force_reinstall = (ctx.force_reinstall && is_explicit)
    };

    if (!get_no_deps_mode()) {
        for (const auto& dep : deps) {
            const std::string idv = Cache::instance().get_installed_version(dep.name);
            bool needs_resolution = idv.empty();
            if (!needs_resolution && !dep.op.empty() && idv != "virtual" && !version_satisfies(idv, dep.op, dep.version_req)) {
                if (!ctx.plan.contains(dep.name)) {
                    log_info(string_format("info.adding_upgrade_to_plan", dep.name, dep.version_req));
                    needs_resolution = true;
                }
            }
            if (needs_resolution) {
                std::string req_ver = "latest";
                if (!dep.op.empty()) {
                    if (auto matching = ctx.repo.find_best_matching_version(dep.name, dep.op, dep.version_req))
                        req_ver = matching->version;
                }
                resolve_package_dependencies(dep.name, req_ver, false, ctx, visited_stack);
            }
            
            std::string cand_v = ctx.plan.contains(dep.name) ? ctx.plan[dep.name].actual_version : Cache::instance().get_installed_version(dep.name);
            if (!dep.op.empty() && !cand_v.empty() && cand_v != "virtual" && !version_satisfies(cand_v, dep.op, dep.version_req))
                throw LpkgException(string_format("error.candidate_dep_version_mismatch", dep.name, cand_v, dep.op, dep.version_req));
        }
    }
    ctx.plan[pkg_name] = std::move(p);
    ctx.install_order.push_back(pkg_name);
    visited_stack.erase(pkg_name);
}

std::set<std::string> check_plan_consistency(const std::map<std::string, InstallPlan>& plan) {
    std::set<std::string> broken;
    auto& cache = Cache::instance();
    std::lock_guard lock(cache.get_mutex());
    for (const auto& [pkg, ver] : cache.get_all_installed()) {
        if (plan.contains(pkg)) continue;
        const fs::path dep_file = DEP_DIR / pkg;
        if (!fs::exists(dep_file)) continue;
        
        std::ifstream f(dep_file);
        std::string line;
        while (std::getline(f, line)) {
            std::stringstream ss(line); std::string dep_name, op, req_v;
            if (ss >> dep_name && plan.contains(dep_name)) {
                const std::string& new_v = plan.at(dep_name).actual_version;
                if (ss >> op >> req_v && !version_satisfies(new_v, op, req_v)) {
                    log_error(string_format("error.conflict_breaks_existing", dep_name, new_v, pkg, op, req_v));
                    broken.insert(pkg);
                }
            }
        }
    }
    return broken;
}

std::unordered_set<std::string> get_all_required_packages() {
    auto& cache = Cache::instance();
    std::unordered_set<std::string> req;
    {
        std::lock_guard lock(cache.get_mutex());
        req = cache.get_all_held();
    }
    std::vector q(req.begin(), req.end());
    size_t head = 0;
    while (head < q.size()) {
        const std::string curr = q[head++]; 
        const fs::path p = DEP_DIR / curr;
        if (!fs::exists(p)) continue;
        
        std::ifstream f(p);
        std::string line;
        while (std::getline(f, line)) {
            std::string d_name = line;
            if (const auto pos = line.find_first_of(" \t<>="); pos != std::string::npos) d_name = line.substr(0, pos);
            
            auto check_and_add = [&](const std::string& name) {
                if (cache.is_installed(name) && !req.contains(name)) {
                    req.insert(name); q.push_back(name);
                }
            };
            
            if (cache.is_installed(d_name)) check_and_add(d_name);
            else for (const auto& prov : cache.get_providers(d_name)) check_and_add(prov);
        }
    }
    return req;
}

} // anonymous namespace

void write_cache() {
    Cache::instance().write();
}

void install_packages(const std::vector<std::string>& pkg_args, const std::string& hash_file_path, bool force_reinstall) {
    Cache::instance().load(); 
    TmpDirManager tmp; 
    Repository repo;
    try { repo.load_index(); } catch (const std::exception& e) { log_warning(string_format("warning.repo_index_load_failed", e.what())); }
    
    std::map<std::string, InstallPlan> plan; 
    std::vector<std::string> order; 
    std::map<std::string, fs::path> locals;
    std::vector<std::pair<std::string, std::string>> targets;

    std::string provided_hash;
    if (!hash_file_path.empty()) {
        std::ifstream hf(hash_file_path);
        if (!(hf >> provided_hash)) throw LpkgException("Failed to read hash from provided file.");
    }

    for (const auto& arg : pkg_args) {
        const fs::path p(arg);
        if (p.extension() == ".zst" || p.extension() == ".lpkg" || arg.find('/') != std::string::npos) {
            if (fs::exists(p)) { 
                try { 
                    auto [n, v] = parse_package_filename(p.filename().string()); 
                    locals[n] = fs::absolute(p); 
                    targets.emplace_back(n, v); 
                } catch (const std::exception& e) { log_error(string_format("warning.skip_invalid_local_pkg", arg, e.what())); }
            } else log_error(string_format("error.local_pkg_not_found", arg));
        } else {
            std::string n = arg, v = "latest";
            if (const auto pos = arg.find(':'); pos != std::string::npos) { n = arg.substr(0, pos); v = arg.substr(pos+1); }
            targets.emplace_back(n, v);
        }
    }

    ResolutionContext ctx{repo, locals, plan, order, force_reinstall};
    for (const auto& [n, v] : targets) { std::set<std::string> vs; resolve_package_dependencies(n, v, true, ctx, vs); }

    if (!provided_hash.empty()) {
        if (locals.empty()) throw LpkgException("--hash can only be used with local package installations.");
        for (auto& [n, p] : plan) if (!p.local_path.empty()) p.sha256 = provided_hash;
    }

    if (plan.empty()) { log_info(get_string("info.all_packages_already_installed")); return; }
    
    if (auto broken = check_plan_consistency(plan); !broken.empty()) {
        log_error(get_string("error.dependency_conflict_title"));
        if (user_confirms(get_string("prompt.remove_conflict_pkgs"))) {
            for (const auto& pkg : broken) remove_package(pkg, true);
            Cache::instance().write();
            install_packages(pkg_args, hash_file_path, force_reinstall);
            return;
        }
        log_info(get_string("info.installation_aborted"));
        return;
    }

    // Confirmation prompt
    std::string prompt;
    for (const auto& n : order) {
        const auto& p = plan.at(n);
        prompt += "  " + string_format(p.is_explicit ? "info.package_list_item" : "info.package_list_item_dep", p.name, p.actual_version) + "\n";
    }
    if (!user_confirms(prompt + get_string("info.confirm_proceed"))) { log_info(get_string("info.installation_aborted")); return; }
    
    std::vector<std::string> installed;
    try {
        for (const auto& n : order) {
            const auto& p = plan.at(n); 
            InstallationTask task(p.name, p.actual_version, p.is_explicit, Cache::instance().get_installed_version(p.name), p.local_path, p.sha256, p.force_reinstall);
            task.run();
            installed.push_back(p.name);
        }
    } catch (const std::exception& e) {
        log_error(get_string("error.installation_failed_rolling_back"));
        for (const auto& name : installed | std::views::reverse) {
            try { remove_package(name, true); } catch (...) {}
        }
        Cache::instance().write();
        throw;
    }

    Cache::instance().write();
    TriggerManager::instance().run_all();
    log_info(get_string("info.install_complete"));
}

void remove_package(const std::string& pkg_name, bool force) {
    const std::string ver = Cache::instance().get_installed_version(pkg_name);
    if (ver.empty()) { log_info(string_format("info.package_not_installed", pkg_name)); return; }
    
    if (!force) {
        if (Cache::instance().is_essential(pkg_name)) { log_error(string_format("error.skip_remove_essential", pkg_name)); return; }
        if (auto rdeps = Cache::instance().get_reverse_deps(pkg_name); !rdeps.empty()) {
            std::string list; for (const auto& d : rdeps) list += d + " ";
            log_info(string_format("info.skip_remove_dependency", pkg_name, list)); return;
        }
        // Check provides rdeps
        const fs::path plist = FILES_DIR / (pkg_name + ".provides");
        if (fs::exists(plist)) {
            std::ifstream f(plist); std::string cap;
            while (std::getline(f, cap)) {
                if (auto rdeps = Cache::instance().get_reverse_deps(cap); !rdeps.empty()) {
                    std::string list; for (const auto& d : rdeps) list += d + " ";
                    log_info(string_format("info.skip_remove_dependency", cap, list)); return;
                }
            }
        }
    }

    log_info(string_format("info.removing_package", pkg_name));
    run_hook(pkg_name, "prerm.sh");
    remove_package_files(pkg_name, force);
    
    // Cleanup cache
    auto& cache = Cache::instance();
    const fs::path dep_file = DEP_DIR / pkg_name;
    if (fs::exists(dep_file)) {
        std::ifstream f(dep_file); std::string l;
        while(std::getline(f, l)) {
            std::stringstream ss(l); std::string dn;
            if (ss >> dn) cache.remove_reverse_dep(dn, pkg_name);
        }
    }
    
    fs::remove(dep_file); 
    fs::remove(DOCS_DIR / (pkg_name + ".man")); 
    fs::remove_all(HOOKS_DIR / pkg_name);
    cache.remove_installed(pkg_name);
    log_info(string_format("info.package_removed_successfully", pkg_name));
}

void remove_package_files(const std::string& pkg_name, bool force) {
    const fs::path list = FILES_DIR / (pkg_name + ".txt");
    if (!fs::exists(list)) return;

    auto& cache = Cache::instance();
    std::vector<fs::path> paths;
    std::map<std::string, std::vector<std::string>> shared;
    
    {
        std::ifstream f(list); std::string l;
        while (std::getline(f, l)) {
            if (l.empty()) continue;
            paths.emplace_back(l);
            for (const auto& owner : cache.get_file_owners(l)) if (owner != pkg_name) shared[l].push_back(owner);
        }
    }

    if (!shared.empty() && !force) {
        std::string msg = get_string("error.shared_file_header") + "\n";
        for (const auto& [file, owners] : shared) {
            std::string os; for (size_t i=0; i<owners.size(); ++i) os += owners[i] + (i==owners.size()-1 ? "" : ", ");
            msg += "  " + string_format("error.shared_file_entry", file, os) + "\n";
        }
        throw LpkgException(msg + get_string("error.removal_aborted"));
    }
    
    std::ranges::sort(paths, std::greater<>{});
    int count = 0;
    for (const auto& p : paths) {
        const fs::path phys = (p.is_absolute()) ? ROOT_DIR / p.relative_path() : ROOT_DIR / p;
        if (fs::exists(phys) || fs::is_symlink(phys)) {
            if (auto owners = cache.get_file_owners(p.string()); owners.contains(pkg_name)) {
                if (owners.size() == 1) { fs::remove(phys); count++; }
                else log_info(string_format("info.skipped_remove", p.string()));
            }
        }
        cache.remove_file_owner(p.string(), pkg_name);
    }
    log_info(string_format("info.files_removed", count)); 
    fs::remove(list);

    // Cleanup dirs and provides
    if (const fs::path dlist = FILES_DIR / (pkg_name + ".dirs"); fs::exists(dlist)) {
        std::ifstream f(dlist); std::string l; std::vector<fs::path> ds;
        while (std::getline(f, l)) if (!l.empty()) ds.emplace_back(l);
        std::ranges::sort(ds, std::greater<>{});
        for (const auto& d : ds) {
            const fs::path phys = (d.is_absolute()) ? ROOT_DIR / d.relative_path() : ROOT_DIR / d;
            if (fs::exists(phys) && fs::is_directory(phys) && fs::is_empty(phys)) fs::remove(phys);
        }
        fs::remove(dlist);
    }
    
    if (const fs::path plist = FILES_DIR / (pkg_name + ".provides"); fs::exists(plist)) {
        std::ifstream f(plist); std::string c;
        while (std::getline(f, c)) if (!c.empty()) cache.remove_provider(c, pkg_name);
        fs::remove(plist);
    }
}

void autoremove() {
    log_info(get_string("info.checking_autoremove"));
    const auto req = get_all_required_packages();
    std::vector<std::string> to_rem;
    auto& cache = Cache::instance();
    {
        std::lock_guard lock(cache.get_mutex());
        for (const auto& name : cache.get_all_installed() | std::views::keys) if (!req.contains(name)) to_rem.push_back(name);
    }
    
    if (to_rem.empty()) log_info(get_string("info.no_autoremove_packages"));
    else {
        log_info(string_format("info.autoremove_candidates", to_rem.size()));
        for (const auto& n : to_rem) try { remove_package(n, true); } catch (...) {}
        log_info(string_format("info.autoremove_complete", to_rem.size()));
    }
}

void upgrade_packages() {
    log_info(get_string("info.checking_upgradable")); 
    Repository repo;
    try { repo.load_index(); } catch (...) {}
    
    std::vector<std::pair<std::string, std::string>> installed;
    {
        std::lock_guard lock(Cache::instance().get_mutex());
        for (const auto& [name, ver] : Cache::instance().get_all_installed()) installed.emplace_back(name, ver);
    }
    
    int count = 0;
    for (const auto& [n, curr] : installed) {
        auto opt = repo.find_package(n); 
        if (!opt) continue;
        
        std::string lat = opt->version;
        
        if (version_compare(curr, lat)) {
            log_info(string_format("info.upgradable_found", n, curr, lat));
            try {
                log_info(string_format("info.upgrading_package", n, curr, lat));
                const std::string hash = opt->sha256;
                const bool held = Cache::instance().is_held(n);
                InstallationTask t(n, lat, held, curr, "", hash, false);
                t.run(); count++;
            } catch (const std::exception& e) { log_error(string_format("error.upgrade_failed", n, e.what())); }
        }
    }
    if (count > 0) log_info(string_format("info.upgraded_packages", count));
    else log_info(get_string("info.all_packages_latest"));
    Cache::instance().write();
}

void show_man_page(const std::string& pkg_name) {
    const fs::path p = DOCS_DIR / (pkg_name + ".man");
    if (!fs::exists(p)) throw LpkgException(string_format("error.no_man_page", pkg_name));
    std::ifstream f(p); if (!f.is_open()) throw LpkgException(string_format("error.open_man_page_failed", p.string()));
    std::cout << f.rdbuf();
}

void reinstall_package(const std::string& arg) {
    std::string name = arg;
    if (arg.find('/') != std::string::npos || arg.ends_with(".lpkg") || arg.ends_with(".tar.zst")) {
        try { name = parse_package_filename(fs::path(arg).filename().string()).first; } catch (...) {}
    }

    if (Cache::instance().get_installed_version(name).empty()) {
        install_packages({arg});
        return;
    }

    log_info(string_format("info.reinstalling_package", name));
    const bool old_ovr = get_force_overwrite_mode();
    set_force_overwrite_mode(true); 
    try { install_packages({arg}, "", true); } catch (...) { set_force_overwrite_mode(old_ovr); throw; }
    set_force_overwrite_mode(old_ovr);
}

void query_package(const std::string& pkg_name) {
    if (Cache::instance().get_installed_version(pkg_name).empty()) {
        log_info(string_format("info.package_not_installed", pkg_name));
        return;
    }
    log_info(string_format("info.package_files", pkg_name));
    const fs::path list = FILES_DIR / (pkg_name + ".txt");
    if (fs::exists(list)) {
        std::ifstream f(list); std::string l;
        while (std::getline(f, l)) if (!l.empty()) std::cout << "  " << l << "\n";
    }
}

void query_file(const std::string& filename) {
    auto& cache = Cache::instance();
    std::string target = filename;
    auto owners = cache.get_file_owners(target);

    if (owners.empty()) {
        try {
            const fs::path abs_p = fs::absolute(filename);
            if (abs_p.string().starts_with(ROOT_DIR.string())) {
                const std::string logical = "/" + fs::relative(abs_p, ROOT_DIR).string();
                owners = cache.get_file_owners(logical);
                if (!owners.empty()) target = logical;
            }
        } catch (...) {}
    }

    if (owners.empty() && !fs::path(filename).is_absolute()) {
        const std::string fallback = (fs::path("/") / filename).string();
        owners = cache.get_file_owners(fallback);
        if (!owners.empty()) target = fallback;
    }

    if (owners.empty()) log_info(string_format("info.file_not_owned", filename));
    else {
        std::string os;
        for (auto it = owners.begin(); it != owners.end(); ++it) os += *it + (std::next(it) == owners.end() ? "" : ", ");
        log_info(string_format("info.file_owned_by", target, os));
    }
}
