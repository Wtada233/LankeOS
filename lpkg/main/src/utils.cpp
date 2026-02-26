#include "utils.hpp"

#include "config.hpp"
#include "exception.hpp"
#include "localization.hpp"

#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <chrono>
#include <ctime>
#include <regex>

namespace fs = std::filesystem;

#include <mutex>

namespace {
    NonInteractiveMode non_interactive_mode = NonInteractiveMode::INTERACTIVE;
    bool force_overwrite_mode = false;
    bool no_hooks_mode = false;
    bool no_deps_mode = false;
    bool testing_mode = false;
    std::mutex log_mutex;
    bool is_stdout_tty = false;
    bool is_stderr_tty = false;
    bool tty_check_performed = false;

    // Helper function to reduce code duplication in logging
    void log_internal(std::string_view prefix, std::string_view color, std::string_view msg, std::ostream& stream) {
        std::lock_guard<std::mutex> lock(log_mutex);

        if (!tty_check_performed) {
            is_stdout_tty = isatty(STDOUT_FILENO);
            is_stderr_tty = isatty(STDERR_FILENO);
            tty_check_performed = true;
        }

        bool current_stream_is_tty = false;
        if (&stream == &std::cout) {
            current_stream_is_tty = is_stdout_tty;
        } else if (&stream == &std::cerr) {
            current_stream_is_tty = is_stderr_tty;
        }

        if (current_stream_is_tty) {
            stream << color << prefix << COLOR_WHITE << msg << COLOR_RESET << std::endl;
        } else {
            stream << prefix << msg << std::endl;
        }
    }
}

void log_info(std::string_view msg) {
    log_internal(get_string("info.log_prefix"), COLOR_GREEN, msg, std::cout);
}

void log_warning(std::string_view msg) {
    log_internal(get_string("warning.prefix") + " ", COLOR_YELLOW, msg, std::cerr);
}

void log_error(std::string_view msg) {
    log_internal(get_string("error.prefix") + " ", COLOR_RED, msg, std::cerr);
}

void log_progress(const std::string& msg, double percentage, int bar_width) {
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        if (!tty_check_performed) {
            is_stdout_tty = isatty(STDOUT_FILENO);
            is_stderr_tty = isatty(STDERR_FILENO);
            tty_check_performed = true;
        }

        if (!is_stdout_tty) {
            return;
        }
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

void set_force_overwrite_mode(bool enable) {
    force_overwrite_mode = enable;
}

bool get_force_overwrite_mode() {
    return force_overwrite_mode;
}

void set_no_hooks_mode(bool enable) {
    no_hooks_mode = enable;
}

bool get_no_hooks_mode() {
    return no_hooks_mode;
}

void set_no_deps_mode(bool enable) {
    no_deps_mode = enable;
}

bool get_no_deps_mode() {
    return no_deps_mode;
}

void set_testing_mode(bool enable) {
    testing_mode = enable;
}

bool get_testing_mode() {
    return testing_mode;
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
        int err = errno;
        close(lock_fd);
        if (err == EWOULDBLOCK) {
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
        lock_fd = -1;
    }
}

TmpDirManager::TmpDirManager() : tmp_dir_path_(get_tmp_dir()) {
    cleanup_tmp_dirs();
    ensure_dir_exists(tmp_dir_path_);
}

TmpDirManager::~TmpDirManager() {
    try {
        fs::remove_all(tmp_dir_path_);
    } catch (const fs::filesystem_error&) {
        // Silent as requested
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
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) result.insert(line);
    }
    return result;
}

void write_set_to_file(const fs::path& path, const std::unordered_set<std::string>& data) {
    fs::path tmp_path = path.string() + ".tmp";
    {
        std::ofstream file(tmp_path);
        if (!file.is_open()) {
            throw LpkgException(string_format("error.create_file_failed", tmp_path.string()));
        }
        for (const auto& item : data) {
            file << item << "\n";
        }
    }
    fs::rename(tmp_path, path);
}

void cleanup_tmp_dirs() {
    static auto last_cleanup = std::chrono::system_clock::now();
    auto now = std::chrono::system_clock::now();
    
    // Only cleanup once every hour in the same process
    if (std::chrono::duration_cast<std::chrono::hours>(now - last_cleanup).count() < 1) {
        return;
    }
    last_cleanup = now;

    const fs::path tmp_path = "/tmp";
    const auto twenty_four_hours = std::chrono::hours(24);

    if (!fs::exists(tmp_path) || !fs::is_directory(tmp_path)) {
        return;
    }

    uid_t current_uid = geteuid();

    for (const auto& entry : fs::directory_iterator(tmp_path)) {
        try {
            if (fs::is_symlink(entry.path())) {
                continue;
            }

            if (entry.is_directory() && entry.path().filename().string().starts_with("lpkg_")) {
                struct stat st;
                if (lstat(entry.path().c_str(), &st) == 0) {
                    if (st.st_uid != current_uid) {
                        continue;
                    }
                }

                auto ftime = fs::last_write_time(entry.path());
                auto sctp = std::chrono::file_clock::to_sys(ftime);

                if ((now - sctp) > twenty_four_hours) {
                    fs::remove_all(entry.path());
                }
            }
        } catch (...) {}
    }
}

std::pair<std::string, std::string> parse_package_filename(const std::string& filename) {
    // Regex to match "name-version.lpkg"
    // Group 1: Name
    // Group 2: Version
    static const std::regex filename_regex(R"(^(.+)-(\d+[0-9a-zA-Z\.\-\+]*)\.lpkg$)");
    std::smatch match;
    if (std::regex_match(filename, match, filename_regex)) {
        return {match[1], match[2]};
    }
    throw LpkgException(string_format("error.parse_pkg_filename_failed", filename.c_str()));
}

std::filesystem::path validate_path(const fs::path& path, const fs::path& root) {
    if (path.is_absolute()) {
         throw LpkgException("Security Violation: Path must be relative: " + path.string());
    }

    fs::path normalized = path.lexically_normal();
    for (const auto& component : normalized) {
        if (component == "..") {
             throw LpkgException("Security Violation: Path traversal detected: " + path.string());
        }
    }
    return root / normalized;
}