#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * 本地状态数据库（单例）：已安装包列表、文件归属、providers、反向依赖等的运行时缓存。
 * 读写均线程安全，修改后由 `write()` 持久化。
 */
class Cache
{
public:
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

    bool is_installed(std::string_view name);
    std::string get_installed_version(std::string_view name);
    bool is_essential(std::string_view name);
    bool is_held(std::string_view name);

    void add_installed(std::string_view name, std::string_view ver, bool hold = false);
    void remove_installed(std::string_view name);

    /// 普通文件强制单一所有者：已归属他人时抛 error.file_already_owned。
    void add_file_owner(std::string_view path, std::string_view pkg);
    /// 目录允许共享（多个包可拥有同一目录）。
    void add_dir_owner(std::string_view path, std::string_view pkg);
    void remove_file_owner(std::string_view path, std::string_view pkg);
    std::unordered_set<std::string> get_file_owners(std::string_view path);
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

    /// 能力名称 -> 包名（`provides` 语义）。
    void add_provider(std::string_view capability, std::string_view pkg);
    void remove_provider(std::string_view capability, std::string_view pkg);
    std::unordered_set<std::string> get_providers(std::string_view capability);

    // ── SONAME 归属（**另一张表**，与上面的虚拟 provider 表并列）──────────────────────────
    // 两张表**故意不合并**：合并了就会让"依赖包名/虚拟能力"与"需要 SONAME"互相误匹配 ——
    // 那正是 8.0.0 字段拆分要根除的毛病（`provides_soname.db` ↔ `provides.db`）。
    void add_soname_provider(std::string_view soname, std::string_view pkg);
    void remove_soname_provider(std::string_view soname, std::string_view pkg);
    /**
     * 谁**满足**这个 SONAME 需求。
     *
     * ⚠️ 参数是**需求串**（`X` / `X@V` / `X@{V1,V2}`，即 `needed_so` 里的写法），不是
     * "某条 provides_soname 声明的原样串"：判定走唯一的 `so_spec_satisfies()`（**保守**：
     * 带符号版本的需求只有"也声明了符号版本且覆盖它"的提供者才算数）。
     * 传裸 `X` 时行为与 8.0.0 之前一致 —— 任何声明了 `X…` 的包都算提供者。
     */
    std::unordered_set<std::string> get_soname_providers(std::string_view need);

    void add_reverse_dep(std::string_view dep, std::string_view pkg);
    void remove_reverse_dep(std::string_view dep, std::string_view pkg);
    std::unordered_set<std::string> get_reverse_deps(std::string_view name);

    void ensure_reverse_deps();
    void ensure_essentials();

    // ===== 反向查询 =====

    std::unordered_set<std::string> get_package_files(std::string_view pkg);
    std::unordered_set<std::string> get_package_provides(std::string_view pkg);
    /// 本包**导出**的 SONAME（区别于它需要的 needed_so）。
    std::unordered_set<std::string> get_package_provides_soname(std::string_view pkg);

    // ===== 迭代支持：**一律值语义快照** =====
    //
    // 不要引用返回、也不要暴露 `get_mutex()`：前者**根本不用锁**就把内部容器交了出去，
    // 后者让"持锁跨文件 I/O"成为可能（`force_solve_conflict` 曾经在锁内做 `ifstream` 与
    // `find_provider`），而正确写法从来不需要那样。
    // 全部是"持锁拷贝一份出去"：调用方拿到的是**快照**，不再需要也不该持有锁。
    // 没有一处调用点需要"跨读-改-写持锁"—— 所有改动本来就都走 Cache 自己的加锁方法。

    /** 已安装包（名称 → 版本）的**快照**。 */
    std::map<std::string, std::string, std::less<>> get_all_installed();

    /** 锁定包名集合的**快照**。 */
    std::unordered_set<std::string> get_all_held();

    /**
     * **整张文件归属表**的快照（路径 → 包名集合）。
     *
     * 专门给"冲突预检要拿一份**原子**的基线、然后在本地把它推演成批内各步之后的样子"
     * 那个用法。给它一个引用访问器等于把坑再挖一遍（调用方得自己加锁、加锁期间不能调
     * 别的方法、还可能顺手改到活状态）。
     */
    std::map<std::string, std::unordered_set<std::string>, std::less<>> snapshot_file_ownership();

    Cache();

    // 文件归属数据库（路径 -> 包名集合）
private:
    std::map<std::string, std::unordered_set<std::string>, std::less<>> file_db;
    // 配置文件哈希数据库（逻辑路径 -> "<pkg>:<sha256>" 集合；见 conf_hashes_db()）
    std::map<std::string, std::unordered_set<std::string>, std::less<>> conf_hashes;
    // xattr 键归属数据库（`<逻辑路径>\x1f<base64 键>` -> 属主包名集合；见 xattr_keys_db()）
    std::map<std::string, std::unordered_set<std::string>, std::less<>> xattr_keys;
    // providers 数据库（**虚拟能力** -> 包名集合）
    std::map<std::string, std::unordered_set<std::string>, std::less<>> providers;
    // SONAME 归属数据库（**规格串** -> 包名集合；规格串原样存用户写法：`X` / `X@V` / `X@{V1,V2}`）
    std::map<std::string, std::unordered_set<std::string>, std::less<>> provides_soname;
    // 上面那张表的**派生索引**：裸 SONAME（`so_spec_key()`）-> 该库的候选规格串（已排序）。
    // 按需重建（`soname_specs_dirty`）：它只有两种变更来源（整表加载 / 加删一条），
    // 重建一次是 O(#规格)，比在两个地方各维护一遍"该增该删"便宜且不会漂移。
    std::map<std::string, std::vector<std::string>, std::less<>> soname_specs_by_name;
    bool soname_specs_dirty = true;
    // 已安装包（包名 -> 版本）
    std::map<std::string, std::string, std::less<>> installed_pkgs;
    std::unordered_set<std::string> holdpkgs;
    std::unordered_set<std::string> essentials;
    // 反向依赖数据库（依赖 -> 依赖它的包集合）
    std::map<std::string, std::unordered_set<std::string>, std::less<>> reverse_deps;

    std::mutex mtx;
    bool dirty = false;
    bool reverse_deps_loaded = false;
    bool essentials_loaded = false;

public:
    /// 不经过缓存，直接从文件读。
    std::map<std::string, std::unordered_set<std::string>, std::less<>> read_db_uncached(
        const std::filesystem::path& path);

    void write_pkgs();
    void write_holdpkgs();
    void write_file_db();
    void write_conf_hashes();
    void write_xattr_keys();
    void write_providers();
    void write_provides_soname();

    /// 重建 SONAME 派生索引（**调用方必须已持 `mtx`**）。
    void ensure_soname_spec_index_locked();
    /// `get_soname_providers()` 的**持锁版本**（`ensure_reverse_deps` 在锁内要用）。
    std::unordered_set<std::string> get_soname_providers_locked(std::string_view need);

    std::unordered_set<std::string> build_pkgs_set() const;

    /// 落盘序列：.tmp + fsync + rename。
    void write_db_file_direct(
        const std::filesystem::path& path,
        const std::map<std::string, std::unordered_set<std::string>, std::less<>>& db);
    /// 落盘序列：.tmp + fsync + rename。
    void write_set_file_direct(const std::filesystem::path& path,
                               const std::unordered_set<std::string>& data);

    // ── WAL 保护的写入 ────────────────────────────────────────────────

    /**
     * write-ahead DB 写入：WAL → fsync → 备份旧文件 → fsync → 写 .tmp →
     * fsync → rename → fsync 父目录
     *
     * @param db_path     DB 文件路径
     * @param db          要写入的 DB 内容
     * @param milestone   里程碑标签（如 ":batch-start" / ":batch-end"）。本函数只由
     *                    `Cache::write(milestone)` 调用，里程碑只有这两个批次级值；
     *                    包级元数据文件走的是 `wal::write_string_file_wal`，不经本函数
     * @param wal_op_type WAL 操作类型（DB / DBNEW / DBRM）
     */
    void write_db_file_wal(
        const std::filesystem::path& db_path,
        const std::map<std::string, std::unordered_set<std::string>, std::less<>>& db,
        const std::string& milestone, const std::string& wal_op_type = "DB");

    /// 与 write_db_file_wal 相同序列。
    void write_set_file_wal(const std::filesystem::path& path,
                            const std::unordered_set<std::string>& data,
                            const std::string& milestone, const std::string& wal_op_type = "DB");
};

// ── WAL 恢复与清理（全局函数） ──────────────────────────────────────

/// 仅用于崩溃恢复。
void recover_packages();

/// 清理已提交批次的 WAL 日志行。
void trim_completed();

/// 清理孤立的 .lpkg_db_bak_before:* 备份文件。
void cleanup_db_backups();

/**
 * WAL 里是否还留着未配对的 BEGIN_PKGS（= 存在未提交批次）。
 *
 * 这是"上一次恢复**没能**把某一行撤掉"的信号：`rollback_uncommitted_region` 在这种情形下
 * **故意不封口**（留给下次 `lpkg rec` 幂等重做）。调用方据此 fail-closed —— `cleanup_db_backups`
 * 用它保住唯一的重试还原点，`run_batch_transaction` 用它**拒绝**在未封口的 WAL 上再开新批次。
 */
bool wal_has_unpaired_batch();
