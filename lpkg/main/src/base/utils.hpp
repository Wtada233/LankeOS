#pragma once

#include "config.hpp"   // NonInteractiveMode
#include "exception.hpp"
#include "constants.hpp"
#include <string>
#include <string_view>
#include <unordered_set>
#include <filesystem>
#include <vector>

// ============ 日志输出 ============

/** 输出普通信息日志 */
void log_info(std::string_view msg);
/** 输出警告日志 */
void log_warning(std::string_view msg);
/** 输出错误日志 */
void log_error(std::string_view msg);
/**
 * 输出带进度条的日志
 * @param percentage 进度百分比 (0-100)
 * @param bar_width 进度条宽度（字符数）
 */
void log_progress(const std::string& msg, double percentage, int bar_width = 50);

// ============ 进程执行 ============

/** 执行外部命令（参数列表形式） */
int run_command(const std::vector<std::string>& args, const std::filesystem::path& work_dir = "");
/** 执行外部命令（Shell 字符串形式） */
int run_shell(const std::string& cmd, const std::filesystem::path& work_dir = "");

// ============ 用户交互 ============

/** 向用户请求确认（非交互模式自动返回 true） */
bool user_confirms(const std::string& prompt);

// ============ 系统检查 ============

/** 检查是否以 root 权限运行，非 root 则退出 */
void check_root();

// ============ 并发控制 ============

/**
 * 数据库锁（RAII）
 * 构造时加锁，析构时自动解锁，防止并发操作数据库
 */
class DBLock {
public:
    DBLock();
    ~DBLock();
    DBLock(const DBLock&) = delete;
    DBLock& operator=(const DBLock&) = delete;
private:
    int lock_fd = -1;   // 锁文件描述符
};

/**
 * 临时目录管理器（RAII）
 * 构造时创建临时目录，析构时自动清理
 */
class TmpDirManager {
public:
    TmpDirManager();
    ~TmpDirManager();
    TmpDirManager(const TmpDirManager&) = delete;
    TmpDirManager& operator=(const TmpDirManager&) = delete;
private:
    std::filesystem::path tmp_dir_path_; // 临时目录路径
};

// ============ 文件系统工具 ============

/** 确保目录存在，不存在则创建 */
void ensure_dir_exists(const std::filesystem::path& path);
/** 确保文件存在，不存在则创建 */
void ensure_file_exists(const std::filesystem::path& path);
/** 从文件读取字符串集合（每行一个元素） */
std::unordered_set<std::string> read_set_from_file(const std::filesystem::path& path);
/** 将字符串集合写入文件（每行一个元素） */
void write_set_to_file(const std::filesystem::path& path, const std::unordered_set<std::string>& data);
/** 清理所有临时目录 */
void cleanup_tmp_dirs();

// ============ 字符串工具 ============

/** 替换字符串中的所有匹配子串 */
void string_replace_all(std::string& str, const std::string& from, const std::string& to);

// ============ 安全 ============

/** 验证路径是否在指定根目录下，防止路径穿越攻击 */
std::filesystem::path validate_path(const std::filesystem::path& path, const std::filesystem::path& root);

// ============ 二进制文件处理(ELF) ============

/** ELF 文件类型枚举 */
enum class BinaryType {
    UNKNOWN,          // 未知格式
    ELF_EXECUTABLE,   // ELF 可执行文件
    ELF_SHARED,       // ELF 共享库(.so)
    ELF_STATIC_LIB    // ELF 静态库(.a)
};

/** 去除 ELF 二进制文件的调试符号 */
void strip_binary(const std::filesystem::path& path);
