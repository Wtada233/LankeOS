#ifndef UTILS_HPP
#define UTILS_HPP

#include "exception.hpp"
#include <string>
#include <unordered_set>

// Color codes
extern const std::string COLOR_GREEN;
extern const std::string COLOR_WHITE;
extern const std::string COLOR_YELLOW;
extern const std::string COLOR_RED;
extern const std::string COLOR_RESET;

// Log functions
void log_info(const std::string& msg);
void log_sync(const std::string& msg);
void log_warning(const std::string& msg);
void log_error(const std::string& msg);

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
