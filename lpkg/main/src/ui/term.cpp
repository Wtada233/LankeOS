#include "ui/term.hpp"

#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>

#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>

#include "base/constants.hpp"
#include "base/utils.hpp"

namespace
{
std::mutex g_out_mutex;

/// 进度条**最少**保留的列数 —— `progress_frame()` 用它给条留位置。
///
/// 2026-10-03 修：这个常量原先写着 20 却**从来没被引用过**，而实现里硬编的是 **12**
/// （同一件事两处表达、其中一处是死的 —— 改一处不改另一处不会有任何编译错误）。
/// 现在它是**唯一出处**：取实现既有的 12（那是有意的，见 `progress_frame` 里的注释），
/// 不是常量上原来写的 20 —— 常量上的数字从来没生效过。
/// 是 `make tidy` 第一轮跑出来的（`clang-diagnostic-unused-const-variable`；
/// gcc 的 `-Wall -Wextra` 对 C++ 里文件作用域未使用的 const 变量**不报**）。
constexpr int kMinBarCols = 12;

/// 一行可用的总列数：留最后一列不打（不然很多终端会在写满时自动换行）。
int line_cols()
{
    const int w = ui::width();
    return w > 1 ? w - 1 : 1;
}

/**
 * 一次性 `setlocale(LC_CTYPE, "")`：`wcwidth()` 在非 C locale 下才能对宽字符给出 2。
 * 只动 LC_CTYPE（不动 LC_NUMERIC 等，避免影响数值格式化）。
 */
bool locale_ready()
{
    static const bool ok = std::setlocale(LC_CTYPE, "") != nullptr;
    return ok;
}

/// 解一个 UTF-8 码点；失败按单字节兜底。返回消耗的字节数。
std::size_t utf8_next(std::string_view s, std::size_t i, std::uint32_t& cp)
{
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) {
        cp = b0;
        return 1;
    }
    std::size_t need = 0;
    std::uint32_t v = 0;
    if ((b0 & 0xE0) == 0xC0) {
        need = 1;
        v = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        need = 2;
        v = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        need = 3;
        v = b0 & 0x07;
    } else {
        cp = b0;  // 非法起始字节 → 当 1 字节处理
        return 1;
    }
    if (i + need > s.size() - 1) {  // 截断的序列
        cp = b0;
        return 1;
    }
    for (std::size_t k = 1; k <= need; ++k) {
        const auto bk = static_cast<unsigned char>(s[i + k]);
        if ((bk & 0xC0) != 0x80) {  // 续字节不合法
            cp = b0;
            return 1;
        }
        v = (v << 6) | (bk & 0x3F);
    }
    cp = v;
    return need + 1;
}

/// 一个可见段（跳过 ANSI 转义）的**显示列宽**。
std::size_t width_of_visible(std::string_view s)
{
    locale_ready();
    std::size_t cols = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            std::size_t j = i + 2;
            while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) ++j;  // 到最终字节
            i = (j < s.size()) ? j + 1 : s.size();
            continue;
        }
        std::uint32_t cp = 0;
        const std::size_t n = utf8_next(s, i, cp);
        if (cp < 0x80) {
            ++cols;  // ASCII 快路径（也避开 wcwidth 对控制字符返回 -1）
        } else {
            const int w = ::wcwidth(static_cast<wchar_t>(cp));
            cols += (w > 0) ? static_cast<std::size_t>(w) : (w == 0 ? 0 : 1);
        }
        i += n;
    }
    return cols;
}

bool env_no_color()
{
    return std::getenv("NO_COLOR") != nullptr;
}

/// 环境变量 `COLUMNS`（合法且 ≥ 20 时采用）——给非 TTY 的对齐留一个可控入口（也方便测试）。
int env_columns()
{
    if (const char* c = std::getenv("COLUMNS")) {
        char* end = nullptr;
        errno = 0;
        const long v = std::strtol(c, &end, 10);
        // 溢出时 strtol 返回 LONG_MAX/LONG_MIN 并置 ERANGE，下面的范围检查本就会拒掉它
        // （v >= 100000 或 v < 20）；显式判 errno 只是不让"拒绝"依赖 LONG_MAX 的具体大小。
        if (errno == 0 && end && *end == '\0' && v >= 20 && v < 100000) return static_cast<int>(v);
    }
    return 0;
}

/// percent → [0,100] 的整数百分比。
///
/// **先夹到合法范围再取整**：`std::lround(NaN/±Inf)` 是未定义行为，而超出 `long` 范围的
/// 有限值同样未指定返回 —— 一旦发生，`bar_text` 里 `pct * hashlen / 100` 会算出越界长度、
/// 甚至负的长度去 `append`。夹取对正常路径（percent ∈ [0,100]）逐字无影响：
/// NaN/-Inf/负数 → 0，+Inf/超范围 → 100。
int percent_to_int(double percent)
{
    double p = percent;
    if (!(p >= 0.0)) p = 0.0;
    if (p > 100.0) p = 100.0;
    return static_cast<int>(std::lround(p));
}
}  // namespace

namespace ui
{

bool redraw()
{
    // **有意**只判定一次并缓存：`isatty(STDOUT_FILENO)` 在单个进程的生存期内不会变，
    // 每帧都去问一次系统调用没有意义（进度条每帧都会调它）。这也是各用例依赖的前提 ——
    // 同一进程内"是不是 TTY"是常量，测试才对输出形态有确定预期。
    static const bool v = ::isatty(STDOUT_FILENO) != 0;
    return v;
}

bool colors()
{
    // 同上：TTY 判定 + NO_COLOR 都是**进程级常量**，缓存一次即可（NO_COLOR 中途改变不在
    // 支持范围；每次重画都读环境变量只会白费功夫）。
    static const bool v = redraw() && !env_no_color();
    return v;
}

int width()
{
    if (const int c = env_columns(); c > 0) return c;
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20) return ws.ws_col;
    return 80;
}

std::size_t visible_len(std::string_view text)
{
    return width_of_visible(text);
}

std::string_view step_prefix()
{
    return "==> ";
}

std::string truncate_width(std::string_view text, std::size_t max_cols)
{
    locale_ready();
    std::size_t cols = 0;
    std::size_t i = 0;
    std::size_t end = 0;  // 可以安全取到的字节边界
    while (i < text.size()) {
        if (text[i] == '\033' && i + 1 < text.size() && text[i + 1] == '[') {
            std::size_t j = i + 2;
            while (j < text.size() && !(text[j] >= '@' && text[j] <= '~')) ++j;
            i = (j < text.size()) ? j + 1 : text.size();
            end = i;
            continue;
        }
        std::uint32_t cp = 0;
        const std::size_t n = utf8_next(text, i, cp);
        const std::size_t w =
            cp < 0x80 ? 1
                      : (::wcwidth(static_cast<wchar_t>(cp)) > 0
                             ? static_cast<std::size_t>(::wcwidth(static_cast<wchar_t>(cp)))
                             : 1);
        if (cols + w > max_cols) break;
        cols += w;
        i += n;
        end = i;
    }
    return std::string(text.substr(0, end));
}

std::string paint(std::string_view ansi_code, std::string_view text)
{
    if (!colors()) return std::string(text);
    std::string out;
    out.reserve(ansi_code.size() + text.size() + 5);
    out.append(ansi_code);
    out.append(text);
    out.append(constants::COLOR_RESET);
    return out;
}

std::string human_size(std::uint64_t bytes)
{
    static constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    if (u == 0)
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else
        std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    return buf;
}

std::string human_rate(double bytes_per_sec)
{
    // 非有限值（±Inf/NaN）与 0/负数走同一条"算不出来"的路：`static_cast<uint64_t>` 对
    // Inf **和**出界的有限值都是未定义行为（2026-10-03）。输入来自下载回调的速率。
    if (!(bytes_per_sec > 0.0) || !std::isfinite(bytes_per_sec)) return "-";
    constexpr double kMaxBytes = 9e18;  // < 2^64，给转换留余量
    if (bytes_per_sec > kMaxBytes) bytes_per_sec = kMaxBytes;
    return human_size(static_cast<std::uint64_t>(bytes_per_sec)) + "/s";
}

std::string human_time(double seconds)
{
    if (!(seconds >= 0.0) || !std::isfinite(seconds)) return "--:--";
    // `std::llround` 而不是 `static_cast<long long>(seconds + 0.5)`：后者正是
    // `bugprone-incorrect-roundings` 那一类 —— `x + 0.5` 在 `x = 0.49999999999999994`
    // 这种"加完正好进位"的值上会先舍入到 1.0 再取整，给出比四舍五入大 1 的结果。
    // （上面的 `!(seconds >= 0.0)` 已挡掉 NaN 与负数，所以这里只剩这一个边角。）
    // 也是 `make tidy` 第一轮跑出来的。
    const auto total = std::llround(seconds);
    char buf[32];
    if (total >= 3600)
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", total / 3600, (total / 60) % 60,
                      total % 60);
    else
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld", total / 60, total % 60);
    return buf;
}

std::string bar_text(double percent, int width_cols)
{
    const int pct = percent_to_int(percent);

    char pct_buf[8];
    std::snprintf(pct_buf, sizeof(pct_buf), "%3d%%", pct);
    // pacman 的 `fill_progress`：整段恰好占 width_cols 列 —— " [" + 填充 + "] " + "%3d%%"。
    // 8 = 1(空格) + 1([) + 1(]) + 1(空格) + 4("%3d%")；宽度不足 8 时连括号都不画（pacman 同）。
    if (width_cols < 8) return pct_buf;
    const int hashlen = width_cols - 8;
    const int hash = pct * hashlen / 100;
    std::string out = " [";
    out.append(static_cast<std::size_t>(hash), '#');
    out.append(static_cast<std::size_t>(hashlen - hash), '-');
    out += "] ";
    out += pct_buf;
    return out;
}

std::string bar(double percent, int width_cols)
{
    // 非重绘终端：不画字符画（日志里几十个 #/- 是纯噪音），只报百分比。
    if (redraw()) return bar_text(percent, width_cols);
    return std::to_string(percent_to_int(percent)) + "%";
}

std::string compose_frame(std::string left, std::string_view tail, std::size_t total_cols)
{
    const auto tail_w = visible_len(tail);
    // 右端（状态/进度条）优先保完整；左文本放不下就按**显示列**截断（不劈 UTF-8/宽字符）。
    if (visible_len(left) + tail_w > total_cols && tail_w < total_cols) {
        const auto keep = total_cols - tail_w;
        // 省略号占 **3 列**，必须计入预算：keep>3 时"预算-3 列文本 + ..."恰好等于 keep；
        // keep≤3 时放不下省略号，直接截到预算、**不补 "..."**（旧写法无条件补，keep≤3 会让
        // 整帧超出 total_cols —— 那正是"旧帧留残影"要防的那件事）。
        if (keep > 3) {
            left = truncate_width(left, keep - 3) + "...";
        } else {
            left = truncate_width(left, keep);
        }
    }
    const auto used = visible_len(left) + tail_w;
    if (used < total_cols) left.append(total_cols - used, ' ');  // ← 恒补齐：旧帧绝不留残影
    left += tail;
    return left;
}

std::string progress_frame(std::string left, std::string_view mid, double percent,
                           std::size_t total_cols)
{
    // 太窄：退化成 `左文本 mid 62%`（不画条，pacman 在 cols 很小时也直接砍掉整段）。
    if (total_cols < 28) {
        std::string out = std::move(left);
        if (!mid.empty()) out += " " + std::string(mid);
        out += " " + bar_text(percent, 0);
        return out;
    }

    // pacman：信息段 = max(60% 列数, 50) 列，进度条 = 剩下的列 —— **每行的条一样长**。
    int info_cols = static_cast<int>(total_cols) * 6 / 10;
    if (info_cols < 50) info_cols = 50;
    if (info_cols > static_cast<int>(total_cols) - kMinBarCols)  // 保住条至少 kMinBarCols 列
        info_cols = static_cast<int>(total_cols) - kMinBarCols;
    const int bar_cols = static_cast<int>(total_cols) - info_cols;

    // 信息段：左文本靠左、mid 靠右，整段恰好 info_cols 列（放不下先截左文本、加 ...）。
    std::string info = std::move(left);
    const auto mid_w = visible_len(mid);
    auto budget_for_left = static_cast<std::size_t>(info_cols);
    if (mid_w > 0) {
        budget_for_left = (static_cast<int>(mid_w) + 1 < info_cols)
                              ? static_cast<std::size_t>(info_cols) - mid_w - 1
                              : 0;
    }
    if (visible_len(info) > budget_for_left) {
        // 同 compose_frame：省略号占 3 列要算进预算；budget≤3 时截到预算即可、不补 "..."，
        // 否则信息段会被撑过 info_cols ⇒ 整帧超宽（进度条那一侧固定，多出来的列全在左边）。
        if (budget_for_left > 3) {
            info = truncate_width(info, budget_for_left - 3) + "...";
        } else {
            info = truncate_width(info, budget_for_left);
        }
    }

    const auto used = visible_len(info) + mid_w;
    if (used < static_cast<std::size_t>(info_cols))
        info.append(static_cast<std::size_t>(info_cols) - used, ' ');
    info += mid;

    return info + bar_text(percent, bar_cols);
}

std::string ok(bool success)
{
    return paint(success ? constants::COLOR_GREEN : constants::COLOR_RED,
                 success ? "[OK]" : "[FAILED]");
}

std::string skipped()
{
    return paint(constants::COLOR_YELLOW, "[SKIPPED]");
}

Line::Line() : done_(true)
{
}  // 惰性：不输出、不换行

Line::Line(std::string left) : left_(std::string(step_prefix()) + std::move(left)), tty_(redraw())
{
    // 统一加 `==> ` 前缀（systemd 风格的动作行）——下载/解压/安装/钩子/触发器都走这里，
    // 于是它们与 `::` 开头的普通信息行一眼可分。
    if (tty_) {
        std::lock_guard lock(g_out_mutex);
        std::cout << '\r' << left_ << "\033[K" << std::flush;
        drawn_ = true;
    }
}

Line::Line(Line&& other) noexcept
    : left_(std::move(other.left_)),
      last_mid_(std::move(other.last_mid_)),
      tty_(other.tty_),
      drawn_(other.drawn_),
      done_(other.done_)
{
    other.done_ = true;  // 被搬走的对象不再负责收尾（否则会多打一个换行）
    other.drawn_ = false;
}

Line& Line::operator=(Line&& other) noexcept
{
    if (this != &other) {
        left_ = std::move(other.left_);
        last_mid_ = std::move(other.last_mid_);
        tty_ = other.tty_;
        drawn_ = other.drawn_;
        done_ = other.done_;
        other.done_ = true;
        other.drawn_ = false;
    }
    return *this;
}

Line::~Line()
{
    if (done_) return;
    // 异常展开时自动收成 `[FAILED]`（scope-failure 惯用法）：调用点不必为每个 throw 记得
    // 收尾，半截的进度行也不会留在终端上。
    finish(std::uncaught_exceptions() > 0 ? ok(false) : std::string_view{});
}

void Line::render(std::string left, std::string_view tail)
{
    if (!tty_) return;  // 非 TTY：什么都不画，finish() 时一次性落一行

    std::string out = compose_frame(std::move(left), tail, static_cast<std::size_t>(line_cols()));
    std::lock_guard lock(g_out_mutex);
    // `\r` 回列 0；`ESC[K` 兜住"行超宽 / 终端宽度变了"——多一层保险，代价 4 字节。
    std::cout << '\r' << out << "\033[K" << std::flush;
    drawn_ = true;
}

void Line::progress(double percent, std::string mid)
{
    if (done_) return;
    // 版式全部交给 `progress_frame()`（纯函数、可测）：信息段固定 60%、进度条固定占剩下的
    // 部分 —— 同一终端上**每一行的条一样长**，多行叠起来右端是齐的（pacman 的口径）。
    //
    // ⚠️ `mid` **按值收是有意的**（2026-10-03 修）：它曾经是 `std::string_view`，而
    // `finish_progress()` 恰好把 `last_mid_` **自己**当视图喂回来 —— 下面那句赋值会把
    // `last_mid_` 的旧缓冲清空（首字节写 `\0`），于是紧接着 `progress_frame()` 透过那个
    // 视图读到的就是**同一块已被改过的内存**。实测症状：解压行的最后一帧把 mid 印成
    // `"\0" + "63.3 KiB / 863.3 KiB"`（原本是 `863.3 KiB / 863.3 KiB`）—— 终端丢弃那个
    // NUL，整行比别的行**少一列**、`863.3` 显示成 `63.3`。
    //
    // 为什么只有它中招：短 mid 落在 `std::string` 的 SSO 缓冲里（不分配堆），长的才走堆
    // —— 安装行的 `46/46 个文件`（15 字节）完好，解压/下载行的长 mid 才被写坏。
    // 按值收 ⟹ 形参永远是一份**独立对象**，别名从类型上不可能；顺带比原来少一次拷贝
    // （调用点传的多是临时串，现在是移动进来）。
    last_mid_ = std::move(mid);
    // 用成员渲染（形参已被 move 空）：这正是"独立副本"的意义所在 —— 被清的是**旧**缓冲。
    render({}, progress_frame(left_, last_mid_, percent, static_cast<std::size_t>(line_cols())));
}

void Line::right(std::string_view text)
{
    if (done_) return;
    render(left_, std::string(text));
}

void Line::finish(std::string_view right)
{
    if (done_) return;
    done_ = true;
    if (!tty_) {
        // 降级：一行纯文本。右状态为空（如 `finish({})`）就不补空格。
        std::lock_guard lock(g_out_mutex);
        std::cout << left_;
        if (!right.empty()) std::cout << ' ' << right;
        std::cout << '\n';
        return;
    }
    render(left_, std::string(right));
    std::lock_guard lock(g_out_mutex);
    std::cout << '\n' << std::flush;
}

void Line::finish_progress()
{
    if (done_) return;
    if (tty_) {
        // 用**最后一帧的 mid** 重画到 100%（条宽由同一份版式算出，不会忽然变长/变短），再换行。
        progress(100.0, last_mid_);
        done_ = true;
        std::lock_guard lock(g_out_mutex);
        std::cout << '\n' << std::flush;
        return;
    }
    done_ = true;
    std::lock_guard lock(g_out_mutex);
    std::cout << left_ << ' ' << bar(100.0, 0) << '\n';  // 非 TTY：一行纯文本
}

void section(std::string_view title)
{
    // pacman 风格：`:: <标题>`。走 `log_info` 是为了与普通信息行**同形同色**
    // （`info.log_prefix` + 同样的着色），不另立一套分割符 —— 早期版本打的是
    // `====== title ======`，用户明确要求换成 pacman 的 `::`。
    log_info(title);
}

}  // namespace ui
