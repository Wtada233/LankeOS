#pragma once

#include "exception.hpp"
#include <string>
#include <string_view>
#include <unordered_set>
#include <filesystem>

namespace fs = std::filesystem;

// Color codes
inline constexpr std::string_view COLOR_GREEN = "[1;32m";
inline constexpr std::string_view COLOR_WHITE = "[1;37m";
inline constexpr std::string_view COLOR_YELLOW = "[1;33m";
inline constexpr std::string_view COLOR_RED = "[1;31m";
inline constexpr std::string_view COLOR_RESET = "[0m";

// Log functions
void log_info(std::string_view msg);
void log_warning(std::string_view msg);
void log_error(std::string_view msg);
void log_progress(const std::string& msg, double percentage, int bar_width = 50);

// Interactive mode control
enum class NonInteractiveMode {
    INTERACTIVE,
    YES,
    NO
};

void set_non_interactive_mode(NonInteractiveMode mode);
NonInteractiveMode get_non_interactive_mode();

bool user_confirms(const std::string& prompt);

// System checks
void check_root();

// Concurrency control (RAII lock)
class DBLock {
public:
    DBLock();
    ~DBLock();
    DBLock(const DBLock&) = delete;
    DBLock& operator=(const DBLock&) = delete;
private:
    int lock_fd = -1;
};

// Filesystem utilities
void ensure_dir_exists(const fs::path& path);
void ensure_file_exists(const fs::path& path);
std::unordered_set<std::string> read_set_from_file(const fs::path& path);
void write_set_to_file(const fs::path& path, const std::unordered_set<std::string>& data);