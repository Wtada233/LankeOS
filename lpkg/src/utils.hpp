#ifndef UTILS_HPP
#define UTILS_HPP

#include <string>
#include <unordered_set>

// Log functions
void log_info(const std::string& msg);
void log_sync(const std::string& msg);
void log_warning(const std::string& msg);
void log_error(const std::string& msg);

// Error handling
void exit_with_error(const std::string& msg);

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
