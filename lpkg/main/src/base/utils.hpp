#pragma once

#include "config.hpp" // NonInteractiveMode
#include "constants.hpp"
#include "exception.hpp"
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
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
void log_progress(const std::string &msg, double percentage,
                  int bar_width = 50);

// ============ 进程执行 ============

/** 执行外部命令（参数列表形式） */
int run_command(const std::vector<std::string> &args,
                const std::filesystem::path &work_dir = "");
/** 执行外部命令（Shell 字符串形式） */
int run_shell(const std::string &cmd,
              const std::filesystem::path &work_dir = "");

// ============ 用户交互 ============

/** 向用户请求确认（非交互模式自动返回 true） */
bool user_confirms(const std::string &prompt);

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
  DBLock(const DBLock &) = delete;
  DBLock &operator=(const DBLock &) = delete;

private:
  int lock_fd = -1; // 锁文件描述符
};

/**
 * 临时目录管理器（RAII）
 * 构造时创建临时目录，析构时自动清理
 */
class TmpDirManager {
public:
  TmpDirManager();
  ~TmpDirManager();
  TmpDirManager(const TmpDirManager &) = delete;
  TmpDirManager &operator=(const TmpDirManager &) = delete;

private:
  std::filesystem::path tmp_dir_path_; // 临时目录路径
};

// ============ 文件系统工具 ============

/** 确保目录存在，不存在则创建 */
void ensure_dir_exists(const std::filesystem::path &path);
/** 确保文件存在，不存在则创建 */
void ensure_file_exists(const std::filesystem::path &path);
/** 从文件读取字符串集合（每行一个元素） */
std::unordered_set<std::string>
read_set_from_file(const std::filesystem::path &path);
/** 将字符串集合写入文件（每行一个元素） */
void write_set_to_file(const std::filesystem::path &path,
                       const std::unordered_set<std::string> &data);
/** 清理所有临时目录 */
void cleanup_tmp_dirs();

/**
 * fsync 目标文件所在父目录，确保 rename 后的 dentry 落盘。
 *
 * rename(2) 在同文件系统内是原子的，但如果父目录的 dentry 未落盘，
 * 断电后目录可能指向旧路径，rename 的"原子性"在磁盘上不会体现。
 * 在所有 fs::rename / rename(2) 之后调用此函数以消除此盲点。
 */
void fsync_parent_dir(const std::filesystem::path &child_path);

// ============ 字符串工具 ============

/** 替换字符串中的所有匹配子串 */
void string_replace_all(std::string &str, const std::string &from,
                        const std::string &to);

/**
 * 按分隔符切分 string_view，返回子串列表（零拷贝，仅分配 vector）
 * @param s  输入的字符串视图
 * @param d  分隔字符
 * @return   切分后的子串列表
 */
inline std::vector<std::string_view> split_string_view(std::string_view s,
                                                       char d) {
  std::vector<std::string_view> r;
  size_t start = 0, end;
  while ((end = s.find(d, start)) != std::string_view::npos) {
    r.push_back(s.substr(start, end - start));
    start = end + 1;
  }
  r.push_back(s.substr(start));
  return r;
}

// ============ 二进制文件处理(ELF) ============

/** ELF 文件类型枚举 */
enum class BinaryType {
  UNKNOWN,        // 未知格式
  ELF_EXECUTABLE, // ELF 可执行文件
  ELF_SHARED,     // ELF 共享库(.so)
  ELF_STATIC_LIB  // ELF 静态库(.a)
};

/** 去除 ELF 二进制文件的调试符号 */
void strip_binary(const std::filesystem::path &path);
