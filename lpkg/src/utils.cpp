#include "utils.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>

namespace fs = std::filesystem;

// Color codes
const std::string COLOR_GREEN = "\033[1;32m";
const std::string COLOR_WHITE = "\033[1;37m";
const std::string COLOR_RED = "\033[1;31m";
const std::string COLOR_RESET = "\033[0m";

void log_info(const std::string& msg) {
    std::cout << COLOR_GREEN << "==> " << COLOR_WHITE << msg << COLOR_RESET << std::endl;
}

void log_sync(const std::string& msg) {
    std::cout << COLOR_GREEN << ">>> " << COLOR_WHITE << msg << COLOR_RESET << std::endl;
}

void log_error(const std::string& msg) {
    std::cerr << COLOR_RED << "错误: " << COLOR_RESET << msg << std::endl;
}

void exit_with_error(const std::string& msg) {
    log_error(msg);
    exit(1);
}

void check_root() {
    if (geteuid() != 0) {
        exit_with_error("需要root权限运行");
    }
}

void ensure_dir_exists(const std::string& path) {
    if (!fs::exists(path)) {
        if (!fs::create_directories(path)) {
            exit_with_error("无法创建目录: " + path);
        }
    } else if (!fs::is_directory(path)) {
        exit_with_error("路径不是目录: " + path);
    }
}

void ensure_file_exists(const std::string& path) {
    if (!fs::exists(path)) {
        std::ofstream file(path);
        if (!file) {
            exit_with_error("无法创建文件: " + path);
        }
    }
}

std::unordered_set<std::string> read_set_from_file(const std::string& path) {
    std::ifstream file(path);
    std::unordered_set<std::string> result;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) result.insert(line);
    }
    return result;
}

void write_set_to_file(const std::string& path, const std::unordered_set<std::string>& data) {
    std::ofstream file(path);
    for (const auto& item : data) {
        file << item << "\n";
    }
}
