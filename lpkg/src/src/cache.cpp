#include "cache.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "exception.hpp"
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

Cache& Cache::instance() {
    static Cache instance;
    return instance;
}

Cache::Cache() {
    load();
}

bool Cache::is_installed(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx);
    return installed_pkgs.contains(name);
}

std::string Cache::get_installed_version(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = installed_pkgs.find(name);
    if (it != installed_pkgs.end()) return it->second;
    if (providers.count(name)) return "virtual";
    return "";
}

bool Cache::is_essential(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx);
    ensure_essentials();
    return essentials.contains(name);
}

bool Cache::is_held(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx);
    return holdpkgs.contains(name);
}

void Cache::add_installed(const std::string& name, const std::string& ver, bool hold) {
    std::lock_guard<std::mutex> lock(mtx);
    installed_pkgs[name] = ver;
    if (hold) holdpkgs.insert(name);
    dirty = true;
}

void Cache::remove_installed(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx);
    installed_pkgs.erase(name);
    holdpkgs.erase(name);
    dirty = true;
}

void Cache::add_file_owner(const std::string& path, const std::string& pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    file_db[path].insert(pkg);
    dirty = true;
}

void Cache::remove_file_owner(const std::string& path, const std::string& pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = file_db.find(path);
    if (it != file_db.end()) {
        it->second.erase(pkg);
        if (it->second.empty()) file_db.erase(it);
        dirty = true;
    }
}

const std::unordered_set<std::string>* Cache::get_file_owners(const std::string& path) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = file_db.find(path);
    return (it != file_db.end()) ? &it->second : nullptr;
}

void Cache::add_provider(const std::string& capability, const std::string& pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    providers[capability].insert(pkg);
    dirty = true;
}

void Cache::remove_provider(const std::string& capability, const std::string& pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = providers.find(capability);
    if (it != providers.end()) {
        it->second.erase(pkg);
        if (it->second.empty()) providers.erase(it);
        dirty = true;
    }
}

const std::unordered_set<std::string>* Cache::get_providers(const std::string& capability) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = providers.find(capability);
    return (it != providers.end()) ? &it->second : nullptr;
}

void Cache::add_reverse_dep(const std::string& dep, const std::string& pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    ensure_reverse_deps();
    reverse_deps[dep].insert(pkg);
}

void Cache::remove_reverse_dep(const std::string& dep, const std::string& pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    ensure_reverse_deps();
    auto it = reverse_deps.find(dep);
    if (it != reverse_deps.end()) {
        it->second.erase(pkg);
        if (it->second.empty()) reverse_deps.erase(it);
    }
}

const std::unordered_set<std::string>* Cache::get_reverse_deps(const std::string& name) {
    std::lock_guard<std::mutex> lock(mtx);
    ensure_reverse_deps();
    auto it = reverse_deps.find(name);
    return (it != reverse_deps.end()) ? &it->second : nullptr;
}

void Cache::load() {
    std::lock_guard<std::mutex> lock(mtx);
    file_db = read_db_uncached(FILES_DB);
    providers = read_db_uncached(PROVIDES_DB);
    
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
    reverse_deps_loaded = false;
    essentials.clear();
    essentials_loaded = false;
    dirty = false;
}

void Cache::ensure_reverse_deps() {
    if (reverse_deps_loaded) return;
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
                        if (ss >> d_name && !d_name.empty()) {
                            reverse_deps[d_name].insert(pkg_name);
                        }
                    }
                }
            }
        }
    }
    reverse_deps_loaded = true;
}

void Cache::ensure_essentials() {
    if (essentials_loaded) return;
    if (fs::exists(ESSENTIAL_FILE)) {
        essentials = read_set_from_file(ESSENTIAL_FILE);
    }
    essentials_loaded = true;
}

void Cache::write() {
    if (dirty) {
        write_file_db();
        write_providers();
        write_pkgs();
        write_holdpkgs();
        dirty = false;
    }
}

void Cache::write_pkgs() { 
    std::unordered_set<std::string> pkg_set;
    for (const auto& [name, ver] : installed_pkgs) {
        pkg_set.insert(name + ":" + ver);
    }
    write_set_to_file(PKGS_FILE, pkg_set); 
}

void Cache::write_holdpkgs() { write_set_to_file(HOLDPKGS_FILE, holdpkgs); }
void Cache::write_file_db() { write_db_uncached(FILES_DB, file_db); }
void Cache::write_providers() { write_db_uncached(PROVIDES_DB, providers); }

std::map<std::string, std::unordered_set<std::string>> Cache::read_db_uncached(const fs::path& path) {
    std::map<std::string, std::unordered_set<std::string>> db;
    std::ifstream db_file(path);
    if (!db_file.is_open()) return db;
    std::string key, value;
    while (db_file >> key >> value) {
        db[key].insert(value);
    }
    return db;
}

void Cache::write_db_uncached(const fs::path& path, const std::map<std::string, std::unordered_set<std::string>>& db) {
    fs::path tmp_path = path.string() + ".tmp";
    {
        std::ofstream db_file(tmp_path, std::ios::trunc);
        if (!db_file.is_open()) throw LpkgException("Failed to create temporary database file");
        for (const auto& [key, values] : db) {
            for (const auto& value : values) {
                db_file << key << " " << value << "\n";
            }
        }
    }
    fs::rename(tmp_path, path);
}