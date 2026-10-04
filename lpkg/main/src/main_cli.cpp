#include "main_cli.hpp"

#include <curl/curl.h>

#include <atomic>
#include <csignal>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "archive/packer.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "build/builder.hpp"
#include "config/cli.hpp"
#include "config/config.hpp"
#include "cxxopts.hpp"
#include "db/cache.hpp"
#include "db/wal_op.hpp"
#include "i18n/localization.hpp"
#include "nlohmann/json.hpp"
#include "pkg/depend_scanner.hpp"
#include "pkg/install_common.hpp"
#include "pkg/package_manager.hpp"
#include "scan/scanner.hpp"

/** RAII：curl 全局初始化/清理 */
struct CurlGlobalInitializer {
    CurlGlobalInitializer()
    {
        // 返回值必须检查：失败后所有下载都会以莫名其妙的方式失败
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
            throw LpkgException(get_string("error.curl_init_failed"));
    }
    ~CurlGlobalInitializer()
    {
        curl_global_cleanup();
    }
};

// ── SIGINT 防护（**单段式**）────────────────────────────────────────────
//   一次 Ctrl+C → 设 graceful 标志，当前操作完成后退出（事务含回滚必须跑完）。
//
//   （订正 2026-09-26：横幅原写"**双段式**"、旁边还留着一个 `static std::atomic<bool>
//     sigint_force` —— 但那个变量**全仓从未被读写**，第二段（"第 2 次 Ctrl+C 强制终止"）
//     **没有实现**。handler 自己的注释早就说明了为什么：不设超时、不回退 `SIG_DFL`、
//     不 `_Exit`，必须让当前事务（含回滚）完整执行完。所以这里如实改成单段式，并删掉那个
//     死变量。若将来真要加第二段：WAL 对"硬杀"本身是安全的（崩溃恢复就是为它设计的），
//     但要清楚它会把"优雅回滚"降级成"下次启动时恢复"。）

/** 优雅退出标志（`sigint_handler` 设、事务各阶段轮询）。**非 static**：`installation_task.cpp`
 *  等也读它（`sigint_graceful.load()`），所以它在进程内是共享状态。
 *
 *  **定义在这里（而不是 main.cpp）**：本文件是 `LPKG_OBJS` 的一员 ⇒ 进测试二进制。
 *  此前测试二进制不链接 main.o，只能由 tests/integration/test_sigint.cpp 自己造一份
 *  （与生产那份互相独立）；现在两个二进制共用同一份定义，测试置位后能真实影响
 *  `run_cli` 走到的那些事务代码，test_sigint.cpp 里那份重复定义必须删掉（否则链接期
 *  重定义报错）。 */
std::atomic<bool> sigint_graceful{false};

extern "C" void sigint_handler(int)
{
    // 第一次 Ctrl+C 设置优雅退出标志，后续忽略
    // 不设超时，不回退到 SIG_DFL，不 _Exit ——
    // 必须确保当前事务（含回滚）完整执行完毕，不留下半残系统。
    sigint_graceful.store(true);
    static const char msg[] = "\nSIGINT: graceful shutdown after current operation...\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
}

/** RAII：安装/移除等事务中安装 SIGINT 防护 */
struct SigIntGuard {
    SigIntGuard()
    {
        sigint_graceful.store(false);
        std::signal(SIGINT, sigint_handler);
    }
    ~SigIntGuard()
    {
        std::signal(SIGINT, SIG_DFL);
    }
};

/**
 * 帮助文本 —— **唯一构建处**。
 *
 * 为什么要有这个函数而不是直接往流里写（2026-10-03 修）：`--help` 是**正常输出**（约定走
 * stdout，否则 `lpkg --help | less`／`grep` 拿到的永远是空 —— 此前全部写 stderr），而
 * "参数数量不合法"时打用法是**错误输出**（约定走 stderr）。两者内容必须逐字一致，所以文本
 * 只在这里构建一次，由两个薄打印函数各自选流。
 */
std::string usage_text(const cxxopts::Options& options)
{
    std::ostringstream out;
    out << options.help();
    out << get_string("info.commands") << '\n';
    out << get_string("info.install_desc") << "  " << get_string("info.install_opts") << '\n';
    out << get_string("info.remove_desc") << "  " << get_string("info.remove_opts") << '\n';
    out << get_string("info.autoremove_desc") << '\n';
    out << get_string("info.upgrade_desc") << "  " << get_string("info.upgrade_opts") << '\n';
    out << get_string("info.force_solve_desc") << '\n';
    out << get_string("info.reinstall_desc") << "  " << get_string("info.reinstall_opts") << '\n';
    out << get_string("info.query_desc") << "  " << get_string("info.query_opts") << '\n';
    out << get_string("info.man_desc") << '\n';
    out << get_string("info.pack_desc") << "  " << get_string("info.pack_opts") << '\n';
    out << get_string("info.build_desc") << '\n';
    out << get_string("info.depend_desc") << '\n';
    out << get_string("info.scan_desc") << '\n';
    out << get_string("info.rec_desc") << '\n';
    return out.str();
}

/// 错误路径：参数不合法时打用法 → **stderr**（`--help` 那条走 stdout，见 `usage_text`）。
void print_usage(const cxxopts::Options& options)
{
    std::cerr << usage_text(options);
}

#include <optional>

namespace fs = std::filesystem;
using json = nlohmann::json;

/** 检查命令行参数数量是否合法 */
void pre_operation_check(const cxxopts::ParseResult& result,
                         const std::function<void()>& print_usage_func, size_t min,
                         std::optional<size_t> max = std::nullopt)
{
    log_info(get_string("info.pre_op_check"));
    size_t count =
        result.count("packages") ? result["packages"].as<std::vector<std::string>>().size() : 0;
    if (count < min || (max.has_value() && count > max.value())) {
        print_usage_func();
        throw LpkgException(get_string("error.invalid_arg_count"));
    }
}

/** 位置参数 `packages` 的取值（未给出 = 空向量；`count()` 对未知键只返回 0，不抛） */
static std::vector<std::string> packages_of(const cxxopts::ParseResult& result)
{
    return result.count("packages") ? result["packages"].as<std::vector<std::string>>()
                                    : std::vector<std::string>{};
}

// ── 子命令 handler：每个函数 = 一个子命令的完整实现 ──────────────────────
// 分派（`handle_command`）只做"命令字符串 → handler"。退出方式沿用原实现：正常返回 =
// 退出码 0；需要非零退出的错误一律抛异常（`run_cli` 的 catch 块负责落成 1）。

/** 本函数负责 `lpkg install`：参数个数校验 → 安装（`--force` 重装、本地包哈希）→ 完成消息 */
static void run_install_command(const cxxopts::ParseResult& result, const std::string& hash_file,
                                const std::function<void()>& usage)
{
    pre_operation_check(result, usage, 1);
    install_packages(packages_of(result), hash_file, result["force"].as<bool>());
    // 完成消息由 `install_packages()` 自己打（它也可能早退：全部已装 / 用户中止 ——
    // 那两种情况下打"安装完成"是**假消息**）。这里**不要**再补一条：曾经两处各打一次，
    // `lpkg install x` 会打印两遍，而 `--no` 中止后仍打印"安装完成"、退出码 0，
    // 脚本/farm 无法区分"装好了"与"什么都没做"。
}

/** 本函数负责 `lpkg remove`：参数校验 → 按 `--recursive` 选移除路径 → 写缓存 → 完成消息 */
static void run_remove_command(const cxxopts::ParseResult& result,
                               const std::function<void()>& usage)
{
    pre_operation_check(result, usage, 1);
    const auto pkgs = packages_of(result);
    const bool force = result["force"].as<bool>();
    const bool purge_config = result["purge-config"].as<bool>();
    size_t removed = 0;
    if (result["recursive"].as<bool>()) {
        // 一次调用 = **一个批次**：多参数跨包原子（逐参数调用等于每参数一批，
        // 后面那个失败时前面那个已经删完并提交，而退出码非零又表示"什么都没发生"）
        removed = remove_packages_recursive(pkgs, force, purge_config);
    } else {
        // 一次调用 = **一个批次**：中途 Ctrl+C/失败会整批回滚
        removed = remove_packages(pkgs, force, purge_config);
    }
    write_cache();
    // **什么都没删就打"卸载完成"是假消息**（2026-10-03 修，与 install 侧同款取舍）：
    // `lpkg remove <从未安装过的包>` 此前照样打印这句 —— 脚本/farm 会以为删掉了。
    // 未安装/保护性跳过已经各自打过一条原因（`info.package_not_installed` /
    // `info.recursive_nothing_to_remove`），这里只在真删了东西时收尾。
    if (removed > 0) log_info(get_string("info.uninstall_complete"));
}

/** 本函数负责 `lpkg autoremove`：清掉不再被依赖的孤儿包（`--purge-config` 透传）。 */
static void run_autoremove_command(const cxxopts::ParseResult& result,
                                   const std::function<void()>& usage)
{
    pre_operation_check(result, usage, 0, 0);
    autoremove(result["purge-config"].as<bool>());
    write_cache();
}

/** 本函数负责 `lpkg upgrade`：全量升级（无位置参数）。 */
static void run_upgrade_command(const cxxopts::ParseResult& result,
                                const std::function<void()>& usage)
{
    pre_operation_check(result, usage, 0, 0);
    upgrade_packages();
    write_cache();
}

/**
 * 本函数负责 `lpkg force-solve`：显式删除所有被当前仓库打破的包（ABI 断裂 / SONAME 缺失
 * 的依赖者）。要输入确认短语才执行；install/upgrade 的冲突不再自动卸载。
 */
static void run_force_solve_command(const cxxopts::ParseResult& result,
                                    const std::function<void()>& usage)
{
    pre_operation_check(result, usage, 0, 0);
    force_solve_conflict(result["purge-config"].as<bool>());
    write_cache();
}

/** 本函数负责 `lpkg reinstall`：重装指定包（一次调用 = 一个批次，多参数跨包原子）。 */
static void run_reinstall_command(const cxxopts::ParseResult& result,
                                  const std::function<void()>& usage)
{
    pre_operation_check(result, usage, 1);
    reinstall_packages(packages_of(result));
    write_cache();
}

/** 本函数负责 `lpkg query`：恰好一个参数；`-p/--pkg-query` 查包，否则查文件归属。 */
static void run_query_command(const cxxopts::ParseResult& result,
                              const std::function<void()>& usage)
{
    pre_operation_check(result, usage, 1, 1);
    const std::string t = packages_of(result)[0];
    if (result.count("pkg-query"))
        query_package(t);
    else
        query_file(t);
}

/** 本函数负责 `lpkg man`：打印指定包的 man 页（唯一不需要 root / 数据库初始化的命令）。 */
static void run_man_command(const cxxopts::ParseResult& result, const std::function<void()>& usage)
{
    pre_operation_check(result, usage, 1, 1);
    show_man_page(packages_of(result)[0]);
}

/** 本函数负责 `lpkg pack`：读源目录的 metadata.json → 打成 `.lpkg`（`--output` 必给）。 */
static void run_pack_command(const cxxopts::ParseResult& result)
{
    if (!result.count("output")) throw LpkgException(get_string("error.pack_no_output"));

    // 从源目录读取 metadata.json
    const std::string source_dir = result["directory"].as<std::string>();
    std::string pkg_name, pkg_ver, man;
    std::vector<std::string> deps, provides, provides_soname, needed_so;
    detail::read_package_metadata(source_dir, pkg_name, pkg_ver, deps, provides, provides_soname,
                                  needed_so, man);

    pack_package(result["output"].as<std::string>(), source_dir, pkg_name, pkg_ver, deps, provides,
                 provides_soname, man, needed_so);
}

/** 本函数负责 `lpkg build`：至多一个目录参数（多给报错，别假装成功）→ 就地构建。 */
static void run_build_command(const cxxopts::ParseResult& result)
{
    std::string dir = ".";
    if (result.count("packages")) {
        const auto v = result["packages"].as<std::vector<std::string>>();
        // `lpkg build a b c` 曾静默只编 a —— 多余参数一律报错，别假装成功
        if (v.size() > 1) throw LpkgException(get_string("error.build_single_dir"));
        if (!v.empty()) dir = v[0];
    }
    run_build(fs::absolute(dir));
}

/**
 * 逐个参数扫描依赖树并打印（depend 的三个子命令共用；只有扫描函数与标题键不同 ——
 * 原先三份复制粘贴，改一处就得记得改另两处）。
 */
static void print_depend_trees(const std::vector<std::string>& args, const std::string& header_key,
                               const std::function<depscan::ScanNode(const std::string&)>& scan)
{
    for (size_t i = 1; i < args.size(); ++i) {
        auto root = scan(args[i]);
        log_info(string_format(header_key, args[i]));
        depscan::print_tree(root);
        if (i + 1 < args.size()) std::cout << "\n";
    }
}

/** 本函数负责 `lpkg depend`：`args[0]` 是子命令（remove/abibreak/install），其余是包名 */
static void run_depend_command(const cxxopts::ParseResult& result)
{
    const auto args = packages_of(result);
    if (args.empty()) throw LpkgException(get_string("error.depend_need_subcmd"));

    const std::string& sub = args[0];
    const bool all = result["all"].as<bool>();

    const auto check_pkg = [&args] {
        if (args.size() < 2) throw LpkgException(get_string("error.depend_need_pkg"));
    };

    if (sub == "remove") {
        check_pkg();
        print_depend_trees(args, "info.depend_remove_header", [all](const std::string& name) {
            return depscan::scan_remove_tree(name, all);
        });
    } else if (sub == "abibreak") {
        check_pkg();
        print_depend_trees(args, "info.depend_abibreak_header", [all](const std::string& name) {
            return depscan::scan_abibreak_tree(name, all);
        });
    } else if (sub == "install") {
        check_pkg();
        print_depend_trees(args, "info.depend_install_header", [all](const std::string& arg) {
            const fs::path p(arg);
            return (p.extension() == constants::EXT_LPKG || p.extension() == constants::EXT_ZST)
                       ? depscan::scan_install_from_file(fs::absolute(p), all)
                       : depscan::scan_install_tree(arg, all);
        });
    } else {
        throw LpkgException(string_format("error.depend_unknown_subcmd", sub));
    }
}

/** 本函数负责 `lpkg scan`：可选一个根目录参数，扫描孤儿文件（无参数 = 默认根）。 */
static void run_scan_command(const cxxopts::ParseResult& result)
{
    const auto args = packages_of(result);
    scan_orphans(args.empty() ? std::string{} : args[0]);
}

/** 本函数负责 `lpkg rec`：从未完成事务中恢复 → 清理已完成批次 → 回收 DB 备份。 */
static void run_rec_command()
{
    log_info(get_string("info.rec_recovering"));
    recover_packages();
    trim_completed();
    cleanup_db_backups();
    log_info(get_string("info.rec_complete"));
}

/**
 * **唯一的**命令名清单（`constants::CMD_*`）。两处消费者：
 *   ① `handle_command` 的 if 链（真正的分派）；
 *   ② `is_known_command` —— 在本文件 `run_cli` 里**先于** `init_database_for` 拒掉未知命令。
 *
 * 为什么 `is_known_command` 必须存在（2026-10-03 补）：`init_database_for` 一进门就
 * `check_root()`，而它此前排在"判断命令是否认识"之前 ⇒ 非 root 用户敲一个拼错的命令
 * （`lpkg instal …`）得到的是 **"Root permission is required to run."** —— 一个与真实
 * 问题无关的错误，用户会去查权限而不是查拼写。现在未知命令在任何 root/锁/恢复动作之前
 * 就被拒，打印用法、退 1。
 *
 * ⚠️ 加新命令时**两处都要动**（这份清单 + `handle_command` 的 if 链）。测试容器里 lpkg
 * 恒以 root 运行 ⇒ `check_root()` 不会失败 ⇒ 这条顺序**无法在套件里断言**；验证走
 * "以非 root 跑一遍真二进制"（见提交信息）。
 */
static constexpr std::string_view KNOWN_COMMANDS[] = {
    constants::CMD_INSTALL,    constants::CMD_REMOVE,    constants::CMD_AUTOREMOVE,
    constants::CMD_UPGRADE,    constants::CMD_REINSTALL, constants::CMD_QUERY,
    constants::CMD_MAN,        constants::CMD_PACK,      constants::CMD_BUILD,
    constants::CMD_DEPEND,     constants::CMD_SCAN,      constants::CMD_REC,
    constants::CMD_FORCE_SOLVE};

static bool is_known_command(std::string_view command)
{
    for (const auto name : KNOWN_COMMANDS) {
        if (command == name) return true;
    }
    return false;
}

/**
 * 命令分发函数：把解析出的命令字符串与常量表匹配后交对应 handler 执行。
 * 所有命令的数据库初始化（root check、filesystem、锁）已在 run_cli 中完成。
 * 未知命令：打印用法并返回退出码 1。
 */
static int handle_command(const std::string& command, const cxxopts::ParseResult& result,
                          const std::string& hash_file, const std::function<void()>& usage)
{
    if (command == constants::CMD_INSTALL) {
        run_install_command(result, hash_file, usage);
    } else if (command == constants::CMD_REMOVE) {
        run_remove_command(result, usage);
    } else if (command == constants::CMD_AUTOREMOVE) {
        run_autoremove_command(result, usage);
    } else if (command == constants::CMD_UPGRADE) {
        run_upgrade_command(result, usage);
    } else if (command == constants::CMD_FORCE_SOLVE) {
        run_force_solve_command(result, usage);
    } else if (command == constants::CMD_REINSTALL) {
        run_reinstall_command(result, usage);
    } else if (command == constants::CMD_QUERY) {
        run_query_command(result, usage);
    } else if (command == constants::CMD_MAN) {
        run_man_command(result, usage);
    } else if (command == constants::CMD_PACK) {
        run_pack_command(result);
    } else if (command == constants::CMD_BUILD) {
        run_build_command(result);
    } else if (command == constants::CMD_DEPEND) {
        run_depend_command(result);
    } else if (command == constants::CMD_SCAN) {
        run_scan_command(result);
    } else if (command == constants::CMD_REC) {
        run_rec_command();
    } else {
        usage();
        return 1;
    }
    return 0;
}

// ── 全局选项：注册、应用、以及 run_cli 分派前后的收尾 ──────────────────────

/** 注册全部命令行选项（通用 + 各子命令 + 位置参数）。解析与分派留在 run_cli。 */
static void register_all_options(cxxopts::Options& options)
{
    // --- 通用选项 ---
    options.add_options(get_string("help.group_general"))("h,help", get_string("info.help_desc"))(
        "v,version", get_string("help.version"));

    // --- 安装/移除选项 ---（在 config/cli.cpp 里注册：单独翻译单元才能被测试驱动）
    register_cli_options(options);

    // --- 查询选项 ---
    options.add_options(get_string("help.group_query"))(
        "p,pkg-query", get_string("help.pkg_query"),
        cxxopts::value<bool>()->default_value("false"));

    // --- 打包选项 ---
    options.add_options(get_string("help.group_pack"))("o,output", get_string("help.output_file"),
                                                       cxxopts::value<std::string>())(
        "d,directory", get_string("help.pack_source"),
        cxxopts::value<std::string>()->default_value(std::string(constants::DEFAULT_PACK_SOURCE)));

    // --- 其他选项 ---
    options.add_options(get_string("help.group_other"))(
        "all", get_string("help.depend_all"), cxxopts::value<bool>()->default_value("false"));

    // 位置参数
    options.add_options("")("command", "", cxxopts::value<std::string>())(
        "packages", "", cxxopts::value<std::vector<std::string>>());

    options.parse_positional({"command", "packages"});
}

/**
 * 应用与子命令无关的全局选项（root / 架构 / 各模式开关），返回 `--hash` 的值。
 * `apply_cli_config`（覆盖豁免 + durable fsync）也在这一步，时机与原先逐字相同：
 * 在 `--yes/--no` 落定**之前**、在任何 handler **之前**。
 */
static std::string apply_global_options(const cxxopts::ParseResult& result)
{
    std::string hash_file;
    if (result.count("hash")) hash_file = result["hash"].as<std::string>();

    if (result.count("no-hooks"))
        Config::instance().set_no_hooks_mode(result["no-hooks"].as<bool>());
    if (result.count("no-deps")) Config::instance().set_no_deps_mode(result["no-deps"].as<bool>());
    if (result.count("missing-so-no-error"))
        Config::instance().set_missing_so_no_error_mode(result["missing-so-no-error"].as<bool>());
    if (result.count("use-system-soname"))
        Config::instance().set_use_system_soname_mode(result["use-system-soname"].as<bool>());
    if (result.count("root")) Config::instance().set_root_path(result["root"].as<std::string>());
    if (result.count("arch")) Config::instance().set_architecture(result["arch"].as<std::string>());
    // 覆盖豁免（--force-overwrite / --overwrite）与 durable fsync 的落定，见
    // config/cli.cpp —— 同样是"抽出去才能被测试驱动"的那部分。
    apply_cli_config(result);

    return hash_file;
}

/** 落定非交互模式：`--yes` → YES、`--no` → NO；同给时报错（曾静默取后者 = 猜用户意图） */
static void apply_non_interactive_mode(const cxxopts::ParseResult& result)
{
    // --yes 与 --no 同给时曾静默取后者（no）——用户意图矛盾，必须报错而不是猜
    const bool want_yes = result.count("yes") && result["yes"].as<bool>();
    const bool want_no = result.count("no") && result["no"].as<bool>();
    if (want_yes && want_no) throw LpkgException(get_string("error.conflicting_yes_no"));
    if (want_yes) Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
    if (want_no) Config::instance().set_non_interactive_mode(NonInteractiveMode::NO);
}

/**
 * 除 `man` 外都需要 root + 数据库初始化：check_root → 文件系统布局 → 数据库锁 →
 * WAL 恢复 → 清理已完成批次 → 回收孤儿 stash。返回持有的锁（`man` 返回 nullptr），
 * 调用方必须让它活到进程结束 —— 提前析构等于事务中途放锁。
 */
static std::unique_ptr<DBLock> init_database_for(const std::string& command)
{
    if (command == constants::CMD_MAN) return nullptr;

    check_root();
    Config::instance().init_filesystem();
    auto db_lock = std::make_unique<DBLock>();
    // WAL 2.0: 先从未完成事务中恢复，再清理已完成批次
    recover_packages();
    trim_completed();
    // 兜底回收孤儿备份 stash（pid 已死才删）
    cleanup_orphan_stashes(wal::referenced_stash_roots());
    return db_lock;
}

/**
 * 哪些命令启用 SIGINT 防护（首次 Ctrl+C 只设 graceful 标志、不打断当前动作）。
 *
 * 写操作（install/remove/autoremove/upgrade/reinstall/force-solve）自不必说；
 * **build / pack 也在内**（2026-10-03 补）：它们是长时操作（下载源码 + 编译），此前不在名单
 * 里 ⇒ Ctrl+C 走 `SIG_DFL` **立刻杀进程**，`run_build` 的清理路径根本没机会跑，
 * `<dir>/build/work|content|hooks` 与已下载的源码整片留在盘上。挂上防护后：子进程（make/cc，
 * 同一前台进程组）照样收到 Ctrl+C 而死，本进程在**阶段边界**把它翻成 `UserAbort`
 * （见 `build/builder.cpp` 的 `run_build_phases`）→ 走带 cleanup 的异常路径。
 */
static bool needs_sigint_guard(const std::string& command)
{
    return command == constants::CMD_INSTALL || command == constants::CMD_REMOVE ||
           command == constants::CMD_AUTOREMOVE || command == constants::CMD_UPGRADE ||
           command == constants::CMD_REINSTALL || command == constants::CMD_FORCE_SOLVE ||
           command == constants::CMD_BUILD || command == constants::CMD_PACK;
}

int run_cli(const std::vector<std::string>& argv)
{
    // cxxopts 只吃 `const char* const*`（真实 main() 里就是 argv 原样）。从 vector<string>
    // 造一份视图：指针指向 vector 元素内部的缓冲区，本函数内不再改动该 vector
    // ⇒ 指针全程有效（别把它传出去）。末尾补 nullptr 哨兵，保持"argv 以 null 结尾"的
    // POSIX 约定（cxxopts 不读它，但调用方按 argc 读到的语义与真实 main() 一致）。
    std::vector<const char*> raw_argv;
    raw_argv.reserve(argv.size() + 1);
    for (const auto& arg : argv) raw_argv.push_back(arg.c_str());
    raw_argv.push_back(nullptr);
    const int argc = static_cast<int>(argv.size());

    try {
        // 注意：本对象的构造**必须**在 try 之内 —— 它的构造函数会抛 LpkgException
        // （curl_global_init 失败），而构造点在 try 之外时异常不经过下面的 catch，
        // 会一路穿透 run_cli/main → std::terminate（abort，而不是干净的错误信息）。
        CurlGlobalInitializer curl_initializer;
        init_localization();

        // argv[0] 是程序名；测试直接调 run_cli 时可能不给（空向量）→ 用 "lpkg" 兜底，
        // 否则 `Options(std::string{})` 会让用法行开头空一块。
        cxxopts::Options options(argv.empty() ? std::string("lpkg") : argv[0]);
        options.custom_help(get_string("info.usage"));
        options.set_width(100);
        register_all_options(options);

        auto result = options.parse(argc, raw_argv.data());

        if (result.count("help")) {
            // `--help` 是**正常输出** → stdout（2026-10-03 修）：此前写 stderr，
            // `lpkg --help | less` / `| grep` 拿到的永远是空。参数不合法时的用法仍走 stderr
            // （`print_usage`），两者文本逐字相同（同一个 `usage_text()`）。
            std::cout << usage_text(options);
            return 0;
        }

        if (result.count("version")) {
            std::cout << string_format("info.version", LPKG_VERSION) << std::endl;
            return 0;
        }

        // 全局选项（--hash / --root / --arch / 各模式开关 / 覆盖豁免）与非交互模式
        const std::string hash_file = apply_global_options(result);
        apply_non_interactive_mode(result);

        // 必须有命令
        if (!result.count("command")) {
            print_usage(options);
            return 1;
        }

        const std::string& command = result["command"].as<std::string>();

        // **未知命令先拒**：必须发生在 `init_database_for`（内含 `check_root()`）**之前**，
        // 否则非 root 用户敲错命令只会看到"需要 root"，与真实问题无关。见 `KNOWN_COMMANDS`。
        if (!is_known_command(command)) {
            print_usage(options);
            return 1;
        }

        // 数据库初始化（非 man 命令）与写操作的 SIGINT 防护；两者的生命周期都到本函数结束
        auto db_lock = init_database_for(command);
        std::optional<SigIntGuard> sig_guard;
        if (needs_sigint_guard(command)) sig_guard.emplace();

        // lambda 在 handle_command 栈帧内被立即消费，不跨函数逃逸，
        // 但精确捕获 [&options] 比通配 [&] 更安全、更自文档化
        return handle_command(command, result, hash_file, [&options]() { print_usage(options); });

    } catch (const cxxopts::exceptions::exception& e) {
        log_error(string_format("error.cmd_parse_error", e.what()));
        return 1;
    } catch (const UserAbort& e) {
        // 用户取消（答"不" / 验证码错 / Ctrl+C）：**不是错误** —— 用 log_info 不打 `Error:` 前缀，
        // 但退出码仍然非零（脚本/farm 必须能区分"什么都没做"与"做完了"）。
        log_info(e.what());
        return 1;
    } catch (const LpkgException& e) {
        log_error(string_format("error.lpkg_error", e.what()));
        return 1;
    } catch (const std::exception& e) {
        log_error(string_format("error.unexpected_error", e.what()));
        return 1;
    }
}
