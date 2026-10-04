#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "op_sink.hpp"
#include "repo/repository.hpp"

/// 安装计划：已解析完毕、待安装的包
struct InstallPlan {
    std::string name, actual_version, sha256;
    bool is_explicit = false;  ///< 用户显式指定安装
    std::filesystem::path local_path;
    std::vector<DependencyInfo> dependencies;
    std::vector<std::string> provides;         // 虚拟 provider
    std::vector<std::string> provides_soname;  // 导出的 SONAME
    std::vector<std::string> needed_so;        // 需要的 SONAME
    bool force_reinstall = false;              ///< 强制重新安装
    bool metadata_verified = false;            ///< 是否已验证元数据
    /**
     * 该包的 content/ 是否已由**整批文件冲突预检**下载解压到标准临时目录
     * （check_batch_file_conflicts 置位）。
     *
     * 预检必须拿到 content 清单才能判定，而事务内的 prepare() 又要再解压一遍 —— 这个标记
     * 让后者复用同一份解压产物（不重复 tar 解压）。**计划在批次内不再变动**（元数据不一致
     * 一律硬报错，见 package_manager.cpp 的 verify_package_metadata），所以这个标记与它所属的
     * InstallPlan 一样，不会在批次中途指向"另一个版本的解压产物"。
     */
    bool content_ready = false;
};

/**
 * 本批**不该处理**这个包：盘上已经是目标版本，且没有 `--force`。
 *
 * **判据只有这一份**：四个阶段（下载 / 解压 / 文件冲突预检 / 安装）与 `InstallationTask::run()`
 * 的早退分支必须同判 —— 各写一份就会漂移（一个循环下载/解压了另一个循环跳过的包，冲突预检
 * 还会去扫一个根本不存在的内容目录）。`p.force_reinstall` 已在 `resolve_with_solver` 里限定为
 * "`--force` 且本包是显式目标"（见 `install_common.cpp`），所以这里不必再判 `is_explicit`。
 */
inline bool plan_member_skipped(bool force_reinstall, const std::string& installed_ver,
                                const std::string& target_ver)
{
    return !force_reinstall && !installed_ver.empty() && installed_ver == target_ver;
}

/// `InstallPlan` 便捷形式（三个阶段循环用）。
inline bool plan_member_skipped(const InstallPlan& p, const std::string& installed_ver)
{
    return plan_member_skipped(p.force_reinstall, installed_ver, p.actual_version);
}

/// 递归安装事务的共享上下文
struct InstallContext {
    Repository& repo;
    std::map<std::string, InstallPlan>& plan;
    std::vector<std::string>& install_order;
    std::map<std::string, std::filesystem::path>& local_candidates;
    std::vector<std::pair<std::string, std::string>>& targets;
    bool force_reinstall;
    bool top_level;                                   ///< 是否为顶级调用（非递归子调用）
    std::unordered_set<std::string> installed_set{};  ///< 本事务中已成功安装的包（O(1) 成员检查）
};

namespace detail
{
/**
 * **计划版本复核**：`dep`（带约束的依赖）若出现在 `plan` 里，其**计划版本**必须满足该约束
 * —— 按 **lpkg 语义**（`version_satisfies_all`），不是 libsolv 的匹配语义。不满足则抛
 * `error.dep_version_mismatch`（点名依赖名、计划版本、发起包）。**唯一实现**。
 *
 * 为什么它还在（2026-10-03 起是**纵深防御**）：在 8.0.0 把版本语义换成 rpm EVR 之前，libsolv
 * 的 EVR 匹配把"要求侧缺 release"当通配，会选出 lpkg 认为不满足的版本，这道复核是唯一拦得住
 * 的地方（当时的用例只能借"求解器产出违规计划"来触达它）。8.0.0 之后版本串原样进池、求解器与
 * 安装期共用同一份判据（见 `vercmp/version.hpp`）⇒ 正常路径上这里
 * **不会触发**。留着的理由是：求解器选版本与 lpkg 判据是**两条独立实现路径**，任一侧将来
 * 改动（换编码、升 libsolv、改 `version_satisfies`）都可能让它们重新分叉，而分叉的代价是
 * "装出坏系统"。⇒ 它的用例**直接喂手搓的计划**（`tests/unit/test_plan_dep_version_check.cpp`），
 * 不再假装靠求解器触达（那种用例在修好之后是假绿）。
 */
void check_planned_dep_version(const DependencyInfo& dep,
                               const std::map<std::string, InstallPlan>& plan,
                               const std::string& pkg_name);
}  // namespace detail

class InstallationTask
{
public:
    InstallationTask(std::string pkg_name, std::string version, bool explicit_install,
                     std::string old_version_to_replace = "",
                     std::filesystem::path local_package_path = "", std::string expected_hash = "",
                     bool force_reinstall = false);

    /// 主入口：执行安装任务，ctx 用于递归依赖发现
    void run(InstallContext* ctx = nullptr);

    // --- 元数据验证模式（公开） ---
    /** 下载并校验包（本地 `.lpkg` 直接用给定路径）。成功路径**不打日志**（进度行已点名包）。 */
    void download_and_verify_package();
    void extract_and_validate_package();

    // --- 测试用（公开） ---
    void copy_package_files();
    void rollback_files();  ///< 包级文件回滚（含 RESTORE_* WAL 审计）
    /// 获取备份 stash 目录列表（供批次成功后 remove_all 清理）
    const std::vector<std::filesystem::path>& get_stashes() const
    {
        return stashes_;
    }

    // 元数据验证调用者的访问器
    const std::vector<std::string>& deps() const
    {
        return deps_;
    }
    const std::vector<std::string>& provides() const
    {
        return provides_;
    }
    const std::vector<std::string>& provides_soname() const
    {
        return provides_soname_;
    }
    const std::filesystem::path& archive_path() const
    {
        return archive_path_;
    }
    const std::filesystem::path& tmp_pkg_dir() const
    {
        return tmp_pkg_dir_;
    }
    void set_tmp_dir(const std::filesystem::path& p)
    {
        tmp_pkg_dir_ = p;
    }

    /**
     * 本次安装写入 hooks_dir/<pkg>/ 的文件名（供**提交后**剪枝新版本已不再提供的 hook）。
     *
     * ⚠ 空返回有**两种**含义完全不同的来源，调用方不得混同：
     *   · `did_process() == true`  → 归档的 hooks/ 里确实没有可装的普通文件
     *     （"新版本没有 hooks"）→ 提交后该把 hooks_dir/<pkg>/ 整目录剪掉；
     *   · `did_process() == false` → **本包压根没被处理**（run() 在"已装同版本"处早退）
     *     → 此刻这个空集什么都不代表，**不许**当成"新版本没有 hooks"去剪目录
     *     （历史缺陷：install 批次里一个已装同版本的成员被求解器以 REINSTALL 步骤带进计划，
     *       它的 hook 目录被 `fs::remove_all` 静默删掉，而包本身仍是已装状态）。
     * 因此调用方一律先用 did_process() 判定，再决定记不记账。
     */
    const std::vector<std::string>& get_hook_files() const
    {
        return hook_files_;
    }

    /**
     * 本次 run() 是否真的处理过本包（"本包确实被处理过"的判据）。
     *
     * run() 在"已装同版本且非 force"时**早退**（什么都不做）——那一支上 `hook_files_`
     * 保持空，但那是"没被处理"而非"没有 hooks"。批次级记账（hook_sets）必须靠这个判据
     * 把两者分开：早退的包不进账，于是它的 hooks 目录不会被当成"新版本没有 hooks"删掉。
     */
    bool did_process() const
    {
        return processed_;
    }

    /// 内容已由整批预检解压到暂存目录 → prepare() 不再重复解压（见 InstallPlan::content_ready）
    void set_content_ready(bool ready)
    {
        content_ready_ = ready;
    }

private:
    std::string pkg_name_;
    std::string version_;
    bool explicit_install_ = false;
    std::filesystem::path tmp_pkg_dir_;
    std::string actual_version_;
    std::filesystem::path archive_path_;
    std::string old_version_to_replace_;
    std::filesystem::path local_package_path_;
    std::string expected_hash_;
    bool has_config_conflicts_ = false;
    bool force_reinstall_ = false;
    /// 内容已由整批预检解压（见 set_content_ready）
    bool content_ready_ = false;
    std::vector<std::string> deps_;
    std::vector<std::string> provides_;
    std::vector<std::string> provides_soname_;
    std::vector<std::string> needed_so_;
    std::string man_content_;

    void prepare(InstallContext* ctx = nullptr);
    void ensure_dependencies_satisfied(InstallContext& ctx);
    void check_for_file_conflicts(InstallContext* ctx = nullptr);
    void backup_existing_files();
    /**
     * 让开趟（后半，第②b 步）：移除**旧版本不再提供**的触碰面 ——
     * 废弃普通文件/符号链接搬进 stash（WAL `REMOVE_OLD`）、`/etc` 废弃条目只撤所有权与配置
     * 记录（**不搬、不删**）、废弃目录空则 `rmdir`（WAL `DIR_RM`）。定义在
     * `installation_task_letgo.cpp`。
     *
     * 原先是个自由函数、五个入参（包名 / 新旧版本 / 临时目录 / stash 记账）—— 那五个恰好就是
     * 本类的成员（唯一调用点就是 `run()` 里原样传进来），拆 TU 时收回成成员（**只改形参来源，
     * 判据 / WAL 行 / 回滚方式一律未变**）。
     */
    void remove_obsolete_files();
    void commit_without_file_ops();
    void register_package();
    /// 把归档 hooks/ 里的脚本落进 hooks_dir/<pkg>/。**不含执行**：postinst 只在批次
    /// **提交之后**跑（package_manager.cpp 的 finish_committed_batch）—— 批次是"全或无"，
    /// 而钩子副作用改的是系统状态，撤不回来。
    void install_hook_files();

public:
    // 测试钩子（非生产用途）：在 copy_package_files 每个文件复制前调用
    std::function<void()> on_before_file_copy;

private:
    std::vector<DependencyInfo> parse_deps() const;

    // 事务状态（仅供文件级备份/清理使用，不含事务保护语义）
    std::vector<std::filesystem::path> stashes_;  // 本次产生的备份 stash 目录（提交后清理）
    std::vector<std::string> hook_files_;         // 本次写入的 hook 文件名（提交后据此剪枝）
    /// run() 是否真的处理过本包（早退分支保持 false，见 did_process()）
    bool processed_ = false;
    /**
     * 本 task 的「路径事实」记录表：让开趟 probe 一次并记事实，写入趟**逐字复用**，
     * 记事实，写入趟**逐字复用**，两趟查决策表时看到同一份 `PathFacts`。
     *
     * 刻意是**成员**而不是函数级静态表：生命周期 = 一个 task ⇒ 同一进程里先后处理同名包
     * （批量安装 / 依赖递归里重名 / 先卸后装）在结构上不可能互相清踩。详见 op_sink.hpp。
     */
    detail::ProbeLedger probe_ledger_;
};

/// 公共 API：安装包
void install_packages(const std::vector<std::string>& pkg_args, const std::string& hash_file = "",
                      bool force_reinstall = false);

/**
 * 整批文件冲突预检 —— 在**进入事务之前**（批次开始、任何 BEGIN_PKGS 之前）对整批一次性
 * 判定文件冲突，冲突则报错中止且不碰任何文件（判定语义见 installation_task.cpp 的实现）。
 *
 * 调用点：install_packages / upgrade_packages 的批次循环**之外**、run_batch_transaction
 * **之前**。逐包的 check_for_file_conflicts 保留为第二道防线。
 *
 * 副作用：把每个成员包的 content/ 下载解压到标准临时目录，并在 plan 里置 content_ready
 * （事务内的 prepare() 据此复用，不重复解压）。
 */
void check_batch_file_conflicts(std::map<std::string, InstallPlan>& plan,
                                const std::vector<std::string>& order);

/**
 * 移除一个包。
 *
 * @param force        跳过安全检查（反向依赖 / 共享文件 / 陈旧文件键）——**不**包含
 *                     "丢配置文件"：`--force` 与配置保留是两个正交维度
 * @param purge_config 显式 `--purge-config`：真删配置文件。默认 false = 把包内配置文件
 *                     改名成 `<路径>.lpkgsave` 保留（无论 force 与否、也无论走哪条移除路径）
 */
void remove_package(const std::string& pkg_name, bool force = false, bool wrap_in_txn = true,
                    bool purge_config = false);
/// 移除多个包：**单批次原子**（多包命令必须走它，逐包调用会失去跨包回滚）。
/// @return 实际移除的包数（"没装过"的不计）—— 调用方据此决定要不要打"卸载完成"
///         （2026-10-03：此前 CLI 无条件打，`lpkg remove <未安装>` 会打印一条假完成消息）。
size_t remove_packages(const std::vector<std::string>& pkg_names, bool force = false,
                       bool purge_config = false);
void autoremove(bool purge_config = false);
void upgrade_packages();
void force_solve_conflict(bool purge_config = false);
void reinstall_package(const std::string& pkg_name);
/// 重装多个包：**单批次原子**（多包命令必须走它，逐包调用会失去跨包回滚）
void reinstall_packages(const std::vector<std::string>& pkg_args);
void query_package(const std::string& pkg_name);
void query_file(const std::string& filename);
void show_man_page(const std::string& pkg_name);
void write_cache();
void remove_package_files(const std::string& pkg_name);
/// 递归移除多个包：**单批次原子**（多参数命令必须走它，逐参数调用会失去跨参数回滚）。
/// @return 实际移除的包数（与 `remove_packages` 同一约定；"没什么可删"的早退返回 0）。
size_t remove_packages_recursive(const std::vector<std::string>& pkg_names, bool force = false,
                                 bool purge_config = false);
