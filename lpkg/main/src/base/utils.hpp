#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "config.hpp"  // NonInteractiveMode
#include "constants.hpp"
#include "exception.hpp"

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
class DBLock
{
public:
    DBLock();
    ~DBLock();
    DBLock(const DBLock&) = delete;
    DBLock& operator=(const DBLock&) = delete;

private:
    int lock_fd = -1;  // 锁文件描述符
};

/**
 * 临时目录管理器（RAII）
 * 构造时创建临时目录，析构时自动清理
 */
class TmpDirManager
{
public:
    TmpDirManager();
    ~TmpDirManager();
    TmpDirManager(const TmpDirManager&) = delete;
    TmpDirManager& operator=(const TmpDirManager&) = delete;

private:
    std::filesystem::path tmp_dir_path_;  // 临时目录路径
};

// ============ 文件系统工具 ============

/** 确保目录存在，不存在则创建 */
void ensure_dir_exists(const std::filesystem::path& path);
/** 确保文件存在，不存在则创建 */
void ensure_file_exists(const std::filesystem::path& path);
/** 从文件读取字符串集合（每行一个元素） */
std::unordered_set<std::string> read_set_from_file(const std::filesystem::path& path);
/** 将字符串集合写入文件（每行一个元素） */
void write_set_to_file(const std::filesystem::path& path,
                       const std::unordered_set<std::string>& data);
/**
 * 原子写入原始字符串内容：.tmp → fsync → rename（safe_rename 内含父目录 fsync）。
 * 断电在 rename 前 → 原文件不变；断电在 rename 后 → 新文件完整。顺序保持内容原样。
 */
void write_string_to_file(const std::filesystem::path& path, std::string_view content);
/** 清理所有临时目录 */
void cleanup_tmp_dirs();

/**
 * 回收孤儿备份 stash（TODO.md §5）：删除各文件系统根下、pid 已死的
 * `.lpkg_bak_<pkg>_<pid>` 目录（崩溃/续传未覆盖的残留）。范围：root_dir 顶层 +
 * 顶层子目录中 st_dev 与 root_dir 不同（= 子挂载点）的直接子目录，扫描有界；
 * 只认"存活进程已消失"（kill(pid,0) 返回 ESRCH）的，绝不碰正在运行/自 pid 的 stash。
 */
void cleanup_orphan_stashes();

/**
 * fsync 目标文件所在父目录，确保 rename 后的 dentry 落盘。
 *
 * rename(2) 在同文件系统内是原子的，但如果父目录的 dentry 未落盘，
 * 断电后目录可能指向旧路径，rename 的"原子性"在磁盘上不会体现。
 * safe_rename() 内部已调用此函数，一般不需直接使用。
 */
void fsync_parent_dir(const std::filesystem::path& child_path);

/**
 * 安全重命名。
 *
 * 仅做 rename(2)，失败一律抛异常。曾对 overlayfs 的 EXDEV 做 copy+remove_all
 * 回退，但 copy_recursive 对"指向目录的符号链接"会跟随链接误判为目录，
 * 递归删除整棵被 rename 的目录树（升级 filesystem 包时 /usr/lib 全树被删）。
 * 开 redirect_dir 的 overlay 目录 rename 本就不返回 EXDEV；宁可失败也不破坏数据。
 *
 * @param from  源路径
 * @param to    目标路径
 * @throw       std::filesystem::filesystem_error  rename 失败时
 */
void safe_rename(const std::filesystem::path& from, const std::filesystem::path& to);

// ============ 包路径 / 备份路径工具 ============

/** 生成随机小写字母+数字后缀（用于 .lpkg_bak / stash 文件名防冲突） */
std::string random_suffix(size_t len = constants::RANDOM_SUFFIX_LEN);

// ============ 字符串工具 ============

/** 替换字符串中的所有匹配子串 */
void string_replace_all(std::string& str, const std::string& from, const std::string& to);

/**
 * 按分隔符切分 string_view，返回子串列表（零拷贝，仅分配 vector）
 * @param s  输入的字符串视图
 * @param d  分隔字符
 * @return   切分后的子串列表
 */
inline std::vector<std::string_view> split_string_view(std::string_view s, char d)
{
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
    UNKNOWN,         // 未知格式
    ELF_EXECUTABLE,  // ELF 可执行文件
    ELF_SHARED,      // ELF 共享库(.so)
    ELF_STATIC_LIB   // ELF 静态库(.a)
};

/** 去除 ELF 二进制文件的调试符号 */
void strip_binary(const std::filesystem::path& path);
