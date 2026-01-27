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

    // Thread-safe accessors and modifiers using string_view
    bool is_installed(std::string_view name);
    std::string get_installed_version(std::string_view name);
    bool is_essential(std::string_view name);
    bool is_held(std::string_view name);
    
    void add_installed(std::string_view name, std::string_view ver, bool hold = false);
    void remove_installed(std::string_view name);
    
    void add_file_owner(std::string_view path, std::string_view pkg);
    void remove_file_owner(std::string_view path, std::string_view pkg);
    std::unordered_set<std::string> get_file_owners(std::string_view path);

    void add_provider(std::string_view capability, std::string_view pkg);
    void remove_provider(std::string_view capability, std::string_view pkg);
    std::unordered_set<std::string> get_providers(std::string_view capability);

    void add_reverse_dep(std::string_view dep, std::string_view pkg);
    void remove_reverse_dep(std::string_view dep, std::string_view pkg);
    std::unordered_set<std::string> get_reverse_deps(std::string_view name);

    void ensure_reverse_deps();
    void ensure_essentials();

    // For iterations (locking should be handled carefully by callers)
    std::mutex& get_mutex() { return mtx; }
    const std::map<std::string, std::string, std::less<>>& get_all_installed() { return installed_pkgs; }
    const std::unordered_set<std::string>& get_all_held() { return holdpkgs; }

private:
    Cache();
    
    std::map<std::string, std::unordered_set<std::string>, std::less<>> file_db;
    std::map<std::string, std::unordered_set<std::string>, std::less<>> providers;
    std::map<std::string, std::string, std::less<>> installed_pkgs;
    std::unordered_set<std::string> holdpkgs;
    std::unordered_set<std::string> essentials;
    std::map<std::string, std::unordered_set<std::string>, std::less<>> reverse_deps;
    
    std::mutex mtx;
    bool dirty = false;
    bool reverse_deps_loaded = false;
    bool essentials_loaded = false;

    std::map<std::string, std::unordered_set<std::string>, std::less<>> read_db_uncached(const std::filesystem::path& path);
    void write_db_uncached(const std::filesystem::path& path, const std::map<std::string, std::unordered_set<std::string>, std::less<>>& db);
    
    void write_pkgs();
    void write_holdpkgs();
    void write_file_db();
    void write_providers();
};