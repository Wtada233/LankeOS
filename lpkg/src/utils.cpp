#include "utils.hpp"
#include "config.hpp"
#include "localization.hpp"
#include "exception.hpp"
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
const std::string COLOR_WHITE = "\033[1;37m";
const std::string COLOR_YELLOW = "\033[1;33m";
const std::string COLOR_RED = "\033[1;31m";
const std::string COLOR_RESET = "\033[0m";

void log_info(const std::string& msg) {
    std::cout << COLOR_GREEN << "==> " << COLOR_WHITE << msg << COLOR_RESET << std::endl;
}

void log_sync(const std::string& msg) {
    std::cout << COLOR_GREEN << ">>> " << COLOR_WHITE << msg << COLOR_RESET << std::endl;
}

void log_warning(const std::string& msg) {
    std::cout << COLOR_YELLOW << get_string("warning.prefix") << " " << COLOR_WHITE << msg << COLOR_RESET << std::endl;
}

void log_error(const std::string& msg) {
    std::cerr << COLOR_RED << get_string("error.prefix") << " " << COLOR_RESET << msg << std::endl;
}

bool user_confirms(const std::string& prompt) {
    std::cout << prompt << " " << get_string("prompt.yes_no") << " ";
    std::string response;
    std::cin >> response;
    return (response == "y" || response == "Y");
}

void check_root() {
    if (geteuid() != 0) {
        throw LpkgException(get_string("error.root_required"));
    }
}

DBLock::DBLock() {
    ensure_dir_exists(LOCK_DIR);
    lock_fd = open(LOCK_FILE.c_str(), O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0) {
        throw LpkgException(string_format("error.create_file_failed", LOCK_FILE));
    }

    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            throw LpkgException(get_string("error.db_locked"));
        } else {
            throw LpkgException(get_string("error.db_lock_failed"));
        }
    }
}

DBLock::~DBLock() {
    if (lock_fd != -1) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
}

void ensure_dir_exists(const std::string& path) {
    if (!fs::exists(path)) {
        if (!fs::create_directories(path)) {
            throw LpkgException(string_format("error.create_dir_failed", path));
        }
    } else if (!fs::is_directory(path)) {
        throw LpkgException(string_format("error.path_not_dir", path));
    }
}

void ensure_file_exists(const std::string& path) {
    if (!fs::exists(path)) {
        std::ofstream file(path);
        if (!file) {
            throw LpkgException(string_format("error.create_file_failed", path));
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
