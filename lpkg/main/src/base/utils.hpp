#pragma once

#include <filesystem>
#include <set>
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

/**
 * 在**目标 root 内**执行 shell 命令。
 *
 * `root_dir == "/"` 时等价于 run_shell；否则 fork + unshare(CLONE_NEWNS) +
 * mount --make-private + chroot + chdir("/") 后再执行——与 `run_hook` 的做法一致。
 * 目标 root 内没有 /bin/bash（或 unshare/chroot 失败）时返回 -1，由调用方告警。
 *
 * 为什么必须有它：外部触发器命令（`systemctl daemon-reload` /
 * `glib-compile-schemas /usr/share/glib-2.0/schemas` / `gtk-update-icon-cache`）里写的是
 * 绝对路径。`lpkg --root /mnt/base install ...` 时若不 chroot，它们会**打在宿主上**，
 * 目标 root 反而没更新（TODO F3）。
 */
int run_shell_in_root(const std::string& cmd);

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

/**
 * 去除首尾空白（空格与制表符）。依赖串/索引字段/配置值都要按同一套规则归一，
 * 否则 " glibc"、"glibc " 会被当成两个不同的包名去找（TODO.md D1）。
 */
std::string trim_copy(std::string_view s);

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

/**
 * 收尾一次"已写好 .tmp"的原子写：fsync(.tmp) → rename → fsync 父目录。
 *
 * **fsync 的返回值必须检查**：ofstream 成功只说明内容进了页缓存，磁盘满/EIO 要等
 * fsync 才暴露；丢掉它就等于把截断内容 rename 进正式位置（DB 静默损坏、且下一轮
 * 还会把这个截断内容当成"备份"）。失败抛 LpkgException。
 *
 * 所有 `.tmp + fsync + rename` 的写入路径都必须走这里，不要各写一套（TODO.md B1）。
 */
void fsync_and_rename(const std::filesystem::path& tmp, const std::filesystem::path& dst);
/** 清理所有临时目录 */
void cleanup_tmp_dirs();

/**
 * 路径 p 是否在 root 之内（含 root 自身）。两边取 lexically_normal、root 尾部分隔符
 * 归一后，按目录边界比较（p == root，或以 root + "/" 开头）。
 *
 * **不要手写 `root + "/"` 前缀拼接**：root == "/" 时得到 "//"，而 lexically_normal
 * 后的路径不可能以 "//" 开头 → 判定恒为 false，把所有路径都判成"不在根内"。
 * root_dir == "/" 恰恰是常规安装（不带 --root）的形态。
 */
bool path_within(const std::filesystem::path& p, const std::filesystem::path& root);

/**
 * 单个路径分量是否安全：非空、不含 '/' 与 NUL、不是 "." / ".."。
 *
 * 包名与版本号来自**不可信来源**（远端索引、.lpkg 内的 metadata.json），而它们会被
 * 直接当成路径分量拼进 tmp_pkg_dir() / dep_dir() / docs_dir() / 下载 URL，
 * 一个 `../` 就能以 root 写到这些目录之外（TODO.md X4）。
 */
bool is_safe_path_component(std::string_view s);

/**
 * 本进程可见的挂载点集合（读 /proc/self/mountinfo，还原 \040 等八进制转义、去尾分隔符）。
 *
 * **判"是否跨文件系统"必须用它，不能用 st_dev**：overlay 上"目录报 overlay 的 dev、
 * upper 层文件报底层 fs 的 dev"，btrfs 子卷/多层镜像里同一条路径链上的 st_dev 也不一致
 * ——拿 st_dev 当边界，会把同一次 rename 的源和目标误判成跨设备（或反之）。真正的
 * EXDEV 边界是 vfsmount：跨挂载点一律 EXDEV，即使同一 superblock 的 bind mount。
 *
 * /proc 未挂载（最小 chroot）时返回空表，调用方自行降级。结果按进程缓存：事务期间
 * lpkg 自身不改挂载表（hook 的 mount namespace 在子进程里）。
 */
const std::vector<std::filesystem::path>& mount_points();

/** p（去尾分隔符后）是否为挂载点（= 其所在文件系统的顶层）。 */
bool is_mount_point(const std::filesystem::path& p);

/**
 * 回收孤儿备份 stash（TODO.md §5）：删除各文件系统根下、pid 已死的
 * `.lpkg_bak_<pkg>_<pid>` 目录（崩溃/续传未覆盖的残留）。范围：root_dir 顶层 +
 * root_dir 内每个挂载点（= stash 的落点集合，见 mount_points）；/proc 不可用时降级为
 * root 顶层 + 顶层子挂载点。只认"存活进程已消失"（kill(pid,0) 返回 ESRCH）的，
 * 绝不碰正在运行/自 pid 的 stash。
 */
void cleanup_orphan_stashes(const std::set<std::filesystem::path>& keep = {});

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
 * 用单引号把一段文本包成安全的 shell 参数；文本内的单引号按 POSIX 方式转义为 `'\''`。
 * 构建阶段命令行与 hook 执行路径共用它（此前两处各写一份）。
 */
std::string shell_quote(std::string_view s);

/**
 * 把 from 的 xattr 全部复制到 to（用 l* 变体：不跟随符号链接）。
 *
 * `std::filesystem::copy` **不复制 xattr**，而 `security.capability` 就是一个 xattr ——
 * ping/fping/traceroute/mtr 这类包靠**文件能力**而非 SUID，丢了它非 root 直接不能用。
 */
void copy_xattrs(const std::filesystem::path& from, const std::filesystem::path& to);

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
