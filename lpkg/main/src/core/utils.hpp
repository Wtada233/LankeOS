#pragma once

#include "config.hpp"   // NonInteractiveMode
#include "exception.hpp"
#include "constants.hpp"
#include <string>
#include <string_view>
#include <unordered_set>
#include <filesystem>
#include <vector>

// Log functions
void log_info(std::string_view msg);
void log_warning(std::string_view msg);
void log_error(std::string_view msg);
void log_progress(const std::string& msg, double percentage, int bar_width = 50);

// Process execution
int run_command(const std::vector<std::string>& args, const std::filesystem::path& work_dir = "");
int run_shell(const std::string& cmd, const std::filesystem::path& work_dir = "");

// User interaction (uses Config::instance().non_interactive_mode())
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

// RAII for temporary directory
class TmpDirManager {
public:
    TmpDirManager();
    ~TmpDirManager();
    TmpDirManager(const TmpDirManager&) = delete;
    TmpDirManager& operator=(const TmpDirManager&) = delete;
private:
    std::filesystem::path tmp_dir_path_;
};

// Filesystem utilities
void ensure_dir_exists(const std::filesystem::path& path);
void ensure_file_exists(const std::filesystem::path& path);
std::unordered_set<std::string> read_set_from_file(const std::filesystem::path& path);
void write_set_to_file(const std::filesystem::path& path, const std::unordered_set<std::string>& data);
void cleanup_tmp_dirs();

// String utilities
void string_replace_all(std::string& str, const std::string& from, const std::string& to);

// Security
std::filesystem::path validate_path(const std::filesystem::path& path, const std::filesystem::path& root);

// Binary processing (ELF)
enum class BinaryType {
    UNKNOWN,
    ELF_EXECUTABLE,
    ELF_SHARED,
    ELF_STATIC_LIB
};

void strip_binary(const std::filesystem::path& path);
