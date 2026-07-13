#include "config/config.hpp"
#include "base/exception.hpp"
#include "i18n/localization.hpp"
#include "pkg/package_manager.hpp"
#include "base/utils.hpp"
#include "cxxopts.hpp"
#include "archive/packer.hpp"
#include "scan/scanner.hpp"
#include "build/builder.hpp"
#include "pkg/depend_scanner.hpp"
#include "pkg/install_common.hpp"
#include "base/constants.hpp"
#include "nlohmann/json.hpp"

#include <curl/curl.h>
#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <csignal>
#include <atomic>
#include <thread>

/** RAII：curl 全局初始化/清理 */
struct CurlGlobalInitializer {
    CurlGlobalInitializer() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ~CurlGlobalInitializer() {
        curl_global_cleanup();
    }
};

// ── SIGINT 双段式防护 ──────────────────────────────────────────────────
//   第 1 次 Ctrl+C → 设 graceful 标志，当前操作完成后退出
std::atomic<bool> sigint_graceful{false};
static std::atomic<bool> sigint_force{false};

extern "C" void sigint_handler(int) {
    // 第一次 Ctrl+C 设置优雅退出标志，后续忽略
    // 不设超时，不回退到 SIG_DFL，不 _Exit ——
    // 必须确保当前事务（含回滚）完整执行完毕，不留下半残系统。
    sigint_graceful.store(true);
    static const char msg[] = "\nSIGINT: graceful shutdown after current operation...\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
}

/** RAII：安装/移除等事务中安装 SIGINT 防护 */
struct SigIntGuard {
    SigIntGuard() { sigint_graceful.store(false); std::signal(SIGINT, sigint_handler); }
    ~SigIntGuard() { std::signal(SIGINT, SIG_DFL); }
};

/** 打印帮助信息 */
void print_usage(const cxxopts::Options& options) {
    std::cerr << options.help();
    std::cerr << get_string("info.commands") << std::endl;
    std::cerr << get_string("info.install_desc")  << "  " << get_string("info.install_opts") << std::endl;
    std::cerr << get_string("info.remove_desc")   << "  " << get_string("info.remove_opts") << std::endl;
    std::cerr << get_string("info.autoremove_desc") << std::endl;
    std::cerr << get_string("info.upgrade_desc")  << "  " << get_string("info.upgrade_opts") << std::endl;
    std::cerr << get_string("info.reinstall_desc") << "  " << get_string("info.reinstall_opts") << std::endl;
    std::cerr << get_string("info.query_desc")    << "  " << get_string("info.query_opts") << std::endl;
    std::cerr << get_string("info.man_desc")      << std::endl;
    std::cerr << get_string("info.pack_desc")     << "  " << get_string("info.pack_opts") << std::endl;
    std::cerr << get_string("info.build_desc")    << std::endl;
    std::cerr << get_string("info.depend_desc")   << std::endl;
    std::cerr << get_string("info.scan_desc")     << std::endl;
    std::cerr << get_string("info.rec_desc")      << "  " << get_string("info.rec_opts") << std::endl;
}

#include <optional>
#include <functional>

namespace fs = std::filesystem;
using json = nlohmann::json;

/** 检查命令行参数数量是否合法 */
void pre_operation_check(const cxxopts::ParseResult& result,
                         std::function<void()> print_usage_func,
                         size_t min, std::optional<size_t> max = std::nullopt)
{
    log_info(get_string("info.pre_op_check"));
    size_t count = result.count("packages")
        ? result["packages"].as<std::vector<std::string>>().size() : 0;
    if (count < min || (max.has_value() && count > max.value())) {
        print_usage_func();
        throw LpkgException(get_string("error.invalid_arg_count"));
    }
}

/**
 * 命令分发函数
 * 解析出的命令字符串与常量表匹配后执行对应操作。
 * 所有命令的数据库初始化（root check、filesystem、锁）已在 main() 中完成。
 */
static int handle_command(const std::string& command,
                          const cxxopts::ParseResult& result,
                          const std::string& hash_file,
                          std::function<void()> usage)
{
    if (command == constants::CMD_INSTALL) {
        pre_operation_check(result, usage, 1);
        install_packages(result["packages"].as<std::vector<std::string>>(),
                         hash_file, result["force"].as<bool>());
        log_info(get_string("info.install_complete"));

    } else if (command == constants::CMD_REMOVE) {
        pre_operation_check(result, usage, 1);
        if (result["recursive"].as<bool>()) {
            for (const auto& pkg : result["packages"].as<std::vector<std::string>>()) {
                remove_package_recursive(pkg);
                write_cache();
            }
        } else {
            for (const auto& pkg : result["packages"].as<std::vector<std::string>>()) {
                remove_package(pkg, result["force"].as<bool>());
                write_cache();
            }
        }
        log_info(get_string("info.uninstall_complete"));

    } else if (command == constants::CMD_AUTOREMOVE) {
        pre_operation_check(result, usage, 0, 0);
        autoremove();
        write_cache();

    } else if (command == constants::CMD_UPGRADE) {
        pre_operation_check(result, usage, 0, 0);
        upgrade_packages();
        write_cache();

    } else if (command == constants::CMD_REINSTALL) {
        pre_operation_check(result, usage, 1);
        for (const auto& pkg : result["packages"].as<std::vector<std::string>>())
            reinstall_package(pkg);
        write_cache();

    } else if (command == constants::CMD_QUERY) {
        pre_operation_check(result, usage, 1, 1);
        std::string t = result["packages"].as<std::vector<std::string>>()[0];
        if (result.count("pkg-query")) query_package(t);
        else query_file(t);

    } else if (command == constants::CMD_MAN) {
        pre_operation_check(result, usage, 1, 1);
        show_man_page(result["packages"].as<std::vector<std::string>>()[0]);

    } else if (command == constants::CMD_PACK) {
        if (!result.count("output"))
            throw LpkgException(get_string("error.pack_no_output"));

        // 从源目录读取 metadata.json
        std::string source_dir = result["directory"].as<std::string>();
        fs::path meta_path = fs::path(source_dir) / constants::PKG_METADATA_FILE;
        std::string pkg_name, pkg_ver, man;
        std::vector<std::string> deps, provides, needed_so;
        detail::read_package_metadata(source_dir, pkg_name, pkg_ver, deps, provides, needed_so, man);

        pack_package(result["output"].as<std::string>(),
                     source_dir, pkg_name, pkg_ver,
                     deps, provides, man, needed_so);

    } else if (command == constants::CMD_BUILD) {
        std::string dir = ".";
        if (result.count("packages"))
            if (auto v = result["packages"].as<std::vector<std::string>>(); !v.empty())
                dir = v[0];
        run_build(fs::absolute(dir));

    } else if (command == constants::CMD_DEPEND) {
        /** depend 命令：分析依赖关系，支持 remove/abibreak/install 三个子命令 */
        auto args = result.count("packages")
            ? result["packages"].as<std::vector<std::string>>()
            : std::vector<std::string>{};
        if (args.empty())
            throw LpkgException(get_string("error.depend_need_subcmd"));

        const std::string& sub = args[0];
        bool all = result["all"].as<bool>();

        auto check_pkg = [&]{
            if (args.size() < 2)
                throw LpkgException(get_string("error.depend_need_pkg"));
        };

        if (sub == "remove") {
            check_pkg();
            for (size_t i = 1; i < args.size(); ++i) {
                auto root = depscan::scan_remove_tree(args[i], all);
                log_info(string_format("info.depend_remove_header", args[i]));
                depscan::print_tree(root);
                if (i + 1 < args.size()) std::cout << "\n";
            }
        } else if (sub == "abibreak") {
            check_pkg();
            for (size_t i = 1; i < args.size(); ++i) {
                auto root = depscan::scan_abibreak_tree(args[i], all);
                log_info(string_format("info.depend_abibreak_header", args[i]));
                depscan::print_tree(root);
                if (i + 1 < args.size()) std::cout << "\n";
            }
        } else if (sub == "install") {
            check_pkg();
            for (size_t i = 1; i < args.size(); ++i) {
                fs::path p(args[i]);
                auto root = (p.extension() == constants::EXT_LPKG
                            || p.extension() == constants::EXT_ZST)
                    ? depscan::scan_install_from_file(fs::absolute(p), all)
                    : depscan::scan_install_tree(args[i], all);
                log_info(string_format("info.depend_install_header", args[i]));
                depscan::print_tree(root);
                if (i + 1 < args.size()) std::cout << "\n";
            }
        } else {
            throw LpkgException(string_format(
                "error.depend_unknown_subcmd", sub));
        }

    } else if (command == constants::CMD_SCAN) {
        std::string r;
        if (result.count("packages"))
            if (auto v = result["packages"].as<std::vector<std::string>>(); !v.empty())
                r = v[0];
        scan_orphans(r);

    } else if (command == constants::CMD_REC) {
        recover_packages();

    } else {
        usage();
        return 1;
    }
    return 0;
}

int main(int argc, char* argv[]) {
    CurlGlobalInitializer curl_initializer;
    try {
        init_localization();

        cxxopts::Options options(argv[0]);
        options.custom_help(get_string("info.usage"));
        options.set_width(100);

        // --- 通用选项 ---
        options.add_options(get_string("help.group_general"))
            ("h,help", get_string("info.help_desc"))
            ("v,version", get_string("help.version"))
            ;

        // --- 安装/移除选项 ---
        options.add_options(get_string("help.group_install"))
            ("y,yes", get_string("help.yes_mode"), cxxopts::value<bool>()->default_value("false"))
            ("n,no", get_string("help.no_mode"), cxxopts::value<bool>()->default_value("false"))
            ("force", get_string("help.force"), cxxopts::value<bool>()->default_value("false"))
            ("force-overwrite", get_string("help.force_overwrite"), cxxopts::value<bool>()->default_value("false"))
            ("no-hooks", get_string("help.no_hooks"), cxxopts::value<bool>()->default_value("false"))
            ("no-deps", get_string("help.no_deps"), cxxopts::value<bool>()->default_value("false"))
            ("r,recursive", get_string("help.recursive"), cxxopts::value<bool>()->default_value("false"))
            ("root", get_string("help.root_dir"), cxxopts::value<std::string>())
            ("arch", get_string("help.target_arch"), cxxopts::value<std::string>())
            ("hash", get_string("help.hash"), cxxopts::value<std::string>())
            ;

        // --- 查询选项 ---
        options.add_options(get_string("help.group_query"))
            ("p,pkg-query", get_string("help.pkg_query"), cxxopts::value<bool>()->default_value("false"))
            ;

        // --- 打包选项 ---
        options.add_options(get_string("help.group_pack"))
            ("o,output", get_string("help.output_file"), cxxopts::value<std::string>())
            ("d,directory", get_string("help.pack_source"), cxxopts::value<std::string>()->default_value(std::string(constants::DEFAULT_PACK_SOURCE)))
            ;

        // --- 其他选项 ---
        options.add_options(get_string("help.group_other"))
            ("all", get_string("help.depend_all"), cxxopts::value<bool>()->default_value("false"))
            ;

        // 位置参数
        options.add_options("")
            ("command", "", cxxopts::value<std::string>())
            ("packages", "", cxxopts::value<std::vector<std::string>>())
            ;

        options.parse_positional({"command", "packages"});

        auto result = options.parse(argc, argv);

        if (result.count("help")) {
            print_usage(options);
            return 0;
        }

        if (result.count("version")) {
            std::cout << string_format("info.version", LPKG_VERSION) << std::endl;
            return 0;
        }

        // 全局选项
        std::string hash_file;
        if (result.count("hash")) hash_file = result["hash"].as<std::string>();

        if (result.count("no-hooks"))
            Config::instance().set_no_hooks_mode(result["no-hooks"].as<bool>());
        if (result.count("no-deps"))
            Config::instance().set_no_deps_mode(result["no-deps"].as<bool>());
        if (result.count("root"))
            Config::instance().set_root_path(result["root"].as<std::string>());
        if (result.count("arch"))
            Config::instance().set_architecture(result["arch"].as<std::string>());
        if (result.count("force-overwrite"))
            Config::instance().set_force_overwrite_mode(result["force-overwrite"].as<bool>());

        if (result["yes"].as<bool>())
            Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        if (result["no"].as<bool>())
            Config::instance().set_non_interactive_mode(NonInteractiveMode::NO);

        // 必须有命令
        if (!result.count("command")) {
            print_usage(options);
            return 1;
        }

        const std::string& command = result["command"].as<std::string>();

        // 除了 man 命令外都需要 root 权限 + 数据库初始化
        std::unique_ptr<DBLock> db_lock;
        if (command != constants::CMD_MAN) {
            check_root();
            Config::instance().init_filesystem();
            db_lock = std::make_unique<DBLock>();
        }

        // 安装/移除/升级等写操作启用 SIGINT 防护
        // 首次 Ctrl+C 设 graceful 标志，第 2 次强制终止
        std::optional<SigIntGuard> sig_guard;
        if (command == constants::CMD_INSTALL || command == constants::CMD_REMOVE
            || command == constants::CMD_AUTOREMOVE || command == constants::CMD_UPGRADE
            || command == constants::CMD_REINSTALL) {
            sig_guard.emplace();
        }

        // lambda 在 handle_command 栈帧内被立即消费，不跨函数逃逸，
        // 但精确捕获 [&options] 比通配 [&] 更安全、更自文档化
        return handle_command(command, result, hash_file,
                              [&options]() { print_usage(options); });

    } catch (const cxxopts::exceptions::exception& e) {
        log_error(string_format("error.cmd_parse_error", e.what()));
        return 1;
    } catch (const LpkgException& e) {
        log_error(string_format("error.lpkg_error", e.what()));
        return 1;
    } catch (const std::exception& e) {
        log_error(string_format("error.unexpected_error", e.what()));
        return 1;
    }
}
