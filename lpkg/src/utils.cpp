#include "utils.hpp"

#include "config.hpp"
#include "exception.hpp"
#include "localization.hpp"

#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>

namespace fs = std::filesystem;

namespace {
    NonInteractiveMode non_interactive_mode = NonInteractiveMode::INTERACTIVE;

    // Helper function to reduce code duplication in logging
    void log_internal(std::string_view prefix, std::string_view color, std::string_view msg, std::ostream& stream) {
        bool is_tty = false;
        if (&stream == &std::cout) {
            is_tty = isatty(STDOUT_FILENO);
        } else if (&stream == &std::cerr) {
            is_tty = isatty(STDERR_FILENO);
        }

        if (is_tty) {
            stream << color << prefix << COLOR_WHITE << msg << COLOR_RESET << std::endl;
        } else {
            stream << prefix << msg << std::endl;
        }
    }
}

void log_info(std::string_view msg) {
    log_internal("==> ", COLOR_GREEN, msg, std::cout);
}

void log_warning(std::string_view msg) {
    log_internal(get_string("warning.prefix") + " ", COLOR_YELLOW, msg, std::cerr);
}

void log_error(std::string_view msg) {
    log_internal(get_string("error.prefix") + " ", COLOR_RED, msg, std::cerr);
}

void log_progress(const std::string& msg, double percentage, int bar_width) {
    if (!isatty(STDOUT_FILENO)) {
        return;
    }

    int pos = static_cast<int>(bar_width * percentage / 100.0);

    std::cout << "\r" << COLOR_GREEN << "==> " << COLOR_WHITE << msg << " [";
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) std::cout << "#";
        else if (i == pos) std::cout << ">";
        else std::cout << "-";
    }
    std::cout << "] " << std::fixed << std::setprecision(1) << percentage << "%" << COLOR_RESET << std::flush;
}

void set_non_interactive_mode(NonInteractiveMode mode) {
    non_interactive_mode = mode;
}

NonInteractiveMode get_non_interactive_mode() {
    return non_interactive_mode;
}

bool user_confirms(const std::string& prompt) {
    switch (get_non_interactive_mode()) {
        case NonInteractiveMode::YES:
            return true;
        case NonInteractiveMode::NO:
            return false;
        case NonInteractiveMode::INTERACTIVE:
        default:
            std::cout << prompt << " " << get_string("prompt.yes_no") << " ";
            std::string response;
            std::cin >> response;
            return (response == "y" || response == "Y");
    }
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
        throw LpkgException(string_format("error.create_file_failed", LOCK_FILE.string()));
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
        if (flock(lock_fd, LOCK_UN) < 0) {
            log_warning(string_format("warning.remove_file_failed", LOCK_FILE.string(), strerror(errno)));
        }
        if (close(lock_fd) < 0) {
            log_warning(string_format("warning.remove_file_failed", LOCK_FILE.string(), strerror(errno)));
        }
    }
}

void ensure_dir_exists(const fs::path& path) {
    if (!fs::exists(path)) {
        std::error_code ec;
        if (!fs::create_directories(path, ec)) {
            throw LpkgException(string_format("error.create_dir_failed", path.string()) + ": " + ec.message());
        }
    }
    else if (!fs::is_directory(path)) {
        throw LpkgException(string_format("error.path_not_dir", path.string()));
    }
}

void ensure_file_exists(const fs::path& path) {
    if (!fs::exists(path)) {
        std::ofstream file(path);
        if (!file) {
            throw LpkgException(string_format("error.create_file_failed", path.string()) + ": " + strerror(errno));
        }
    }
}

std::unordered_set<std::string> read_set_from_file(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw LpkgException(string_format("error.open_file_failed", path.string()));
    }
    std::unordered_set<std::string> result;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) result.insert(line);
    }
    return result;
}

void write_set_to_file(const fs::path& path, const std::unordered_set<std::string>& data) {
    std::ofstream file(path);
    if (!file.is_open()) {
        throw LpkgException(string_format("error.create_file_failed", path.string()));
    }
    for (const auto& item : data) {
        file << item << "\n";
    }
}