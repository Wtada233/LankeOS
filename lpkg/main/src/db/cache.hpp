#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * 本地状态数据库（单例）
 *
 * 维护已安装包列表、文件归属、providers、反向依赖等状态的运行时缓存。
 * 所有读写操作均为线程安全，修改后通过 write() 持久化到磁盘。
 */
class Cache
{
public:
    /** 获取全局单例实例 */
    static Cache& instance();

    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    /**
     * 从磁盘加载所有缓存数据。
     *
     * @param tolerate_missing_set_files
     *        **默认 false**：pkgs / holdpkgs 缺失即抛 error.open_file_failed —— 正常操作
     *        路径上"库丢了"必须是个响亮的错误，绝不能退化成"静默空库"（那会让所有已装包
     *        凭空消失）。
     *        true **只给崩溃恢复路径**（recover_packages 的回滚收尾）用：一条缺失记录不该
     *        把整个恢复作废（其余文件的还原、WAL 收尾、备份清理都还要做）；缺库会逐个告警
     *        （warning.db_set_file_missing），且"存在却打不开"照旧抛。
     *        即使容忍，后续正常操作路径（install/remove/upgrade 入口的 load()）仍按严格
     *        语义报错 —— 损失只被容忍一次，不会被静默延续。
     */
    void load(bool tolerate_missing_set_files = false);
    /** 将所有脏数据写入磁盘 */
    void write();
    /** 将所有脏数据写入磁盘（兼容旧接口，wal_tag 已无意义） */
    void write(const std::string& wal_tag);

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

    /** 记录文件归属（普通文件强制单一所有者，已归属他人时抛 error.file_already_owned） */
    void add_file_owner(std::string_view path, std::string_view pkg);
    /** 记录目录归属（允许共享——多个包可拥有同一目录） */
    void add_dir_owner(std::string_view path, std::string_view pkg);
    /** 移除文件归属 */
    void remove_file_owner(std::string_view path, std::string_view pkg);
    /** 查询文件归属的包集合 */
    std::unordered_set<std::string> get_file_owners(std::string_view path);
    /** 检查某文件是否由指定包所有 */
    bool is_file_owned_by(std::string_view path, std::string_view pkg);

    // ===== 配置文件哈希（升级时三哈希分流的 hash_orig） =====

    /**
     * 记下"**本包这次往该路径装进去的内容**"的哈希（覆盖本包自己那条记录）。
     *
     * 调用者必须传**包内内容**的哈希，不是"盘上此刻那份"的哈希：走 `.lpkgnew` 分支时盘上
     * 仍是用户的文件，把它记成 hash_orig 会让**下一次**升级满足"盘上 == 旧记录"而把用户
     * 改过的配置静默覆盖 —— 那正是 `.lpkgnew` 要防的事。
     */
    void set_conf_hash(std::string_view path, std::string_view pkg, std::string_view hash);
    /**
     * 撤销本包在该路径上的哈希记录（包被移除、或新版本不再提供该文件）。
     *
     * 记录**随包走**：一个已不在册的包留下的记录，会让重新装回来的包把"上一次装的哈希"
     * 当成旧记录，从而把一份它并不拥有的同名文件静默覆盖。
     */
    void remove_conf_hash(std::string_view path, std::string_view pkg);
    /** 查询本包在该路径上记录过的哈希（**无记录 → 空串** = 无从判定，调用者按保守处理） */
    std::string get_conf_hash(std::string_view path, std::string_view pkg);

    // ===== xattr 键归属（升级/移除时撤销"本包不再声明"的键） =====

    /**
     * 登记"本包在**这个目录**上声明了**这个 xattr 键**"。
     *
     * 调用点只有一个方向上的正当性：**写入侧真的往那个目录写了这个键**（`write_dir_entry` /
     * `let_go_make_dir`）。登记不等于"这个键现在在盘上"——它记的是"**这是我们装的**"，
     * 与 `files.db` 记"这个路径是我们装的"同一个口径。
     *
     * **不可登记的键**（返回 false，调用方应当据此不撤销它）：`path` 里含 `\x1f`（记录键的
     * 分隔符）或 `\t`/`\n`/`\r`（DB 格式本身的分隔符 —— 与 `files.db` 同一个约束）。这类
     * 路径在现行归档成员名消毒下造不出来，但消毒**不保证**这一点（它只挡 `\n`/`\r`/`\0` 与
     * 字面 `" → "`），所以这里是**唯一**能把它们挡在 DB 之外的地方。宁可不记（那个键将来
     * 不会被撤销 = 留一份陈旧 xattr），也不能记成一条会被解析歪的记录（那会导致**删错键**）。
     */
    bool add_xattr_key_owner(std::string_view path, std::string_view key, std::string_view pkg);
    /** 摘掉本包对该 (路径, 键) 的登记（**其他属主仍在时只摘本包**；无人持有则整条删掉） */
    void remove_xattr_key_owner(std::string_view path, std::string_view key, std::string_view pkg);
    /** 该 (路径, 键) 的属主集合（**键不存在 → 空集**） */
    std::unordered_set<std::string> get_xattr_key_owners(std::string_view path,
                                                         std::string_view key);
    /**
     * 本包声明过的全部 (路径, 键) 对 —— 撤销趟遍历它就是遍历"我们可能要撤的键"。
     * 路径用**逻辑形态**（`/usr/share/x/`，目录键带尾斜杠，与 `files.db` 同形）。
     */
    std::vector<std::pair<std::string, std::string>> get_package_xattr_keys(std::string_view pkg);

    /** 添加 provider（能力名称 -> 包名） */
    void add_provider(std::string_view capability, std::string_view pkg);
    /** 移除 provider */
    void remove_provider(std::string_view capability, std::string_view pkg);
    /** 查询提供某能力的包集合 */
    std::unordered_set<std::string> get_providers(std::string_view capability);
    /** 检查某能力是否由指定包提供 */
    bool is_provided_by(std::string_view capability, std::string_view pkg);

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
    std::mutex& get_mutex()
    {
        return mtx;
    }
    /** 获取所有已安装包（名称 -> 版本） */
    const std::map<std::string, std::string, std::less<>>& get_all_installed()
    {
        return installed_pkgs;
    }
    /** 获取所有锁定包名集合 */
    const std::unordered_set<std::string>& get_all_held()
    {
        return holdpkgs;
    }

    Cache();

    // 文件归属数据库（路径 -> 包名集合）
    std::map<std::string, std::unordered_set<std::string>, std::less<>> file_db;
    // 配置文件哈希数据库（逻辑路径 -> "<pkg>:<sha256>" 集合；见 conf_hashes_db()）
    std::map<std::string, std::unordered_set<std::string>, std::less<>> conf_hashes;
    // xattr 键归属数据库（`<逻辑路径>\x1f<base64 键>` -> 属主包名集合；见 xattr_keys_db()）
    std::map<std::string, std::unordered_set<std::string>, std::less<>> xattr_keys;
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

    std::mutex mtx;                    // 线程安全互斥锁
    bool dirty = false;                // 是否有未写入的修改
    bool reverse_deps_loaded = false;  // 反向依赖是否已加载
    bool essentials_loaded = false;    // 核心包是否已加载

    /** 从文件读取多值数据库（不经过缓存） */
    std::map<std::string, std::unordered_set<std::string>, std::less<>> read_db_uncached(
        const std::filesystem::path& path);

    /** 直接写入已安装包列表 */
    void write_pkgs();
    /** 直接写入锁定包列表 */
    void write_holdpkgs();
    /** 直接写入文件归属数据库 */
    void write_file_db();
    /** 直接写入配置文件哈希数据库 */
    void write_conf_hashes();
    /** 直接写入 xattr 键归属数据库 */
    void write_xattr_keys();
    /** 直接写入 providers 数据库 */
    void write_providers();

    /** 从 installed_pkgs 构建 set 格式数据 */
    std::unordered_set<std::string> build_pkgs_set() const;

    /** 直接写入 DB 文件（.tmp + fsync + rename） */
    void write_db_file_direct(
        const std::filesystem::path& path,
        const std::map<std::string, std::unordered_set<std::string>, std::less<>>& db);
    /** 直接写入 set 文件（.tmp + fsync + rename） */
    void write_set_file_direct(const std::filesystem::path& path,
                               const std::unordered_set<std::string>& data);

    // ── WAL 保护的写入 ────────────────────────────────────────────────

    /**
     * write-ahead DB 写入：WAL → fsync → 备份旧文件 → fsync → 写 .tmp →
     * fsync → rename → fsync 父目录
     *
     * @param db_path     DB 文件路径
     * @param db          要写入的 DB 内容
     * @param milestone   里程碑标签（如 "glibc:installed"）
     * @param wal_op_type WAL 操作类型（DB / DBNEW / DBRM）
     */
    void write_db_file_wal(
        const std::filesystem::path& db_path,
        const std::map<std::string, std::unordered_set<std::string>, std::less<>>& db,
        const std::string& milestone, const std::string& wal_op_type = "DB");

    /**
     * write-ahead set 写入：与 write_db_file_wal 相同序列
     */
    void write_set_file_wal(const std::filesystem::path& path,
                            const std::unordered_set<std::string>& data,
                            const std::string& milestone, const std::string& wal_op_type = "DB");
};

// ── WAL 恢复与清理（全局函数） ──────────────────────────────────────

/// 从未完成的 WAL 事务中恢复（仅用于崩溃恢复）
void recover_packages();

/// 清理已提交批次的 WAL 日志行
void trim_completed();

/// 清理孤立的 .lpkg_db_bak_before:* 备份文件
void cleanup_db_backups();
