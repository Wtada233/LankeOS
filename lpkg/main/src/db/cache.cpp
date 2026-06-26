#include "cache.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

/**
 * 获取 Cache 单例实例
 */
Cache& Cache::instance() {
    static Cache instance;
    return instance;
}

Cache::Cache() {
    load();
}

/**
 * 检查包是否已安装
 */
bool Cache::is_installed(std::string_view name) {
    std::lock_guard<std::mutex> lock(mtx);
    return installed_pkgs.contains(name);
}

/**
 * 获取已安装包的版本号
 * 如果是虚拟包（提供者），返回 "virtual"；未安装返回空字符串
 */
std::string Cache::get_installed_version(std::string_view name) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = installed_pkgs.find(name);
    if (it != installed_pkgs.end()) return it->second;
    if (providers.count(name)) return "virtual";
    return "";
}

/**
 * 检查包是否被标记为 essential（系统必需）
 */
bool Cache::is_essential(std::string_view name) {
    std::lock_guard<std::mutex> lock(mtx);
    ensure_essentials();
    return essentials.contains(std::string(name));
}

/**
 * 检查包是否被锁定，禁止更新或删除
 */
bool Cache::is_held(std::string_view name) {
    std::lock_guard<std::mutex> lock(mtx);
    return holdpkgs.contains(std::string(name));
}

/**
 * 将包标记为已安装并记录版本号
 * @param hold 是否同时将其加入锁定列表
 */
void Cache::add_installed(std::string_view name, std::string_view ver, bool hold) {
    std::lock_guard<std::mutex> lock(mtx);
    installed_pkgs[std::string(name)] = std::string(ver);
    if (hold) holdpkgs.insert(std::string(name));
    dirty = true;
}

/**
 * 从已安装列表中移除包，同时清理对应的锁定状态
 */
void Cache::remove_installed(std::string_view name) {
    std::lock_guard<std::mutex> lock(mtx);
    installed_pkgs.erase(std::string(name));
    holdpkgs.erase(std::string(name));
    dirty = true;
}

/**
 * 记录文件由哪个包所有（文件归属关系）
 */
void Cache::add_file_owner(std::string_view path, std::string_view pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    file_db[std::string(path)].insert(std::string(pkg));
    dirty = true;
}

/**
 * 移除文件与包的所有权关联，若文件无其他所有者则清理条目
 */
void Cache::remove_file_owner(std::string_view path, std::string_view pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = file_db.find(path);
    if (it != file_db.end()) {
        it->second.erase(std::string(pkg));
        if (it->second.empty()) file_db.erase(it);
        dirty = true;
    }
}

/**
 * 检查某文件是否由指定包所有
 * 相比 get_file_owners() 返回整个集合，此方法仅做存在性检查，避免拷贝
 */
bool Cache::is_file_owned_by(std::string_view path, std::string_view pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = file_db.find(path);
    return it != file_db.end() && it->second.contains(std::string(pkg));
}

/**
 * 查询某文件由哪些包所有
 */
std::unordered_set<std::string> Cache::get_file_owners(std::string_view path) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = file_db.find(path);
    return (it != file_db.end()) ? it->second : std::unordered_set<std::string>{};
}

/**
 * 注册某个功能/虚拟包由哪个包提供
 */
void Cache::add_provider(std::string_view capability, std::string_view pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    providers[std::string(capability)].insert(std::string(pkg));
    dirty = true;
}

/**
 * 移除某个功能/虚拟包的提供者记录
 */
void Cache::remove_provider(std::string_view capability, std::string_view pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = providers.find(capability);
    if (it != providers.end()) {
        it->second.erase(std::string(pkg));
        if (it->second.empty()) providers.erase(it);
        dirty = true;
    }
}

/**
 * 查询提供指定功能/虚拟包的所有包
 */
std::unordered_set<std::string> Cache::get_providers(std::string_view capability) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = providers.find(capability);
    return (it != providers.end()) ? it->second : std::unordered_set<std::string>{};
}

/**
 * 检查某能力是否由指定包提供（避免全量集合拷贝）
 */
bool Cache::is_provided_by(std::string_view capability, std::string_view pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = providers.find(capability);
    return it != providers.end() && it->second.contains(std::string(pkg));
}

/**
 * 注册某包被另一个包所依赖（反向依赖关系）
 */
void Cache::add_reverse_dep(std::string_view dep, std::string_view pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    ensure_reverse_deps();
    reverse_deps[std::string(dep)].insert(std::string(pkg));
}

/**
 * 移除反向依赖记录
 */
void Cache::remove_reverse_dep(std::string_view dep, std::string_view pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    ensure_reverse_deps();
    auto it = reverse_deps.find(dep);
    if (it != reverse_deps.end()) {
        it->second.erase(std::string(pkg));
        if (it->second.empty()) reverse_deps.erase(it);
    }
}

/**
 * 查询依赖指定包的所有包（反向依赖）
 */
std::unordered_set<std::string> Cache::get_reverse_deps(std::string_view name) {
    std::lock_guard<std::mutex> lock(mtx);
    ensure_reverse_deps();
    auto it = reverse_deps.find(std::string(name));
    return (it != reverse_deps.end()) ? it->second : std::unordered_set<std::string>{};
}

/**
 * 查询某包安装的所有文件列表
 * 遍历文件数据库，筛选出属于该包的文件
 */
std::unordered_set<std::string> Cache::get_package_files(std::string_view pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    std::string pkg_str(pkg);
    std::unordered_set<std::string> result;
    for (const auto& [file, owners] : file_db) {
        if (owners.contains(pkg_str)) {
            result.insert(file);
        }
    }
    return result;
}

/**
 * 查询某包提供的所有功能/虚拟包列表
 */
std::unordered_set<std::string> Cache::get_package_provides(std::string_view pkg) {
    std::lock_guard<std::mutex> lock(mtx);
    std::string pkg_str(pkg);
    std::unordered_set<std::string> result;
    for (const auto& [cap, owners] : providers) {
        if (owners.contains(pkg_str)) {
            result.insert(cap);
        }
    }
    return result;
}

/**
 * 从磁盘加载所有缓存数据
 * 包括文件数据库、提供者数据库、已安装包列表、锁定包列表等
 * 反向依赖和 essential 列表初始化为未加载状态，按需读取
 */
void Cache::load() {
    std::lock_guard<std::mutex> lock(mtx);
    file_db = read_db_uncached(Config::instance().files_db());
    providers = read_db_uncached(Config::instance().provides_db());

    auto pkg_set = read_set_from_file(Config::instance().pkgs_file());
    installed_pkgs.clear();
    for (const auto& line : pkg_set) {
        if (auto pos = line.find(':'); pos != std::string::npos) {
            installed_pkgs[line.substr(0, pos)] = line.substr(pos + 1);
        }
    }

    holdpkgs = read_set_from_file(Config::instance().holdpkgs_file());
    reverse_deps.clear();
    reverse_deps_loaded = false;
    essentials.clear();
    essentials_loaded = false;
    dirty = false;
}

/**
 * 按需加载反向依赖信息
 * 读取 dep_dir/ 目录下每个包的反向依赖文件（每行一个依赖项）
 */
void Cache::ensure_reverse_deps() {
    if (reverse_deps_loaded) return;
    reverse_deps.clear();
    if (fs::exists(Config::instance().dep_dir()) && fs::is_directory(Config::instance().dep_dir())) {
        for (const auto& entry : fs::directory_iterator(Config::instance().dep_dir())) {
            if (entry.is_regular_file()) {
                std::string pkg_name = entry.path().filename().string();
                std::ifstream f(entry.path());
                std::string line;
                while (std::getline(f, line)) {
                    if (line.empty()) continue;
                    std::string_view sv = line;
                    if (sv.back() == '\r') sv.remove_suffix(1);
                    if (auto pos = sv.find_first_of(" \t"); pos != std::string_view::npos) {
                        sv = sv.substr(0, pos);
                    }
                    if (!sv.empty()) {
                        reverse_deps[std::string(sv)].insert(pkg_name);
                    }
                }
            }
        }
    }
    reverse_deps_loaded = true;
}

/**
 * 按需加载 essential 包列表
 * 从配置文件读取系统必需包的集合
 */
void Cache::ensure_essentials() {
    if (essentials_loaded) return;
    if (fs::exists(Config::instance().essential_file())) {
        essentials = read_set_from_file(Config::instance().essential_file());
    }
    essentials_loaded = true;
}

/**
 * 如果缓存数据有变动，将所有数据写回磁盘
 */
void Cache::write() {
    if (dirty) {
        write_file_db();
        write_providers();
        write_pkgs();
        write_holdpkgs();
        dirty = false;
    }
}

/**
 * 将已安装包列表写入磁盘，格式为 包名:版本号
 */
void Cache::write_pkgs() {
    std::unordered_set<std::string> pkg_set;
    for (const auto& [name, ver] : installed_pkgs) {
        pkg_set.insert(name + ":" + ver);
    }
    write_set_to_file(Config::instance().pkgs_file(), pkg_set);
}

/**
 * 将锁定包列表写入磁盘
 */
void Cache::write_holdpkgs() { write_set_to_file(Config::instance().holdpkgs_file(), holdpkgs); }

/**
 * 将文件归属数据库写入磁盘
 */
void Cache::write_file_db() { write_db_uncached(Config::instance().files_db(), file_db); }

/**
 * 将提供者数据库写入磁盘
 */
void Cache::write_providers() { write_db_uncached(Config::instance().provides_db(), providers); }

/**
 * 从磁盘读取键-多值数据库（制表符分隔）
 * 格式：key\tvalue，相同 key 的多行合并为集合
 */
std::map<std::string, std::unordered_set<std::string>, std::less<>> Cache::read_db_uncached(const fs::path& path) {
    std::map<std::string, std::unordered_set<std::string>, std::less<>> db;
    std::ifstream db_file(path);
    if (!db_file.is_open()) return db;
    std::string line;
    while (std::getline(db_file, line)) {
        if (line.empty()) continue;
        size_t tab_pos = line.find('\t');
        if (tab_pos != std::string::npos) {
            std::string key = line.substr(0, tab_pos);
            std::string value = line.substr(tab_pos + 1);
            if (!value.empty() && value.back() == '\r') value.pop_back();
            db[key].insert(value);
        }
    }
    return db;
}

/**
 * 将键-多值数据库写入磁盘（原子写入：先写临时文件再重命名）
 */
void Cache::write_db_uncached(const fs::path& path, const std::map<std::string, std::unordered_set<std::string>, std::less<>>& db) {
    fs::path tmp_path = path.string() + ".tmp";
    {
        std::ofstream db_file(tmp_path, std::ios::trunc);
        if (!db_file.is_open()) throw LpkgException(get_string("error.create_tmp_db_failed"));
        for (const auto& [key, values] : db) {
            for (const auto& value : values) {
                db_file << key << "\t" << value << "\n";
            }
        }
    }
    fs::rename(tmp_path, path);
}