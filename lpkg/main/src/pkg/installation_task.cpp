#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "archive.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "config/config.hpp"
#include "crypto/hash.hpp"
#include "db/cache.hpp"
#include "db/test_breakpoints.hpp"
#include "db/transaction_log.hpp"
#include "db/wal_op.hpp"
#include "downloader.hpp"
#include "i18n/localization.hpp"
#include "install_common.hpp"
#include "op_sink.hpp"
#include "trigger/trigger.hpp"
#include "vercmp/version.hpp"

extern std::atomic<bool> sigint_graceful;

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
/**
 * 升级/移除的目录清理追踪。设 LPKG_TRACE_REMOVE=1 时逐条打印废弃文件/目录的决策与
 * 底层操作（与 strace 的 unlink/rmdir/rename 对照，定位"哪些目录没被删、为什么"）。
 * 默认静默，不引入日志噪音。
 */
void trace_remove(const std::string& msg)
{
    if (const char* v = std::getenv("LPKG_TRACE_REMOVE"); v && std::string_view(v) == "1")
        fprintf(stderr, "[lpkg-remove-trace] %s\n", msg.c_str());
}

/**
 * 配置文件升级时的**三哈希分流**结论（pacman `add.c` 的语义）。
 *
 * 三份哈希：`hash_local` = 盘上那份的内容哈希；`hash_orig` = **上一次我们往这个包的
 * 这个路径里装进去的内容**的哈希（记在 `confhashes.db` 里，见 `Config::conf_hashes_db()`）；
 * `hash_pkg` = 本次包里那个条目的内容哈希。
 */
enum class ConfigDisposition {
    /// ① 盘上 == 上次装进去的 → 用户**没改过** → 静默换成新版（不产生 `.lpkgnew`）
    InstallNew,
    /// ② 上次装进去的 == 本次的 → 包本身没改这个配置 → 保留用户文件、**连 `.lpkgnew` 都不产生**
    KeepLocal,
    /// ③ 三者互异 / 无从判定 → 新版落 `.lpkgnew` + 告警
    SaveLpkgnew,
};

/**
 * 三哈希分流的判定表。语义只有这一份（pacman `add.c` 的三条分支 + 老 DB 的退化路径），
 * 别在调用点再写一遍。
 *
 * **有旧记录**（正常路径）：
 *   ① 盘上 == 旧记录   → `InstallNew`（用户没改过，静默换新版）
 *   ② 旧记录 == 新包   → `KeepLocal`（包没改这个配置，保留用户文件、不产生 `.lpkgnew`）
 *   ③ 其余（含 `hash_local` 拿不到 —— 盘上是符号链接/读不到）→ `SaveLpkgnew`（原文件不动）
 *
 * **无旧记录**（老 DB：本特性之前装的包，`confhashes.db` 里没有这一条）——只算两份哈希：
 *   · 盘上那份 == 新包那份 → `KeepLocal`（内容一致 = 没有"新东西"要给用户审阅，
 *     也就不该产生 `.lpkgnew`）；
 *   · 不一致（或盘上那份不可读）→ `SaveLpkgnew`（无从判定用户改没改过 → 保守）。
 *   两个分支都由调用点把 **`hash_pkg`（包内那份）**的哈希写进 DB —— 这里**没有例外**：
 *   盘上那份（退化路径上往往还是**无主**文件）永不被追认（历史与后果见 `record` 处的注释）。
 *   ⚠ 代价说明（有意为之）：盘上那份与记录不相等时，包每改一次配置都会再落一份**可见的**
 *   `.lpkgnew`（吵，但绝不静默）；用户把 `.lpkgnew` 合并进盘上那份（此后盘上 == 记录）就
 *   回到正常分流。曾经的做法是"追认盘上那份"——换来"`.lpkgnew` 不刷"，代价是把用户文件
 *   当成我们装的：下一次升级满足 ① 静默覆盖它，且记录被删（升级丢弃 /etc 条目 /
 *   remove_conf_hash / 移除包，三处都会删）之后"追认"会被重新武装、反复发生。
 */
ConfigDisposition classify_config_update(std::string_view hash_local, std::string_view hash_orig,
                                         std::string_view hash_pkg)
{
    if (hash_orig.empty()) {
        if (!hash_local.empty() && hash_local == hash_pkg) return ConfigDisposition::KeepLocal;
        return ConfigDisposition::SaveLpkgnew;
    }
    if (!hash_local.empty() && hash_local == hash_orig) return ConfigDisposition::InstallNew;
    if (hash_orig == hash_pkg) return ConfigDisposition::KeepLocal;
    return ConfigDisposition::SaveLpkgnew;
}
}  // namespace

// ===== InstallationTask 实现 =====

InstallationTask::InstallationTask(std::string pkg_name, std::string version, bool explicit_install,
                                   std::string old_version_to_replace, fs::path local_package_path,
                                   std::string expected_hash, bool force_reinstall)
    : pkg_name_(std::move(pkg_name)),
      version_(std::move(version)),
      explicit_install_(explicit_install),
      tmp_pkg_dir_(Config::get_tmp_dir() / pkg_name_),
      actual_version_(version_),
      old_version_to_replace_(std::move(old_version_to_replace)),
      local_package_path_(std::move(local_package_path)),
      expected_hash_(std::move(expected_hash)),
      force_reinstall_(force_reinstall)
{
    // 包名来自**不可信来源**（远端索引 / .lpkg 内的 metadata.json），而它会被当成路径
    // 分量拼进 tmp_pkg_dir()、dep_dir()、needed_so_dir()、docs_dir()、hooks_dir()
    // —— 一个 `../` 就能以 root 写到这些目录之外（TODO.md X4）。此处在唯一的构造入口挡住。
    if (!is_safe_path_component(pkg_name_)) {
        throw LpkgException(
            string_format("error.unsafe_path_component", "package name", pkg_name_));
    }
}

/**
 * 执行安装流程（WAL 2.0 原子事务）：
 *   WAL: BEGIN → 预检 → 备份(BACKUP/NEW/NEW_DIR) → 复制(COPY) →
 *        注册 → COMMIT → END
 *
 * 如果 copy_package_files() 抛出异常，执行包级文件回滚(RESTORE_x/REMOVE_x)
 * 并写 ROLLBACK/END。
 *
 * .lpkg_bak 文件延迟到 COMMIT_PKGS 后的统一清理阶段才删除，
 * 确保批量回滚时可以恢复每个已安装包的文件。
 */
void InstallationTask::run(InstallContext* ctx)
{
    const std::string current_installed_version =
        Cache::instance().get_installed_version(pkg_name_);
    if (!force_reinstall_ && !current_installed_version.empty() &&
        current_installed_version == actual_version_) {
        // **早退：本包未被处理**。`processed_` 保持 false —— 这正是不变量所在：此刻
        // `hook_files_` 也是空的，但它表示"本任务没跑过"，**不是**"新版本没有 hooks"。
        // 批次级记账（package_manager 的 hook_sets）据此区分，否则会把一个已装同版本
        // 计划成员的 hooks 目录当成"新版本没有 hooks"整个删掉（见 did_process()）。
        log_info(string_format("info.package_already_installed", pkg_name_));
        return;
    }
    processed_ = true;  // 从这里往下：本任务真的会动盘 / 改 DB

    log_info(string_format("info.installing_package", pkg_name_, version_));
    ensure_dir_exists(tmp_pkg_dir_);

    // 第一阶段：预检——不碰文件，只做检查
    prepare(ctx);

    // WAL: BEGIN <pkg> <ver> + fsync
    wal::log_wal_line("BEGIN " + pkg_name_ + " " + actual_version_);

    // 断点：begin 之后，可用于模拟安装刚开始就崩溃
    BreakpointManager::instance().hit("install_after_begin_" + pkg_name_);

    try {
        // 第二阶段：备份 + 复制（含 WAL 条目）
        backup_existing_files();
        copy_package_files();

        // 第三阶段：注册——数据库修改
        commit_without_file_ops();

        // WAL: COMMIT <pkg> <ver> + fsync
        wal::log_wal_line("COMMIT " + pkg_name_ + " " + actual_version_);
        // 断点：COMMIT 之后、END 之前（模拟批量事务中某包成功后崩溃）
        BreakpointManager::instance().hit("after_commit_" + pkg_name_);

        // WAL: END <pkg> <ver> + fsync
        wal::log_wal_line("END " + pkg_name_ + " " + actual_version_);

        // 注意：不在此处清理 .lpkg_bak！
        // 所有备份文件延迟到 COMMIT_PKGS 后统一清理（见 batch_transaction.hpp）

        log_info(string_format("info.package_installed_successfully", pkg_name_));
    } catch (...) {
        // 包级文件回滚
        rollback_files();
        throw;
    }
}

/** 准备阶段：下载并验证包、解压、检查依赖和文件冲突 */
void InstallationTask::prepare(InstallContext* ctx)
{
    download_and_verify_package();
    extract_and_validate_package();
    if (ctx) {
        ensure_dependencies_satisfied(*ctx);
    }
    check_for_file_conflicts(ctx);
}

/**
 * 提交安装（无文件操作部分）：注册包 -> 移除旧版废弃文件 -> 运行 post-install
 * 钩子
 */
void InstallationTask::commit_without_file_ops()
{
    std::unordered_set<std::string> old_files;
    if (!old_version_to_replace_.empty()) {
        old_files = Cache::instance().get_package_files(pkg_name_);
        log_info(string_format("info.upgrade_old_files_check", pkg_name_, old_version_to_replace_,
                               actual_version_, old_files.size()));
    }

    register_package();

    // 移除新版本中不再包含的旧文件/目录（REMOVE_OLD / DIR_RM，原子、可回滚）。
    //
    // **备份进 stash（TODO.md）后目录删除天然成立**：旧文件搬到每文件系统 stash（不在
    // 原目录里占位），旧目录随即**为空** → 最深优先逐个 `rmdir` + `DIR_RM`（元数据记录），
    // 不再需要整目录实体备份，也不会再有"刚 rename 的 bak 占着目录 → 单层判空失败 →
    // 嵌套第 2 层空壳（dist-info/licenses/）残留"的历史缺陷。
    // 目录非空（含无主内容/状态目录/conffile/其他包文件）→ 保留，绝不误删不属于本包的东西。
    if (!old_files.empty()) {
        // 写入层原语：废弃文件/目录的 WAL 行与物理操作成对发生（见 op_sink.hpp）
        detail::OpSink sink(pkg_name_, &stashes_);
        const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
        auto new_files = detail::scan_content_files(content_dir);
        std::unordered_set<std::string> new_set;
        for (const auto& f : new_files) new_set.insert((fs::path("/") / f).string());

        auto& cache = Cache::instance();
        const fs::path root = Config::instance().root_dir();
        const auto to_phys = [&](const std::string& logical) -> fs::path {
            return fs::path(logical).is_absolute() ? root / fs::path(logical).relative_path()
                                                   : root / logical;
        };

        // ── 阶段 1：废弃的普通文件（含符号链接）→ rename 进 stash（REMOVE_OLD）──
        for (const auto& old_file : old_files) {
            if (old_file.ends_with('/')) continue;  // 目录 → 阶段 2
            if (old_file.starts_with(std::string(constants::DIR_ETC_PREFIX))) {
                if (!new_set.contains(old_file)) {
                    cache.remove_file_owner(old_file, pkg_name_);
                    // 本包不再提供这个配置 → 记的"我们装进去过什么"也失效：留着它，等
                    // 新版本**重新**发这个文件时会把旧记录的哈希当成 hash_orig。
                    cache.remove_conf_hash(old_file, pkg_name_);
                }
                continue;
            }
            if (new_set.contains(old_file)) continue;
            auto owners = cache.get_file_owners(old_file);
            if (!owners.contains(pkg_name_)) continue;
            cache.remove_file_owner(old_file, pkg_name_);
            if (!cache.get_file_owners(old_file).empty()) continue;

            const fs::path phys = strip_trailing_slash(to_phys(old_file));
            // 新版本把该路径变成了**目录**（文件→目录的升级，TODO E4）：内容已由拷贝阶段
            // 替换好，这里绝不能再把它当"废弃旧文件"搬进 stash——否则刚建好的目录被搬走，
            // 升级"成功"但目录消失（实测）。目录的清理由阶段 2 负责。
            if (fs::is_directory(phys) && !fs::is_symlink(phys)) continue;
            // 盘上是 symlink→目录、而新版本在该路径登记了**目录条目**（`path/`）：同理 —— 拷贝
            // 阶段已**穿过链接**把内容写进真实目录，把链接搬进 stash 会让这个路径整个消失
            // （实测：v1 发 symlink `var/run` → v2 发目录 `var/run/`，升级后 /var/run 干脆不存在，
            // 比原事故更糟 —— 原事故至少还留下一个实体目录）。
            if (fs::is_directory(phys) && new_set.contains(old_file + "/")) continue;
            if (!(fs::exists(phys) || fs::is_symlink(phys))) continue;
            trace_remove("obsolete FILE " + old_file + " → rename into stash");
            log_info(string_format("info.removing_obsolete_file", old_file));
            // WAL: REMOVE_OLD + rename 进 stash（一次调用，顺序不可能写反）
            sink.backup_obsolete(phys);
        }

        // ── 阶段 2：废弃的目录（最深优先，仅最后持有者）→ 空则 DIR_RM（rmdir+元数据）──
        std::vector<fs::path> obsolete_dirs;
        for (const auto& old_file : old_files) {
            if (!old_file.ends_with('/')) continue;
            if (old_file.starts_with(std::string(constants::DIR_ETC_PREFIX))) {
                // 配置目录从不物理删除（与 /etc 文件一致，保留系统配置）
                if (!new_set.contains(old_file)) {
                    cache.remove_file_owner(old_file, pkg_name_);
                    cache.remove_conf_hash(old_file, pkg_name_);  // 记录随归属一起撤（同阶段 1）
                }
                continue;
            }
            if (new_set.contains(old_file)) continue;
            auto owners = cache.get_file_owners(old_file);
            if (!owners.contains(pkg_name_)) continue;
            cache.remove_file_owner(old_file, pkg_name_);
            // 目录键带尾斜杠 → 物理路径先规范化：否则下面阶段里的 is_symlink 守卫恒假，
            // rmdir 会穿过链接删掉链接指向的真实目录（见 strip_trailing_slash）
            if (cache.get_file_owners(old_file).empty())
                obsolete_dirs.push_back(strip_trailing_slash(to_phys(old_file)));
        }
        // 子目录先于父目录处理（父目录只有在子目录被 rmdir 后才为空）
        std::ranges::sort(obsolete_dirs, [](const fs::path& a, const fs::path& b) {
            return a.string().size() > b.string().size();
        });

        for (const auto& phys : obsolete_dirs) {
            std::error_code ec;
            if (!fs::is_directory(phys, ec) || fs::is_symlink(phys)) continue;
            if (!fs::is_empty(phys, ec)) {
                trace_remove("obsolete DIR  " + phys.string() + " → SKIP（含无主内容，整树保留）");
                continue;
            }
            trace_remove("obsolete DIR  " + phys.string() + " → rmdir + DIR_RM");
            log_info(string_format("info.removing_obsolete_file", phys.string()));
            // WAL: DIR_RM（含 mode/uid/gid） + rmdir（一次调用）
            // 目录型**挂载点**由 remove_empty_dir 挡下（rmdir 恒 EBUSY）→ 跳过 + 告警
            if (sink.remove_empty_dir(phys) == detail::DirRemoval::SkippedMountPoint)
                log_warning(string_format("warning.remove_mount_point", phys.string()));
        }
    }

    install_hook_files();
}

/** 备份所有将被覆盖的文件为 .lpkgbak，同时写入 WAL 条目 */
void InstallationTask::backup_existing_files()
{
    // 写入层原语：BACKUP/NEW/NEW_DIR 的 WAL 行与物理操作成对发生（见 op_sink.hpp）
    detail::OpSink sink(pkg_name_, &stashes_);
    const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
    auto files = detail::scan_content_files(content_dir);

    for (const auto& f : files) {
        if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

        fs::path rel_f = f;
        if (rel_f.is_absolute()) rel_f = rel_f.relative_path();
        const fs::path physical_path = Config::instance().root_dir() / rel_f;
        const fs::path phys_dir = physical_path.parent_path();

        const bool is_config = f.starts_with(std::string(constants::DIR_ETC));
        if (is_config) continue;

        std::error_code ec;
        if (f.ends_with('/')) {
            // **必须先去掉尾斜杠再判存在**：目录条目的 physical_path 以 '/' 结尾，而
            // `fs::exists("/x/foo/")` 对**已存在的普通文件**返回 false（尾斜杠要求它是目录）
            // ——直接用 physical_path 判会把"这里有个文件"误判成"路径不存在"，
            // 于是既不备份也不删除，随后 copy_package_files 的 ensure_dir_exists 抛
            // "Failed to create directory: File exists" 让整批中止（实测）。
            // 判之前必须先剥尾斜杠：否则 lstat / is_symlink 会把末尾的符号链接**解引用**
            // （pacman 为此专门写了 llstat()，见 FS#51377 / commit 16b91f79）。
            const fs::path probe = strip_trailing_slash(physical_path);
            const bool path_exists = fs::exists(probe) || fs::is_symlink(probe);
            bool is_new_dir = !path_exists;

            // 盘上不是**真目录**（lstat 语义：普通文件、符号链接都算"非目录"）而归档要求目录
            // → 按文件冲突接管：搬进 stash（BACKUP，可回滚）+ 建目录（TODO E4 的"文件→目录"）。
            // **不写穿 symlink→目录**：pacman 明确不支持该语义（"We do not support treating
            // symlinks to directories as directories. They are considered a file."），而且包内容
            // 会落进链接目标（`/var/run` 这类通常是 tmpfs，重启即蒸发），DB 记的逻辑路径与实际
            // 落点也会脱节。挡住这类接管的责任在 check_for_file_conflicts：只要该路径不是**本包
            // 旧版本**以非目录形态持有（E4），就在那里判冲突、整批中止，触碰不到这里。
            if (path_exists && (!fs::is_directory(probe) || fs::is_symlink(probe))) {
                // WAL: BACKUP + rename 进 stash（一次调用；此路径无 write-ahead 断点）
                sink.backup(probe);
                is_new_dir = true;
            }
            if (is_new_dir) {
                new_dirs_.push_back(physical_path);
                // WAL: NEW_DIR <path>  (write-ahead: 先写 WAL 再做实际操作)
                sink.new_dir(physical_path);
            }
            fs::create_directories(phys_dir, ec);
            if (is_new_dir) {
                fs::create_directories(physical_path, ec);
                std::string dir_rel = f;
                if (dir_rel.ends_with('/')) dir_rel.pop_back();
                const fs::path src_dir = content_dir / dir_rel;
                struct stat st;
                if (lstat(src_dir.c_str(), &st) == 0) {
                    (void)lchown(physical_path.c_str(), st.st_uid, st.st_gid);
                    (void)chmod(physical_path.c_str(), st.st_mode & constants::PERM_MASK_ALL);
                }
            }
            continue;
        }

        fs::create_directories(phys_dir, ec);
        if (fs::exists(physical_path) || fs::is_symlink(physical_path)) {
            // is_directory 会跟随符号链接：symlink→目录 会被误判为"目录"而跳过备份。
            // 但 copy_package_files 的 symlink/普通文件分支都会直接替换该路径（且无 WAL
            // 记录），回滚时没有 BACKUP 可恢复 → 旧符号链接永久丢失。故符号链接（含指向
            // 目录的）一律备份；仅真正的目录（非 symlink）由目录逻辑处理、跳过。
            if (fs::is_symlink(physical_path) || !fs::is_directory(physical_path)) {
                // check_for_file_conflicts 已处理文件冲突，此处无需重复检测
                // 备份进每文件系统 stash（不再原位占目录，避免父目录删除时 bak 被包进去）
                // WAL: BACKUP + rename（断点位于 write-ahead 窗口内，只能在 sink 里命中）
                sink.backup(physical_path, "backup_after_wal_" + pkg_name_);
            }
        } else {
            new_files_.push_back(physical_path);
            // WAL: NEW <path>
            sink.new_file(physical_path);
        }
    }
}

/**
 * 包级回滚标记（WAL 2.0）。
 *
 * **这里不做任何文件系统撤销**——所有正向操作的逆序执行统一由
 * `batch_rollback` → `reverse_execute` 完成（所有 install/upgrade 都经
 * `run_batch_transaction`，包级失败必然触发批次回滚，撤销紧接着发生）。
 *
 * 曾在此处先恢复文件再写 RESTORE 审计，与 `reverse_execute` 形成**双重回滚**：
 * rollback_files 从 .lpkg_bak 恢复的旧文件，被 reverse_execute 的 COPY 逆操作
 * （无条件删除 dst）再次删除 → 升级中途失败的包丢失旧文件（数据破坏）。
 * 撤销职责收拢到 reverse_execute 单一路径后，该问题消失，且"rollback_files 删除
 * 新文件失败"的残留也能由 reverse_execute 兜底重试。
 *
 * 本函数只清理内存追踪并写 ROLLBACK/END 标记（失败包的包级回滚记录；
 * 成功包的 ROLLBACK 由 batch_rollback 统一写）。
 */
void InstallationTask::rollback_files()
{
    // WAL: ROLLBACK + END
    wal::log_wal_line("ROLLBACK " + pkg_name_ + " " + actual_version_);
    wal::log_wal_line("END " + pkg_name_ + " " + actual_version_);

    // 清空内部追踪（撤销由 reverse_execute 依据 WAL 完成）
    stashes_.clear();
    new_files_.clear();
    new_dirs_.clear();
}

/**
 * 下载并验证包文件
 * 如果是本地包文件，直接使用并校验 SHA256 哈希
 * 如果是从仓库安装，从镜像 URL 下载并校验
 */
void InstallationTask::download_and_verify_package()
{
    if (!local_package_path_.empty()) {
        if (!fs::exists(local_package_path_))
            throw LpkgException(
                string_format("error.local_pkg_not_found", local_package_path_.string()));
        log_info(string_format("info.installing_local_file", local_package_path_.string()));
        archive_path_ = local_package_path_;
        if (!expected_hash_.empty() && calculate_sha256(archive_path_) != expected_hash_)
            throw LpkgException(string_format("error.hash_mismatch", pkg_name_));
        return;
    }

    const std::string mirror_url = Config::instance().get_mirror_url();
    const std::string arch = Config::instance().get_architecture();

    if (actual_version_.empty() || actual_version_ == constants::VER_LATEST) {
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

    // 版本号同样不可信（CLI `pkg:版本` 或远端索引），它会被拼进下载落点与
    // `tmp_pkg_dir_ / (版本 + ".lpkg")` —— 含 `../` 即可写到临时目录之外（TODO.md X4）。
    if (!is_safe_path_component(actual_version_)) {
        throw LpkgException(
            string_format("error.unsafe_path_component", "version", actual_version_));
    }

    const std::string download_url = mirror_url + arch + "/" + pkg_name_ + "/" + actual_version_ +
                                     std::string(constants::EXT_LPKG);
    archive_path_ = tmp_pkg_dir_ / (actual_version_ + std::string(constants::EXT_LPKG));

    if (!fs::exists(archive_path_)) download_with_retries(download_url, archive_path_, 5, true);
    if (!expected_hash_.empty() && calculate_sha256(archive_path_) != expected_hash_)
        throw LpkgException(string_format("error.hash_mismatch", pkg_name_));
}

/** 解压包文件到临时目录，验证包结构完整性 */
void InstallationTask::extract_and_validate_package()
{
    // 内容已由整批预检下载解压（同一归档、同一解压根）→ 不重复 tar 解压，只做结构校验与
    // 元数据回读。标记只在预检成功解压后置位，而计划重解会整体重建 InstallPlan（标记归零，
    // 见 InstallPlan::content_ready），故不会指向别的版本的解压产物。
    if (!content_ready_) {
        log_info(string_format("info.extracting_to_tmp", pkg_name_));
        extract_tar_zst(archive_path_, tmp_pkg_dir_, pkg_name_);
    }

    for (const auto& meta : {constants::PKG_METADATA_FILE, constants::DIR_CONTENT}) {
        if (!fs::exists(tmp_pkg_dir_ / meta))
            throw LpkgException(
                string_format("error.incomplete_package", (tmp_pkg_dir_ / meta).string()));
    }

    std::string meta_name, meta_version;
    detail::read_package_metadata(tmp_pkg_dir_, meta_name, meta_version, deps_, provides_,
                                  needed_so_, man_content_);
    if (meta_name != pkg_name_) {
        log_warning(string_format("warning.package_name_mismatch", pkg_name_, meta_name));
    }
}

void InstallationTask::ensure_dependencies_satisfied(InstallContext& ctx)
{
    if (Config::instance().no_deps_mode()) return;
    auto actual_deps = detail::parse_dep_strings(deps_);
    if (actual_deps.empty()) return;

    log_info(string_format("info.checking_deps", pkg_name_));

    for (const auto& dep : actual_deps) {
        const std::string& dep_name = dep.name;
        const std::string installed_ver = Cache::instance().get_installed_version(dep_name);

        if (!installed_ver.empty()) {
            if (dep.constraints.empty() || version_satisfies_all(installed_ver, dep.constraints)) {
                continue;  // 真实包已安装且满足
            }
        } else if (!Cache::instance().get_providers(dep_name).empty()) {
            // 名称是能力（无同名真实包，但已有包注册提供该能力）→ 视为满足。
            // 能力（多为无版本 SONAME）不带版本，约束对之无意义。
            continue;
        }

        if (ctx.plan.contains(dep_name)) continue;  // 计划中已有同名真实包

        // 命名能力：由计划中某包提供（能力名≠包名，libsolv 已按能力解析）。
        // 提供者的**真实元数据可能与本索引不一致**（如 DynamicProviderChange 场景）——
        // 把该能力记入 ctx.targets，供元数据验证触发的重解（i=0 重启）重新拉取正确
        // 提供者。**绝不在此重解**：中途改写 order 且不重置批次游标会导致依赖者先于
        // 提供者安装、产生重复计划项（曾因此乱序）。
        bool provided_by_plan = false;
        for (const auto& [pn, plan_pkg] : ctx.plan) {
            for (const auto& prov : plan_pkg.provides) {
                if (prov == dep_name) {
                    provided_by_plan = true;
                    break;
                }
            }
            if (provided_by_plan) break;
        }
        if (provided_by_plan) {
            bool already_target = false;
            for (const auto& [tn, tv] : ctx.targets)
                if (tn == dep_name) {
                    already_target = true;
                    break;
                }
            if (!already_target)
                ctx.targets.emplace_back(dep_name, std::string(constants::VER_LATEST));
            continue;
        }

        // 已是目标（libsolv 会处理）
        bool is_target = false;
        for (const auto& [tn, tv] : ctx.targets)
            if (tn == dep_name) {
                is_target = true;
                break;
            }
        if (is_target) continue;

        // 走到这里 = solver/plan 不一致：依赖未安装、不在计划、也不由计划包提供。
        // 元数据验证（install_packages/upgrade_packages）在批次开始前已按真实元数据
        // 重解并 i=0 重启，这里不应再发现新依赖。**绝不在此重解**（中途改写 order 且
        // 不重置游标 → 乱序安装），改为显式报错，整批回滚。
        throw LpkgException(string_format("error.dep_missing_from_plan", dep_name, pkg_name_));
    }

    if (!needed_so_.empty()) {
        for (const auto& soname : needed_so_) {
            bool provided = false;

            for (const auto& [pn, plan_pkg] : ctx.plan) {
                for (const auto& prov : plan_pkg.provides) {
                    if (prov == soname) {
                        provided = true;
                        break;
                    }
                }
                if (provided) break;
            }

            if (!provided) {
                auto providers = Cache::instance().get_providers(soname);
                for (const auto& p : providers) {
                    if (Cache::instance().is_installed(p)) {
                        if (!ctx.plan.contains(p)) {
                            provided = true;
                            break;
                        }
                    }
                }
            }

            if (!provided) {
                if (auto prov_pkg = ctx.repo.find_provider(soname)) {
                    // 提供者已被"认领"（已安装，或在本批次计划中以另一版本出现）时，
                    // 其版本已被锁定——仓库里其它版本提供此 SONAME 不能算数。
                    // 否则依赖者声明的版本约束/已装版本会被仓库旧版本悄悄绕过。
                    bool claimed = ctx.plan.contains(prov_pkg->name) ||
                                   Cache::instance().is_installed(prov_pkg->name);
                    if (!claimed) {
                        for (const auto& prov : prov_pkg->provides) {
                            if (prov == soname) {
                                provided = true;
                                break;
                            }
                        }
                    }
                }
            }

            // --use-system-soname：系统 /usr/lib 已有该 .so（如 backup 的旧 SONAME）→ 视为满足。
            // 配合 farm 的 ABI 过渡机制：旧二进制在过渡期加载旧 .so，新构建用新 .so。
            if (!provided && Config::instance().use_system_soname_mode() &&
                Config::instance().has_system_soname(soname)) {
                provided = true;
            }

            if (!provided) {
                if (Config::instance().missing_so_no_error_mode()) {
                    // --missing-so-no-error：bootstrap/过渡期容忍缺失 SONAME，警告继续。
                    log_warning(string_format("warning.missing_so_no_error", soname, pkg_name_));
                } else {
                    throw LpkgException(
                        string_format("error.unresolvable_drift", pkg_name_,
                                      string_format("error.unresolved_soname", soname)));
                }
            }
        }
    }
}

namespace
{
/**
 * 一个逻辑路径"此刻"的盘面状态（lstat 语义：符号链接算**存在**但**不是目录**）。
 *
 * 判之前一律 `strip_trailing_slash`：尾斜杠会把末尾的符号链接**解引用**
 * （pacman 为此专门写了 `llstat()`，见 FS#51377 / commit 16b91f79），`is_symlink` 恒假、
 * "别动 symlink→目录"的守卫集体失效。`root / <绝对路径>` 会**丢弃**左值（archive.cpp
 * 里踩过同一个坑），所以这里先把逻辑路径转成相对路径再拼。
 */
struct PathProbe {
    bool exists = false;
    bool is_dir = false;
};

PathProbe probe_path(const fs::path& root, const std::string& logical_bare)
{
    fs::path rel = logical_bare;
    if (rel.is_absolute()) rel = rel.relative_path();
    const fs::path phys = strip_trailing_slash(root / rel);
    std::error_code ec;
    PathProbe p;
    p.exists = fs::exists(phys, ec) || fs::is_symlink(phys);
    p.is_dir = p.exists && fs::is_directory(phys, ec) && !fs::is_symlink(phys);
    return p;
}

/**
 * 冲突判定需要看到的"世界"。逐包检查（InstallationTask::check_for_file_conflicts）把它
 * 绑到**真实** Cache 与真实盘面；整批预检（check_batch_file_conflicts）绑到"当前状态 +
 * 本批次已处理成员的效果"的模拟状态。
 *
 * **六条判定语义只在 collect_content_conflicts 里写一份** —— 两处各写一份必然漂移，
 * 而漂移的后果是"预检放行、逐包又拦下"（用户付了整批回滚的代价）或反过来。
 */
struct ConflictView {
    /// bare 逻辑路径（无尾斜杠，如 "/usr/share/x"）此刻的盘面状态
    std::function<PathProbe(const std::string& bare)> probe;
    /// key 是否由 pkg 持有（key 是逻辑路径；目录键带尾斜杠）
    std::function<bool(const std::string& key, const std::string& pkg)> owned_by;
    /// key 的全部持有者
    std::function<std::vector<std::string>(const std::string& key)> owners;
    /// 撤销 key 的全部持有者（所有权接管；只改内存状态，落盘/回滚由 WAL 负责）
    std::function<void(const std::string& key)> drop_owners;
    /// 这些持有者是否"即将在本批次里被替换"（两侧语义各自适配，见各调用点说明）
    std::function<bool(const std::vector<std::string>& holders)> all_upgrading;
};

/**
 * 文件冲突判定核心（pacman conflict.c 的安装侧语义）。六条必须逐字保持：
 *
 *   ① 自持短路：该路径由**本包**持有 → 不判冲突（重装 / 升级自己）；
 *   ② `ours` 豁免是**方向性**的：只豁免"归档**目录**条目接管本包旧版本的**文件/符号链接**"
 *      （文件→目录升级，TODO E4）；反方向 dir→文件 **永不**豁免（pacman 的 case 5 对任何
 *      持有者都不放行，实测放行会 rename 撞 EISDIR、回滚还把空目录 rmdir 掉）；
 *   ③ `--overwrite <glob>`（含等价的 `--force-overwrite` ≡ `--overwrite '*'`）只豁免
 *      **命中该路径**的同一类冲突（`force_exempts = entry_is_dir`），不豁免 dir→文件；
 *      豁免是**逐路径**问 `Config::overwrite_allows(path)` 的，不是"开了就全局放行"。
 *   ④ 冲突信息点名**真实持有者**（该路径在 DB 里的 owner；无人持有 → "未知（手动文件）"）；
 *   ⑤ 持有者全部 `all_upgrading` → 直接接管（不判冲突）；该路径被豁免 → 同样接管。
 *      接管的落点是**内存**状态，批次失败时随 DB 一起回到旧值（ARCH.md §4.5 OWNER_OVERRIDE）；
 *   ⑥ 无人持有、但盘上已有同名物 → 冲突（"未知（手动文件）"），只有该路径被豁免才放行。
 *
 * 本函数只判"归档条目"与"此刻世界"的关系；"谁会先被替换"（顺序）由 view.all_upgrading
 * 表达，这里不关心它怎么算出来的。
 */
void collect_content_conflicts(const std::vector<std::string>& files, const std::string& pkg_name,
                               const ConflictView& view,
                               std::map<std::string, std::string>& conflicts)
{
    for (const auto& f : files) {
        fs::path rel_f = f;
        if (rel_f.is_absolute()) rel_f = rel_f.relative_path();
        const fs::path logical_path = fs::path("/") / rel_f;
        const std::string path_str = logical_path.string();
        std::string bare = path_str;
        if (bare.ends_with('/')) bare.pop_back();

        // ── 类型变更冲突（pacman 的 case 4 / case 5）────────────────────────────
        // 盘上与归档里"目录 / 非目录"不一致时判冲突。lstat 语义下 **symlink 一律算非目录**
        // （pacman conflict.c CHECK 2：只有 lstat 意义上的真目录才 `continue` 免检）。
        //   归档是目录 `x/`、盘上是文件/符号链接 → 不能"搬走它再建目录"（实测事故：把
        //     filesystem 的 `/var/run -> ../run` 换成实体目录）
        //   归档是文件 `x`、盘上是真目录     → 永不覆盖（pacman："not overwriting dir with file"）
        // 例外：该路径由**本包**以另一形态持有（文件→目录升级，TODO E4）→ 照旧接管。
        {
            const PathProbe disk = view.probe(bare);
            if (disk.exists && disk.is_dir != path_str.ends_with('/')) {
                const bool ours =
                    view.owned_by(bare, pkg_name) || view.owned_by(bare + "/", pkg_name);
                // 上面这层只在"类型不一致"时进入，所以 `!disk.is_dir` 恒等于"归档是目录条目"。
                const bool entry_is_dir = !disk.is_dir;
                const bool ours_exempts = ours && entry_is_dir;
                const bool force_exempts = entry_is_dir;  // 豁免也只覆盖这一类
                if (!ours_exempts &&
                    !(force_exempts && Config::instance().overwrite_allows(bare))) {
                    auto holders = view.owners(bare);
                    if (holders.empty()) holders = view.owners(bare + "/");
                    conflicts[path_str] =
                        holders.empty() ? get_string("error.unknown_manual_file") : holders.front();
                }
            }
        }

        if (path_str.ends_with('/')) continue;

        if (view.owned_by(path_str, pkg_name)) continue;

        auto holders = view.owners(path_str);
        if (!holders.empty()) {
            if (view.all_upgrading(holders)) {
                // 旧持有者即将在本批次里被替换 → 提前交棒，本包注册时自然接手
                view.drop_owners(path_str);
                continue;
            }
            if (!Config::instance().overwrite_allows(path_str)) {
                conflicts[path_str] = holders.front();
            } else {
                // 不变量：豁免在此**仅改内存**状态（此刻尚未写 BEGIN WAL 行）。
                // 批次成功 → cache.write(pkg:installed) 落盘；批次失败 → batch_rollback 的
                // reverse_execute 恢复磁盘 DB 后必然 cache.load()（ARCH.md §4.5 OWNER_OVERRIDE）。
                // 改动任何回滚路径时须保持"每次都 cache.load()"。
                view.drop_owners(path_str);
            }
            continue;
        }

        {
            // 类型变更块可能已经写入了**真正的持有者**，别用泛化消息盖掉它（实测：归档文件撞
            // 别的包的真目录，报的会是 "unknown (manual file)" 而不是那个包的名字）。
            if (view.probe(bare).exists && !Config::instance().overwrite_allows(bare) &&
                !conflicts.contains(path_str)) {
                conflicts[path_str] = get_string("error.unknown_manual_file");
            }
        }
    }
}

/** 冲突集合非空 → 按既有格式报错中止（逐包检查与整批预检共用同一份措辞） */
void throw_on_file_conflicts(const std::map<std::string, std::string>& conflicts)
{
    if (conflicts.empty()) return;
    std::string msg = get_string("error.file_conflict_header") + "\n";
    for (const auto& [file, owner] : conflicts)
        msg += "  " + string_format("error.file_conflict_entry", file, owner) + "\n";
    throw LpkgException(msg + get_string("error.installation_aborted"));
}

/** 判定视图：绑到**真实** Cache 与真实盘面（逐包检查用；第二道防线） */
ConflictView real_conflict_view(InstallContext* ctx)
{
    auto& cache = Cache::instance();
    const fs::path root = Config::instance().root_dir();
    return ConflictView{[root](const std::string& bare) { return probe_path(root, bare); },
                        [&cache](const std::string& key, const std::string& pkg) {
                            return cache.is_file_owned_by(key, pkg);
                        },
                        [&cache](const std::string& key) {
                            auto holders = cache.get_file_owners(key);
                            return std::vector<std::string>(holders.begin(), holders.end());
                        },
                        [&cache](const std::string& key) {
                            // 接管 = "归属被摘"：所有权与**哈希记录**是同一条声明（"这个路径归
                            // 我 / 我往这里装过什么"），摘一个就必须摘另一个。原先只摘所有权，
                            // 记录只由 remove_package_files / 升级丢弃 /etc 条目这两处清（且都
                            // 要求"此刻仍持有"）→ 被接管的包留下的记录永久残留：一个不在册的
                            // 包留下的记录会被重新装回来的它当成 hash_orig，"记录随包走"在接管
                            // 场景不成立。
                            //
                            // 选**删除**而不是"改名转手给新持有者"：转手会让新持有者在**它自己
                            // 这次安装**里读到一条它从未装过的 hash_orig（copy_package_files 先
                            // 查 get_conf_hash 再 set_conf_hash）——盘上那份若恰好等于那条外来
                            // 记录，判定表就落到 ① **静默就地替换**，正是本轮刚修掉的那类静默
                            // 覆盖；何况它随后必被新持有者自己的 set_conf_hash（先删同包前缀）
                            // 抹掉，转手是纯亏。删除与移除侧、升级丢弃侧的口径也一致。
                            //
                            // 与所有权一样只改**内存**：批次成功 → 随 cache.write(<pkg>:installed)
                            // 落盘；批次失败 → batch_rollback 恢复磁盘 DB 后必然 cache.load()
                            // （ARCH §4.5）。
                            for (const auto& holder : cache.get_file_owners(key)) {
                                cache.remove_file_owner(key, holder);
                                if (key.starts_with(std::string(constants::DIR_ETC_PREFIX)))
                                    cache.remove_conf_hash(key, holder);
                            }
                        },
                        [ctx](const std::vector<std::string>& holders) {
                            // 逐包语义：持有者都在本批次 plan
                            // 里、且**还没被这一批装过**。`installed_set` 只累积已处理完的包 →
                            // 等价于"本包之后才轮到它"（见 check_batch_file_conflicts
                            // 里对这条的改写）。
                            if (!ctx) return false;
                            for (const auto& holder : holders) {
                                if (!ctx->plan.contains(holder)) return false;
                                if (ctx->installed_set.contains(holder)) return false;
                            }
                            return true;
                        }};
}
}  // namespace

void InstallationTask::check_for_file_conflicts(InstallContext* ctx)
{
    // **第二道防线**：正常路径下整批预检（check_batch_file_conflicts）已拦下所有冲突，
    // 走到这里说明预检放行。判得与预检**完全一样**（共用 collect_content_conflicts），
    // 唯一差别是这里的"世界"是真实 Cache 与真实盘面 —— 负责兜住"预检算漏了"（本批次内
    // 成员互相造出的形态变化）与"预检之后中途状态变了"。
    std::map<std::string, std::string> conflicts;
    collect_content_conflicts(detail::scan_content_files(tmp_pkg_dir_ / constants::DIR_CONTENT),
                              pkg_name_, real_conflict_view(ctx), conflicts);
    throw_on_file_conflicts(conflicts);
}

/**
 * 整批文件冲突预检 —— 在**进入事务之前**（批次开始、任何 BEGIN_PKGS 之前）把本批次所有
 * 包的 content 清单 + 当前所有权状态 + 本批次内的接管顺序**一起**算一遍，判定会不会冲突；
 * 有冲突就在**一个文件都没动**的情况下拒绝。
 *
 * **为什么必须整批**：冲突判定原先只在逐包检查（InstallationTask::check_for_file_conflicts）
 * 里做，而它跑在批次循环内 —— 语义后果是"该批次前面若干包已经完整落地（文件 + DB 里程碑）
 * 之后才发现后面某包的冲突"，然后整批回滚：文件能撤，已经出去的副作用撤不回来。上游
 * libalpm 相反：`alpm_trans_commit` 的第一步就把**整笔事务**的冲突检完
 * （`_alpm_sync_check` 对 `trans->add`），要么全不动要么全动。
 *
 * **逐包检查保留为第二道防线**（见 check_for_file_conflicts 的说明）：预检算漏的（本批次
 * 成员互相新造出的文件/目录形态变化、预检之后中途状态变了）仍由它兜底。判定与逐包检查
 * **共用** collect_content_conflicts（六条语义只有一份实现），差别只在"看到的世界"：
 *   · 所有权 = 盘上 DB 的当前值 + **本批次前序成员装完的效果**（模拟，含所有权接管）；
 *   · 盘面   = 真实盘面 + 前序成员**物理移除**过的路径（升级丢弃的废弃文件）；
 *   · `all_upgrading` = "该持有者也在本批次 plan 里，且在本批次里**比本包晚**轮到"。
 *
 * ── `all_upgrading` 的新判定方式（逐包语义的等价改写）─────────────────────────
 * 逐包检查里这一条是 `plan.contains(owner) && !installed_set.contains(owner)`，而
 * `installed_set` 只累积**已经装完**的包，于是它等价于"该持有者在本批次里还没轮到"
 * = "它排在本包**后面**"。它解决的正是"升级时新增依赖与旧版本文件冲突"：
 *   包 A v1 持有 F → 升级发现新依赖 B 也持有 F；B 先装（依赖优先）、A 后升级
 *   → B 检查时 A 在 plan 里且尚未装 → 放行，A 的旧所有权交给 B；
 *   随后 A 升级时 `owners` 里已没有 A，废弃文件清理也不会把 F 删掉。
 * 预检时"谁都还没装"，所以只能按 **order 里的位置**判：`index(owner) > index(本包)`。
 *
 * **不改变失败语义**：预检拒绝时尚未 `run_batch_transaction`，WAL 里不会出现 BEGIN_PKGS
 * —— "什么都没发生"（与 check_removal_preconditions 前移后的形态一致）。
 */
void check_batch_file_conflicts(std::map<std::string, InstallPlan>& plan,
                                const std::vector<std::string>& order)
{
    auto& cache = Cache::instance();
    const fs::path root = Config::instance().root_dir();

    // order 里的位置。**含"会被跳过"的包**：逐包语义只问"在不在 plan 里 / 装过没有"，
    // 与它最终是否真装无关，故位置表按整个 order 建。
    std::map<std::string, size_t> order_index;
    for (size_t i = 0; i < order.size(); ++i) order_index.emplace(order[i], i);

    // 模拟状态：所有权（初值 = 盘上 DB 的 file_db，键含目录键的尾斜杠形态）
    std::map<std::string, std::set<std::string>> owners;
    {
        std::lock_guard lock(cache.get_mutex());
        for (const auto& [path, holders] : cache.file_db)
            owners[path].insert(holders.begin(), holders.end());
    }
    // 模拟状态：本批次前序成员已**物理移除**的 bare 逻辑路径（升级丢弃的废弃文件）。
    // **不**模拟"前序成员新造出来的路径"：那要把拷贝/备份/目录创建的每个分支都抄一遍，
    // 而漏判的方向由逐包检查兜底（第二道防线）。
    std::set<std::string> released;

    const auto probe_now = [&](const std::string& bare) -> PathProbe {
        if (released.contains(bare)) return PathProbe{false, false};
        return probe_path(root, bare);
    };

    for (const auto& n : order) {
        const auto plan_it = plan.find(n);
        if (plan_it == plan.end()) continue;  // 与批次循环一致：不在计划里的条目不会被处理
        InstallPlan& p = plan_it->second;
        const size_t cur = order_index.at(n);

        const std::string old_ver = cache.get_installed_version(n);
        // 批次循环对"已装同版本且非强制重装"的包直接返回（run() 的早退分支 / 升级循环的
        // 显式 skip）：不碰文件、不注册、也不下载。预检同样跳过。
        if (!p.force_reinstall && !old_ver.empty() && old_ver == p.actual_version) continue;

        // ── 取内容清单：下载 + 解压到标准临时目录（**不碰目标 root**）──
        // 用的是与事务内 prepare() 完全相同的 InstallationTask 布局，解压产物由
        // InstallationTask::extract_and_validate_package 依据 content_ready 复用
        // （否则整批要多解压一遍 tar）。
        InstallationTask task(p.name, p.actual_version, p.is_explicit, old_ver, p.local_path,
                              p.sha256, p.force_reinstall);
        ensure_dir_exists(task.tmp_pkg_dir());
        task.download_and_verify_package();
        task.extract_and_validate_package();
        const auto files = detail::scan_content_files(task.tmp_pkg_dir() / constants::DIR_CONTENT);
        p.content_ready = true;

        // ── 判定（与逐包检查同一份语义，只有"世界"不同）──
        const ConflictView view{probe_now,
                                [&owners](const std::string& key, const std::string& pkg) {
                                    const auto it = owners.find(key);
                                    return it != owners.end() && it->second.contains(pkg);
                                },
                                [&owners](const std::string& key) {
                                    const auto it = owners.find(key);
                                    if (it == owners.end()) return std::vector<std::string>{};
                                    return std::vector<std::string>(it->second.begin(),
                                                                    it->second.end());
                                },
                                [&owners](const std::string& key) { owners[key].clear(); },
                                [&](const std::vector<std::string>& holders) {
                                    for (const auto& holder : holders) {
                                        if (!plan.contains(holder)) return false;
                                        const auto oi = order_index.find(holder);
                                        // 不在 order 里的计划成员：批次循环根本走不到它 →
                                        // 逐包语义下它
                                        // **永远**不在 installed_set 中（视为"还没轮到"）
                                        if (oi != order_index.end() && oi->second <= cur)
                                            return false;
                                    }
                                    return true;
                                }};
        std::map<std::string, std::string> conflicts;
        collect_content_conflicts(files, n, view, conflicts);
        // 抛出 → 一个文件都没落地，WAL 里没有 BEGIN_PKGS（未进入事务）
        throw_on_file_conflicts(conflicts);

        // ── 叠加"本成员装完之后"的效果，供后面的成员判定 ──
        std::set<std::string> new_keys;
        for (const auto& e : files) {
            const std::string key = (fs::path("/") / e).string();
            new_keys.insert(key);
            // register_package 的语义：目录键累加持有者（目录可共享），普通文件键**独占**
            if (e.ends_with('/'))
                owners[key].insert(n);
            else
                owners[key] = {n};
        }
        // 升级：旧版本里"新版本不再发"的键 → 本包的所有权被撤销，且**非目录**键在盘上
        // 确实存在（且不是真目录）时会被搬进 stash（commit_without_file_ops 的
        // REMOVE_OLD 阶段，发生在后续成员被处理之前）。
        for (const auto& old_key : cache.get_package_files(n)) {
            if (new_keys.contains(old_key)) continue;
            owners[old_key].erase(n);
            // /etc 的废弃条目只撤所有权、文件留在盘上（改名 .lpkgsave 是移除侧的事）
            if (old_key.starts_with(std::string(constants::DIR_ETC_PREFIX))) continue;
            // 废弃**目录**键不在这里模拟：阶段 2 只在"本包是最后持有者且目录为空"时 rmdir，
            // 漏建模的方向是"预检偏保守"，且此类形态变化由逐包检查兜底。
            if (old_key.ends_with('/')) continue;
            // 还有别的持有者 → 文件不搬走（REMOVE_OLD 的 `!owners.empty()` 分支）
            if (!owners[old_key].empty()) continue;
            const std::string bare = strip_trailing_slash(old_key);
            // 新版本把它变成了**目录**（文件→目录升级，TODO E4）：文件不消失、也不搬 stash
            if (new_keys.contains(bare + "/")) continue;
            const PathProbe disk = probe_now(bare);
            if (disk.exists && !disk.is_dir) released.insert(bare);
        }
    }
}

void InstallationTask::copy_package_files()
{
    log_info(string_format("info.copying_files", pkg_name_));
    // 写入层原语：COPY 的 WAL 行与 rename 成对发生（见 op_sink.hpp）
    detail::OpSink sink(pkg_name_, &stashes_);
    const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
    auto files = detail::scan_content_files(content_dir);
    // ① 分支（用户没改过 → 静默换新版）本次碰到的配置路径：**只记账，循环结束后按包
    // 聚合一行**。逐配置文件打日志在真实场景里是数量级的：`lpkg upgrade` 一次几百个包、
    // 每个包几个 /etc 条目 ⇒ 光这一句就刷几百行（pacman 对静默替换一行都不打）。
    std::vector<std::string> silently_updated_configs;

    for (const auto& f : files) {
        if (on_before_file_copy) on_before_file_copy();

        if (sigint_graceful.load()) throw LpkgException(get_string("info.sigint_aborted"));

        fs::path rel_f = f;
        if (rel_f.is_absolute()) rel_f = rel_f.relative_path();
        const fs::path src_path = content_dir / f;
        const fs::path physical_path = Config::instance().root_dir() / rel_f;

        if (!fs::exists(src_path) && !fs::is_symlink(src_path)) continue;

        fs::path parent = physical_path.parent_path();
        std::vector<fs::path> to_create;
        while (!parent.empty() && !fs::exists(parent)) {
            to_create.push_back(parent);
            if (parent == Config::instance().root_dir()) break;
            parent = parent.parent_path();
        }
        for (const auto& d : to_create | std::views::reverse) {
            ensure_dir_exists(d);
        }

        if (fs::is_symlink(src_path)) {
            fs::path link_target = fs::read_symlink(src_path);
            fs::path dest = physical_path;

            const bool is_config = f.starts_with(std::string(constants::DIR_ETC));
            // 注意 `fs::is_directory` 会跟随符号链接：`/etc/x -> /some/dir` 会被判成"目录"
            // 从而绕过配置保护（既不备份也不留 .lpkgnew，直接替换）。故符号链接一律按冲突处理。
            const bool cfg_conflict = fs::exists(physical_path) || fs::is_symlink(physical_path);
            if (is_config && cfg_conflict &&
                (fs::is_symlink(physical_path) || !fs::is_directory(physical_path))) {
                dest += std::string(constants::SUFFIX_LPKG_NEW);
                log_warning(string_format("warning.config_conflict", physical_path.string(),
                                          dest.string()));
                has_config_conflicts_ = true;
            }

            // 目录无法被符号链接替换：backup_existing_files 不备份目录，若静默
            // remove 会留下无 WAL 记录的破坏且回滚无法恢复——作为文件冲突拒绝。
            if (fs::is_directory(dest) && !fs::is_symlink(dest)) {
                throw LpkgException(string_format("error.copy_failed_rollback", f,
                                                  physical_path.string(),
                                                  get_string("error.dir_replaced_by_symlink")));
            }

            // 落位这份链接同样是"这个批次改了盘面"（改的不是配置本体而已），**必须能回滚**：
            // 裸 `fs::remove(dest)` + `fs::create_symlink(...)` 是**事务外的副作用** —— 批次被
            // 回滚后包根本没装上，/etc 上却多出一份「请审阅」链接（dest 可能是 `<配置>.lpkgnew`），
            // 盘面与 WAL 描述的世界不一致；批次前已有的那份也会被无条件删掉、回不来。
            // 故与普通文件分支共用**同一套写入层原语**（OpSink）：
            //   · 目标已存在（上一次留下的 .lpkgnew / 被链接接管的旧文件）→ 先 `backup` 把它
            //     **让开**（WAL BACKUP + 搬进 stash）。这里**不能**再单独 fs::remove 一次 ——
            //     旧的那份已经进 stash 了，删掉的是别人（它在回滚时要被 rename 回来）；
            //   · 无论哪条路径，随后先写 `NEW <dest>` 行（逆操作 = 删除该路径，对符号链接
            //     幂等），再 `create_symlink` —— 此时 dest 必然不存在，是**单次原子操作**。
            // 行序 BACKUP → NEW 不可反：逆序回滚才会"先撤新链接（NEW 的逆操作）、再还原旧那份
            // （BACKUP 的逆操作）"。
            if (fs::exists(dest) || fs::is_symlink(dest)) sink.backup(dest);
            sink.new_file(dest);
            fs::create_symlink(link_target, dest);
            struct stat st;
            if (lstat(src_path.c_str(), &st) == 0) {
                (void)lchown(dest.c_str(), st.st_uid, st.st_gid);
            }
            fsync_parent_dir(dest);
            TriggerManager::instance().check_file((fs::path("/") / f).string());
            continue;
        }

        if (fs::is_directory(src_path)) {
            // 目录条目的物理路径带尾斜杠：先规范化（尾斜杠会让下面的 lstat/chmod 解引用链接）
            const fs::path probe = strip_trailing_slash(physical_path);
            bool existed = fs::exists(probe);
            ensure_dir_exists(physical_path);
            // 落在 symlink→目录 上（`/lib64 -> usr/lib`、`/var/run -> ../run`）：内容要**穿过**
            // 链接写进真实目录，但目录条目的 uid/mode **不能**跟着穿过去 —— lchown/chmod 会跟随
            // 链接，把**别的包持有的**目录（如 /usr/lib）的属主/权限改成包内值（对普通用户直接
            // 变成不可读/不可执行）。链接目标与包内目录本就不是同一个对象，也谈不上"权限不一致"，
            // 故整个元数据块跳过。
            struct stat st;
            if (!fs::is_symlink(probe) && lstat(src_path.c_str(), &st) == 0) {
                (void)lchown(probe.c_str(), st.st_uid, st.st_gid);
                mode_t pkg_mode = st.st_mode & constants::PERM_MASK_ALL;
                if (existed) {
                    struct stat dst_st;
                    if (lstat(probe.c_str(), &dst_st) == 0) {
                        mode_t cur_mode = dst_st.st_mode & constants::PERM_MASK_ALL;
                        if (cur_mode != pkg_mode) {
                            log_warning(string_format("warning.dir_perm_mismatch", probe.string(),
                                                      static_cast<int>(cur_mode),
                                                      static_cast<int>(pkg_mode)));
                        }
                    }
                }
                (void)chmod(probe.c_str(), pkg_mode);
            }
            continue;
        }

        try {
            const bool is_config = f.starts_with(std::string(constants::DIR_ETC));
            fs::path final_dest = physical_path;

            // 盘上该路径已有**非目录**物。符号链接也算"已被占用"：`fs::is_directory` 会
            // 跟随链接，指向目录的链接会被误判成"目录"，那样配置保护整段被绕过（既不备份
            // 也不留 .lpkgnew，直接替换）—— 故 `target_is_dir` 显式排掉符号链接。
            const bool target_taken = fs::exists(physical_path) || fs::is_symlink(physical_path);
            const bool target_is_dir =
                target_taken && !fs::is_symlink(physical_path) && fs::is_directory(physical_path);

            // ── 三哈希分流（pacman add.c）────────────────────────────────────────
            // 判定表只有一份：classify_config_update()。这里只负责**备料**（三份哈希）与
            // **记录**（把"这次装进去的内容"写回 DB，供下次升级当 hash_orig）。
            auto disposition = ConfigDisposition::InstallNew;
            if (is_config) {
                const std::string logical_path = (fs::path("/") / f).string();
                const std::string hash_pkg = calculate_sha256(src_path);  // 包内那份
                // 盘上那份（hash_local）：只有**普通文件**才有可比的内容。符号链接、
                // 目录、读不到的路径 → 留空（= 无从判定 → 保守路径）。
                std::string hash_local;
                if (target_taken && !fs::is_symlink(physical_path) &&
                    fs::is_regular_file(physical_path)) {
                    try {
                        hash_local = calculate_sha256(physical_path);
                    } catch (const std::exception&) {
                        hash_local.clear();
                    }
                }
                auto& cache = Cache::instance();
                const std::string hash_orig = cache.get_conf_hash(logical_path, pkg_name_);
                if (target_taken && !target_is_dir) {
                    disposition = classify_config_update(hash_local, hash_orig, hash_pkg);
                }
                // 记录值（判定表之外的**另一半**语义，见 classify_config_update 的说明）：
                // 永远是**包内内容**的哈希 —— 记录只声明"这个包的这个版本提供过什么"。
                // 用户的文件**永不**被追认成我们的内容：追认它，下一次升级就满足
                // "盘上 == 旧记录"而把用户改过的配置**静默覆盖**（踩中底线）。而退化路径
                // （无旧记录 + 盘上已有这份配置）上那份往往正是**用户**的文件（无主文件撞
                // 包内文件时 `--overwrite` 是唯一合法入口），追认它就是把"用户那份"宣布成
                // 我们装的 —— 且这不是一次性的：记录有三处会按设计被删除（升级丢弃 /etc
                // 条目、remove_conf_hash、移除包），该路径重新归本包时"追认"会被重新武装。
                // 代价（有意为之）：老 DB 的配置在用户把 `.lpkgnew` **合并进**盘上那份之前，
                // 包每改一次配置都会再落一份**可见的** `.lpkgnew`（吵，但绝不静默）；合并后
                // 盘上 == 记录，回到正常分流。判定表本身不受影响：无记录时仍是"两份一致 →
                // 保留原文件、不一致 → 落 `.lpkgnew` + 告警"，用户可见行为不变。
                cache.set_conf_hash(logical_path, pkg_name_, hash_pkg);
            }

            // 落到目标路径本体的分支（①）/ 不落地的分支（② 保留、③ 落 .lpkgnew）
            bool install_in_place = true;
            if (is_config && target_taken && !target_is_dir) {
                switch (disposition) {
                    case ConfigDisposition::InstallNew:
                        // ① 盘上 == 上次装进去的（用户没改过）→ 静默换新版。**会真的改盘**，
                        // 所以先把盘上那份搬进 stash（BACKUP，可回滚）：批次失败时 reverse_execute
                        // 把它 rename 回原位，静默替换不留"改得动、撤不回"的缺口。
                        // （提交后它随 stash 清理掉是预期的：用户没改过，没有要保留的内容；
                        //   "保留"是移除侧 .lpkgsave 的语义。）
                        sink.backup(physical_path, "conf_replace_after_wal_" + pkg_name_);
                        // 降噪：此处**不打日志**（逐配置文件一行会是几百行）。记账，
                        // 循环结束后按包聚合一行 —— 条数与完整路径都在那一行里（见下）。
                        silently_updated_configs.push_back(physical_path.string());
                        break;
                    case ConfigDisposition::KeepLocal:
                        // ② 包本身没改这个配置（旧记录 == 新包）→ 保留用户文件，**连
                        // .lpkgnew 都不产生**：没有"新东西"要给用户审阅，产生它只会让 /etc
                        // 越堆越多。本分支不碰盘面，故无需进 WAL。
                        install_in_place = false;
                        break;
                    case ConfigDisposition::SaveLpkgnew: {
                        // ③ 三者互异（含"无从判定"）→ 新版落 .lpkgnew + 告警（原行为）
                        install_in_place = false;
                        final_dest += std::string(constants::SUFFIX_LPKG_NEW);
                        log_warning(string_format("warning.config_conflict", physical_path.string(),
                                                  final_dest.string()));
                        has_config_conflicts_ = true;
                        // "请审阅"文件也**必须能回滚**：落 .lpkgnew 同样是"这个批次改了盘面"，
                        // 只是改的不是配置本身。裸 fs::copy 是事务外的副作用 —— 批次回滚后包
                        // 没装上、配置也退回了批次前，却平白多出一份（还盖掉了上一次留下的）
                        // "请审阅"副本，盘面与 WAL 描述的世界不一致。故与普通文件分支走**同一套
                        // 写入层原语**：内容先写进 .lpkgtmp（+ xattr + 属主/权限 + fsync），再
                        // commit_copy 落位（WAL COPY）。
                        fs::path tmp_path = final_dest;
                        tmp_path += ".lpkgtmp";
                        fs::copy(
                            src_path, tmp_path,
                            fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                        copy_xattrs(src_path,
                                    tmp_path);  // 先搬到 .lpkgtmp，rename 后 xattr 随之生效
                        struct stat st;
                        if (lstat(src_path.c_str(), &st) == 0) {
                            (void)lchown(tmp_path.c_str(), st.st_uid, st.st_gid);
                            if (!S_ISLNK(st.st_mode)) {
                                (void)chmod(tmp_path.c_str(),
                                            st.st_mode & constants::PERM_MASK_ALL);
                            }
                        }
                        // fsync .lpkgtmp 后再写 WAL（断电丢内容的话，WAL 指向的就是空文件）
                        if (durable_fsync_enabled()) {
                            if (int cfd = ::open(tmp_path.c_str(), O_RDONLY); cfd >= 0) {
                                ::fsync(cfd);
                                ::close(cfd);
                            }
                        }
                        // 目标已存在（上一次留下的 .lpkgnew，用户可能还没审阅）→ 先 BACKUP 进
                        // stash 再落新的：回滚时它是"把旧那份 rename 回来"，而不是连带删掉
                        // 一份与本批次无关、用户尚未处理的审阅文件（顺序也不可反：BACKUP 行
                        // 必须先于 COPY 行，逆序回滚才会"先撤新那份、再还原旧那份"）。
                        if (fs::exists(final_dest) || fs::is_symlink(final_dest))
                            sink.backup(final_dest);
                        sink.commit_copy(tmp_path, final_dest);
                        break;
                    }
                }
            }

            if (install_in_place) {
                fs::path tmp_path = final_dest;
                tmp_path += ".lpkgtmp";
                fs::copy(src_path, tmp_path,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                copy_xattrs(src_path, tmp_path);  // 先搬到 .lpkgtmp，rename 后 xattr 随之生效

                struct stat st;
                if (lstat(src_path.c_str(), &st) == 0) {
                    (void)lchown(tmp_path.c_str(), st.st_uid, st.st_gid);
                    if (!S_ISLNK(st.st_mode)) {
                        (void)chmod(tmp_path.c_str(), st.st_mode & constants::PERM_MASK_ALL);
                    }
                }
                // fsync .lpkgtmp 后再写 WAL
                if (durable_fsync_enabled()) {
                    int fd = ::open(tmp_path.c_str(), O_RDONLY);
                    if (fd >= 0) {
                        ::fsync(fd);
                        ::close(fd);
                    }
                }
                // WAL: COPY <tmp> → <dst> (write-ahead: WAL 先于 rename)
                // （断点位于 write-ahead 窗口内，只能在 sink 里命中）
                sink.commit_copy(tmp_path, final_dest, "copy_after_wal_" + pkg_name_);
            }

            TriggerManager::instance().check_file((fs::path("/") / f).string());
        } catch (const std::exception& e) {
            throw LpkgException(
                string_format("error.copy_failed_rollback", f, physical_path.string(), e.what()));
        }
    }

    // ① 分支的降噪出口：每包**一行**（条数 + 完整路径清单）。信息一点没丢 —— 用户仍然
    // 查得到"到底哪些配置被静默换了"，只是不再一个文件占一行。放在循环之后（而不是每
    // 次碰到就打印）是聚合的前提；批次中途失败时这一行不会打，但那时整批回滚、盘面回到
    // 批次前，没有"静默替换"可言。
    if (!silently_updated_configs.empty()) {
        std::string joined;
        for (const auto& p : silently_updated_configs) {
            if (!joined.empty()) joined += ", ";
            joined += p;
        }
        log_info(
            string_format("info.config_updated_batch", silently_updated_configs.size(), joined));
    }
    if (has_config_conflicts_) log_warning(get_string("info.config_review_reminder"));
}

void InstallationTask::register_package()
{
    auto& cache = Cache::instance();

    if (!old_version_to_replace_.empty()) {
        const fs::path old_dep_file = Config::instance().dep_dir() / pkg_name_;
        if (fs::exists(old_dep_file)) {
            std::ifstream f(old_dep_file);
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty()) {
                    std::stringstream ss(line);
                    std::string dn;
                    if (ss >> dn) cache.remove_reverse_dep(dn, pkg_name_);
                }
            }
        }
        for (const auto& cap : cache.get_package_provides(pkg_name_)) {
            cache.remove_provider(cap, pkg_name_);
        }
        // 旧 needed_so 文件不在此处删除——由下方的 write_string_file_wal 备份后
        // 覆盖（回滚时可恢复旧版 needed_so 元数据）。
    }

    std::unordered_set<std::string> dep_entries;
    for (const auto& d : deps_) {
        dep_entries.insert(d);
        std::string name = d;
        if (const auto pos = d.find_first_of(" \t<>="); pos != std::string::npos)
            name = d.substr(0, pos);
        cache.add_reverse_dep(name, pkg_name_);
    }

    for (const auto& soname : needed_so_) {
        auto providers = cache.get_providers(soname);
        for (const auto& prov_pkg : providers) {
            if (prov_pkg != pkg_name_ && cache.is_installed(prov_pkg)) {
                // 只记反向依赖（移除时阻止误删），不把 SONAME 提供者写进 deps/：
                // deps/ 只放命名依赖。写入会污染 deps/ 且依赖安装顺序
                // （is_installed 检查），让 autoremove 对提供者误判。
                cache.add_reverse_dep(prov_pkg, pkg_name_);
            }
        }
    }

    std::vector<std::string> sorted_deps(dep_entries.begin(), dep_entries.end());
    std::sort(sorted_deps.begin(), sorted_deps.end());
    // WAL → 原子写（write-ahead：已存在的旧文件先备份，升级回滚可恢复旧版元数据）
    {
        std::string deps_content;
        for (const auto& entry : sorted_deps) {
            deps_content += entry;
            deps_content += constants::NL;
        }
        wal::write_string_file_wal((Config::instance().dep_dir() / pkg_name_).string(),
                                   deps_content, pkg_name_ + ":installed",
                                   /*create_empty=*/true);
    }

    {
        std::string nso_content;
        for (const auto& sn : needed_so_) {
            nso_content += sn;
            nso_content += constants::NL;
        }
        // 空内容 → DBRM 备份并删除旧文件（回滚恢复）；非空 → DBNEW/DB + 备份
        wal::write_string_file_wal((Config::instance().needed_so_dir() / pkg_name_).string(),
                                   nso_content, pkg_name_ + ":installed");
    }

    const fs::path content_dir = tmp_pkg_dir_ / constants::DIR_CONTENT;
    for (const auto& f : detail::scan_content_files(content_dir)) {
        // 目录允许共享所有权（多个包安装到同一目录是正常的，如 /usr/bin/），
        // 走 add_dir_owner 累加所有者；普通文件强制单一所有者，冲突由
        // add_file_owner 的 error.file_already_owned 检测。
        if (f.ends_with('/'))
            cache.add_dir_owner((fs::path("/") / f).string(), pkg_name_);
        else
            cache.add_file_owner((fs::path("/") / f).string(), pkg_name_);
    }

    const fs::path man_path =
        Config::instance().docs_dir() / (pkg_name_ + std::string(constants::SUFFIX_MAN));
    // 空 man → DBRM 备份并删除旧文件（回滚恢复）；非空 → DBNEW/DB + 备份
    wal::write_string_file_wal(man_path.string(), man_content_, pkg_name_ + ":installed");

    for (const auto& cap : provides_) {
        cache.add_provider(cap, pkg_name_);
    }
    cache.add_installed(pkg_name_, actual_version_, explicit_install_);
}

/**
 * 把本包归档 hooks/ 里的脚本落进 hooks_dir/<pkg>/，并记下文件名（提交后据此剪枝）。
 *
 * **这里不执行任何钩子**（原实现在末尾直接 `run_hook(POSTINST_SH)`）。执行时机只有一个：
 * 批次**提交之后**的 finish_committed_batch() —— 批次是"全或无"，回滚能撤销文件与 DB，
 * 却撤不回钩子的副作用（钩子以 root 跑 systemd-sysusers / tmpfiles --create / useradd，
 * 改的是系统状态）：留在批次内 = 已经回滚掉的批次在系统上留下撤不掉的痕迹。上游 libalpm
 * 同理：POST hook 整段在"提交/中断"判定之后，提交失败则一个 hook 都不跑。
 *
 * **脚本文件本身走写入层原语（OpSink），因而在事务内可回滚**。原先直接
 * `fs::copy(..., overwrite_existing)` 是错的：hook 按**包名**存在 hooks_dir/<pkg>/ 下，
 * 新版本覆盖旧版本即永久丢失 —— 批次一旦回滚，包体回到旧版本而钩子是新版本的内容，
 * 之后 remove/upgrade 跑的是**错版本**的脚本（上游 libalpm 把脚本按版本存在 local DB 的包
 * 目录里：新版本进新目录、旧目录提交末尾才删，故不存在"旧脚本被覆盖"）。这里改为
 * BACKUP（旧脚本进 stash，回滚原样搬回）+ COPY（新脚本 .lpkgtmp → fsync → rename）。
 * 顺带修掉 `fs::copy(overwrite_existing)` 落在**符号链接**上时会写穿链接、改掉链接目标的
 * 隐患（rename 不跟随末段链接）。
 */
void InstallationTask::install_hook_files()
{
    const fs::path hook_src = tmp_pkg_dir_ / constants::DIR_HOOKS;
    if (!fs::exists(hook_src) || !fs::is_directory(hook_src)) return;

    detail::OpSink sink(pkg_name_, &stashes_);
    const fs::path dest_dir = Config::instance().hooks_dir() / pkg_name_;
    if (!fs::exists(dest_dir)) {
        // 目录本体也进事务：NEW_DIR 的逆操作会删掉它，失败批次不在 hooks_dir 下留空壳
        sink.new_dir(dest_dir);
        ensure_dir_exists(dest_dir);
    }
    for (const auto& entry : fs::directory_iterator(hook_src)) {
        if (!entry.is_regular_file()) continue;
        const fs::path dest = dest_dir / entry.path().filename();

        // hooks_dir/<pkg>/ 下的同名**实体目录**：不是本包能接管的东西。搬进 stash 会在提交后
        // 被 remove_all 连带删掉整棵树（ARCH §3.6：无主内容一律不碰），所以宁可直接失败、
        // 让批次回滚 —— 这也与原实现一致（fs::copy 到目录目标会失败）。
        std::error_code dec;
        if (fs::is_directory(dest, dec) && !fs::is_symlink(dest)) {
            throw LpkgException(string_format("error.hook_path_is_dir", dest.string()));
        }
        // 旧版本的同一个 hook（含符号链接）先搬进 stash：批次回滚时原样搬回
        if (fs::exists(dest) || fs::is_symlink(dest)) sink.backup(dest);

        fs::path tmp = dest;
        tmp += ".lpkgtmp";
        fs::copy(entry.path(), tmp, fs::copy_options::overwrite_existing);
        copy_xattrs(entry.path(), tmp);  // 先搬到 .lpkgtmp，rename 后 xattr 随之生效
        fs::permissions(tmp, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add);
        // fsync .lpkgtmp 后再写 WAL（与包内容同一纪律）
        if (durable_fsync_enabled()) {
            if (int fd = ::open(tmp.c_str(), O_RDONLY); fd >= 0) {
                ::fsync(fd);
                ::close(fd);
            }
        }
        // WAL: COPY <tmp> → <dst>（write-ahead：WAL 先于 rename）
        sink.commit_copy(tmp, dest);
        hook_files_.push_back(dest.filename().string());  // 提交后据此剪枝陈旧 hook
    }
}

std::vector<DependencyInfo> InstallationTask::parse_deps() const
{
    return detail::parse_dep_strings(deps_);
}
