#include "config.hpp"
#include "utils.hpp"
#include "package_manager.hpp"
#include "localization.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <memory>

void print_usage(const char* prog_name) {
    std::cerr << string_format("info.usage", prog_name) << "\n"
              << get_string("info.commands") << "\n"
              << get_string("info.install_desc") << "\n"
              << get_string("info.remove_desc") << "\n"
              << get_string("info.autoremove_desc") << "\n"
              << get_string("info.upgrade_desc") << "\n"
              << get_string("info.man_desc") << "\n";
}

int main(int argc, char* argv[]) {
    init_localization();

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const std::vector<std::string> args(argv + 1, argv + argc);
    const std::string& command = args[0];

    std::unique_ptr<DBLock> db_lock;
    if (command != "man") {
        check_root();
        init_filesystem();
        db_lock = std::make_unique<DBLock>();
    }

    try {
        if (command == "install" && args.size() >= 2) {
            for (size_t i = 1; i < args.size(); ++i) {
                const std::string& arg = args[i];
                size_t pos = arg.find(':');
                if (pos == std::string::npos) {
                    install_package(arg, "latest");
                } else {
                    install_package(arg.substr(0, pos), arg.substr(pos + 1));
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
    } catch (const std::exception& e) {
        log_error(e.what());
        return 1;
    }

    return 0;
}

