/**
 * 安装事务的骨架 + 文件冲突判定引擎。
 *
 * 本文件负责：
 *   · `InstallationTask` 的生命周期 —— 构造、`run()` 对三趟的编排（BEGIN/COMMIT/END 的
 *     WAL 行、SIGINT 早退、异常 → 包级回滚标记）、元数据校验与依赖校验、`parse_deps()`；
 *   · 冲突判定引擎 —— `collect_content_conflicts()`（六条语义只有一份）与它绑定的两种
 *     "世界"（逐包的真实视图 `real_conflict_view`、整批预检的模拟视图），
 *     以及两个入口 `check_for_file_conflicts()` / `check_batch_file_conflicts()`。
 *
 * 按趟拆出去的三个 TU（本文件原本是它们加上上面这些的总和）：
 *   · `installation_task_letgo.cpp`    —— 让开趟（②）：`backup_existing_files` /
 * `remove_obsolete_files` · `installation_task_copy.cpp`     —— 写入趟（③）：`copy_package_files`
 *   · `installation_task_register.cpp` —— 注册趟（④）：`commit_without_file_ops` /
 * `register_package` / `install_hook_files`
 *
 * ⚠️ 拆分是**纯代码搬移**：每一段都是原文件的逐字副本，一行逻辑都没改（`Makefile` 用
 *    wildcard 收集 `main/src/pkg` 下的全部 `.cpp`，新 TU 自动纳入）。
 *    此处刻意整块沿用原文件的 include 列表 —— 按符号逐个收敛 include 是另一件事，
 *    混在纯搬移里会让"没改逻辑"这个论证失去意义。
 */

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
#include <optional>
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

    // 版本号是**不可信**输入：本地 .lpkg 的版本取自归档 metadata.json、远端索引与 CLI
    // `pkg:版本` 同理（package_manager 读出后放进 InstallPlan::actual_version）。而它被原样
    // 拼进下面（以及 COMMIT/END/ROLLBACK）的 WAL 行，WAL 又是**按行、空格分帧**的文本协议
    // —— 一个 `\n` 就能把 BEGIN 行截断，第二行成为**攻击者可控的合法 WAL 行**。回滚
    // （batch_rollback）与 `lpkg rec`（recover_packages）都会把那些行交给 reverse_execute，
    // 而该函数**没有任何路径 confinement**：它按 WAL 里的绝对路径直接 rename/remove/chmod
    // —— 一条 `NEW /etc/sudoers` 行就是一次任意删除。
    //
    // 校验点放在这里而不是 download_and_verify_package()：那里对**本地包**提前 return
    // （archive_path_ 直接取本地路径，版本号根本不参与拼路径），够不到；这里是所有来源的
    // 汇合点，COMMIT/END/ROLLBACK 行又都用同一个 actual_version_，一个点覆盖全部出口。
    // 位置必须在 BEGIN 行**之前**：BEGIN 是 WAL 里"本包已开始"的标记，写出去就晚了。
    // （那一处原有的同名校验**保留**：它挡的是"版本号当路径分量拼进下载 URL 与归档落点"，
    //   与这里挡的"进 WAL"是两件事，都不是多余的。）
    if (!is_safe_path_component(actual_version_)) {
        throw LpkgException(
            string_format("error.unsafe_path_component", "version", actual_version_));
    }

    // WAL: BEGIN <pkg> <ver> + fsync
    wal::log_wal_line("BEGIN " + pkg_name_ + " " + actual_version_);

    // 断点：begin 之后，可用于模拟安装刚开始就崩溃
    BreakpointManager::instance().hit("install_after_begin_" + pkg_name_);

    try {
        // 第二阶段：让开 —— **先移除旧版本的全部触碰面**（②a 归档条目 / ②b DB 旧键）：
        //   · backup_existing_files：新版本会碰的每个路径（挡路物搬进 stash / `/etc` 配置
        //     搬进 stash 待判定 / 该 `.lpkgsave` 的整树改名 / 建目录）；
        //   · remove_obsolete_files：旧版本有、新版本不再提供的触碰面（搬进 stash / 撤所有权
        //     / 空目录 rmdir）。
        // 第③步（改执行顺序）把第二步从原来的位置（写入**之后**、`commit_without_file_ops`
        // 里）挪到这里 —— pacman 也是"先删旧包文件（含目录）、再解压新包"，写入阶段因此
        // 看到的是**已经让开的盘面**（类型转换不再是特例）。挪动只改时序、不改语义：
        // 废弃清除的判据、WAL 行、回滚方式一字未变（见 remove_obsolete_files 的说明）。
        backup_existing_files();
        remove_obsolete_files();

        // 第三阶段：写入新内容（路径已被让开）
        copy_package_files();

        // 第四阶段：注册——数据库修改 + hooks 落位
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
        // 用户给的路径也是外部输入：环在这里应当报"本地包不存在"（点名的 LpkgException），
        // 而不是让 std::filesystem 的原始异常穿出去
        if (!exists_follow(local_package_path_))
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

namespace
{

/** 计划中是否有包提供该名字（能力名≠包名时，libsolv 已按能力解析过） */
bool plan_provides(const InstallContext& ctx, const std::string& name)
{
    for (const auto& [pn, plan_pkg] : ctx.plan)
        for (const auto& prov : plan_pkg.provides)
            if (prov == name) return true;
    return false;
}

/** 是否已是本批次的目标（libsolv 会处理） */
bool is_planned_target(const InstallContext& ctx, const std::string& name)
{
    for (const auto& [tn, tv] : ctx.targets)
        if (tn == name) return true;
    return false;
}

/**
 * 依赖是否**已被盘面满足**：真实包已安装且满足约束，或名字是能力（无同名真实包，
 * 但已有包注册提供该能力）→ 视为满足。能力（多为无版本 SONAME）不带版本，约束对之无意义。
 */
bool dep_satisfied_on_disk(const DependencyInfo& dep)
{
    const std::string installed_ver = Cache::instance().get_installed_version(dep.name);
    if (!installed_ver.empty()) {
        return dep.constraints.empty() || version_satisfies_all(installed_ver, dep.constraints);
    }
    return !Cache::instance().get_providers(dep.name).empty();
}

/**
 * 对一个**未被满足**的依赖做最终裁定：计划中已有同名真实包 / 由计划中某包提供 /
 * 已是目标（libsolv 会处理）→ 都算处理完，返回。
 *
 * 命名能力那一支要把能力名记入 `ctx.targets`：提供者的**真实元数据可能与本索引不一致**
 * （如 DynamicProviderChange 场景），供元数据验证触发的重解（i=0 重启）重新拉取正确提供者。
 * **绝不在此重解**：中途改写 order 且不重置批次游标会导致依赖者先于提供者安装、产生
 * 重复计划项（曾因此乱序）。
 *
 * 三者皆非 = solver/plan 不一致：依赖未安装、不在计划、也不由计划包提供。元数据验证
 * （install_packages/upgrade_packages）在批次开始前已按真实元数据重解并 i=0 重启，这里
 * 不应再发现新依赖 → 显式报错，整批回滚。
 */
void resolve_unmet_dep(InstallContext& ctx, const std::string& dep_name,
                       const std::string& pkg_name)
{
    if (ctx.plan.contains(dep_name)) return;  // 计划中已有同名真实包

    if (plan_provides(ctx, dep_name)) {
        if (!is_planned_target(ctx, dep_name))
            ctx.targets.emplace_back(dep_name, std::string(constants::VER_LATEST));
        return;
    }

    if (is_planned_target(ctx, dep_name)) return;  // 已是目标（libsolv 会处理）

    throw LpkgException(string_format("error.dep_missing_from_plan", dep_name, pkg_name));
}

/**
 * 已安装的包中有该 SONAME 的提供者，且该提供者**不在本批次计划里** —— 在计划里的那份
 * 会被本批次换成另一版本，不能算作"盘上已满足"。
 */
bool installed_provider_available(const InstallContext& ctx, const std::string& soname)
{
    for (const auto& p : Cache::instance().get_providers(soname)) {
        if (Cache::instance().is_installed(p) && !ctx.plan.contains(p)) return true;
    }
    return false;
}

/**
 * 仓库里该 SONAME 的提供者，且**未被"认领"** —— 提供者已安装、或在本批次计划中以另一版本
 * 出现时，其版本已被锁定，仓库里其它版本提供此 SONAME 不能算数（否则依赖者声明的版本
 * 约束/已装版本会被仓库旧版本悄悄绕过）。
 */
bool unclaimed_repo_provider(const InstallContext& ctx, const std::string& soname)
{
    auto prov_pkg = ctx.repo.find_provider(soname);
    if (!prov_pkg) return false;
    if (ctx.plan.contains(prov_pkg->name) || Cache::instance().is_installed(prov_pkg->name))
        return false;
    for (const auto& prov : prov_pkg->provides) {
        if (prov == soname) return true;
    }
    return false;
}

/**
 * 单个 SONAME 是否已满足：计划中有包提供 / 已装的提供者（且不随本批被换掉）/ 仓库里
 * 未认领的提供者 / `--use-system-soname` 下系统 /usr/lib 已有该 `.so`。
 *
 * `--use-system-soname`（如 backup 的旧 SONAME）配合 farm 的 ABI 过渡机制：旧二进制在
 * 过渡期加载旧 .so，新构建用新 .so。
 */
bool soname_satisfied(const InstallContext& ctx, const std::string& soname)
{
    if (plan_provides(ctx, soname)) return true;
    if (installed_provider_available(ctx, soname)) return true;
    if (unclaimed_repo_provider(ctx, soname)) return true;

    return Config::instance().use_system_soname_mode() &&
           Config::instance().has_system_soname(soname);
}

}  // namespace

/**
 * 校验依赖与 needed_so 是否都被满足（不碰盘、不改 DB，只读）。
 *
 * 主体只剩两条循环：每个依赖交给 `dep_satisfied_on_disk` / `resolve_unmet_dep`，
 * 每个 SONAME 交给 `soname_satisfied` —— 判定与消息都在那几个小函数里，
 * 原先 8 层嵌套现在最深 4 层。
 */
void InstallationTask::ensure_dependencies_satisfied(InstallContext& ctx)
{
    if (Config::instance().no_deps_mode()) return;
    auto actual_deps = detail::parse_dep_strings(deps_);
    if (actual_deps.empty()) return;

    log_info(string_format("info.checking_deps", pkg_name_));

    for (const auto& dep : actual_deps) {
        if (dep_satisfied_on_disk(dep)) continue;
        resolve_unmet_dep(ctx, dep.name, pkg_name_);
    }

    if (!needed_so_.empty()) {
        for (const auto& soname : needed_so_) {
            if (soname_satisfied(ctx, soname)) continue;

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
namespace
{
/**
 * 一个逻辑路径"此刻"的盘面状态（lstat 语义：符号链接算**存在**但**不是目录**）。
 *
 * 判之前一律 `strip_trailing_slash`：尾斜杠会把末尾的符号链接**解引用**
 * （pacman 为此专门写了 `llstat()`，见 FS#51377 / commit 16b91f79），`is_symlink` 恒假、
 * "别动 symlink→目录"的守卫集体失效。`root / <绝对路径>` 会**丢弃**左值（archive.cpp
 * 里踩过同一个坑），所以这里先把逻辑路径转成相对路径再拼。
 *
 * ⚠️ **判据一律走不抛谓词族**（2026-09-26 修）：这两个字段**对每个归档条目**都会被求值
 * （install/upgrade 的批次预检入口），而老的写法
 * `fs::exists(phys, ec) || fs::is_symlink(phys)` 在**中间段**是符号链接环时**必抛**：
 * `fs::exists` 带 ec 对 ELOOP 返回 false（不抛），于是 `||` **必然**求值那个抛型的
 * `fs::is_symlink`，而它对 `"self/x"`（中间段成环）抛 code=40 —— 抛出来的还是 **raw
 * `filesystem_error`**（不是 LpkgException、无 l10n 文案），整批中止、异常穿透到 CLI。
 * 触发形状毫不特殊：盘上 `/usr/share/pylib -> pylib`（自环）**加**包内
 * `content/usr/share/pylib/real.txt`（普通文件）就够了。
 * （实测细节见 base/utils.hpp 那族谓词的说明 —— `fs::is_symlink` 只在**末段**是环时不抛。）
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
    PathProbe p;
    p.exists = exists_no_follow(phys);
    p.is_dir = is_real_directory(phys);
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
 * "`dir_bare` 下的整棵子树是否只属于 pkg 或本批次正在升级的包"（pacman 的
 * `dir_belongsto_pkgs`）—— 用于放行"盘上是真目录、新条目是文件/符号链接"（dir → 非目录）
 * 的接管。**不加视图成员**：判据只需要 `owners` 与 `all_upgrading` 两个既有成员，
 * 由调用处直接传入，两个视图（真实 / 整批预检的模拟世界）自动共用同一段遍历逻辑。
 *
 * 为什么要"整棵树"而不是"这个目录路径归我"：目录可以被多个包共享（目录键是**累加**持有者
 * 的），把一棵被共享的目录整树让开（搬进 stash / 改名 .lpkgsave）会**连带搬走别人的文件**。
 * pacman 为此在 `conflict.c` 里写了 `dir_belongsto_pkgs`：遍历目录、逐条查归属，任何一条
 * 不属于"本包 ∪ 本次要移除/升级的包"就判否。
 *
 * 走**盘面**取条目（与 pacman 同构：无主文件在盘上就是盘上），归属与"是否在升级"则分别问
 * 两个回调 —— 真实视图查 Cache、整批预检查模拟世界，两种"世界"共用这一段遍历逻辑。
 */
/// `out_first_foreign`（可空）：判否时写入**第一个不属于我们的条目**的持有者名；该条目无主时
/// 写入空串。供调用方在报错里点名**真实**冲突源，而不是含糊地报"这个目录归本包"。
bool dir_tree_entirely_ours(
    const fs::path& root, const std::string& dir_bare, const std::string& pkg,
    const std::function<std::vector<std::string>(const std::string&)>& owners_of,
    const std::function<bool(const std::vector<std::string>&)>& is_upgrading,
    std::string* out_first_foreign = nullptr)
{
    fs::path rel = dir_bare;
    if (rel.is_absolute()) rel = rel.relative_path();
    const fs::path phys = strip_trailing_slash(root / rel);
    std::error_code ec;
    if (!fs::is_directory(phys, ec)) return false;  // 不是真目录 → 该判据不适用

    for (fs::recursive_directory_iterator
             it(phys, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) return false;
        // 目录键在 DB 里**带尾斜杠**；符号链接一律算非目录（与 probe_path/§3.6.1 同口径）
        const bool real_dir = !it->is_symlink(ec) && it->is_directory(ec);
        if (ec) return false;
        // **`lexically_relative` 而不是 `fs::relative`**（2026-09-26 修）：后者会**解析
        // 符号链接（含末段）**，于是"逐条查归属"查的是**链接目标**的键，而不是这个名字的。
        // 两个后果都是实的：① 树里有一个**别的包持有**的链接、其解析目标归本包 ⇒ 判"整树
        // 都是我们的" ⇒ 整树搬进 stash ⇒ 提交后 stash 被 `remove_all` ⇒ 别人那份文件**永久
        // 消失**，而它的 DB 归属还在原位（所有权脱节 —— 正是本函数存在的唯一意义）；
        // ② 中间段是链接（usr-merge `/bin`→`usr/bin`、`/lib64`）⇒ 键查不到 ⇒ 判"无主" ⇒
        // **拒绝**本可放行的 dir→非目录 升级。同仓库 `scan/scanner.cpp:93` 早已为同一问题
        // 改用 `lexically_relative` 并写明"假孤儿"的成因。
        const fs::path rel_entry = it->path().lexically_relative(root);
        if (rel_entry.empty()) return false;  // 词法上也对不上 → 判不了 → 拒绝（保守）
        fs::path logical = fs::path("/") / rel_entry;
        std::string key = logical.generic_string();
        if (real_dir) key += "/";

        const std::vector<std::string> holders = owners_of(key);
        if (holders.empty()) {
            if (out_first_foreign) out_first_foreign->clear();  // 无主 → 渲染成"未知（手动文件）"
            return false;
        }
        for (const auto& h : holders) {
            if (h == pkg) continue;
            if (!is_upgrading({h})) {
                if (out_first_foreign) *out_first_foreign = h;
                return false;
            }
        }
    }
    return true;
}

/**
 * 文件冲突判定核心（pacman conflict.c 的安装侧语义）。六条必须逐字保持：
 *
 *   ① 自持短路：该路径由**本包**持有 → 不判冲突（重装 / 升级自己）；
 *   ② `ours` 豁免是**方向性**的：只豁免"归档**目录**条目接管本包旧版本的**文件/符号链接**"
 *      （文件→目录升级，TODO E4）；反方向 dir→文件 **永不**豁免。
 *
 *      理由**不是**"pacman 也不允许"（订正 2026-09-25，原文如此写、引证错层）：pacman 的
 *      **冲突层**其实允许它 —— `conflict.c` 的 `_alpm_db_find_fileconflicts` 里有
 *      "check if all files of the dir belong to the installed pkg"，当目录里外全属于
 *      `dbpkg ∪ rem`（本包已装版本 ∪ 本次要移除的包）时 `resolved_conflict = 1`，
 *      **不报冲突**；`add.c` 那条 "extract: not overwriting dir with file"（case 5）是
 *      **解压层**的无条件拒绝，pacman 之所以撞不到它，是因为它**先删旧包文件（含目录）、
 *      再解压新包**，解压时那个路径已不存在。
 *
 *      lpkg 撞得到，是**阶段顺序**决定的：旧目录由 `commit_without_file_ops()`（第 ④ 步）
 *      清理，而新文件在 `copy_package_files()`（第 ③ 步）就要落位 —— 此时旧目录还在盘上，
 *      `rename(.lpkgtmp → <该路径>)` 撞 EISDIR，回滚还会把已清空的目录 rmdir 掉。
 *      所以这里必须前置拒绝、整批中止。**要支持对称语义得把"旧目录清除"挪到拷贝之前**
 *      （动事务内阶段顺序），不是改这一处判定就够的。
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
                // **dir → 非目录**（归档是文件/符号链接、盘上是真目录）对称放行，但要满足
                // pacman 的 `dir_belongsto_pkgs`：**整棵目录树**都归本包或本批次正在升级的包。
                // 为什么必须"整棵树"：目录键是**累加**持有者的（目录可被多包共享），只凭
                // "这个目录路径归我"就整树让开，会连带搬走别的包的文件。
                std::string
                    foreign_holder;  // 判否时 = 树里第一个"不是我们的"条目的持有者（无主则空）
                const bool dir_takeover =
                    disk.is_dir && ours &&
                    dir_tree_entirely_ours(Config::instance().root_dir(), bare, pkg_name,
                                           view.owners, view.all_upgrading, &foreign_holder);
                const bool ours_exempts = ours && (entry_is_dir || dir_takeover);
                // `--overwrite` 仍**不**豁免 dir→非目录（pacman 的 add.c 对
                // "not overwriting dir with file" 也是无条件拒绝；dir → 非目录只能靠
                // "整棵树都是我们自己的"这条正路走通，不能靠开关强推）。
                const bool force_exempts = entry_is_dir;
                if (!ours_exempts &&
                    !(force_exempts && Config::instance().overwrite_allows(bare))) {
                    auto holders = view.owners(bare);
                    if (holders.empty()) holders = view.owners(bare + "/");
                    // 报错要点名**真实**冲突源。盘上是本包的目录、但树里有别人的/无主的条目时，
                    // 直接报那个条目的持有者；只报"本包持有这个目录"会把人引去查一个不存在的
                    // 冲突源（实测：报 "owned by other packages: owned by package <自己>"）。
                    if (disk.is_dir && ours) {
                        conflicts[path_str] = foreign_holder.empty()
                                                  ? get_string("error.unknown_manual_file")
                                                  : foreign_holder;
                    } else {
                        conflicts[path_str] = holders.empty()
                                                  ? get_string("error.unknown_manual_file")
                                                  : holders.front();
                    }
                }
                // **放行之后必须跳出**：`dir_takeover` 判真说明这次是"盘上是本包的目录、归档是
                // 文件/符号链接"，而**目录在 DB 里的键带尾斜杠**（`<bare>/`）。放行后若继续往下
                // 走尾部的"路径级"检查，那里按 bare（无尾斜杠）查归属 → 什么都查不到，会把刚
                // 放行的接管又判成"无主手工文件"重新拒掉（实测：报 "owned by package unknown
                // (manual file)"，整条升级路径依旧装不上；`/etc` 的 save_config 分支也因此
                // 永远不可达）。
                if (dir_takeover) continue;
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

std::vector<DependencyInfo> InstallationTask::parse_deps() const
{
    return detail::parse_dep_strings(deps_);
}
