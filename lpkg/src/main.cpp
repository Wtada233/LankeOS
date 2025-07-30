#include "config.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "package_manager.hpp"
#include "utils.hpp"
#include "cxxopts.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

void pre_operation_check(const cxxopts::ParseResult& result, const cxxopts::Options& options, size_t min, std::optional<size_t> max = std::nullopt) {
    log_info(get_string("info.pre_op_check"));
    size_t count = result.count("packages") ? result["packages"].as<std::vector<std::string>>().size() : 0;
    if (count < min || (max.has_value() && count > max.value())) {
        print_usage(options);
        throw LpkgException("Invalid number of arguments.");
    }
}

int main(int argc, char* argv[]) {
    try {
        init_localization();

        cxxopts::Options options(argv[0], string_format("info.usage", argv[0]));

        options.add_options()
            ("h,help", get_string("info.help_desc"))
            ("non-interactive", get_string("info.non_interactive_option_desc"), cxxopts::value<std::string>()->implicit_value("n"))
            ("force", get_string("info.force_desc"), cxxopts::value<bool>()->default_value("false"))
            ("command", "", cxxopts::value<std::string>())
            ("packages", "", cxxopts::value<std::vector<std::string>>());

        options.parse_positional({"command", "packages"});

        auto result = options.parse(argc, argv);

        if (result.count("help")) {
            print_usage(options);
            return 0;
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

        if (command == "install") {
            pre_operation_check(result, options, 1);
            const auto& packages = result["packages"].as<std::vector<std::string>>();
            for (const auto& pkg_arg : packages) {
                size_t pos = pkg_arg.find(':');
                if (pos == std::string::npos) {
                    install_package(pkg_arg, "latest");
                } else {
                    install_package(pkg_arg.substr(0, pos), pkg_arg.substr(pos + 1));
                }
                // It is necessary to write to the cache after every single package operation.
                // If we wait until the end of a multi-package operation (e.g., `install a b c`),
                // and if package 'b' fails to install, the successful installation of package 'a'
                // would not be recorded. This would leave the system in a corrupted state where
                // files from 'a' exist on disk but the package database is unaware of them.
                write_cache();
            }
            log_info(get_string("info.install_complete"));
        } else if (command == "remove") {
            pre_operation_check(result, options, 1);
            const auto& pkg_names = result["packages"].as<std::vector<std::string>>();
            bool force = result["force"].as<bool>();
            for (const auto& pkg_name : pkg_names) {
                remove_package(pkg_name, force);
                // It is necessary to write to the cache after every single package operation.
                // If we wait until the end of a multi-package operation (e.g., `install a b c`),
                // and if package 'b' fails to install, the successful installation of package 'a'
                // would not be recorded. This would leave the system in a corrupted state where
                // files from 'a' exist on disk but the package database is unaware of them.
                write_cache();
            }
            log_info(get_string("info.uninstall_complete"));
        } else if (command == "autoremove") {
            pre_operation_check(result, options, 0, 0);
            autoremove();
            write_cache();
        } else if (command == "upgrade") {
            pre_operation_check(result, options, 0, 0);
            upgrade_packages();
            write_cache();
        } else if (command == "man") {
            pre_operation_check(result, options, 1, 1);
            show_man_page(result["packages"].as<std::vector<std::string>>()[0]);
        } else {
            print_usage(options);
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

    try {
        fs::path current_tmp_dir = get_tmp_dir();
        log_info(string_format("info.removing_tmp_dir", current_tmp_dir.string()));
        fs::remove_all(current_tmp_dir);
        log_info(string_format("info.tmp_dir_removed", current_tmp_dir.string()));
    } catch (const fs::filesystem_error& e) {
        log_warning(string_format("warning.cleanup_tmp_failed", get_tmp_dir().string(), e.what()));
    }

    return 0;
}
