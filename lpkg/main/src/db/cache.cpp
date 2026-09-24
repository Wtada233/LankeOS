#include "cache.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

#include "base/constants.hpp"
#include "config.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "transaction_log.hpp"
#include "utils.hpp"
#include "wal_op.hpp"

namespace fs = std::filesystem;

namespace
{
/**
 * 多值集合 → **排序后**的 ',' 连接文本（一个 DB 键的取值列）。
 *
 * 为什么必须排序：`unordered_set` 的迭代顺序由**插入顺序 + 桶布局**决定，同一组取值在
 * 不同历史下序列化出不同字节。而"回滚/恢复后 DB 与批次前**逐字节**相同"这条不变式
 * （`test_db_backup_chain.cpp`、`test_upgrade_rollback_fidelity.cpp` 都在断言）要求
 * 「同一个逻辑状态 → 同样的字节」：恢复只把**某一次先前写出的内容**放回原位，任何后续
 * 重写（`batch_rollback` 恢复 DB 后必然 `load()` 再 `write(":batch-start")`）都不该改变
 * 字节。不排序时这件事只是**碰巧**成立（取决于容器历史），实测崩溃恢复路径上就会出现
 * "同一组所有者、顺序不同"的差异。
 *
 * 排序把序列化变成**唯一形态**：只由逻辑状态决定，与读取顺序、容器历史无关。
 * 读侧（`read_db_uncached` / `read_set_from_file`）按 ',' / 换行切分入集合，顺序无语义；
 * 代价仅是新旧二进制交替时首次写盘会按字典序规范化一次。
 */
std::string join_sorted(const std::unordered_set<std::string>& values)
{
    std::vector<std::string_view> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    std::string joined;
    for (const auto& v : sorted) {
        if (!joined.empty()) joined += ',';
        joined += v;
    }
    return joined;
}

/// 集合 → 排序后的多行文本（一行一个取值，如 pkgs / holdpkgs）
std::string lines_sorted(const std::unordered_set<std::string>& items)
{
    std::vector<std::string_view> sorted(items.begin(), items.end());
    std::sort(sorted.begin(), sorted.end());
    std::string out;
    for (const auto& v : sorted) {
        out += v;
        out += '\n';
    }
    return out;
}
}  // namespace

Cache& Cache::instance()
{
    static Cache instance;
    return instance;
}

Cache::Cache()
{
    load();
}

bool Cache::is_installed(std::string_view name)
{
    std::lock_guard<std::mutex> lock(mtx);
    return installed_pkgs.contains(name);
}

std::string Cache::get_installed_version(std::string_view name)
{
    std::lock_guard<std::mutex> lock(mtx);
    auto it = installed_pkgs.find(name);
    if (it != installed_pkgs.end()) return it->second;
    // 不再对能力名返回 "virtual"：调用方用本函数判断"真实包是否已装"，能力提供
    // 与否应显式用 get_providers 判断。曾返回 "virtual" 使 remove/upgrade/reinstall
    // 把能力名误当成已装包处理（如 `remove foo` 对能力 foo 跑空批次、`install foo`
    // 把能力名当旧版本做升级语义）。
    return "";
}

bool Cache::is_essential(std::string_view name)
{
    std::lock_guard<std::mutex> lock(mtx);
    ensure_essentials();
    return essentials.contains(std::string(name));
}

bool Cache::is_held(std::string_view name)
{
    std::lock_guard<std::mutex> lock(mtx);
    return holdpkgs.contains(std::string(name));
}

void Cache::add_installed(std::string_view name, std::string_view ver, bool hold)
{
    std::lock_guard<std::mutex> lock(mtx);
    installed_pkgs[std::string(name)] = std::string(ver);
    if (hold) holdpkgs.insert(std::string(name));
    dirty = true;
}

void Cache::remove_installed(std::string_view name)
{
    std::lock_guard<std::mutex> lock(mtx);
    installed_pkgs.erase(std::string(name));
    holdpkgs.erase(std::string(name));
    dirty = true;
}

void Cache::add_file_owner(std::string_view path, std::string_view pkg)
{
    std::lock_guard<std::mutex> lock(mtx);
    auto& owners = file_db[std::string(path)];
    if (!owners.empty() && !owners.contains(std::string(pkg))) {
        std::string existing = *owners.begin();
        throw LpkgException(string_format("error.file_already_owned", path, existing, pkg));
    }
    owners.insert(std::string(pkg));
    dirty = true;
}

void Cache::add_dir_owner(std::string_view path, std::string_view pkg)
{
    std::lock_guard<std::mutex> lock(mtx);
    // 目录允许多所有者共享（add_file_owner 的单一所有权检查不适用），
    // 共享目录靠移除端的引用计数管理（最后持有者离开时删除目录）。
    file_db[std::string(path)].insert(std::string(pkg));
    dirty = true;
}

void Cache::remove_file_owner(std::string_view path, std::string_view pkg)
{
    std::lock_guard<std::mutex> lock(mtx);
    auto it = file_db.find(path);
    if (it != file_db.end()) {
        it->second.erase(std::string(pkg));
        if (it->second.empty()) file_db.erase(it);
        dirty = true;
    }
}

bool Cache::is_file_owned_by(std::string_view path, std::string_view pkg)
{
    std::lock_guard<std::mutex> lock(mtx);
    auto it = file_db.find(path);
    return it != file_db.end() && it->second.contains(std::string(pkg));
}

std::unordered_set<std::string> Cache::get_file_owners(std::string_view path)
{
    std::lock_guard<std::mutex> lock(mtx);
    auto it = file_db.find(path);
    return (it != file_db.end()) ? it->second : std::unordered_set<std::string>{};
}

// ── 配置文件哈希（升级三哈希分流的 hash_orig）─────────────────────────────
//
// 存储形态：DB 键 = **逻辑路径**（与 files.db 的键同形，如 `/etc/foo.conf`），
//           取值集合 = `"<pkg>:<sha256>"`（一个路径理论上可被接管给别的包，故用集合）。
//
// 为什么不并进 files.db：那里的取值是**属主包名集合**，被 add_file_owner（单一属主检查）、
// get_file_owners、*owners.begin() 当属主集合直接读 —— 混入哈希串会污染所有权语义。
//
// 为什么带包名：pacman 的 hash_orig 取自"**被升级的那个包**在本地 DB 里的记录"，不是
// "这个路径上任何包的记录"。路径被 `--overwrite` 接管给别的包时，两个包的记录必须能共存、
// 且各自的升级只看自己那条。

namespace
{
/// sha256 十六进制串的长度（32 字节 → 64 个十六进制字符）
constexpr size_t kConfHashHexLen = 64;
}  // namespace

void Cache::set_conf_hash(std::string_view path, std::string_view pkg, std::string_view hash)
{
    if (path.empty() || pkg.empty() || hash.empty()) return;  // 空值不记录（见 get_conf_hash 语义）
    std::lock_guard<std::mutex> lock(mtx);
    auto& values = conf_hashes[std::string(path)];
    const std::string prefix = std::string(pkg) + std::string(constants::CONF_HASH_SEP);
    // 先删本包自己那条（一个包对一个路径只留一份记录：升级 = 覆盖）
    std::erase_if(values, [&](const std::string& v) { return v.starts_with(prefix); });
    values.insert(prefix + std::string(hash));
    dirty = true;
}

void Cache::remove_conf_hash(std::string_view path, std::string_view pkg)
{
    std::lock_guard<std::mutex> lock(mtx);
    auto it = conf_hashes.find(path);
    if (it == conf_hashes.end()) return;
    const std::string prefix = std::string(pkg) + std::string(constants::CONF_HASH_SEP);
    std::erase_if(it->second, [&](const std::string& v) { return v.starts_with(prefix); });
    if (it->second.empty()) conf_hashes.erase(it);
    dirty = true;
}

std::string Cache::get_conf_hash(std::string_view path, std::string_view pkg)
{
    std::lock_guard<std::mutex> lock(mtx);
    auto it = conf_hashes.find(path);
    if (it == conf_hashes.end()) return {};
    const std::string prefix = std::string(pkg) + std::string(constants::CONF_HASH_SEP);
    for (const auto& v : it->second) {
        if (!v.starts_with(prefix)) continue;
        const std::string_view hash = std::string_view(v).substr(prefix.size());
        // 长度校验：包名里出现分隔符时前缀可能误命中**别的包**的记录，而这种误命中
        // 会让"用户改过的配置"被静默覆盖 —— 宁可返回"无记录"（保守路径）。
        if (hash.size() != kConfHashHexLen) continue;
        return std::string(hash);
    }
    return {};
}

void Cache::add_provider(std::string_view capability, std::string_view pkg)
{
    std::lock_guard<std::mutex> lock(mtx);
    providers[std::string(capability)].insert(std::string(pkg));
    dirty = true;
}

void Cache::remove_provider(std::string_view capability, std::string_view pkg)
{
    std::lock_guard<std::mutex> lock(mtx);
    auto it = providers.find(capability);
    if (it != providers.end()) {
        it->second.erase(std::string(pkg));
        if (it->second.empty()) providers.erase(it);
        dirty = true;
    }
}

std::unordered_set<std::string> Cache::get_providers(std::string_view capability)
{
    std::lock_guard<std::mutex> lock(mtx);
    auto it = providers.find(capability);
    return (it != providers.end()) ? it->second : std::unordered_set<std::string>{};
}

bool Cache::is_provided_by(std::string_view capability, std::string_view pkg)
{
    std::lock_guard<std::mutex> lock(mtx);
    auto it = providers.find(capability);
    return it != providers.end() && it->second.contains(std::string(pkg));
}

void Cache::add_reverse_dep(std::string_view dep, std::string_view pkg)
{
    std::lock_guard<std::mutex> lock(mtx);
    ensure_reverse_deps();
    reverse_deps[std::string(dep)].insert(std::string(pkg));
}

void Cache::remove_reverse_dep(std::string_view dep, std::string_view pkg)
{
    std::lock_guard<std::mutex> lock(mtx);
    ensure_reverse_deps();
    auto it = reverse_deps.find(dep);
    if (it != reverse_deps.end()) {
        it->second.erase(std::string(pkg));
        if (it->second.empty()) reverse_deps.erase(it);
    }
}

std::unordered_set<std::string> Cache::get_reverse_deps(std::string_view name)
{
    std::lock_guard<std::mutex> lock(mtx);
    ensure_reverse_deps();
    auto it = reverse_deps.find(std::string(name));
    return (it != reverse_deps.end()) ? it->second : std::unordered_set<std::string>{};
}

std::unordered_set<std::string> Cache::get_package_files(std::string_view pkg)
{
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

std::unordered_set<std::string> Cache::get_package_provides(std::string_view pkg)
{
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

void Cache::load(bool tolerate_missing_set_files)
{
    std::lock_guard<std::mutex> lock(mtx);
    file_db = read_db_uncached(Config::instance().files_db());
    providers = read_db_uncached(Config::instance().provides_db());
    // 老 DB（本特性之前装的包）没有这个文件 → 空表。**这不是错误**：升级时拿不到
    // hash_orig 的路径按"三者互异"保守处理（保留原文件 + .lpkgnew），与老行为一致。
    conf_hashes = read_db_uncached(Config::instance().conf_hashes_db());

    // pkgs / holdpkgs 是**集合文件**，缺文件在 read_set_from_file 里是硬错误（不同于上面
    // 三个 DB 文件的"缺 = 空表"语义）。默认严格：库丢了就必须报错。
    // 容忍只在恢复路径开启，且**只**针对"文件不存在"（连备份都没了，无处可还原）——此时
    // 逐个告警（不静默），存在的文件仍按严格语义读。为什么不让它彻底静默：见 cache.hpp
    // 的 load() 说明与 wal_op.cpp 的 :batch-start 注释（静默空库 = 已装包/归属凭空归零）。
    auto read_set = [&](const fs::path& path) {
        if (!tolerate_missing_set_files) return read_set_from_file(path);
        if (!fs::exists(path))
            log_warning(string_format("warning.db_set_file_missing", path.string()));
        return read_set_from_file(path, MissingSetFilePolicy::Empty);
    };

    auto pkg_set = read_set(Config::instance().pkgs_file());
    installed_pkgs.clear();
    for (const auto& line : pkg_set) {
        if (auto pos = line.find(':'); pos != std::string::npos) {
            installed_pkgs[line.substr(0, pos)] = line.substr(pos + 1);
        }
    }

    holdpkgs = read_set(Config::instance().holdpkgs_file());
    reverse_deps.clear();
    reverse_deps_loaded = false;
    essentials.clear();
    essentials_loaded = false;
    dirty = false;
}

void Cache::ensure_reverse_deps()
{
    if (reverse_deps_loaded) return;
    reverse_deps.clear();
    if (fs::exists(Config::instance().dep_dir()) &&
        fs::is_directory(Config::instance().dep_dir())) {
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
    // SONAME 依赖：needed_so/ 里的 soname → 提供者包。
    // 缺这段时反向依赖（移除阻止、get_all_required_packages 语义）对纯 SONAME
    // 链路失效。用 providers_ 映射直读（ensure_reverse_deps 常持锁被调用，
    // 不能调 get_providers 再拿锁）。
    if (fs::exists(Config::instance().needed_so_dir()) &&
        fs::is_directory(Config::instance().needed_so_dir())) {
        for (const auto& entry : fs::directory_iterator(Config::instance().needed_so_dir())) {
            if (!entry.is_regular_file()) continue;
            std::string pkg_name = entry.path().filename().string();
            std::ifstream f(entry.path());
            std::string so;
            while (std::getline(f, so)) {
                if (so.empty()) continue;
                if (so.back() == '\r') so.pop_back();
                auto it = providers.find(so);
                if (it == providers.end()) continue;
                for (const auto& prov : it->second)
                    if (prov != pkg_name) reverse_deps[prov].insert(pkg_name);  // 不自引用
            }
        }
    }
    reverse_deps_loaded = true;
}

void Cache::ensure_essentials()
{
    if (essentials_loaded) return;
    if (fs::exists(Config::instance().essential_file())) {
        essentials = read_set_from_file(Config::instance().essential_file());
    }
    essentials_loaded = true;
}

// ── 直接写入（无 WAL 保护） ────────────────────────────────────────
// 注意：本版本不提供 WAL 保护的原子写入。数据库文件直接覆写。
// WAL 保护的写入将在新的事务架构中重新实现。

void Cache::write()
{
    std::lock_guard<std::mutex> lock(mtx);
    if (dirty) {
        write_file_db();
        write_providers();
        write_conf_hashes();
        write_pkgs();
        write_holdpkgs();
        dirty = false;
    }
}

void Cache::write(const std::string& milestone)
{
    // WAL 2.0: 对所有 DB 文件使用 write-ahead 顺序写入
    //   WAL DB <path> <milestone> → fsync → 备份 → fsync →
    //   .tmp → fsync → rename → fsync parent
    // 反向回滚时从 .lpkg_db_bak_before:<milestone> 恢复
    std::lock_guard<std::mutex> lock(mtx);

    auto& config = Config::instance();
    auto pkgs_data = build_pkgs_set();
    write_set_file_wal(config.pkgs_file(), pkgs_data, milestone, "DB");
    write_db_file_wal(config.files_db(), file_db, milestone, "DB");
    write_db_file_wal(config.provides_db(), providers, milestone, "DB");
    // 配置文件哈希与 files.db 同族（同样走 WAL + 备份）：批次回滚时由 reverse_execute 的
    // DB 分支还原到批次前 —— "静默替换配置"能成立的前提之一（改得动，也撤得回）。
    write_db_file_wal(config.conf_hashes_db(), conf_hashes, milestone, "DB");
    write_set_file_wal(config.holdpkgs_file(), holdpkgs, milestone, "DB");
    dirty = false;
}

// build_pkgs_set: 将 installed_pkgs map 转换为 set<string> 格式
std::unordered_set<std::string> Cache::build_pkgs_set() const
{
    std::unordered_set<std::string> result;
    for (const auto& [name, ver] : installed_pkgs) result.insert(name + ":" + ver);
    return result;
}

void Cache::write_pkgs()
{
    std::unordered_set<std::string> pkg_set;
    for (const auto& [name, ver] : installed_pkgs) {
        pkg_set.insert(name + ":" + ver);
    }
    write_set_file_direct(Config::instance().pkgs_file(), pkg_set);
}

void Cache::write_holdpkgs()
{
    write_set_file_direct(Config::instance().holdpkgs_file(), holdpkgs);
}

void Cache::write_file_db()
{
    write_db_file_direct(Config::instance().files_db(), file_db);
}

void Cache::write_conf_hashes()
{
    write_db_file_direct(Config::instance().conf_hashes_db(), conf_hashes);
}

void Cache::write_providers()
{
    write_db_file_direct(Config::instance().provides_db(), providers);
}

void Cache::write_db_file_direct(
    const fs::path& path,
    const std::map<std::string, std::unordered_set<std::string>, std::less<>>& db)
{
    // DB 写永远持久化：内容只进页缓存、而唯一备份（.lpkg_db_bak_before:*）在批次提交后
    // 立即被删 → 断电即"库丢了且备份已删"（不受 --fsync 影响的理由见 DurableFsyncGuard）
    DurableFsyncGuard durable;
    const fs::path tmp = path.string() + ".tmp";

    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open()) throw LpkgException(string_format("error.create_tmp_db_failed"));
        for (const auto& [key, values] : db) {
            f << key << "\t" << join_sorted(values) << "\n";
        }
        // 磁盘满/IO 错误会使 ofstream 进入 fail 态——不检查会把截断的 DB 文件
        // rename 进正式位置，静默损坏数据库。先 flush 让缓冲错误在析构前暴露。
        f.flush();
        if (!f) throw LpkgException(string_format("error.db_write_failed", tmp.string()));
    }

    fsync_and_rename(tmp, path);
}

void Cache::write_set_file_direct(const fs::path& path, const std::unordered_set<std::string>& data)
{
    DurableFsyncGuard durable;  // DB 写永远持久化（理由见 write_db_file_direct）
    const fs::path tmp = path.string() + ".tmp";

    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open())
            throw LpkgException(string_format("error.create_file_failed", tmp.string()));
        f << lines_sorted(data);
        f.flush();
        if (!f) throw LpkgException(string_format("error.db_write_failed", tmp.string()));
    }

    fsync_and_rename(tmp, path);
}

// ============================================================================
// WAL 保护的写入方法（write-ahead 顺序）
// ============================================================================

void Cache::write_db_file_wal(
    const fs::path& db_path,
    const std::map<std::string, std::unordered_set<std::string>, std::less<>>& db,
    const std::string& milestone, const std::string& wal_op_type)
{
    /*
     * 顺序（I-FSYNC-2）：
     *   WAL: <wal_op_type> <path> <milestone>
     *   fsync WAL
     *   备份：rename old → .lpkg_db_bak_before:<milestone>
     *   fsync 备份父目录
     *   写 .tmp
     *   fsync .tmp
     *   rename .tmp → <path>
     *   fsync <path> 父目录
     *
     * 序列中的三个 fsync **不受 durable_fsync_enabled() 影响**（DurableFsyncGuard）：
     * 上面那步"备份旧文件"产出的 .lpkg_db_bak_before:<milestone> 会在批次提交后立刻被
     * cleanup_db_backups() 删掉 —— 若新库只 rename 到页缓存，断电就落在"库丢了且唯一
     * 备份已删"的不可恢复窗口里。
     */

    DurableFsyncGuard durable;
    const bool is_new = !fs::exists(db_path);

    // 1. WAL 行 — 新建文件使用 DBNEW（确保回滚时删除），已存在文件使用调用者指定的类型
    const auto effective_op_type =
        (is_new && wal_op_type == "DB") ? std::string("DBNEW") : wal_op_type;
    wal::log_wal_line(effective_op_type + " " + db_path.string() + " " + milestone);

    // 2. 备份旧文件（仅当文件已存在时）
    std::string bak_path = db_path.string() + ".lpkg_db_bak_before:" + milestone;
    if (!is_new) {
        safe_rename(db_path, bak_path);
    }

    // 3. 写 .tmp
    const fs::path tmp = db_path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open()) throw LpkgException(string_format("error.create_tmp_db_failed"));
        for (const auto& [key, values] : db) {
            f << key << "\t" << join_sorted(values) << "\n";
        }
        // 写失败（磁盘满等）必须中止，不能把截断文件 rename 进正式位置
        f.flush();
        if (!f) throw LpkgException(string_format("error.db_write_failed", tmp.string()));
    }

    // 4. fsync .tmp
    fsync_and_rename(tmp, db_path);
}

void Cache::write_set_file_wal(const fs::path& path, const std::unordered_set<std::string>& data,
                               const std::string& milestone, const std::string& wal_op_type)
{
    /*
     * 与 write_db_file_wal 相同序列（含同样的"永远持久化"要求：DurableFsyncGuard）
     */

    DurableFsyncGuard durable;
    const bool is_new = !fs::exists(path);

    // 1. WAL 行 — 新建文件使用 DBNEW（确保回滚时删除），已存在文件使用调用者指定的类型
    const auto effective_op_type =
        (is_new && wal_op_type == "DB") ? std::string("DBNEW") : wal_op_type;
    wal::log_wal_line(effective_op_type + " " + path.string() + " " + milestone);

    // 2. 备份旧文件
    std::string bak_path = path.string() + ".lpkg_db_bak_before:" + milestone;
    if (!is_new) {
        safe_rename(path, bak_path);
    }

    // 3. 写 .tmp
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f.is_open())
            throw LpkgException(string_format("error.create_file_failed", tmp.string()));
        f << lines_sorted(data);
        f.flush();
        if (!f) throw LpkgException(string_format("error.db_write_failed", tmp.string()));
    }

    // 4. fsync .tmp + rename + fsync parent
    fsync_and_rename(tmp, path);
}

std::map<std::string, std::unordered_set<std::string>, std::less<>> Cache::read_db_uncached(
    const fs::path& path)
{
    std::map<std::string, std::unordered_set<std::string>, std::less<>> db;
    std::ifstream db_file(path);
    if (!db_file.is_open()) return db;
    std::string line;
    while (std::getline(db_file, line)) {
        if (line.empty()) continue;
        size_t tab_pos = line.find('\t');
        if (tab_pos != std::string::npos) {
            std::string key = line.substr(0, tab_pos);
            std::string values = line.substr(tab_pos + 1);
            if (!values.empty() && values.back() == '\r') values.pop_back();
            size_t start = 0, end;
            while ((end = values.find(',', start)) != std::string::npos) {
                if (end > start) db[key].insert(values.substr(start, end - start));
                start = end + 1;
            }
            if (start < values.size()) db[key].insert(values.substr(start));
        }
    }
    return db;
}
