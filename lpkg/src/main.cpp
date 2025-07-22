#include "config.hpp"
#include "utils.hpp"
#include "package_manager.hpp"
#include <iostream>
#include <string>

void print_usage(const char* prog_name) {
    std::cerr << "用法: " << prog_name << " <命令> [参数]\n"
    << "命令:\n"
    << "  install <包名>[:版本]  安装指定包 (默认最新版)\n"
    << "  remove <包名>         移除指定包\n"
    << "  autoremove            自动移除不再需要的包\n"
    << "  upgrade               升级所有可升级的包\n"
    << "  man <包名>            显示包的man page\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string command = argv[1];

    if (command != "man") {
        check_root();
        init_filesystem();
    }

    try {
        if (command == "install" && argc == 3) {
            std::string arg = argv[2];
            size_t pos = arg.find(':');
            if (pos == std::string::npos) {
                install_package(arg, "latest");
            } else {
                install_package(arg.substr(0, pos), arg.substr(pos + 1));
            }
        } else if (command == "remove" && argc == 3) {
            remove_package(argv[2]);
        } else if (command == "autoremove" && argc == 2) {
            autoremove();
        } else if (command == "upgrade" && argc == 2) {
            upgrade_packages();
        } else if (command == "man" && argc == 3) {
            show_man_page(argv[2]);
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
