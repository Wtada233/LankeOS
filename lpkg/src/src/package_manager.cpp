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
#include <sys/mount.h> 
#include <sys/wait.h> 
#include <unistd.h> 

namespace fs = std::filesystem;

namespace {

// --- 缓存机制 ---
struct Cache {
    std::map<std::string, std::unordered_set<std::string>> file_db; // 文件路径 -> {所有者包名}
    std::map<std::string, std::unordered_set<std::string>> providers; // 虚拟包名 -> {真实包名}
    std::map<std::string, std::string> installed_pkgs; // 包名 -> 已安装版本
    std::unordered_set<std::string> holdpkgs; // 手动安装的包列表
    std::map<std::string, std::unordered_set<std::string>> reverse_deps; // 依赖包 -> {依赖它的包}
    bool dirty = false;
    std::mutex mtx;

    Cache() {
        load();
    }

    void load() {
        file_db = read_db_uncached(FILES_DB);
        providers = read_db_uncached(PROVIDES_DB);
        
        // 加载已安装包列表 (格式为 name:version)
        auto pkg_set = read_set_from_file(PKGS_FILE);
        installed_pkgs.clear();
        for (const auto& line : pkg_set) {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                installed_pkgs[line.substr(0, pos)] = line.substr(pos + 1);
            }
        }

        holdpkgs = read_set_from_file(HOLDPKGS_FILE);
        
        reverse_deps.clear();
        if (fs::exists(DEP_DIR) && fs::is_directory(DEP_DIR)) {
             for (const auto& entry : fs::directory_iterator(DEP_DIR)) {
                 if (entry.is_regular_file()) {
                     std::string pkg_name = entry.path().filename().string();
                     std::ifstream f(entry.path());
                     std::string dep_line;
                     while(std::getline(f, dep_line)) {
                         if(!dep_line.empty()) {
                             if (dep_line.back() == '\r') dep_line.pop_back();
                             std::stringstream ss(dep_line);
                             std::string d_name;
                             ss >> d_name;
                             if (!d_name.empty()) {
                                 reverse_deps[d_name].insert(pkg_name);
                             }
                         }
                     }
                 }
             }
        }
        dirty = false;
    }

    void write_pkgs() { 
        std::unordered_set<std::string> pkg_set;
        for (const auto& [name, ver] : installed_pkgs) {
            pkg_set.insert(name + ":" + ver);
        }
        write_set_to_file(PKGS_FILE, pkg_set); 
    }
    void write_holdpkgs() { write_set_to_file(HOLDPKGS_FILE, holdpkgs); }
    void write_file_db() { write_db_uncached(FILES_DB, file_db); }
    void write_providers() { write_db_uncached(PROVIDES_DB, providers); }

private:
    std::map<std::string, std::unordered_set<std::string>> read_db_uncached(const fs::path& path) {
        std::map<std::string, std::unordered_set<std::string>> db;
        std::ifstream db_file(path);
        if (!db_file.is_open()) return db;
        std::string key, value;
        while (db_file >> key >> value) {
            db[key].insert(value);
        }
        return db;
    }

    void write_db_uncached(const fs::path& path, const std::map<std::string, std::unordered_set<std::string>>& db) {
        fs::path tmp_path = path.string() + ".tmp";
        {
            std::ofstream db_file(tmp_path, std::ios::trunc);
            if (!db_file.is_open()) throw LpkgException(string_format("error.create_file_failed", tmp_path.string()));
            for (const auto& [key, values] : db) {
                for (const auto& value : values) {
                    db_file << key << " " << value << "\n";
                }
            }
        }
        fs::rename(tmp_path, path);
    }
};

Cache& get_cache() {
    static Cache cache;
    return cache;
}

// Forward declarations for internal logic
std::string get_installed_version(const std::string& pkg_name);
bool is_manually_installed(const std::string& pkg_name);
bool is_essential_package(const std::string& pkg_name);
void run_hook(const std::string& pkg_name, const std::string& hook_name);

void force_reload_cache() {
    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);
    cache.load();
}

bool is_essential_package(const std::string& pkg_name) {
    static std::unordered_set<std::string> essentials;
    static bool loaded = false;
    if (!loaded) {
        if (fs::exists(ESSENTIAL_FILE)) {
            essentials = read_set_from_file(ESSENTIAL_FILE);
        }
        loaded = true;
    }
    return essentials.contains(pkg_name);
}

// 获取已安装包的版本。如果未安装则返回空字符串。
std::string get_installed_version(const std::string& pkg_name) {
    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);
    
    // 首先从已安装映射中查找
    auto it = cache.installed_pkgs.find(pkg_name);
    if (it != cache.installed_pkgs.end()) {
        return it->second;
    }
    
    // 检查是否由其他包提供（虚拟包）
    if (cache.providers.count(pkg_name)) return "virtual";
    return "";
}

bool is_manually_installed(const std::string& pkg_name) {
    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);
    return cache.holdpkgs.contains(pkg_name);
}

// Chroot 环境下的伪文件系统挂载保护类 (RAII)
class ChrootMountGuard {
public:
    explicit ChrootMountGuard(const fs::path& root) : root_(root) {
        if (root_ == "/" || root_.empty()) return;
        // 挂载必要的伪文件系统，以便在 chroot 内部运行复杂的脚本
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
        // 逆序卸载，确保没有路径被占用
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

void run_hook(const std::string& pkg_name, const std::string& hook_name) {
    fs::path hook_path = HOOKS_DIR / pkg_name / hook_name;
    if (fs::exists(hook_path) && fs::is_regular_file(hook_path)) {
        log_info(string_format("info.running_hook", hook_name.c_str()));
        bool use_chroot = (ROOT_DIR != "/" && !ROOT_DIR.empty());
        if (use_chroot) {
            pid_t pid = fork();
            if (pid == -1) {
                log_warning(string_format("error.hook_fork_failed", std::string(strerror(errno))));
                return;
            } else if (pid == 0) {
                // 进入新的命名空间并执行脚本
                if (unshare(CLONE_NEWNS) != 0) _exit(1);
                mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
                {
                    ChrootMountGuard mount_guard(ROOT_DIR);
                    if (chroot(ROOT_DIR.c_str()) != 0) _exit(1);
                    if (chdir("/") != 0) _exit(1);
                    fs::path hook_rel = fs::relative(hook_path, ROOT_DIR);
                    std::string hook_cmd = "/" + hook_rel.string();
                    execl("/bin/sh", "sh", "-c", hook_cmd.c_str(), (char*)NULL);
                    _exit(1);
                }
            } else {
                int status;
                waitpid(pid, &status, 0);
                if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                     log_warning(string_format("warning.hook_failed_exec", hook_name.c_str(), std::to_string(WEXITSTATUS(status))));
                }
            }
        } else {
            // 在宿主系统直接运行，使用 fork/exec 避免 shell 注入风险
            pid_t pid = fork();
            if (pid == -1) {
                log_warning(string_format("error.hook_fork_failed", std::string(strerror(errno))));
            } else if (pid == 0) {
                execl("/bin/sh", "sh", "-c", hook_path.c_str(), (char*)NULL);
                _exit(1);
            } else {
                int status;
                waitpid(pid, &status, 0);
                if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                    log_warning(string_format("warning.hook_failed_exec", hook_name.c_str(), std::to_string(WEXITSTATUS(status))));
                }
            }
        }
    }
}

} // anonymous namespace

// --- InstallationTask Implementation ---

InstallationTask::InstallationTask(std::string pkg_name, std::string version, bool explicit_install, std::string old_version_to_replace, fs::path local_package_path, std::string expected_hash)
    : pkg_name_(std::move(pkg_name)), version_(std::move(version)), explicit_install_(explicit_install), 
      tmp_pkg_dir_(get_tmp_dir() / pkg_name_), actual_version_(version_), old_version_to_replace_(std::move(old_version_to_replace)),
      local_package_path_(std::move(local_package_path)), expected_hash_(std::move(expected_hash)) {}

void InstallationTask::run() {
    // 检查是否已经安装了相同版本
    std::string current_installed_version = get_installed_version(pkg_name_);
    if (!current_installed_version.empty() && current_installed_version == actual_version_) {
        log_info(string_format("info.package_already_installed", pkg_name_.c_str()));
        return;
    }
    
    log_info(string_format("info.installing_package", pkg_name_.c_str(), version_.c_str()));
    ensure_dir_exists(tmp_pkg_dir_);
    
    try {
        prepare(); // 准备阶段：下载、校验、解压、检查冲突
        commit(); // 提交阶段：复制文件、注册数据库、运行钩子
    } catch (const std::exception& e) {
        rollback_files(); // 失败回滚
        throw;
    }
    
    log_info(string_format("info.package_installed_successfully", pkg_name_.c_str()));
}

void InstallationTask::prepare() {
    download_and_verify_package();
    extract_and_validate_package();
    // resolve_dependencies() is now handled by the planning phase in install_packages
    check_for_file_conflicts();
}

void InstallationTask::rollback_files() {
    log_error(string_format("error.rollback_install", pkg_name_.c_str()));
    for (const auto& file : installed_files_) {
        try { if (fs::exists(file) || fs::is_symlink(file)) fs::remove(file); } catch (...) {}
    }
    for (const auto& backup : backups_) {
        try { if (fs::exists(backup.second)) fs::rename(backup.second, backup.first); } catch (...) {}
    }
    std::vector<fs::path> sorted_dirs(created_dirs_.begin(), created_dirs_.end());
    std::sort(sorted_dirs.rbegin(), sorted_dirs.rend());
    for (const auto& dir : sorted_dirs) {
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
                if (!new_files.contains(old_file)) {
                    auto& cache = get_cache();
                    std::unique_lock<std::mutex> lock(cache.mtx);
                    
                    // Remove ownership of the old file
                    auto it = cache.file_db.find(old_file);
                    if (it != cache.file_db.end()) {
                        it->second.erase(pkg_name_);
                        if (it->second.empty()) {
                            cache.file_db.erase(it);
                            lock.unlock(); // Unlock before disk IO
                            
                            fs::path physical_path = (fs::path(old_file).is_absolute()) ? ROOT_DIR / fs::path(old_file).relative_path() : ROOT_DIR / old_file;
                            if (fs::exists(physical_path) || fs::is_symlink(physical_path)) {
                                log_info(string_format("info.removing_obsolete_file", old_file.c_str()));
                                std::error_code ec;
                                fs::remove(physical_path, ec);
                            }
                            lock.lock();
                        }
                    }
                }
            }

            // Cleanup obsolete directories
            fs::path old_dirs_list_path = FILES_DIR / (pkg_name_ + ".dirs");
            if (fs::exists(old_dirs_list_path)) {
                std::ifstream f(old_dirs_list_path);
                std::string line;
                std::vector<fs::path> old_dirs;
                while (std::getline(f, line)) if (!line.empty()) old_dirs.push_back(line);
                std::sort(old_dirs.rbegin(), old_dirs.rend());
                for (const auto& d : old_dirs) {
                    fs::path physical_path = (d.is_absolute()) ? ROOT_DIR / d.relative_path() : ROOT_DIR / d;
                    if (fs::exists(physical_path) && fs::is_directory(physical_path) && fs::is_empty(physical_path)) {
                        try { fs::remove(physical_path); } catch (...) {}
                    }
                }
            }
        }
    } catch (...) { throw; }
    
    for (const auto& backup : backups_) { std::error_code ec; fs::remove(backup.second, ec); }
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
    for (const auto& file : {"man.txt", "deps.txt", "files.txt", "content/"}) {
        if (!fs::exists(tmp_pkg_dir_ / file)) throw LpkgException(string_format("error.incomplete_package", (tmp_pkg_dir_ / file).string().c_str()));
    }
}

void InstallationTask::check_for_file_conflicts() {
    std::map<std::string, std::string> conflicts;
    std::ifstream files_list(tmp_pkg_dir_ / "files.txt");
    std::string src, dest;
    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);
    
    while (files_list >> src >> dest) {
        fs::path logical_dest_path = fs::path(dest) / src;
        std::string path_str = logical_dest_path.string();
        
        // 检查数据库中是否已有其他包拥有该文件
        auto it = cache.file_db.find(path_str);
        if (it != cache.file_db.end()) {
            bool owned_by_others = false;
            for (const auto& owner : it->second) {
                if (owner != pkg_name_) {
                    conflicts[path_str] = owner;
                    owned_by_others = true;
                    break;
                }
            }
            if (owned_by_others) continue;
        }

        // 即使数据库没记录，也要检查物理文件是否存在（防止覆盖非包管理文件）
        // 如果是升级同一个包，我们允许覆盖
        if (old_version_to_replace_.empty()) {
            fs::path physical_path = (logical_dest_path.is_absolute()) ? ROOT_DIR / logical_dest_path.relative_path() : ROOT_DIR / logical_dest_path;
            if (fs::exists(physical_path) || fs::is_symlink(physical_path)) {
                // 如果数据库没记录但物理文件存在，标记为 "unknown"
                if (it == cache.file_db.end()) {
                    if (!get_force_overwrite_mode()) {
                        conflicts[path_str] = "unknown (manual file)";
                    }
                }
            }
        }
    }
    if (!conflicts.empty()) {
        std::string msg = get_string("error.file_conflict_header") + "\n";
        for (const auto& [file, owner] : conflicts) msg += "  " + string_format("error.file_conflict_entry", file.c_str(), owner.c_str()) + "\n";
        throw LpkgException(msg + get_string("error.installation_aborted"));
    }
}

void InstallationTask::copy_package_files() {
    log_info(get_string("info.copying_files"));
    std::ifstream files_list(tmp_pkg_dir_ / "files.txt");
    std::string src, dest;
    while (files_list >> src >> dest) {
        const fs::path src_path = tmp_pkg_dir_ / "content" / src;
        const fs::path logical_dest_path = fs::path(dest) / src;
        fs::path physical_dest_path = (logical_dest_path.is_absolute()) ? ROOT_DIR / logical_dest_path.relative_path() : ROOT_DIR / logical_dest_path;
        if (!fs::exists(src_path) && !fs::is_symlink(src_path)) continue;
        fs::path parent = physical_dest_path.parent_path();
        std::vector<fs::path> to_create;
        while (!parent.empty() && parent != ROOT_DIR && !fs::exists(parent)) { to_create.push_back(parent); parent = parent.parent_path(); }
        std::reverse(to_create.begin(), to_create.end());
        for (const auto& d : to_create) { ensure_dir_exists(d); created_dirs_.insert(d); }
        try {
            if (fs::exists(physical_dest_path) || fs::is_symlink(physical_dest_path)) {
                if (!fs::is_directory(physical_dest_path)) {
                    fs::path bak = physical_dest_path; bak += ".lpkg_bak_" + pkg_name_; 
                    fs::rename(physical_dest_path, bak); backups_.emplace_back(physical_dest_path, bak);
                }
            }
            fs::copy(src_path, physical_dest_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks);
            installed_files_.push_back(physical_dest_path);
        } catch (const fs::filesystem_error& e) { throw LpkgException(string_format("error.copy_failed_rollback", src_path.string().c_str(), physical_dest_path.string().c_str(), e.what())); }
    }
    std::ofstream pkg_f(FILES_DIR / (pkg_name_ + ".txt"));
    std::ifstream fl2(tmp_pkg_dir_ / "files.txt");
    while (fl2 >> src >> dest) pkg_f << (fs::path(dest) / src).string() << "\n";
    std::ofstream dir_f(FILES_DIR / (pkg_name_ + ".dirs"));
    for (const auto& d : created_dirs_) dir_f << d.string() << "\n";
}

void InstallationTask::register_package() {
    std::ifstream deps_in(tmp_pkg_dir_ / "deps.txt");
    std::ofstream deps_out(DEP_DIR / pkg_name_);
    std::string d;
    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);
    
    // Clear old reverse deps if it's an upgrade
    if (!old_version_to_replace_.empty()) {
        fs::path p = DEP_DIR / pkg_name_;
        if (fs::exists(p)) {
            std::ifstream f(p); std::string l;
            while(std::getline(f, l)) {
                if (!l.empty()) {
                    if (l.back() == '\r') l.pop_back();
                    std::stringstream ss(l); std::string dn; ss >> dn;
                    if (!dn.empty()) cache.reverse_deps[dn].erase(pkg_name_);
                }
            }
        }
    }

    // Clear old provides if it's an upgrade
    if (!old_version_to_replace_.empty()) {
        fs::path p = FILES_DIR / (pkg_name_ + ".provides");
        if (fs::exists(p)) {
            std::ifstream f(p); std::string c;
            while (std::getline(f, c)) {
                if (!c.empty()) {
                    if (c.back() == '\r') c.pop_back();
                    cache.providers[c].erase(pkg_name_);
                    if (cache.providers[c].empty()) cache.providers.erase(c);
                }
            }
        }
    }

    // 注册依赖关系
    while (std::getline(deps_in, d)) {
        if (!d.empty()) {
            if (d.back() == '\r') d.pop_back();
            deps_out << d << "\n";
            std::stringstream ss(d); std::string dn; ss >> dn;
            if (!dn.empty()) cache.reverse_deps[dn].insert(pkg_name_);
        }
    }
    
    // 注册文件归属权
    std::ifstream fl(FILES_DIR / (pkg_name_ + ".txt"));
    std::string fp;
    while (std::getline(fl, fp)) if (!fp.empty()) cache.file_db[fp].insert(pkg_name_);
    
    // 安装帮助文档
    fs::copy(tmp_pkg_dir_ / "man.txt", DOCS_DIR / (pkg_name_ + ".man"), fs::copy_options::overwrite_existing);
    
    // 更新已安装包映射
    cache.installed_pkgs[pkg_name_] = actual_version_;
    
    // 处理 Provides (虚拟包)
    std::ifstream prov_in(tmp_pkg_dir_ / "provides.txt");
    if (prov_in.is_open()) {
        std::ofstream prov_out(FILES_DIR / (pkg_name_ + ".provides"));
        std::string cap;
        while (std::getline(prov_in, cap)) if (!cap.empty()) { 
            cache.providers[cap].insert(pkg_name_); 
            prov_out << cap << "\n"; 
        }
    }
    
    if (explicit_install_) cache.holdpkgs.insert(pkg_name_);
    cache.dirty = true;
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

// --- Internal Helpers ---

namespace {

struct InstallPlan {
    std::string name, version_spec, actual_version, sha256;
    bool is_explicit = false;
    fs::path local_path;
    std::vector<DependencyInfo> dependencies;
};

void resolve_package_dependencies(const std::string& pkg_name, const std::string& version_spec, bool is_explicit, std::map<std::string, InstallPlan>& plan,
    std::vector<std::string>& install_order, std::set<std::string>& visited_stack, const std::map<std::string, fs::path>& local_candidates, Repository& repo) {
    if (visited_stack.count(pkg_name)) { 
        log_warning(string_format("warning.circular_dependency", pkg_name.c_str(), pkg_name.c_str())); 
        return; 
    }
    
    if (plan.count(pkg_name)) { 
        if (is_explicit) plan.at(pkg_name).is_explicit = true; 
        return; 
    }

    std::string installed_version = get_installed_version(pkg_name);
    fs::path local_path; std::string latest_version, pkg_hash; std::vector<DependencyInfo> deps;
    
    static std::map<fs::path, std::vector<DependencyInfo>> local_deps_cache;
    static std::map<fs::path, std::string> local_version_cache;

    auto it = local_candidates.find(pkg_name);
    if (it != local_candidates.end()) {
        local_path = it->second;
        if (local_version_cache.count(local_path)) {
            latest_version = local_version_cache[local_path];
            deps = local_deps_cache[local_path];
        } else {
            latest_version = parse_package_filename(local_path.filename().string()).second;
            std::stringstream deps_ss(extract_file_from_archive(local_path, "deps.txt"));
            std::string line;
            while (std::getline(deps_ss, line)) {
                if (line.empty()) continue;
                if (line.back() == '\r') line.pop_back();
                std::stringstream ss(line); std::string dn, op, rv;
                if (ss >> dn) { DependencyInfo d; d.name = dn; if (ss >> op >> rv) { d.op = op; d.version_req = rv; } deps.push_back(d); }
            }
            local_version_cache[local_path] = latest_version;
            local_deps_cache[local_path] = deps;
        }
    } else {
        auto opt = repo.find_package(pkg_name); 
        std::optional<PackageInfo> pkg_info = (version_spec == "latest") ? opt : repo.find_package(pkg_name, version_spec);
        if (!pkg_info && version_spec == "latest") pkg_info = repo.find_provider(pkg_name);
        
        if (!pkg_info) { 
            if (installed_version.empty()) log_warning(string_format("warning.package_not_in_repo", pkg_name.c_str())); 
            return; 
        }
        
        if (pkg_info->name != pkg_name) { 
            resolve_package_dependencies(pkg_info->name, pkg_info->version, is_explicit, plan, install_order, visited_stack, local_candidates, repo); 
            return; 
        }
        latest_version = pkg_info->version; pkg_hash = pkg_info->sha256; deps = pkg_info->dependencies;
    }

    // Bug fix: allow explicit version installation even if newer is installed
    if (!is_explicit && !installed_version.empty() && !version_compare(installed_version, latest_version)) return;
    if (is_explicit && !installed_version.empty() && installed_version == latest_version) return;

    visited_stack.insert(pkg_name);
    InstallPlan p; p.name = pkg_name; p.version_spec = version_spec; p.actual_version = latest_version; p.is_explicit = is_explicit; p.local_path = local_path; p.sha256 = pkg_hash; p.dependencies = deps;
    
    for (const auto& dep : deps) {
        std::string idv = get_installed_version(dep.name);
        bool needs_resolution = false;

        if (idv.empty()) {
            needs_resolution = true;
        } else if (!dep.op.empty() && idv != "virtual" && !version_satisfies(idv, dep.op, dep.version_req)) {
            // Check if already in plan (will be upgraded)
            if (!plan.count(dep.name)) {
                log_info(string_format("info.adding_upgrade_to_plan", dep.name.c_str(), dep.version_req.c_str()));
                needs_resolution = true;
            }
        }

        if (needs_resolution) {
            std::string req_ver = "latest";
            if (!dep.op.empty()) {
                auto matching = repo.find_best_matching_version(dep.name, dep.op, dep.version_req);
                if (matching) req_ver = matching->version;
            }
            resolve_package_dependencies(dep.name, req_ver, false, plan, install_order, visited_stack, local_candidates, repo);
        }
        
        // Final sanity check for constraints
        std::string candidate_version;
        if (plan.count(dep.name)) candidate_version = plan[dep.name].actual_version;
        else candidate_version = get_installed_version(dep.name);
        
        if (!dep.op.empty() && !candidate_version.empty() && candidate_version != "virtual" && !version_satisfies(candidate_version, dep.op, dep.version_req))
            throw LpkgException(string_format("error.candidate_dep_version_mismatch", dep.name.c_str(), candidate_version.c_str(), dep.op.c_str(), dep.version_req.c_str()));
    }
    plan[pkg_name] = p; install_order.push_back(pkg_name); visited_stack.erase(pkg_name);
}

// Check if upgrading or installing packages in the plan breaks existing system packages
std::set<std::string> check_plan_consistency(const std::map<std::string, InstallPlan>& plan) {
    std::set<std::string> broken_pkgs;
    auto& cache = get_cache();
    std::lock_guard<std::mutex> lock(cache.mtx);

    for (const auto& [pkg_to_be_mod, installed_ver] : cache.installed_pkgs) {
        // If this package is being replaced in the plan, we don't check it here (it's checked during resolution)
        if (plan.count(pkg_to_be_mod)) continue;

        // Check if any of its dependencies are being upgraded to an incompatible version
        fs::path dep_file = DEP_DIR / pkg_to_be_mod;
        if (fs::exists(dep_file)) {
            std::ifstream f(dep_file);
            std::string line;
            while (std::getline(f, line)) {
                if (line.empty()) continue;
                if (line.back() == '\r') line.pop_back();
                std::stringstream ss(line);
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
    InstallationTask task(plan.name, plan.actual_version, plan.is_explicit, get_installed_version(plan.name), plan.local_path, plan.sha256);
    task.run();
}

void rollback_installed_package(const std::string& pkg_name, [[maybe_unused]] const std::string& version) {
    log_info(string_format("info.rolling_back", pkg_name.c_str()));
    remove_package_files(pkg_name, true);
    fs::remove(DEP_DIR / pkg_name);
    fs::remove(DOCS_DIR / (pkg_name + ".man"));
    fs::remove_all(HOOKS_DIR / pkg_name);
    auto& cache = get_cache(); std::lock_guard<std::mutex> lock(cache.mtx);
    cache.installed_pkgs.erase(pkg_name);
    if (cache.holdpkgs.contains(pkg_name)) cache.holdpkgs.erase(pkg_name);
    cache.dirty = true;
}

std::unordered_set<std::string> get_all_required_packages() {
    auto& cache = get_cache(); std::lock_guard<std::mutex> lock(cache.mtx);
    std::unordered_set<std::string> req = cache.holdpkgs;
    std::vector<std::string> q(req.begin(), req.end());
    size_t head = 0;
    while (head < q.size()) {
        std::string curr = q[head++]; fs::path p = DEP_DIR / curr;
        if (fs::exists(p)) {
            std::ifstream f(p); std::string d_line;
            while (std::getline(f, d_line)) {
                if (d_line.empty()) continue;
                if (d_line.back() == '\r') d_line.pop_back();
                std::stringstream ss(d_line); std::string d_name;
                if (ss >> d_name) {
                    if (cache.installed_pkgs.contains(d_name)) {
                        if (!req.contains(d_name)) { req.insert(d_name); q.push_back(d_name); }
                    } else if (cache.providers.contains(d_name)) {
                        for (const auto& provider : cache.providers.at(d_name)) {
                            if (cache.installed_pkgs.contains(provider) && !req.contains(provider)) {
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

// --- Public API ---

void install_package(const std::string& pkg_name, const std::string& version) {
    std::string arg = pkg_name;
    if (version != "latest") arg += ":" + version;
    install_packages({arg});
}

void install_packages(const std::vector<std::string>& pkg_args) {
    force_reload_cache(); TmpDirManager tmp; Repository repo;
    try { repo.load_index(); } catch (const std::exception& e) { log_warning(string_format("warning.repo_index_load_failed", e.what())); }
    std::map<std::string, InstallPlan> plan; std::vector<std::string> order; std::map<std::string, fs::path> locals;
    std::vector<std::pair<std::string, std::string>> targets;
    for (const auto& arg : pkg_args) {
        fs::path p(arg);
        if (p.extension() == ".zst" || p.extension() == ".lpkg" || arg.find('/') != std::string::npos) {
            if (fs::exists(p)) { 
                try { auto nv = parse_package_filename(p.filename().string()); locals[nv.first] = fs::absolute(p); targets.emplace_back(nv.first, nv.second); } 
                catch (const std::exception& e) { log_error(string_format("warning.skip_invalid_local_pkg", arg.c_str(), e.what())); }
            } else log_error(string_format("error.local_pkg_not_found", arg.c_str()));
        } else {
            std::string n = arg, v = "latest"; size_t pos = arg.find(':');
            if (pos != std::string::npos) { n = arg.substr(0, pos); v = arg.substr(pos+1); }
            targets.emplace_back(n, v);
        }
    }
    for (const auto& t : targets) { std::set<std::string> vs; resolve_package_dependencies(t.first, t.second, true, plan, order, vs, locals, repo); }
    if (plan.empty()) { log_info(get_string("info.all_packages_already_installed")); return; }

    // Consistent check: verify if plan breaks other packages
    std::set<std::string> broken = check_plan_consistency(plan);
    if (!broken.empty()) {
        log_error(get_string("error.dependency_conflict_title"));
        if (user_confirms(get_string("prompt.remove_conflict_pkgs"))) {
            for (const auto& pkg : broken) {
                remove_package(pkg, true);
            }
            write_cache();
            // Re-run installation with current args to rebuild plan after removals
            install_packages(pkg_args);
            return;
        } else {
            log_info(get_string("info.installation_aborted"));
            return;
        }
    }

    std::string prompt;
    for (const auto& n : order) {
        const auto& p = plan.at(n);
        prompt += "  " + string_format(p.is_explicit ? "info.package_list_item" : "info.package_list_item_dep", p.name.c_str(), p.actual_version.c_str()) + "\n";
    }
    if (!user_confirms(prompt + get_string("info.confirm_proceed"))) { log_info(get_string("info.installation_aborted")); return; }
    std::vector<std::string> installed;
    try {
        for (const auto& n : order) {
            const auto& p = plan.at(n); 
            commit_package_installation(p); installed.push_back(p.name);
        }
        write_cache();
    } catch (const LpkgException& e) {
        log_error(get_string("error.installation_failed_rolling_back"));
        std::reverse(installed.begin(), installed.end());
        for (const auto& n : installed) try { rollback_installed_package(n, plan.at(n).actual_version); } catch (...) {}
        write_cache(); throw;
    }
}

void remove_package(const std::string& pkg_name, bool force) {
    std::string ver = get_installed_version(pkg_name);
    if (ver.empty()) { log_info(string_format("info.package_not_installed", pkg_name.c_str())); return; }
    if (!force) {
        if (is_essential_package(pkg_name)) { log_error(string_format("error.skip_remove_essential", pkg_name.c_str())); return; }
        auto& cache = get_cache(); std::lock_guard<std::mutex> lock(cache.mtx);
        
        // Check direct dependencies
        auto it = cache.reverse_deps.find(pkg_name);
        if (it != cache.reverse_deps.end() && !it->second.empty()) {
            std::string deps; for (const auto& d : it->second) deps += d + " ";
            log_info(string_format("info.skip_remove_dependency", pkg_name.c_str(), deps.c_str())); return;
        }

        // Check dependencies on provided capabilities
        const fs::path plist = FILES_DIR / (pkg_name + ".provides");
        if (fs::exists(plist)) {
            std::ifstream f(plist); std::string cap;
            while (std::getline(f, cap)) {
                if (cap.empty()) continue;
                if (cap.back() == '\r') cap.pop_back();
                auto it_cap = cache.reverse_deps.find(cap);
                if (it_cap != cache.reverse_deps.end() && !it_cap->second.empty()) {
                    std::string deps; for (const auto& d : it_cap->second) deps += d + " ";
                    log_info(string_format("info.skip_remove_dependency", cap.c_str(), deps.c_str())); return;
                }
            }
        }
    }
    log_info(string_format("info.removing_package", pkg_name.c_str()));
    run_hook(pkg_name, "prerm.sh");
    remove_package_files(pkg_name, force);
    {
        auto& cache = get_cache(); std::lock_guard<std::mutex> lock(cache.mtx);
        fs::path p = DEP_DIR / pkg_name;
        if (fs::exists(p)) {
            std::ifstream f(p); std::string l;
            while(std::getline(f, l)) {
                if (!l.empty()) { if (l.back() == '\r') l.pop_back(); std::stringstream ss(l); std::string dn; ss >> dn;
                if (!dn.empty()) cache.reverse_deps[dn].erase(pkg_name); }
            }
        }
    }
    fs::remove(DEP_DIR / pkg_name); fs::remove(DOCS_DIR / (pkg_name + ".man")); fs::remove_all(HOOKS_DIR / pkg_name);
    auto& cache = get_cache(); std::lock_guard<std::mutex> lock(cache.mtx);
    cache.installed_pkgs.erase(pkg_name); 
    if (cache.holdpkgs.contains(pkg_name)) cache.holdpkgs.erase(pkg_name);
    cache.dirty = true; log_info(string_format("info.package_removed_successfully", pkg_name.c_str()));
}

void remove_package_files(const std::string& pkg_name, bool force) {
    const fs::path list = FILES_DIR / (pkg_name + ".txt");
    if (fs::exists(list)) {
        std::map<std::string, std::vector<std::string>> shared;
        auto& cache = get_cache(); std::unique_lock<std::mutex> lock(cache.mtx);
        std::ifstream f(list); std::string l;
        while (std::getline(f, l)) {
            if (l.empty()) continue;
            auto it = cache.file_db.find(l);
            if (it != cache.file_db.end()) {
                for (const auto& owner : it->second) if (owner != pkg_name) shared[l].push_back(owner);
            }
        }
        if (!shared.empty() && !force) {
            std::string msg = get_string("error.shared_file_header") + "\n";
            for (const auto& [file, owners] : shared) {
                std::string os; for(size_t i=0; i<owners.size(); ++i) os += owners[i] + (i==owners.size()-1 ? "" : ", ");
                msg += "  " + string_format("error.shared_file_entry", file.c_str(), os.c_str()) + "\n";
            }
            throw LpkgException(msg + get_string("error.removal_aborted"));
        }
        lock.unlock(); f.clear(); f.seekg(0);
        std::vector<fs::path> paths; while (std::getline(f, l)) if (!l.empty()) paths.emplace_back(l);
        std::sort(paths.rbegin(), paths.rend());
        int count = 0;
        for (const auto& p : paths) {
            fs::path phys = (p.is_absolute()) ? ROOT_DIR / p.relative_path() : ROOT_DIR / p;
            if (fs::exists(phys) || fs::is_symlink(phys)) {
                lock.lock(); 
                auto it = cache.file_db.find(p.string());
                bool owned_by_me = (it != cache.file_db.end() && it->second.count(pkg_name));
                bool owned_by_others = (it != cache.file_db.end() && it->second.size() > 1);
                lock.unlock();

                if (owned_by_me) {
                    if (!owned_by_others) {
                        fs::remove(phys); 
                        count++; 
                    } else {
                        log_info(string_format("info.skipped_remove", p.string().c_str()));
                    }
                }
            }
        }
        log_info(string_format("info.files_removed", count)); fs::remove(list);
        lock.lock();
        for (const auto& p : paths) {
            auto it = cache.file_db.find(p.string());
            if (it != cache.file_db.end()) { it->second.erase(pkg_name); if (it->second.empty()) cache.file_db.erase(it); }
        }
        cache.dirty = true;
    }
    const fs::path dlist = FILES_DIR / (pkg_name + ".dirs");
    if (fs::exists(dlist)) {
        std::ifstream f(dlist); std::string l; std::vector<fs::path> ps; 
        while (std::getline(f, l)) if (!l.empty()) ps.emplace_back(l);
        std::sort(ps.rbegin(), ps.rend());
        for (const auto& d : ps) {
            fs::path phys = (d.is_absolute()) ? ROOT_DIR / d.relative_path() : ROOT_DIR / d;
            if (fs::exists(phys) && fs::is_directory(phys) && fs::is_empty(phys)) try { fs::remove(phys); } catch (...) {}
        }
        fs::remove(dlist);
    }
    const fs::path plist = FILES_DIR / (pkg_name + ".provides");
    if (fs::exists(plist)) {
        auto& cache = get_cache(); std::unique_lock<std::mutex> lock(cache.mtx);
        std::ifstream f(plist); std::string c;
        while (std::getline(f, c)) if (!c.empty()) {
            cache.providers[c].erase(pkg_name); if (cache.providers[c].empty()) cache.providers.erase(c);
        }
        fs::remove(plist); cache.dirty = true;
    }
}

void autoremove() {
    log_info(get_string("info.checking_autoremove"));
    auto req = get_all_required_packages(); std::vector<std::string> to_rem;
    auto& cache = get_cache(); std::unique_lock<std::mutex> lock(cache.mtx);
    for (const auto& [name, ver] : cache.installed_pkgs) { if (!req.contains(name)) to_rem.push_back(name); }
    lock.unlock();
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
    auto& cache = get_cache(); std::unique_lock<std::mutex> lock(cache.mtx);
    for (const auto& [name, ver] : cache.installed_pkgs) { installed.emplace_back(name, ver); }
    lock.unlock();
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
            InstallationTask t(n, lat, is_manually_installed(n), curr, "", hash);
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

void write_cache() {
    auto& cache = get_cache(); std::lock_guard<std::mutex> lock(cache.mtx);
    if (cache.dirty) { cache.write_file_db(); cache.write_providers(); cache.write_pkgs(); cache.write_holdpkgs(); cache.dirty = false; }
}