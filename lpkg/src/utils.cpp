#include "utils.hpp"
#include "config.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <sys/file.h>
#include <cerrno>

namespace fs = std::filesystem;

// Color codes
const std::string COLOR_GREEN = "\033[1;32m";
const std::string COLOR_WHITE = "[1;37m";
const std::string COLOR_YELLOW = "[1;33m";
const std::string COLOR_RED = "[1;31m";
const std::string COLOR_RESET = "[0m";

void log_info(const std::string& msg) {
    std::cout << COLOR_GREEN << "==> " << COLOR_WHITE << msg << COLOR_RESET << std::endl;
}

void log_sync(const std::string& msg) {
    std::cout << COLOR_GREEN << ">>> " << COLOR_WHITE << msg << COLOR_RESET << std::endl;
}

void log_warning(const std::string& msg) {
    std::cout << COLOR_YELLOW << "警告: " << COLOR_WHITE << msg << COLOR_RESET << std::endl;
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

static int lock_fd = -1;

void create_lock() {
    ensure_dir_exists(LOCK_DIR);
    lock_fd = open(LOCK_FILE.c_str(), O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0) {
        exit_with_error("无法创建或打开锁文件: " + LOCK_FILE);
    }

    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            exit_with_error("lpkg数据库被另一进程锁定，请稍后重试。");
        } else {
            exit_with_error("无法锁定数据库。");
        }
    }
}

void remove_lock() {
    if (lock_fd != -1) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        fs::remove(LOCK_FILE);
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
