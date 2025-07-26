#include "config.hpp"
#include "utils.hpp"
#include "package_manager.hpp"
#include "localization.hpp"
#include "exception.hpp"
#include "cxxopts.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <memory>

void print_usage(const cxxopts::Options& options) {
    std::cerr << options.help({""});
    std::cerr << get_string("info.commands") << std::endl;
    std::cerr << get_string("info.install_desc") << std::endl;
    std::cerr << get_string("info.remove_desc") << std::endl;
    std::cerr << get_string("info.autoremove_desc") << std::endl;
    std::cerr << get_string("info.upgrade_desc") << std::endl;
    std::cerr << get_string("info.man_desc") << std::endl;
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
            if (!result.count("packages")) {
                print_usage(options);
                return 1;
            }
            const auto& packages = result["packages"].as<std::vector<std::string>>();
            for (const auto& pkg_arg : packages) {
                size_t pos = pkg_arg.find(':');
                if (pos == std::string::npos) {
                    install_package(pkg_arg, "latest");
                } else {
                    install_package(pkg_arg.substr(0, pos), pkg_arg.substr(pos + 1));
                }
            }
        } else if (command == "remove") {
            if (!result.count("packages")) {
                print_usage(options);
                return 1;
            }
            const auto& pkg_names = result["packages"].as<std::vector<std::string>>();
            bool force = result["force"].as<bool>();
            for (const auto& pkg_name : pkg_names) {
                remove_package(pkg_name, force);
            }
        } else if (command == "autoremove") {
            autoremove();
        } else if (command == "upgrade") {
            upgrade_packages();
        } else if (command == "man") {
            if (!result.count("packages") || result["packages"].as<std::vector<std::string>>().size() != 1) {
                print_usage(options);
                return 1;
            }
            show_man_page(result["packages"].as<std::vector<std::string>>()[0]);
        } else {
            print_usage(options);
            return 1;
        }
    } catch (const cxxopts::exceptions::exception& e) {
        log_error(e.what());
        return 1;
    } catch (const LpkgException& e) {
        log_error(e.what());
        return 1;
    } catch (const std::exception& e) {
        log_error(e.what());
        return 1;
    }

    return 0;
}