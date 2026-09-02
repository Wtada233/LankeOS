#include "utils.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

#include "config.hpp"
#include "elf/strip.hpp"
#include "exception.hpp"
#include "localization.hpp"
namespace fs = std::filesystem;

#include <mutex>

/** 在 main.cpp 中定义，由 SIGINT 信号处理函数设置（SigIntGuard 生命周期内生效） */
extern std::atomic<bool> sigint_graceful;

namespace
{
std::mutex log_mutex;
bool is_stdout_tty = false;
bool is_stderr_tty = false;
bool tty_check_performed = false;

/** 执行一次 tty 检测，缓存结果供后续使用（线程安全） */
void ensure_tty_check()
{
    if (!tty_check_performed) {
        is_stdout_tty = isatty(STDOUT_FILENO);
        is_stderr_tty = isatty(STDERR_FILENO);
        tty_check_performed = true;
    }
}

/**
 * 日志输出内部辅助函数
 * 支持终端彩色输出（tty 检测），非 tty 时仅输出纯文本
 */
void log_internal(std::string_view prefix, std::string_view color, std::string_view msg,
                  std::ostream& stream)
{
    std::lock_guard<std::mutex> lock(log_mutex);

    ensure_tty_check();

    bool current_stream_is_tty = false;
    if (&stream == &std::cout) {
        current_stream_is_tty = is_stdout_tty;
    } else if (&stream == &std::cerr) {
        current_stream_is_tty = is_stderr_tty;
    }

    if (current_stream_is_tty) {
        stream << color << prefix << constants::COLOR_WHITE << msg << constants::COLOR_RESET
               << std::endl;
    } else {
        stream << prefix << msg << std::endl;
    }
}
}  // namespace

/**
 * 输出信息级别日志
 */
void log_info(std::string_view msg)
{
    log_internal(get_string("info.log_prefix"), constants::COLOR_GREEN, msg, std::cout);
}

/**
 * 输出警告级别日志
 */
void log_warning(std::string_view msg)
{
    log_internal(get_string("warning.prefix") + " ", constants::COLOR_YELLOW, msg, std::cerr);
}

/**
 * 输出错误级别日志
 */
void log_error(std::string_view msg)
{
    log_internal(get_string("error.prefix") + " ", constants::COLOR_RED, msg, std::cerr);
}

/**
 * 输出进度条信息（仅 tty 终端生效）
 * 格式: ==> 消息 [########>-----] 66.7%
 */
void log_progress(const std::string& msg, double percentage, int bar_width)
{
    {
        std::lock_guard<std::mutex> lock(log_mutex);
        ensure_tty_check();
        if (!is_stdout_tty) {
            return;
        }
    }

    int pos = static_cast<int>(bar_width * percentage / 100.0);

    std::cout << "\r" << constants::COLOR_GREEN << "==> " << constants::COLOR_WHITE << msg << " [";
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos)
            std::cout << "#";
        else if (i == pos)
            std::cout << ">";
        else
            std::cout << "-";
    }
    std::cout << "] " << std::fixed << std::setprecision(1) << percentage << "%"
              << constants::COLOR_RESET << std::flush;
}

/**
 * 执行外部命令（fork + exec）
 * 参数以字符串向量形式传入，可选设置工作目录
 * @return 子进程退出码，执行失败返回 -1
 */
int run_command(const std::vector<std::string>& args, const fs::path& work_dir)
{
    if (args.empty()) return -1;
    pid_t pid = fork();
    if (pid == -1) return -1;
    if (pid == 0) {
        if (!work_dir.empty()) {
            if (chdir(work_dir.c_str()) != 0) {
                perror("chdir");
                _exit(1);
            }
        }
        std::vector<char*> c_args;
        for (const auto& arg : args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);
        execvp(c_args[0], c_args.data());
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) == -1) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/**
 * 通过 shell 执行命令（等价于 sh -c <cmd>）
 */
int run_shell(const std::string& cmd, const fs::path& work_dir)
{
    return run_command({std::string(constants::BIN_BASH), "-c", cmd}, work_dir);
}

/**
 * 向用户请求确认（y/n）
 * 根据非交互模式配置自动返回 yes/no
 *
 * 交互模式用轮询读 stdin：安装/移除等事务中的 SIGINT（Ctrl+C）会由 main.cpp 的
 * SigIntGuard 设置 sigint_graceful 并打印提示——轮询循环检测到即视为用户取消
 * （返回 false），而不是卡在 std::cin 上对 Ctrl+C 无响应。
 */
bool user_confirms(const std::string& prompt)
{
    switch (Config::instance().non_interactive_mode()) {
        case NonInteractiveMode::YES:
            return true;
        case NonInteractiveMode::NO:
            return false;
        case NonInteractiveMode::INTERACTIVE:
        default: {
            std::cout << prompt << " " << get_string("prompt.yes_no") << " ";
            std::cout.flush();

            std::string response;
            char ch;
            while (!sigint_graceful.load()) {
                struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
                const int r = ::poll(&pfd, 1, 100);  // 100ms 轮询，期间可响应信号
                if (r < 0) {
                    if (errno == EINTR) continue;  // 信号打断 poll → 重新检查 flag
                    return false;
                }
                if (r == 0) continue;  // 超时 → 继续轮询（保持响应 Ctrl+C）
                if (pfd.revents & (POLLIN | POLLHUP)) {
                    const ssize_t n = ::read(STDIN_FILENO, &ch, 1);
                    if (n == 0) return false;  // EOF
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        return false;
                    }
                    if (ch == '\n' || ch == '\r') break;
                    response.push_back(ch);
                }
            }
            if (sigint_graceful.load()) return false;  // Ctrl+C → 视为取消

            // 与旧的 std::cin >> 语义一致：忽略首尾空白后匹配 y/Y
            while (!response.empty() && (response.front() == ' ' || response.front() == '\t'))
                response.erase(response.begin());
            while (!response.empty() && (response.back() == ' ' || response.back() == '\t'))
                response.pop_back();
            return (response == "y" || response == "Y");
        }
    }
}

/**
 * 检查是否以 root 身份运行，否则抛出异常
 */
void check_root()
{
    if (geteuid() != 0) {
        throw LpkgException(get_string("error.root_required"));
    }
}

/**
 * 构造函数：尝试获取数据库文件锁（排他锁，非阻塞）
 * 如果锁已被占用则抛出异常，防止并发访问数据库
 */
DBLock::DBLock()
{
    ensure_dir_exists(Config::instance().lock_dir());
    lock_fd = open(Config::instance().lock_file().c_str(), O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0) {
        throw LpkgException(
            string_format("error.create_file_failed", Config::instance().lock_file().string()));
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

/**
 * 析构函数：释放文件锁并关闭文件描述符
 */
DBLock::~DBLock()
{
    if (lock_fd != -1) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        lock_fd = -1;
    }
}

/**
 * 构造函数：清理旧的临时目录后创建新的临时目录
 */
TmpDirManager::TmpDirManager() : tmp_dir_path_(Config::get_tmp_dir())
{
    cleanup_tmp_dirs();
    ensure_dir_exists(tmp_dir_path_);
}

/**
 * 析构函数：清理并删除临时目录及其所有内容
 */
TmpDirManager::~TmpDirManager()
{
    try {
        fs::remove_all(tmp_dir_path_);
    } catch (const fs::filesystem_error&) {
        // 静默处理删除失败，避免在析构中抛出异常
    }
}

/**
 * 确保目录存在，不存在则递归创建
 * 如果路径存在但不是目录则抛出异常
 */
void ensure_dir_exists(const fs::path& path)
{
    if (!fs::exists(path)) {
        std::error_code ec;
        if (!fs::create_directories(path, ec)) {
            throw LpkgException(string_format("error.create_dir_failed", path.string()) + ": " +
                                ec.message());
        }
    } else if (!fs::is_directory(path)) {
        throw LpkgException(string_format("error.path_not_dir", path.string()));
    }
}

/**
 * 确保文件存在，不存在则创建空文件
 */
void ensure_file_exists(const fs::path& path)
{
    if (!fs::exists(path)) {
        std::ofstream file(path);
        if (!file) {
            throw LpkgException(string_format("error.create_file_failed", path.string()) + ": " +
                                strerror(errno));
        }
    }
}

/**
 * 从文件读取字符串集合（每行一个元素，自动去除 \r 换行符）
 */
std::unordered_set<std::string> read_set_from_file(const fs::path& path)
{
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

/**
 * 将字符串集合写入文件（原子写入：先写临时文件再重命名）
 */
void write_set_to_file(const fs::path& path, const std::unordered_set<std::string>& data)
{
    std::string content;
    for (const auto& item : data) {
        content += item;
        content += '\n';
    }
    write_string_to_file(path, content);
}

void write_string_to_file(const fs::path& path, std::string_view content)
{
    fs::path tmp_path = path.string() + ".tmp";
    {
        std::ofstream file(tmp_path);
        if (!file.is_open()) {
            throw LpkgException(string_format("error.create_file_failed", tmp_path.string()));
        }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.flush();
        // 磁盘满/IO 错误不检查会静默产生截断文件并 rename 进正式位置
        if (!file) {
            throw LpkgException(string_format("error.db_write_failed", tmp_path.string()));
        }
    }
    // fsync 确保 .tmp 内容在断电前完整落盘，然后 rename 原子替换
    int fd = ::open(tmp_path.c_str(), O_WRONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
    safe_rename(tmp_path, path);
}

/**
 * fsync 目录条目。
 * open + fsync + close 确保目录元数据（包括其中的 dentry）落盘。
 */
static void fsync_dir_internal(const fs::path& dir)
{
    int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        ::fsync(dir_fd);
        ::close(dir_fd);
    }
}

void fsync_parent_dir(const fs::path& child_path)
{
    fs::path parent = child_path.parent_path();
    if (parent.empty()) return;
    std::error_code ec;
    if (fs::exists(parent, ec)) {
        fsync_dir_internal(parent);
    }
}

// ============================================================================
// 包路径 / 备份路径工具
// ============================================================================

/**
 * 生成随机小写字母+数字后缀（用于 .lpkg_bak 重命名防冲突）
 */
std::string random_suffix(size_t len)
{
    static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    static std::random_device rd;
    std::string s;
    for (size_t i = 0; i < len; ++i) s += chars[rd() % (sizeof(chars) - 1)];
    return s;
}

// ============================================================================
// overlayfs 安全重命名
// ============================================================================

/**
 * 安全重命名。
 *
 * 仅做 rename(2)，失败一律抛异常，**不做 EXDEV copy+remove 回退**。
 *
 * 历史：曾对 overlayfs 的 EXDEV（跨设备/跨层 rename）退回到 copy_recursive
 * （逐条目复制后 remove_all 源）。但 copy_recursive 用 fs::is_directory(from)
 * 判断源类型——对"指向目录的符号链接"会跟随链接判成目录，进而**递归删除整棵
 * 被 rename 的目录树**。升级 filesystem 包（usr-merge 布局，/lib → usr/lib 等
 * 根级目录符号链接）时，backup 阶段对这类符号链接的 safe_rename 一旦落到
 * fallback，/usr/lib 全树被删（overlayFS 下表现为整目录 whiteout）。
 *
 * 且该 fallback 仅对"未开 redirect_dir 的 overlayfs"有意义；开 redirect 的
 * overlay 目录 rename 本就不返回 EXDEV。宁可 rename 失败抛错，也不静默破坏
 * 数据——失败可由 WAL 回滚安全处理。
 */
void safe_rename(const fs::path& from, const fs::path& to)
{
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) {
        throw std::filesystem::filesystem_error(std::string("safe_rename failed: ") + ec.message(),
                                                from, to, ec);
    }
    fsync_parent_dir(to);
}

/**
 * 清理孤儿 lpkg_* 临时目录。
 *
 * 仅基于 PID 存活性检查：lpkg_<PID> 目录若所属进程已死则安全删除。
 *    kill(pid, 0) 是内核级 O(1) 操作——遍历整个 /tmp 的开销也远小于一次 stat，
 *    因此不需要时间回退策略或速率限制。
 */
void cleanup_tmp_dirs()
{
    const fs::path tmp_path = "/tmp";
    if (!fs::exists(tmp_path) || !fs::is_directory(tmp_path)) return;

    for (const auto& entry : fs::directory_iterator(tmp_path)) {
        try {
            if (fs::is_symlink(entry.path()) || !entry.is_directory()) continue;
            const std::string dirname = entry.path().filename().string();
            if (!dirname.starts_with("lpkg_")) continue;

            const auto pid_str = dirname.substr(5);
            if (pid_str.empty()) continue;

            int pid = std::stoi(pid_str);
            if (pid <= 0 || pid == getpid()) continue;

            if (::kill(pid, 0) != 0 && errno == ESRCH) {
                fs::remove_all(entry.path());
            }
        } catch (const std::invalid_argument&) {
            // 非 PID 命名的 lpkg_* 目录——忽略，不删除
        } catch (const std::exception& e) {
            log_warning(string_format("warning.cleanup_old_tmp_failed", entry.path().string()) +
                        ": " + e.what());
        }
    }
}

/**
 * 回收孤儿备份 stash（TODO.md §5）：崩溃/续传没清掉的
 * `<fsroot>/.lpkg_bak_<pkg>_<pid>`。扫描范围有界：root_dir 顶层 + 顶层子目录里
 * st_dev 与 root_dir 不同的（= 子挂载点）的直接子目录。pid 已死（kill ESRCH）才删，
 * 绝不碰自己/存活进程的 stash。stash 正常由 CLEANUP 清除，本函数只是兜底安全网。
 */
void cleanup_orphan_stashes()
{
    const fs::path root = Config::instance().root_dir();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    ec.clear();
    struct stat root_st{};
    if (::lstat(root.c_str(), &root_st) != 0) return;

    auto reap_dir = [&](const fs::path& d) {
        for (auto it = fs::directory_iterator(d, ec); it != fs::directory_iterator{};
             it.increment(ec)) {
            if (ec) {
                ec.clear();
                break;
            }
            const fs::path p = it->path();
            const std::string name = p.filename().string();
            if (name.rfind(".lpkg_bak_", 0) != 0) continue;
            if (!it->is_directory() || fs::is_symlink(p)) continue;
            const auto sep = name.rfind('_');
            if (sep == std::string::npos || sep + 1 >= name.size()) continue;
            int pid = 0;
            try {
                pid = std::stoi(name.substr(sep + 1));
            } catch (...) {
                continue;
            }
            if (pid <= 0 || pid == ::getpid()) continue;
            if (::kill(pid, 0) != 0 && errno == ESRCH) {
                std::error_code ec2;
                fs::remove_all(p, ec2);
            }
        }
    };

    reap_dir(root);
    for (auto it = fs::directory_iterator(root, ec); it != fs::directory_iterator{};
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            break;
        }
        const fs::path p = it->path();
        if (fs::is_symlink(p) || !it->is_directory()) continue;
        struct stat st{};
        if (::lstat(p.c_str(), &st) != 0) continue;
        if (st.st_dev != root_st.st_dev) reap_dir(p);  // 顶层子挂载点根
    }
}

/**
 * 替换字符串中所有匹配的子串（in-place 替换）
 */
void string_replace_all(std::string& str, const std::string& from, const std::string& to)
{
    if (from.empty()) return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

/**
 * 对二进制文件执行 strip 操作
 * 失败时仅记录警告而不中断流程
 */
void strip_binary(const fs::path& path)
{
    std::string error_msg;
    if (!strip_file(path, error_msg)) {
        if (!error_msg.empty()) {
            log_warning(string_format("warning.strip_failed", path.string(), error_msg));
        }
    }
}
