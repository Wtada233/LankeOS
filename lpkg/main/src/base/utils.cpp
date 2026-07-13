#include "utils.hpp"
#include "elf/strip.hpp"

#include "config.hpp"
#include "exception.hpp"
#include "localization.hpp"

#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <sys/wait.h>
namespace fs = std::filesystem;

#include <mutex>

namespace {
    std::mutex log_mutex;
    bool is_stdout_tty = false;
    bool is_stderr_tty = false;
    bool tty_check_performed = false;

    /** 执行一次 tty 检测，缓存结果供后续使用（线程安全） */
    void ensure_tty_check() {
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
    void log_internal(std::string_view prefix, std::string_view color, std::string_view msg, std::ostream& stream) {
        std::lock_guard<std::mutex> lock(log_mutex);

        ensure_tty_check();

        bool current_stream_is_tty = false;
        if (&stream == &std::cout) {
            current_stream_is_tty = is_stdout_tty;
        } else if (&stream == &std::cerr) {
            current_stream_is_tty = is_stderr_tty;
        }

        if (current_stream_is_tty) {
            stream << color << prefix << constants::COLOR_WHITE << msg << constants::COLOR_RESET << std::endl;
        } else {
            stream << prefix << msg << std::endl;
        }
    }
}

/**
 * 输出信息级别日志
 */
void log_info(std::string_view msg) {
    log_internal(get_string("info.log_prefix"), constants::COLOR_GREEN, msg, std::cout);
}

/**
 * 输出警告级别日志
 */
void log_warning(std::string_view msg) {
    log_internal(get_string("warning.prefix") + " ", constants::COLOR_YELLOW, msg, std::cerr);
}

/**
 * 输出错误级别日志
 */
void log_error(std::string_view msg) {
    log_internal(get_string("error.prefix") + " ", constants::COLOR_RED, msg, std::cerr);
}

/**
 * 输出进度条信息（仅 tty 终端生效）
 * 格式: ==> 消息 [########>-----] 66.7%
 */
void log_progress(const std::string& msg, double percentage, int bar_width) {
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
        if (i < pos) std::cout << "#";
        else if (i == pos) std::cout << ">";
        else std::cout << "-";
    }
    std::cout << "] " << std::fixed << std::setprecision(1) << percentage << "%" << constants::COLOR_RESET << std::flush;
}

/**
 * 执行外部命令（fork + exec）
 * 参数以字符串向量形式传入，可选设置工作目录
 * @return 子进程退出码，执行失败返回 -1
 */
int run_command(const std::vector<std::string>& args, const fs::path& work_dir) {
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
int run_shell(const std::string& cmd, const fs::path& work_dir) {
    return run_command({std::string(constants::BIN_SH), "-c", cmd}, work_dir);
}

/**
 * 向用户请求确认（y/n）
 * 根据非交互模式配置自动返回 yes/no
 */
bool user_confirms(const std::string& prompt) {
    switch (Config::instance().non_interactive_mode()) {
        case NonInteractiveMode::YES:
            return true;
        case NonInteractiveMode::NO:
            return false;
        case NonInteractiveMode::INTERACTIVE:
        default:
            std::cout << prompt << " " << get_string("prompt.yes_no") << " ";
            std::string response;
            std::cin >> response;
            return (response == "y" || response == "Y");
    }
}

/**
 * 检查是否以 root 身份运行，否则抛出异常
 */
void check_root() {
    if (geteuid() != 0) {
        throw LpkgException(get_string("error.root_required"));
    }
}

/**
 * 构造函数：尝试获取数据库文件锁（排他锁，非阻塞）
 * 如果锁已被占用则抛出异常，防止并发访问数据库
 */
DBLock::DBLock() {
    ensure_dir_exists(Config::instance().lock_dir());
    lock_fd = open(Config::instance().lock_file().c_str(), O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0) {
        throw LpkgException(string_format("error.create_file_failed", Config::instance().lock_file().string()));
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
DBLock::~DBLock() {
    if (lock_fd != -1) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        lock_fd = -1;
    }
}

/**
 * 构造函数：清理旧的临时目录后创建新的临时目录
 */
TmpDirManager::TmpDirManager() : tmp_dir_path_(Config::get_tmp_dir()) {
    cleanup_tmp_dirs();
    ensure_dir_exists(tmp_dir_path_);
}

/**
 * 析构函数：清理并删除临时目录及其所有内容
 */
TmpDirManager::~TmpDirManager() {
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
void ensure_dir_exists(const fs::path& path) {
    if (!fs::exists(path)) {
        std::error_code ec;
        if (!fs::create_directories(path, ec)) {
            throw LpkgException(string_format("error.create_dir_failed", path.string()) + ": " + ec.message());
        }
    }
    else if (!fs::is_directory(path)) {
        throw LpkgException(string_format("error.path_not_dir", path.string()));
    }
}

/**
 * 确保文件存在，不存在则创建空文件
 */
void ensure_file_exists(const fs::path& path) {
    if (!fs::exists(path)) {
        std::ofstream file(path);
        if (!file) {
            throw LpkgException(string_format("error.create_file_failed", path.string()) + ": " + strerror(errno));
        }
    }
}

/**
 * 从文件读取字符串集合（每行一个元素，自动去除 \r 换行符）
 */
std::unordered_set<std::string> read_set_from_file(const fs::path& path) {
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
void write_set_to_file(const fs::path& path, const std::unordered_set<std::string>& data) {
    fs::path tmp_path = path.string() + ".tmp";
    {
        std::ofstream file(tmp_path);
        if (!file.is_open()) {
            throw LpkgException(string_format("error.create_file_failed", tmp_path.string()));
        }
        for (const auto& item : data) {
            file << item << "\n";
        }
        file.flush();
    }
    // fsync 确保 .tmp 内容在断电前完整落盘，然后 rename 原子替换
    int fd = ::open(tmp_path.c_str(), O_WRONLY);
    if (fd >= 0) { ::fsync(fd); ::close(fd); }
    fs::rename(tmp_path, path);
}

/**
 * 清理孤儿 lpkg_* 临时目录。
 *
 * 仅基于 PID 存活性检查：lpkg_<PID> 目录若所属进程已死则安全删除。
 *    kill(pid, 0) 是内核级 O(1) 操作——遍历整个 /tmp 的开销也远小于一次 stat，
 *    因此不需要时间回退策略或速率限制。
 */
void cleanup_tmp_dirs() {
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
            log_warning(string_format("warning.cleanup_old_tmp_failed", entry.path().string()) + ": " + e.what());
        }
    }
}

/**
 * 替换字符串中所有匹配的子串（in-place 替换）
 */
void string_replace_all(std::string& str, const std::string& from, const std::string& to) {
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
void strip_binary(const fs::path& path) {
    std::string error_msg;
    if (!strip_file(path, error_msg)) {
        if (!error_msg.empty()) {
            log_warning(string_format("warning.strip_failed", path.string(), error_msg));
        }
    }
}
