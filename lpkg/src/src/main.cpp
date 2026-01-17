#include "config.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "package_manager.hpp"
#include "utils.hpp"
#include "cxxopts.hpp"

#include <curl/curl.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <functional>

// RAII for curl global init/cleanup
struct CurlGlobalInitializer {
    CurlGlobalInitializer() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ~CurlGlobalInitializer() {
        curl_global_cleanup();
    }
};

void print_usage(const cxxopts::Options& options) {
    std::cerr << options.help({""});
    std::cerr << get_string("info.commands") << std::endl;
    std::cerr << get_string("info.install_desc") << std::endl;
    std::cerr << get_string("info.remove_desc") << std::endl;
    std::cerr << get_string("info.autoremove_desc") << std::endl;
    std::cerr << get_string("info.upgrade_desc") << std::endl;
    std::cerr << get_string("info.man_desc") << std::endl;
}

#include <optional>

#include <functional>

void pre_operation_check(const cxxopts::ParseResult& result, std::function<void()> print_usage_func, size_t min, std::optional<size_t> max = std::nullopt) {
    log_info(get_string("info.pre_op_check"));
    size_t count = result.count("packages") ? result["packages"].as<std::vector<std::string>>().size() : 0;
    if (count < min || (max.has_value() && count > max.value())) {
        print_usage_func();
        throw LpkgException(get_string("error.invalid_arg_count"));
    }
}

int main(int argc, char* argv[]) {
    CurlGlobalInitializer curl_initializer;
    try {
        init_localization();

        cxxopts::Options options(argv[0], string_format("info.usage", argv[0]));

        options.add_options()
            ("h,help", get_string("info.help_desc"))
            ("non-interactive", get_string("info.non_interactive_option_desc"), cxxopts::value<std::string>()->implicit_value("n"))
            ("force", get_string("info.force_desc"), cxxopts::value<bool>()->default_value("false"))
            ("force-overwrite", "Overwrite untracked files", cxxopts::value<bool>()->default_value("false"))
            ("root", get_string("help.root_dir"), cxxopts::value<std::string>())
            ("arch", get_string("help.target_arch"), cxxopts::value<std::string>())
            ("command", "", cxxopts::value<std::string>())
            ("packages", "", cxxopts::value<std::vector<std::string>>());

        options.parse_positional({"command", "packages"});

        auto result = options.parse(argc, argv);

        if (result.count("help")) {
            print_usage(options);
            return 0;
        }

        if (result.count("root")) {
            set_root_path(result["root"].as<std::string>());
        }

        if (result.count("arch")) {
            set_architecture(result["arch"].as<std::string>());
        }

        if (result.count("force-overwrite")) {
            set_force_overwrite_mode(result["force-overwrite"].as<bool>());
        }

        if (result.count("non-interactive")) {
            std::string value = result["non-interactive"].as<std::string>();
            if (value == "y" || value == "Y") {
                set_non_interactive_mode(NonInteractiveMode::YES);
            } else if (value == "n" || value == "N") {
                set_non_interactive_mode(NonInteractiveMode::NO);
            } else {
                log_error(get_string("error.invalid_non_interactive_value"));
                return 1;
            }
        }

        if (!result.count("command")) {
            print_usage(options);
            return 1;
        }

        const std::string& command = result["command"].as<std::string>();

        std::unique_ptr<DBLock> db_lock;
        if (command != "man") {
            check_root();
            init_filesystem();
            db_lock = std::make_unique<DBLock>();
        }

        auto usage_printer = [&]() { print_usage(options); };

        if (command == "install") {
            pre_operation_check(result, usage_printer, 1);
            const auto& packages = result["packages"].as<std::vector<std::string>>();
            install_packages(packages);
            log_info(get_string("info.install_complete"));
        } else if (command == "remove") {
            pre_operation_check(result, usage_printer, 1);
            const auto& pkg_names = result["packages"].as<std::vector<std::string>>();
            bool force = result["force"].as<bool>();
            for (const auto& pkg_name : pkg_names) {
                remove_package(pkg_name, force);
                write_cache();
            }
            log_info(get_string("info.uninstall_complete"));
        } else if (command == "autoremove") {
            pre_operation_check(result, usage_printer, 0, 0);
            autoremove();
            write_cache();
        } else if (command == "upgrade") {
            pre_operation_check(result, usage_printer, 0, 0);
            upgrade_packages();
            write_cache();
        } else if (command == "man") {
            pre_operation_check(result, usage_printer, 1, 1);
            show_man_page(result["packages"].as<std::vector<std::string>>()[0]);
        } else {
            usage_printer();
            return 1;
        }

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

    return 0;
}
