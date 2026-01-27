#pragma once

#include <string>
#include <map>
#include <unordered_set>
#include <mutex>
#include <filesystem>

class Cache {
public:
    static Cache& instance();

    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    void load();
    void write();

    // Thread-safe accessors and modifiers
    bool is_installed(const std::string& name);
    std::string get_installed_version(const std::string& name);
    bool is_essential(const std::string& name);
    bool is_held(const std::string& name);
    
    void add_installed(const std::string& name, const std::string& ver, bool hold = false);
    void remove_installed(const std::string& name);
    
    void add_file_owner(const std::string& path, const std::string& pkg);
    void remove_file_owner(const std::string& path, const std::string& pkg);
    const std::unordered_set<std::string>* get_file_owners(const std::string& path);

    void add_provider(const std::string& capability, const std::string& pkg);
    void remove_provider(const std::string& capability, const std::string& pkg);
    const std::unordered_set<std::string>* get_providers(const std::string& capability);

    void add_reverse_dep(const std::string& dep, const std::string& pkg);
    void remove_reverse_dep(const std::string& dep, const std::string& pkg);
    const std::unordered_set<std::string>* get_reverse_deps(const std::string& name);

    void ensure_reverse_deps();
    void ensure_essentials();

    // For iterations (locking should be handled carefully by callers)
    std::mutex& get_mutex() { return mtx; }
    const std::map<std::string, std::string>& get_all_installed() { return installed_pkgs; }
    const std::unordered_set<std::string>& get_all_held() { return holdpkgs; }

private:
    Cache();
    
    std::map<std::string, std::unordered_set<std::string>> file_db;
    std::map<std::string, std::unordered_set<std::string>> providers;
    std::map<std::string, std::string> installed_pkgs;
    std::unordered_set<std::string> holdpkgs;
    std::unordered_set<std::string> essentials;
    std::map<std::string, std::unordered_set<std::string>> reverse_deps;
    
    std::mutex mtx;
    bool dirty = false;
    bool reverse_deps_loaded = false;
    bool essentials_loaded = false;

    std::map<std::string, std::unordered_set<std::string>> read_db_uncached(const std::filesystem::path& path);
    void write_db_uncached(const std::filesystem::path& path, const std::map<std::string, std::unordered_set<std::string>>& db);
    
    void write_pkgs();
    void write_holdpkgs();
    void write_file_db();
    void write_providers();
};