#include "config.hpp"
#include "utils.hpp"
#include "package_manager.hpp"
#include "localization.hpp"
#include "exception.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

void print_usage(const char* prog_name) {
    std::cerr << string_format("info.usage", prog_name) << "\n"
              << get_string("info.commands") << "\n"
              << get_string("info.non_interactive_desc") << "\n"
              << get_string("info.install_desc") << "\n"
              << get_string("info.remove_desc") << "\n"
              << get_string("info.autoremove_desc") << "\n"
              << get_string("info.upgrade_desc") << "\n"
              << get_string("info.man_desc") << "\n";
}

int main(int argc, char* argv[]) {
    try {
        init_localization();

        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg.rfind("--non-interactive", 0) == 0) {
                if (arg == "--non-interactive") {
                    set_non_interactive_mode(NonInteractiveMode::NO);
                    continue;
                }
                size_t pos = arg.find('=');
                if (pos != std::string::npos) {
                    std::string value = arg.substr(pos + 1);
                    if (value == "y" || value == "Y") {
                        set_non_interactive_mode(NonInteractiveMode::YES);
                    } else if (value == "n" || value == "N") {
                        set_non_interactive_mode(NonInteractiveMode::NO);
                    } else {
                        log_error(get_string("error.invalid_non_interactive_value"));
                        return 1;
                    }
                } else {
                    set_non_interactive_mode(NonInteractiveMode::NO);
                }
            } else {
                args.push_back(arg);
            }
        }

        if (args.empty()) {
            print_usage(argv[0]);
            return 1;
        }

        const std::string& command = args[0];

        std::unique_ptr<DBLock> db_lock;
        if (command != "man") {
            check_root();
            init_filesystem();
            db_lock = std::make_unique<DBLock>();
        }

        if (command == "install" && args.size() >= 2) {
            for (size_t i = 1; i < args.size(); ++i) {
                const std::string& pkg_arg = args[i];
                size_t pos = pkg_arg.find(':');
                if (pos == std::string::npos) {
                    install_package(pkg_arg, "latest");
                } else {
                    install_package(pkg_arg.substr(0, pos), pkg_arg.substr(pos + 1));
                }
            }
        } else if (command == "remove" && args.size() >= 2) {
            std::vector<std::string> pkg_names;
            bool force = false;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--force") {
                    force = true;
                } else {
                    pkg_names.push_back(args[i]);
                }
            }

            if (pkg_names.empty()) {
                print_usage(argv[0]);
                return 1;
            }
            for (const auto& pkg_name : pkg_names) {
                remove_package(pkg_name, force);
            }
        } else if (command == "autoremove" && args.size() == 1) {
            autoremove();
        } else if (command == "upgrade" && args.size() == 1) {
            upgrade_packages();
        } else if (command == "man" && args.size() == 2) {
            show_man_page(args[1]);
        } else {
            print_usage(argv[0]);
            return 1;
        }
    } catch (const LpkgException& e) {
        log_error(e.what());
        return 1;
    } catch (const std::exception& e) {
        log_error(e.what());
        return 1;
    }

    return 0;
}
