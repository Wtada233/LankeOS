#pragma once

#include <string>
#include <map>
#include <unordered_set>
#include <mutex>
#include <filesystem>

/**
 * 本地状态数据库（单例）
 *
 * 维护已安装包列表、文件归属、providers、反向依赖等状态的运行时缓存。
 * 所有读写操作均为线程安全，修改后通过 write() 持久化到磁盘。
 */
class Cache {
public:
    /** 获取全局单例实例 */
    static Cache& instance();

    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    /** 从磁盘加载所有缓存数据 */
    void load();
    /** 将所有脏数据写入磁盘 */
    void write();

    // ===== 包状态查询 =====

    /** 查询包是否已安装 */
    bool is_installed(std::string_view name);
    /** 获取已安装包的版本号 */
    std::string get_installed_version(std::string_view name);
    /** 查询包是否为核心包 */
    bool is_essential(std::string_view name);
    /** 查询包是否被锁定 */
    bool is_held(std::string_view name);

    /** 将包标记为已安装 */
    void add_installed(std::string_view name, std::string_view ver, bool hold = false);
    /** 移除已安装包 */
    void remove_installed(std::string_view name);

    /** 记录文件归属 */
    void add_file_owner(std::string_view path, std::string_view pkg);
    /** 移除文件归属 */
    void remove_file_owner(std::string_view path, std::string_view pkg);
    /** 查询文件归属的包集合 */
    std::unordered_set<std::string> get_file_owners(std::string_view path);

    /** 添加 provider（能力名称 -> 包名） */
    void add_provider(std::string_view capability, std::string_view pkg);
    /** 移除 provider */
    void remove_provider(std::string_view capability, std::string_view pkg);
    /** 查询提供某能力的包集合 */
    std::unordered_set<std::string> get_providers(std::string_view capability);

    /** 添加反向依赖记录 */
    void add_reverse_dep(std::string_view dep, std::string_view pkg);
    /** 移除反向依赖记录 */
    void remove_reverse_dep(std::string_view dep, std::string_view pkg);
    /** 查询某包的反向依赖集合 */
    std::unordered_set<std::string> get_reverse_deps(std::string_view name);

    /** 确保反向依赖数据已加载 */
    void ensure_reverse_deps();
    /** 确保核心包数据已加载 */
    void ensure_essentials();

    // ===== 反向查询 =====

    /** 获取某包拥有的所有文件 */
    std::unordered_set<std::string> get_package_files(std::string_view pkg);
    /** 获取某包提供的所有能力 */
    std::unordered_set<std::string> get_package_provides(std::string_view pkg);

    // ===== 迭代支持（调用者需自行管理锁） =====

    /** 获取内部互斥锁引用 */
    std::mutex& get_mutex() { return mtx; }
    /** 获取所有已安装包（名称 -> 版本） */
    const std::map<std::string, std::string, std::less<>>& get_all_installed() { return installed_pkgs; }
    /** 获取所有锁定包名集合 */
    const std::unordered_set<std::string>& get_all_held() { return holdpkgs; }

private:
    Cache();

    // 文件归属数据库（路径 -> 包名集合）
    std::map<std::string, std::unordered_set<std::string>, std::less<>> file_db;
    // providers 数据库（能力 -> 包名集合）
    std::map<std::string, std::unordered_set<std::string>, std::less<>> providers;
    // 已安装包（包名 -> 版本）
    std::map<std::string, std::string, std::less<>> installed_pkgs;
    // 锁定包名集合
    std::unordered_set<std::string> holdpkgs;
    // 核心包名集合
    std::unordered_set<std::string> essentials;
    // 反向依赖数据库（依赖 -> 依赖它的包集合）
    std::map<std::string, std::unordered_set<std::string>, std::less<>> reverse_deps;

    std::mutex mtx;             // 线程安全互斥锁
    bool dirty = false;          // 是否有未写入的修改
    bool reverse_deps_loaded = false;  // 反向依赖是否已加载
    bool essentials_loaded = false;     // 核心包是否已加载

    /** 从文件读取多值数据库（不经过缓存） */
    std::map<std::string, std::unordered_set<std::string>, std::less<>> read_db_uncached(const std::filesystem::path& path);
    /** 将多值数据库写入文件（不经过缓存） */
    void write_db_uncached(const std::filesystem::path& path, const std::map<std::string, std::unordered_set<std::string>, std::less<>>& db);

    /** 写入已安装包列表 */
    void write_pkgs();
    /** 写入锁定包列表 */
    void write_holdpkgs();
    /** 写入文件归属数据库 */
    void write_file_db();
    /** 写入 providers 数据库 */
    void write_providers();
};