#include "utils.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

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
/**
 * 严格解析十进制 PID：必须非空且全为数字。
 * `std::stoi` 接受数字前缀（"1234junk" → 1234），会让 `/tmp/lpkg_1234junk` 这类
 * **非本工具**创建的目录被判成"自己的临时目录"并连带 pid 存活检查一起误删。
 */
bool parse_pid_strict(const std::string& s, int& out)
{
    if (s.empty() || s.size() > 9) return false;
    for (const char c : s)
        if (c < '0' || c > '9') return false;
    out = std::stoi(s);
    return out > 0;
}

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

int run_shell_in_root(const std::string& cmd)
{
    const fs::path root = Config::instance().root_dir();
    if (root == "/" || root.string() == "/") return run_shell(cmd);

    const fs::path bash_rel = fs::path("/bin/bash").relative_path();
    if (!fs::exists(root / bash_rel)) return -1;  // 目标 root 里没有 bash → 无法执行

    pid_t pid = fork();
    if (pid == -1) return -1;
    if (pid == 0) {
        if (unshare(CLONE_NEWNS) != 0) _exit(1);
        mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);
        if (chroot(root.c_str()) != 0) _exit(1);
        if (chdir("/") != 0) _exit(1);
        const char* argv[] = {"/bin/bash", "-c", cmd.c_str(), nullptr};
        execv(argv[0], const_cast<char**>(argv));
        _exit(1);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited == -1 && errno == EINTR);
    return (waited == pid && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
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
    // O_CLOEXEC：锁是 flock 在 open file description 上的，fork/exec 的子进程若继承
    // 这个 fd，就会在 lpkg 退出后继续持有锁 → 之后每次 lpkg 都报 "database is locked"，
    // 而现场没有任何 lpkg 在跑（构建/hook 起的长命子进程是常见来源，TODO.md C3）。
    lock_fd = open(Config::instance().lock_file().c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
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
std::string trim_copy(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return std::string(s);
}

bool is_safe_path_component(std::string_view s)
{
    if (s.empty() || s == "." || s == "..") return false;
    if (s.find('/') != std::string_view::npos || s.find('\0') != std::string_view::npos)
        return false;
    // 空白也必须拒绝：包名会进入 WAL 的**里程碑**字段（`DB <path> <pkg>:<state>`），
    // 而 WAL 是空格分帧的（尾字段从右锚定）——带空格的里程碑会让 reverse_execute 推出的
    // 备份名与实际不符 → DB 回滚被静默跳过、备份随后被 cleanup_db_backups 删掉。
    for (const char c : s)
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return false;
    return true;
}

void copy_xattrs(const fs::path& from, const fs::path& to)
{
    ssize_t len = ::llistxattr(from.c_str(), nullptr, 0);
    if (len <= 0) return;
    std::vector<char> names(static_cast<size_t>(len));
    len = ::llistxattr(from.c_str(), names.data(), names.size());
    if (len <= 0) return;
    for (const char* n = names.data(); n < names.data() + len; n += std::strlen(n) + 1) {
        if (*n == '\0') continue;
        const ssize_t vlen = ::lgetxattr(from.c_str(), n, nullptr, 0);
        if (vlen < 0) continue;
        std::vector<char> val(static_cast<size_t>(vlen));
        if (::lgetxattr(from.c_str(), n, val.data(), val.size()) != vlen) continue;
        (void)::lsetxattr(to.c_str(), n, val.data(), val.size(), 0);
    }
}

std::string shell_quote(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '\'';
    for (const char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += '\'';
    return out;
}

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

fs::path strip_trailing_slash(const fs::path& p)
{
    std::string s = p.string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return fs::path(s);
}

namespace
{
// 默认关闭：只保证 rename 原子性 + kill/回滚语义，不保证断电持久性（见 utils.hpp 的说明）。
bool g_durable_fsync = false;
// 受开关影响的原语实际发出过多少次 ::fsync（仅测试读取，见 utils.hpp 的说明）。
std::atomic<size_t> g_durable_fsync_count{0};
}  // namespace

bool durable_fsync_enabled()
{
    return g_durable_fsync;
}

void set_durable_fsync_enabled(bool on)
{
    g_durable_fsync = on;
}

size_t durable_fsync_count_for_tests()
{
    return g_durable_fsync_count.load();
}

DurableFsyncGuard::DurableFsyncGuard() : prev_(g_durable_fsync)
{
    g_durable_fsync = true;
}

DurableFsyncGuard::~DurableFsyncGuard()
{
    g_durable_fsync = prev_;  // 还原（不是无条件 false：嵌套/显式 --fsync 下都要保持原值）
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
 *
 * policy=Empty 时**仅**对"路径不存在"退化成空集（崩溃恢复路径：一条缺失记录不该让整个
 * recover_packages() 打挂）。"存在却打不开"（权限/IO/半损）在两种策略下都抛 —— 把
 * 不可读的库当空库是**静默归零**，比报错危险得多。
 */
std::unordered_set<std::string> read_set_from_file(const fs::path& path,
                                                   MissingSetFilePolicy policy)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        std::error_code ec;
        const bool absent = !fs::exists(path, ec) && !ec;
        if (policy == MissingSetFilePolicy::Empty && absent) return {};
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

void fsync_and_rename(const fs::path& tmp, const fs::path& dst)
{
    // O_RDONLY 足以 fsync；打开失败必须报错（不能"跳过 fsync 直接 rename"）
    const int fd = ::open(tmp.c_str(), O_RDONLY);
    if (fd < 0) {
        throw LpkgException(string_format("error.open_file_failed", tmp.string()) + ": " +
                            std::strerror(errno));
    }
    // 默认模式跳过 fsync：rename 的原子性仍在，断电持久性不做（见 durable_fsync_enabled）。
    // 磁盘满/IO 错误仍有信号 —— 内容是在 write_string_to_file 里写并检查 ofstream 状态的。
    const bool do_fsync = durable_fsync_enabled();
    const int rc = do_fsync ? ::fsync(fd) : 0;
    if (do_fsync) ++g_durable_fsync_count;
    const int err = errno;
    ::close(fd);
    if (rc != 0) {
        // 磁盘满/IO 错误的**唯一**可靠信号（此前被无条件丢弃）
        throw LpkgException(string_format("error.db_write_failed", tmp.string()) + ": " +
                            std::strerror(err));
    }
    safe_rename(tmp, dst);  // 内含 fsync 父目录
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
    fsync_and_rename(tmp_path, path);
}

/**
 * fsync 目录条目。
 * open + fsync + close 确保目录元数据（包括其中的 dentry）落盘。
 */
static void fsync_dir_internal(const fs::path& dir)
{
    if (!durable_fsync_enabled()) return;  // 默认模式：目录项不落盘（见 durable_fsync_enabled）
    int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        if (::fsync(dir_fd) == 0) ++g_durable_fsync_count;
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
 * 路径 p 是否在 root 之内（含 root 自身）。
 *
 * 边界必须按目录比较：root=/lanke 时 /lankefoo 不算在根内。**且 root=="/" 必须成立**——
 * 朴素写法 `p == root || p.starts_with(root + "/")` 在 root=="/" 时拼出 "//"，而
 * lexically_normal 后的路径不可能是 "//" 开头，于是判定恒 false：调用方（stash 落点、
 * query-file 路径解析）会把所有路径都当成"不在根内"，root_dir=="/"（常规安装）时全线失效。
 */
bool path_within(const fs::path& p, const fs::path& root)
{
    std::string rs = root.lexically_normal().string();
    while (rs.size() > 1 && rs.back() == '/') rs.pop_back();  // "/mnt/base/" ≡ "/mnt/base"
    if (rs.empty()) rs = "/";
    const std::string ps = p.lexically_normal().string();
    if (rs == "/") return !ps.empty() && ps.front() == '/';
    return ps == rs || ps.rfind(rs + "/", 0) == 0;
}

namespace
{
/** mountinfo 字段的八进制转义还原（\040 空格、\011 制表、\012 换行、\134 反斜杠） */
std::string unescape_mountinfo(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const bool oct = s[i] == '\\' && i + 4 <= s.size() && s[i + 1] >= '0' && s[i + 1] <= '7' &&
                         s[i + 2] >= '0' && s[i + 2] <= '7' && s[i + 3] >= '0' && s[i + 3] <= '7';
        if (oct) {
            out += static_cast<char>(((s[i + 1] - '0') << 6) | ((s[i + 2] - '0') << 3) |
                                     (s[i + 3] - '0'));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

/** lexically_normal 后去掉尾部分隔符（"/mnt/base/" 与 "/mnt/base" 视为同一个目录） */
fs::path strip_trailing_sep(const fs::path& p)
{
    std::string s = p.lexically_normal().string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return fs::path(s);
}
}  // namespace

const std::vector<fs::path>& mount_points()
{
    static const std::vector<fs::path> points = [] {
        std::vector<fs::path> v;
        std::ifstream f("/proc/self/mountinfo");
        std::string line;
        while (std::getline(f, line)) {
            // 字段：mount_id parent_id major:minor root mount_point options [optional…] - …
            std::istringstream is(line);
            std::string id, parent, dev, root, mp;
            if (!(is >> id >> parent >> dev >> root >> mp)) continue;
            v.push_back(strip_trailing_sep(fs::path(unescape_mountinfo(mp))));
        }
        return v;
    }();
    return points;
}

bool is_mount_point(const fs::path& p)
{
    const fs::path n = strip_trailing_sep(p);
    const auto& mps = mount_points();
    return std::ranges::find(mps, n) != mps.end();
}

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

            // 生产端（config.cpp 的 TmpDirManager）名字是 `lpkg_<pid>_<rand>`，所以只取第一个
            // '_' 之前那段做 PID。此前整串都喂 parse_pid_strict，带 `_<rand>` 后缀的名字永远
            // 解析失败 → SIGKILL/断电残留的临时目录**永远不被回收**（实测：/tmp 里 lpkg_<pid>_<n>
            // 长期堆积，正常退出才有 TmpDirManager 析构清理）。
            const std::string rest = dirname.substr(5);
            const auto sep = rest.find('_');
            const std::string pid_str = (sep == std::string::npos) ? rest : rest.substr(0, sep);
            if (pid_str.empty()) continue;

            int pid = 0;
            if (!parse_pid_strict(pid_str, pid) || pid == getpid()) continue;

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
void cleanup_orphan_stashes(const std::set<fs::path>& keep)
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
            // WAL 仍引用（回滚/续传还要用）→ 绝不回收
            if (keep.contains(p.lexically_normal())) continue;
            const auto sep = name.rfind('_');
            if (sep == std::string::npos || sep + 1 >= name.size()) continue;
            int pid = 0;
            if (!parse_pid_strict(name.substr(sep + 1), pid) || pid == ::getpid()) continue;
            if (::kill(pid, 0) != 0 && errno == ESRCH) {
                std::error_code ec2;
                fs::remove_all(p, ec2);
            }
        }
    };

    reap_dir(root);
    // stash 落点 = 文件系统顶层 = 挂载点（见 detail::stash_parent_dir）。挂载点可以深于
    // 一层（ESP 挂在 /mnt/base、tmpfs 挂在 /run/user/1000），只扫 root 顶层会漏。
    for (const auto& mp : mount_points()) {
        if (mp == root.lexically_normal()) continue;  // root 本身已扫
        if (!path_within(mp, root)) continue;         // chroot 边界外不归本进程管
        reap_dir(mp);
    }
    // /proc 不可用（挂载表为空）时的降级：root 顶层 + 顶层子挂载点（st_dev 判定）。
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
    // strip 是**尽力而为**的步骤：出任何问题都只该让包大一点，绝不能失败整个构建。
    // 曾因 strip 内部异常（SHT_NOBITS 的 d_buf 为 NULL → bad_alloc）逃出本函数，
    // 把 lankebuild_package 阶段整个打死（llvm 白跑一次）。这里兜住所有异常。
    try {
        std::string error_msg;
        if (!strip_file(path, error_msg) && !error_msg.empty()) {
            log_warning(string_format("warning.strip_failed", path.string(), error_msg));
        }
    } catch (const std::exception& e) {
        log_warning(string_format("warning.strip_failed", path.string(), e.what()));
    } catch (...) {
        log_warning(
            string_format("warning.strip_failed", path.string(), get_string("error.unknown")));
    }
}
