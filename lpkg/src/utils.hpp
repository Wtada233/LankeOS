#ifndef UTILS_HPP
#define UTILS_HPP

#include "exception.hpp"
#include <string>
#include <string_view>
#include <unordered_set>

// Color codes
inline constexpr std::string_view COLOR_GREEN = "\033[1;32m";
inline constexpr std::string_view COLOR_WHITE = "\033[1;37m";
inline constexpr std::string_view COLOR_YELLOW = "\033[1;33m";
inline constexpr std::string_view COLOR_RED = "\033[1;31m";
inline constexpr std::string_view COLOR_RESET = "\033[0m";

// Log functions
void log_info(const std::string& msg);
void log_sync(const std::string& msg);
void log_warning(const std::string& msg);
void log_error(const std::string& msg);

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
void ensure_dir_exists(const std::string& path);
void ensure_file_exists(const std::string& path);
std::unordered_set<std::string> read_set_from_file(const std::string& path);
void write_set_to_file(const std::string& path, const std::unordered_set<std::string>& data);

#endif // UTILS_HPP