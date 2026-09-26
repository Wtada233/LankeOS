/**
 * 注册趟（第④步，跑在**写入之后**）—— 本文件负责：
 *
 *   · `InstallationTask::commit_without_file_ops()`：本趟的两个动作（注册 + 落 hooks）；
 *   · `InstallationTask::register_package()`：DB 记账 —— 反向依赖、needed_so、所有权
 *     （目录累加 / 普通文件独占）、man 哈希、provides、已装版本（元数据文件走
 *     `wal::write_string_file_wal`，可回滚）；
 *   · `InstallationTask::install_hook_files()`：归档 `hooks/` 落进 `hooks_dir/<pkg>/`
 *     （`OpSink` 原语，可回滚）。**不执行任何钩子** —— 执行在批次提交之后。
 *
 * ⚠️ 拆分是**纯代码搬移**（详见 installation_task.cpp 顶部说明）。
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

/**
 * 注册趟（第④步，跑在**写入之后**）：注册包 -> 落位 hook 文件。
 *
 * **废弃文件/目录的清除不在这里**（第③步起挪到 `remove_obsolete_files()`，跑在写入
 * **之前**）—— 让开与写入不在同一趟，正是这一步要消除的"同一路径被两趟各自判断"。
 * 本函数因此只剩"新版本落地之后的记账"：DB 归属/依赖/提供者 + hooks 目录。
 */
void InstallationTask::commit_without_file_ops()
{
    register_package();
    install_hook_files();
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
    // `hook_src` 是包内内容（tmp_pkg_dir_/hooks）：环上不抛
    if (!exists_follow(hook_src) || !is_directory_follow(hook_src)) return;

    detail::OpSink sink(pkg_name_, &stashes_);
    const fs::path dest_dir = Config::instance().hooks_dir() / pkg_name_;
    if (!fs::exists(dest_dir)) {
        // 目录本体也进事务：NEW_DIR 的逆操作会删掉它，失败批次不在 hooks_dir 下留空壳
        sink.new_dir(dest_dir);
        ensure_dir_exists(dest_dir);
    }
    for (const auto& entry : fs::directory_iterator(hook_src)) {
        // `directory_entry::is_regular_file()` 走 status() → 对环**抛**（谓词族的说明里有
        // 实测记录）。用 ec 重载：环不是普通文件 → 跳过（原来会整批抛出去）。
        std::error_code eec;
        if (!entry.is_regular_file(eec) || eec) continue;

        // 包内 hook 的**符号链接**成员（2026-09-26 修）：`directory_entry::is_regular_file()`
        // 与下面的 `fs::copy` **都跟随末段链接** ⇒ `hooks/postinst.sh -> /etc/shadow` 会让
        // root 把**宿主那份文件的内容**拷成 `hooks_dir/<pkg>/postinst.sh`（mode 随源 + 执行位）
        // 并当 postinst **执行** —— 也就是"包内容读出了包外、还以 root 执行"。
        // 边界取"**解析后仍落在本包解压目录之内**"：包内互指（`postinst.sh -> ./real.sh`）
        // 是合法用法、照旧跟随复制（这也是今天的行为，不改变它）；指到包外的整包拒绝
        // （与归档成员名消毒同款处置），错误**点名那个条目与它解析到的目标**（§8 第 7 条）。
        if (entry.is_symlink()) {
            std::error_code rec_ec;
            const fs::path resolved = fs::weakly_canonical(entry.path(), rec_ec);
            const fs::path pkg_root = fs::weakly_canonical(tmp_pkg_dir_, rec_ec);
            if (rec_ec || !path_within(resolved, pkg_root)) {
                throw LpkgException(string_format("error.hook_symlink_escapes_package",
                                                  entry.path().string(), resolved.string()));
            }
        }
        const fs::path dest = dest_dir / entry.path().filename();

        // hooks_dir/<pkg>/ 下的同名**实体目录**：不是本包能接管的东西。搬进 stash 会在提交后
        // 被 remove_all 连带删掉整棵树（ARCH §3.6：无主内容一律不碰），所以宁可直接失败、
        // 让批次回滚 —— 这也与原实现一致（fs::copy 到目录目标会失败）。
        // 一次 lstat 的真目录判据（2026-09-26 修）：`&&` 的右操作数原来是抛型的
        // `fs::is_symlink`，中间段成环时必抛；`is_real_directory` 就是这条表达式的
        // 不抛版（且只发一次 lstat）。
        if (is_real_directory(dest)) {
            throw LpkgException(string_format("error.hook_path_is_dir", dest.string()));
        }
        // 旧版本的同一个 hook（含符号链接）先搬进 stash：批次回滚时原样搬回
        if (exists_no_follow(dest)) sink.backup(dest);

        fs::path tmp = dest;
        tmp += ".lpkgtmp";
        // 落位前先挡住"tmp 路径是符号链接"：`fs::copy`/`fs::permissions` 都会跟随它，把 hook
        // 脚本的内容与执行位写到链接目标上（同包内容分支的处理，见 refuse_symlink_tmp_path）
        detail::refuse_symlink_tmp_path(tmp);
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
