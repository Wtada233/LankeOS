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
#include <array>
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
#include "exception.hpp"
#include "localization.hpp"
namespace fs = std::filesystem;

#include <mutex>

/** 在 main/src/main_cli.cpp 中定义，由 SIGINT 信号处理函数设置（SigIntGuard 生命周期内生效） */
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
/**
 * 日志输出内部辅助函数。着色**对齐 pacman**（`src/pacman/conf.c` 的 `colstr` +
 * `src/pacman/util.c` 的 `colon_printf` / `pm_printf`）：
 *   · `::` 前缀 = **BOLDBLUE**，正文 = **BOLD** —— pacman 的 `colon` 串就是
 *     `BOLDBLUE "::" BOLD " "`，正文在 BOLD 生效期间打印；
 *   · `error:` / `warning:` 前缀 = BOLDRED / BOLDYELLOW，而**正文不着色**（pacman 打完前缀
 *     立刻 reset，正文用默认色）。
 * 非 TTY 一律纯文本（不写任何转义）。
 */
void log_internal(std::string_view prefix, std::string_view prefix_color,
                  std::string_view body_color, std::string_view msg, std::ostream& stream)
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
        stream << prefix_color << prefix;
        // 正文色为空 ⇒ 前缀后立刻 reset（pacman 的 error/warning 就是这个形状）
        stream << (body_color.empty() ? constants::COLOR_RESET : body_color);
        stream << msg << constants::COLOR_RESET << std::endl;
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
    // 前缀后补一个空格（与 warning/error 一致）：pacman 风格的信息前缀是 `::`。
    // 颜色：`::` 粗蓝 + 正文粗体（pacman 的 `colstr.colon` / `colstr.title`）。
    log_internal(get_string("info.log_prefix") + " ", constants::COLOR_BOLDBLUE,
                 constants::COLOR_BOLD, msg, std::cout);
}

/**
 * 输出警告级别日志
 */
void log_warning(std::string_view msg)
{
    log_internal(get_string("warning.prefix") + " ", constants::COLOR_YELLOW, {}, msg, std::cerr);
}

/**
 * 输出错误级别日志
 */
void log_error(std::string_view msg)
{
    log_internal(get_string("error.prefix") + " ", constants::COLOR_RED, {}, msg, std::cerr);
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
        c_args.reserve(args.size() + 1);  // +1 给末尾的 nullptr
        for (const auto& arg : args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);
        execvp(c_args[0], c_args.data());
        _exit(127);
    }
    int status;
    // waitpid 可能被信号中断（EINTR）—— 必须重试，否则把"被信号打断"误报成"命令失败"。
    // 与 run_shell_in_root 的处理一致。
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) return -1;
    }
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
    // 符号链接环会让 `fs::exists` 抛（见 utils.hpp 的不抛谓词说明）——这里是"root 里有没有
    // bash"的存在性判定，不该有能力把 chroot 执行路径打断，故用 follow 语义的不抛版本。
    if (!exists_follow(root / bash_rel)) return -1;  // 目标 root 里没有 bash → 无法执行

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
 * 从 stdin 读**一行**（读到 `\n` / `\r` 为止，行尾不进结果），期间**轮询 `sigint_graceful`**。
 *
 * 为什么不能用 `std::cin >> x` / `std::getline(std::cin, …)`：`std::signal()` 装的 handler 在
 * glibc 下带 `SA_RESTART`，被信号打断的 `read(2)` 会被**自动重启**；而 iostreams 也会重试。
 * 于是 Ctrl+C 只是把 `sigint_graceful` 置位、打印一句提示，进程**仍旧卡在输入上** ——
 * 用户看到的现象是"**Ctrl+C 无效，只能 kill -9**"（`std::getline` 尤其明显：它连 EINTR 都
 * 不往外抛）。轮询把"信号"与"输入"解耦：100ms 一轮，每轮先看标志。
 *
 * **所有交互式输入都必须走这里**（`user_confirms` 与两处确认短语都是）。别在别处再写
 * `std::cin`：那会让该处又变回"输入期间不可中断"。
 *
 * @return true = 读到一行（不含行尾）；false = 被 Ctrl+C 打断 **或** stdin 到 EOF。
 *         两者对调用方的处置通常相同（放弃当前操作），故不区分。
 */
bool read_line_interruptible(std::string& out)
{
    out.clear();
    while (!sigint_graceful.load()) {
        struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
        const int r = ::poll(&pfd, 1, 100);  // 100ms 轮询，期间可响应信号
        if (r < 0) {
            if (errno == EINTR) continue;  // 信号打断 poll → 重新检查 flag
            return false;
        }
        if (r == 0) continue;  // 超时 → 继续轮询（保持响应 Ctrl+C）
        if (pfd.revents & (POLLIN | POLLHUP)) {
            char ch = 0;
            const ssize_t n = ::read(STDIN_FILENO, &ch, 1);
            if (n == 0) return false;  // EOF
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (ch == '\n' || ch == '\r') return true;
            out.push_back(ch);
        }
    }
    return false;  // Ctrl+C
}

/**
 * 向用户请求确认（y/n）
 * 根据非交互模式配置自动返回 yes/no
 *
 * 交互模式走 `read_line_interruptible()`：SIGINT（Ctrl+C）由 `main_cli.cpp` 的 `SigIntGuard`
 * 置 `sigint_graceful`，轮询循环检测到即视为用户取消（返回 false），而不是卡在 `std::cin` 上
 * 对 Ctrl+C 无响应。
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
            if (!read_line_interruptible(response)) return false;  // Ctrl+C / EOF → 视为取消
            // 与旧的 std::cin >> 语义一致：忽略首尾空白后匹配 y/Y
            const std::string trimmed = trim_copy(response);
            return (trimmed == "y" || trimmed == "Y");
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
    // 而现场没有任何 lpkg 在跑（构建/hook 起的长命子进程是常见来源，历史 TODO.md C3）。
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
    } catch (const fs::filesystem_error&) {  // NOLINT(bugprone-empty-catch) — 析构里不能抛
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
    // **分帧字符一律拒绝**：包名/版本号会被写进几种"行式 + 分隔符"的状态文件，重载时按那些
    // 分隔符切分 —— 带进去就等于把一条记录重新分帧成另一条合法记录：
    //   `:`  `pkgs` 集合（`name:version`）与索引行的版本块（`<ver>:<hash>:<deps>:…`）
    //   `|`  索引行的一级字段（`<name>|<版本块>|<…>`）
    //   `;`  索引行的版本块分隔
    //   `,`  `files.db` 的属主集合（`a,b,c`）与索引行的 deps 字段
    // 2026-10-03 审计实测的后果（`coreutils,evil` 这种名字）：`files.db` 读回变成两个幽灵
    // 属主 ⇒ 该包能"卸载成功"（退 0）却把文件与归属全留下，而真实包 `coreutils` 会**误含**
    // 它的文件（autoremove / remove -r / force-solve 这些内部 force 路径会把文件搬走删掉）。
    // `:` 也是**分帧**字符（上面已列：`pkgs` 的 `name:version`、索引版本块的
    // `<ver>:<hash>:…`）。⚠️ **订正 2026-10-04（8.0.0）**：本条此前还拒 `^` / `~`，理由是
    // "版本桥接的保留字符" —— 桥已拆掉（见 vercmp/version.hpp），`~` 现在是**合法的预发布
    // 标记**（rpm 语义）、`^` 无特殊含义，两者都**放行**。
    // 本函数只用于包名与版本号，不用于内容文件路径，所以不会误伤合法文件名。
    for (const char c : s)
        if (c == '|' || c == ';' || c == ',' || c == ':') return false;
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

std::vector<std::string> list_xattr_keys(const fs::path& p)
{
    std::vector<std::string> keys;
    // 两次调用是 llistxattr 的既定用法（第一次问长度、第二次取值），中间可能因并发而变化：
    // 第二次返回的长度若变大，多出来的部分取不到（内核会截断）—— 对"列键"这件事无害
    // （下一轮/下一个调用点会看到），所以不重试，也**不报错**（见头文件的语义说明）。
    ssize_t len = ::llistxattr(p.c_str(), nullptr, 0);
    if (len <= 0) return keys;
    std::vector<char> names(static_cast<size_t>(len));
    len = ::llistxattr(p.c_str(), names.data(), names.size());
    if (len <= 0) return keys;
    for (const char* n = names.data(); n < names.data() + len; n += std::strlen(n) + 1) {
        if (*n == '\0') continue;
        keys.emplace_back(n);
    }
    return keys;
}

std::optional<std::vector<char>> read_xattr(const fs::path& p, const std::string& key)
{
    const ssize_t vlen = ::lgetxattr(p.c_str(), key.c_str(), nullptr, 0);
    // < 0 = 键不存在（ENODATA）/ 不支持 xattr（ENOTSUP）/ 其它读失败 —— 一律当"没有"。
    // **不能把 0 也当"没有"**：0 是合法的"值长度为零"，与"键不存在"必须分开（后者返回
    // nullopt，前者返回空 vector）—— 分不开就会在回滚时把"本来就有的空值键"误判成
    // "本来没有"而删掉它。
    if (vlen < 0) return std::nullopt;
    std::vector<char> val(static_cast<size_t>(vlen));
    if (vlen > 0 &&
        ::lgetxattr(p.c_str(), key.c_str(), val.data(), val.size()) != static_cast<ssize_t>(vlen))
        return std::nullopt;  // 并发改变等：当作读不到（保守，不返回半份数据）
    return val;
}

bool write_xattr(const fs::path& p, const std::string& key, const std::vector<char>& value)
{
    return ::lsetxattr(p.c_str(), key.c_str(), value.data(), value.size(), 0) == 0;
}

bool remove_xattr(const fs::path& p, const std::string& key)
{
    return ::lremovexattr(p.c_str(), key.c_str()) == 0;
}

namespace
{
constexpr char B64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// 反查表（0..255 → 0..63，非法字符为 -1）。只建一次。
const std::array<signed char, 256>& b64_reverse()
{
    static const std::array<signed char, 256> table = [] {
        std::array<signed char, 256> t{};
        t.fill(-1);
        for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(B64_ALPHABET[i])] = i;
        return t;
    }();
    return table;
}
}  // namespace

std::string base64_encode(const std::vector<char>& data)
{
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        const unsigned v = (static_cast<unsigned char>(data[i]) << 16) |
                           (static_cast<unsigned char>(data[i + 1]) << 8) |
                           static_cast<unsigned char>(data[i + 2]);
        out += B64_ALPHABET[(v >> 18) & 0x3F];
        out += B64_ALPHABET[(v >> 12) & 0x3F];
        out += B64_ALPHABET[(v >> 6) & 0x3F];
        out += B64_ALPHABET[v & 0x3F];
        i += 3;
    }
    const size_t rem = data.size() - i;
    if (rem == 1) {
        const unsigned v = static_cast<unsigned char>(data[i]) << 16;
        out += B64_ALPHABET[(v >> 18) & 0x3F];
        out += B64_ALPHABET[(v >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        const unsigned v = (static_cast<unsigned char>(data[i]) << 16) |
                           (static_cast<unsigned char>(data[i + 1]) << 8);
        out += B64_ALPHABET[(v >> 18) & 0x3F];
        out += B64_ALPHABET[(v >> 12) & 0x3F];
        out += B64_ALPHABET[(v >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

std::optional<std::vector<char>> base64_decode(std::string_view s)
{
    const auto& rev = b64_reverse();
    std::vector<char> out;
    out.reserve((s.size() / 4) * 3);
    unsigned acc = 0;
    int nbits = 0;
    size_t pad = 0;
    for (const char c : s) {
        if (c == '=') {  // 填充只在末尾、最多两个
            if (++pad > 2) return std::nullopt;
            continue;
        }
        if (pad > 0) return std::nullopt;  // 填充之后还有数据 → 非法
        const signed char v = rev[static_cast<unsigned char>(c)];
        if (v < 0) return std::nullopt;  // 非字母表字符（含空格/换行/`→`）
        acc = (acc << 6) | static_cast<unsigned>(v);
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out.push_back(static_cast<char>((acc >> nbits) & 0xFF));
        }
    }
    // 收尾：剩余位数必须是"被填充掉的"（0 位 = 干净收尾；否则输入被截断/非法）
    if (nbits != 0 && nbits != 2 && nbits != 4) return std::nullopt;
    if (nbits == 2 || nbits == 4) {
        if (pad == 0) return std::nullopt;  // 该有填充却没有 → 非法
    } else if (pad != 0) {
        return std::nullopt;  // 不该有填充却有 → 非法
    }
    return out;
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
    // 判定必须走**不抛**的 exists_follow：`fs::exists` 在符号链接环（ELOOP）上抛
    // filesystem_error，那会把"创建一个目录"变成"进程带着半截事务崩掉"。解不开的路径
    // 在这里的答案是"不存在"→ 交给 create_directories 去撞真实错误（EEXIST/ELOOP），
    // 于是报错是 LpkgException（点名路径）而不是 std::filesystem 的原始异常。
    if (!exists_follow(path)) {
        std::error_code ec;
        if (!fs::create_directories(path, ec)) {
            throw LpkgException(string_format("error.create_dir_failed", path.string()) + ": " +
                                ec.message());
        }
    } else if (!is_directory_follow(path)) {
        // 判据**保持跟随语义**（与改前的 `fs::is_directory` 逐字一致）：这里的父目录/状态
        // 目录可以是指向别处的符号链接（usr-merge 的 `/lib -> usr/lib`、管理员搬走的
        // `/var/lib/lpkg`），换成 lstat 语义会让这些布局直接报"不是目录"。本次只把
        // "抛"换成"自己报错"；解不开（ELOOP）的路径在上面 exists_follow 处已归到
        // "不存在"，走的是 create_directories 分支，落成 error.create_dir_failed。
        throw LpkgException(string_format("error.path_not_dir", path.string()));
    }
}

bool exists_no_follow(const fs::path& p)
{
    std::error_code ec;
    (void)fs::symlink_status(p, ec);  // lstat：末段链接**不**解引用
    return !ec;                       // 只有 lstat 成功才是"名字被占"
}

bool exists_follow(const fs::path& p)
{
    std::error_code ec;
    const bool found = fs::exists(p, ec);
    return found && !ec;  // 悬空链接/ELOOP/EACCES 下 ec 非零 → false（绝不抛）
}

bool is_directory_follow(const fs::path& p)
{
    std::error_code ec;
    return fs::is_directory(p, ec) && !ec;
}

bool is_real_directory(const fs::path& p)
{
    std::error_code ec;
    const auto st = fs::symlink_status(p, ec);
    return !ec && st.type() == fs::file_type::directory;
}

bool is_regular_file_no_follow(const fs::path& p)
{
    std::error_code ec;
    const auto st = fs::symlink_status(p, ec);
    return !ec && st.type() == fs::file_type::regular;
}

bool is_symlink_no_follow(const fs::path& p)
{
    // `symlink_status` = lstat：末段**不**解引用，且把 ELOOP/EACCES 放进 ec 而不是抛。
    // 注意这对"中间段成环"同样成立（`symlink_status("self/x")` 只报 ec，不抛）——
    // 与 `fs::is_symlink` 的关键差别就在这里（实测见 utils.hpp 那段订正）。
    std::error_code ec;
    const auto st = fs::symlink_status(p, ec);
    return !ec && st.type() == fs::file_type::symlink;
}

bool symlink_targets_equal(const fs::path& a, const fs::path& b)
{
    // 任一侧读不出（不存在／不是符号链接／中间段成环／权限不足）一律判**不同**：
    // 保守方向见头文件（判"不同"只多留一份 `.lpkgnew`，判"相同"可能放过真正改过的链接）。
    std::error_code ea;
    const fs::path ta = fs::read_symlink(a, ea);
    if (ea) return false;
    std::error_code eb;
    const fs::path tb = fs::read_symlink(b, eb);
    return !eb && ta == tb;
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
    // 同 ensure_dir_exists：判定不抛（ELOOP 会让 fs::exists 抛，见 base/path_predicates.hpp
    // 的谓词说明）
    if (!exists_follow(path)) {
        // 用 ::open 而不是 ofstream：iostreams **不保证**在失败时设置 errno，读 strerror(errno)
        // 可能打出上一次系统调用留下的陈旧 errno（误导定位）。open 失败后 errno 才是这次失败
        // 的可靠原因。mode 0666 与 ofstream 的默认创建权限一致（同样受 umask 约束）。
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0666);
        if (fd < 0) {
            throw LpkgException(string_format("error.create_file_failed", path.string()) + ": " +
                                std::strerror(errno));
        }
        ::close(fd);
    }
}

std::vector<RepoIndexVersionBlock> parse_repo_index_line(std::string_view line)
{
    std::vector<RepoIndexVersionBlock> blocks;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line[0] == '#') return blocks;

    const auto parts = split_string_view(line, constants::PIPE_CHAR);
    if (parts.size() < 2) return blocks;

    const std::string pkg_name(parts[0]);
    // ⚠️ **行内第 3 段（包级 provides）已废除**（2026-10-04，8.0.0）：`provides_soname` 拆出来之后
    // 版本块的字段数固定为 6，再留一个"包级兜底"只会让"某个字段没写"变成静默的另一种解释。
    // 这里**只读前两段**，第 3 段及之后一律忽略。

    for (auto version_info_sv : split_string_view(parts[1], constants::SEMICOLON_CHAR)) {
        if (version_info_sv.empty()) continue;

        const auto vh = split_string_view(version_info_sv, constants::COLON_CHAR);
        // **恰好 6 个字段**：`版本:哈希:依赖:provides:provides_soname:needed_so`。
        // 旧格式（4/5 字段）在这里被**拒绝**而不是被误读 —— 否则"少一个字段"会让后面的字段
        // 整体错位（例如把 provides 当成 needed_so），那正是当年两份解析器字段数不一致
        // 制造静默错误答案的老路。拒绝的后果是索引解析出 0 个包 → 落
        // `warning.repo_index_empty`，响亮地失败。
        if (vh.size() != 6) continue;
        if (vh[0].empty()) continue;  // 版本号为空的畸形块不成版本

        RepoIndexVersionBlock b;
        b.name = pkg_name;
        b.version = std::string(vh[0]);
        b.hash = std::string(vh[1]);
        b.deps = std::string(vh[2]);
        b.provides = std::string(vh[3]);
        b.provides_soname = std::string(vh[4]);
        b.needed_so = std::string(vh[5]);
        blocks.push_back(std::move(b));
    }
    return blocks;
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
 *
 * 口径说明：`fsync_and_rename` 对 fsync 失败是**抛**的，但这里**只告警、不抛**。差别在于
 * 调用位置 —— 本函数经 `fsync_parent_dir` ← `safe_rename` 挂在**事务中途**：在那里抛会把
 * 一次已经开始的 rename/事务打断，代价比"目录项可能没落盘"更高。所以持久化失败在这里是
 * **尽力而为 + 可见**：告警点名目录，让用户知道断电可能丢这些数据，但流程继续。
 * 返回值/计数口径不变（`g_durable_fsync_count` 仍是"成功的 fsync 次数"，供测试观察）；
 * 默认模式下仍然什么都不做（见 durable_fsync_enabled）。
 */
static void fsync_dir_internal(const fs::path& dir)
{
    if (!durable_fsync_enabled()) return;  // 默认模式：目录项不落盘（见 durable_fsync_enabled）
    int dir_fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        if (::fsync(dir_fd) == 0) {
            ++g_durable_fsync_count;
        } else {
            // 此前静默忽略：断电可能丢掉这个目录项（进而丢掉它下面的 rename），而调用方
            // 以为已经持久化。告警（不抛），点名目录。
            log_warning(string_format("warning.fsync_failed", dir.string()));
        }
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

bool path_resolves_within(const fs::path& dir, const fs::path& root)
{
    if (dir.empty() || root.empty()) return true;
    if (root == fs::path("/")) return true;  // 生产 root：任何绝对路径都在其内，不付检索代价
    std::error_code ec;
    const fs::path cd = fs::weakly_canonical(dir, ec);
    if (ec) return true;  // 解不开（ELOOP 等）→ 放行（见头文件）
    return path_within(cd, root);
}

bool path_within_resolved(const fs::path& p, const fs::path& root)
{
    // 空路径的含义由各调用方自己处理（WAL 侧历史上就是"放行"）——见 utils.hpp 的说明。
    if (p.empty() || root.empty()) return true;
    // ① 词法级：**复用上面那份唯一实现**（此前这里与 db/wal_op.cpp 各写了一套分量比较，
    //    只靠注释约束"两侧同一套规则"——那正是要消灭的形态）。
    if (!path_within(p, root)) return false;

    const fs::path r = strip_trailing_slash(root.lexically_normal());
    const fs::path q = p.lexically_normal();
    if (q == r) return true;  // p 就是 root 自身：其父目录落在 root 之外很正常

    // ② canonical 复核：**只解析父目录，末段一律不解析**（理由见 utils.hpp）。
    std::error_code ec_r;
    std::error_code ec_q;
    const fs::path cr = fs::weakly_canonical(r, ec_r);
    const fs::path cq = fs::weakly_canonical(q.parent_path(), ec_q);
    if (ec_r || ec_q) return true;  // 解不开（ELOOP / 中间段未重建）→ 不判越界
    return path_within(cq, cr);
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
    // 复用 `strip_trailing_slash` 这**唯一**一份"剥尾斜杠"实现：这里只是先 `lexically_normal`
    // （近重复两处极易漂移 —— 改一处不改另一处不会有任何编译错误）。
    return strip_trailing_slash(p.lexically_normal());
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
    // 判定不抛：/tmp 是符号链接环时 `fs::exists` 会抛，而这里是启动期的清理兜底
    // （见 utils.hpp 的不抛谓词说明）。**跟随**语义与原来的 exists+is_directory 一致
    // （/tmp 可以是指向 /var/tmp 的符号链接）。
    if (!is_directory_follow(tmp_path)) return;

    for (const auto& entry : fs::directory_iterator(tmp_path)) {
        try {
            // 一次 lstat 的"真目录"判据（2026-09-26 修）：原来的
            // `fs::is_symlink(p) || !entry.is_directory()` 两个操作数**都会在环上抛** ——
            // 前者对中间段成环抛、后者（`directory_entry::is_directory()` 走 `status()`）
            // 对末段成环抛。而本函数跑在清理路径上：一次抛 = 包已落地、DB 已提交、命令报失败。
            if (!is_real_directory(entry.path())) continue;
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
        } catch (const std::invalid_argument&) {  // NOLINT(bugprone-empty-catch) — 名字不是 PID
            // 非 PID 命名的 lpkg_* 目录——忽略，不删除
        } catch (const std::exception& e) {
            log_warning(string_format("warning.cleanup_old_tmp_failed", entry.path().string()) +
                        ": " + e.what());
        }
    }
}

bool is_stash_dir_name(std::string_view name)
{
    return name.starts_with(constants::SUFFIX_LPKG_BAK);
}

/**
 * 回收孤儿备份 stash（历史 TODO.md §5）：崩溃/续传没清掉的
 * `<fsroot>/.lpkg_bak_<pkg>_<pid>`。扫描范围有界：root_dir 顶层 + 顶层子目录里
 * st_dev 与 root_dir 不同的（= 子挂载点）的直接子目录。pid 已死（kill ESRCH）才删，
 * 绝不碰自己/存活进程的 stash。stash 正常由 CLEANUP 清除，本函数只是兜底安全网。
 *
 * ⚠️ 这里**只按名字**认 stash：包若能在 `<fsroot>` 顶层装出一个
 * `.lpkg_bak_<任意>_<已死 pid>/` 目录，它会被本函数连同里面的文件一起删掉（盘面与
 * `files.db` 当场脱节）。挡在入口的是**归档成员名守卫**
 * （`constants::RESERVED_MEMBER_NAMES` 里的 `.lpkg_bak_` 前缀那条）：本层没有更硬的判据可
 * 用 —— "这真的是个 stash 吗"在盘上没有可分辨的特征，而本层（`base/`）也不许反向依赖 `db/`
 * 去查包归属。所以这条防线是**入口那道**，不是这里。
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
            if (!is_stash_dir_name(name)) continue;
            // 同上（2026-09-26 修）：两个操作数在环上都抛；换成一次 lstat 的真目录判据。
            if (!is_real_directory(p)) continue;
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
        // 同上（2026-09-26 修）。
        if (!is_real_directory(p)) continue;
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
