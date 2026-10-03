/**
 * test_term.cpp — `ui/term` 的**纯函数**不变量（原地刷新不花屏的那条）
 *
 * 核心不变量（对齐 pacman 的 `cb_progress` / `fill_progress`）：**每一帧都恰好占满整行**。
 * 只要每帧等宽，前几帧更长的部分**不可能**留在屏上；反之（"新帧比旧帧短就补空格"）在宽字符
 * （中文占 2 列）下会**少补**，于是出现 "MiB MiB" 这类叠字 —— 这正是 `compose_frame()`
 * 存在的理由，也是本文件要钉住的东西。
 *
 * 测试环境**不是 TTY**（gtest 在容器里跑），所以 `Line` 的原地刷新分支走不到；
 * 能测的、也最该测的，是 `compose_frame` / `bar_text` / `visible_len` / `truncate_width`
 * 这些**纯函数**。多数断言按"自洽"写（`visible_len(结果) == 预期列数`）；但宽字符那一条
 * 可以写**精确值**：`visible_len` 按 `wcwidth()` 计列，而测试容器是 alpine/musl —— musl 的
 * 宽字符分类与 locale 无关、UTF-8 恒可用，CJK 每字恰 2 列（见
 * `VisibleLenSkipsAnsiAndCountsColumns`）。 在无 UTF-8 locale 的 glibc 上 `wcwidth` 会退 1
 * 列，那条断言会红 —— 那不是本套件的目标环境。
 */

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

#include "ui/term.hpp"

namespace
{
constexpr const char* kAnsiGreen = "\033[1;32m";
constexpr const char* kAnsiReset = "\033[0m";
}  // namespace

TEST(TermTest, VisibleLenSkipsAnsiAndCountsColumns)
{
    EXPECT_EQ(ui::visible_len(""), 0u);
    EXPECT_EQ(ui::visible_len("abc"), 3u);
    EXPECT_EQ(ui::visible_len(std::string(kAnsiGreen) + "x" + kAnsiReset), 1u);
    // 宽字符按 wcwidth() 计：CJK 每字 2 列 ⇒ "中文" 恰 4 列。测试容器是 alpine/musl（宽字符
    // 分类与 locale 无关、UTF-8 恒可用），故这里有精确值可钉 —— 原先的 EXPECT_GE(..., 2u)
    // 只挡住"比 ASCII 窄"，会把"少算成每字 1 列 / 误按字节数计"这类真实回归放过去。
    EXPECT_EQ(ui::visible_len("中文"), 4u);
}

TEST(TermTest, TruncateWidthNeverExceedsBudgetAndKeepsPrefix)
{
    const std::string s = "==> 正在解压 libdep";
    for (std::size_t n : {0u, 1u, 7u, 12u, 100u}) {
        const std::string t = ui::truncate_width(s, n);
        EXPECT_LE(ui::visible_len(t), n) << "n=" << n << " -> " << t;
        EXPECT_EQ(s.compare(0, t.size(), t), 0) << "截断结果不是原串前缀（劈开了 UTF-8？）";
    }
    EXPECT_EQ(ui::truncate_width(s, 1000), s);
}

TEST(TermTest, BarTextOccupiesExactlyTheRequestedColumns)
{
    for (int w : {8, 10, 20, 40, 63}) {
        for (int pct : {0, 1, 33, 50, 99, 100}) {
            const std::string b = ui::bar_text(pct, w);
            EXPECT_EQ(ui::visible_len(b), static_cast<std::size_t>(w))
                << "pct=" << pct << " w=" << w << " -> " << b;
        }
    }
    // 太窄：pacman 连括号都不画，只留百分比
    EXPECT_EQ(ui::visible_len(ui::bar_text(50, 4)), 4u);
}

TEST(TermTest, BarTextFillIsMonotone)
{
    EXPECT_EQ(ui::bar_text(0, 20).find('#'), std::string::npos);
    EXPECT_EQ(ui::bar_text(100, 20).find('-'), std::string::npos);
    // 50% 时两类字符都在
    const std::string half = ui::bar_text(50, 21);  // 奇数宽保证两侧都有
    EXPECT_NE(half.find('#'), std::string::npos);
    EXPECT_NE(half.find('-'), std::string::npos);
}

// ── 本文件的核心：**每帧等宽**（旧帧不留残影）────────────────────────────────────

TEST(TermTest, ComposeFrameAlwaysFillsExactlyTheLine)
{
    const std::size_t total = 79;  // 模拟 COLUMNS=80
    for (const std::string& left : {std::string("==> Installing foo 1.0"), std::string(""),
                                    std::string("==> 正在安装 foo 1.0"),
                                    std::string("==> a-very-long-package-name-that-cannot-fit")}) {
        const std::string f = ui::compose_frame(left, "[OK]", total);
        EXPECT_EQ(ui::visible_len(f), total) << "左文本=" << left;
        EXPECT_EQ(f.compare(f.size() - 4, 4, "[OK]"), 0) << "右端状态必须贴在最右侧：" << f;
    }
}

TEST(TermTest, ShorterFrameCannotLeaveResidueFromLongerFrame)
{
    // 这是用户实测报过的那类 bug：上一帧长、下一帧短 → 旧字符留在屏上。
    // 只要两帧都恰好 total 列，就不可能发生。
    const std::size_t total = 60;
    const std::string long_frame = ui::compose_frame(
        "==> Installing something (1234567890/9876543210 files)", ui::bar_text(37, 20), total);
    const std::string short_frame = ui::compose_frame("==> OK", ui::bar_text(100, 20), total);
    EXPECT_EQ(ui::visible_len(long_frame), total);
    EXPECT_EQ(ui::visible_len(short_frame), total);
    // 短帧后面全是空格补齐（不是"只有它自己那么长"）
    ASSERT_GE(short_frame.size(), total);
    EXPECT_EQ(short_frame.substr(ui::visible_len("==> OK"), 4), "    ");
}

TEST(TermTest, ComposeFrameTruncatesLeftWithEllipsisBeforeTail)
{
    const std::size_t total = 32;
    const std::string f = ui::compose_frame("==> a-very-long-package-name-here", "[FAILED]", total);
    EXPECT_EQ(ui::visible_len(f), total);
    EXPECT_NE(f.find("..."), std::string::npos) << "放不下时左文本该以 ... 收尾：" << f;
    EXPECT_EQ(f.compare(f.size() - 8, 8, "[FAILED]"), 0) << f;
}

TEST(TermTest, ComposeFrameNeverOverflowsWhenTailLeavesNoRoomForEllipsis)
{
    // 预算 keep = total_cols - tail_w ≤ 3 时放不下 "..."（占 3 列）。旧实现无条件补 "..."，
    // 于是整帧 = 3 + tail_w > total_cols —— 又一次"旧帧留残影"。这里钉住"绝不超宽"。
    for (std::size_t total : {5u, 6u, 7u}) {  // tail "[OK]"=4 列 ⇒ keep = 1/2/3
        const std::string f = ui::compose_frame("==> a-very-long-package-name", "[OK]", total);
        EXPECT_EQ(ui::visible_len(f), total) << "total=" << total << " -> " << f;
        EXPECT_EQ(f.compare(f.size() - 4, 4, "[OK]"), 0) << "右端状态必须贴在最右侧：" << f;
    }
}

TEST(TermTest, HumanReadableHelpers)
{
    EXPECT_EQ(ui::human_size(0), "0 B");
    EXPECT_EQ(ui::human_size(512), "512 B");
    EXPECT_EQ(ui::human_size(1024), "1.0 KiB");
    EXPECT_EQ(ui::human_size(1536), "1.5 KiB");
    EXPECT_EQ(ui::human_size(1024 * 1024 * 3 / 2), "1.5 MiB");
    EXPECT_EQ(ui::human_rate(0.0), "-");
    EXPECT_NE(ui::human_rate(1024.0 * 4).find("/s"), std::string::npos);
    EXPECT_EQ(ui::human_time(0), "00:00");
    EXPECT_EQ(ui::human_time(65), "01:05");
    EXPECT_EQ(ui::human_time(3661), "1:01:01");
    EXPECT_EQ(ui::human_time(-1), "--:--");
}

TEST(TermTest, NonTtyDegradesToPercentOnly)
{
    // 测试进程不是 TTY → 进度条不刷字符画（日志里几十个 #/- 是纯噪音）。
    ASSERT_FALSE(ui::redraw()) << "本用例的前提是「非 TTY」——在真 TTY 下跑这一步会不成立";
    EXPECT_EQ(ui::bar(40.0, 40), "40%");
    EXPECT_EQ(ui::bar(100.0, 40), "100%");
}

TEST(TermTest, ProgressBarsShareOneColumnLayout)
{
    // pacman 的 `draw_pacman_progress_bar`：信息段固定 max(60% 列数, 50)、进度条固定占剩下
    // 的部分 —— 于是**每一行的条起止列都相同**，多行叠起来右端是齐的。
    // （早期版本让条"填满左文本之后的剩余空间"，每行条长都不一样、右端参差。）
    const std::size_t total = 149;
    const std::string a = ui::progress_frame("==> Installing foo 1.0", "427/427 files", 62, total);
    const std::string b =
        ui::progress_frame("==> Downloading bar-2.0.lpkg", "1.2 MiB  4.5 MiB/s  00:02", 12, total);
    const std::string c =
        ui::progress_frame("==> 正在解压 一个很长很长很长很长的包名", "", 100, total);

    const auto bar_span = [](const std::string& s) {
        const auto i = s.find(" [");
        const auto j = s.rfind(']');
        return std::pair{ui::visible_len(s.substr(0, i == std::string::npos ? 0 : i + 1)),
                         ui::visible_len(s.substr(0, j == std::string::npos ? 0 : j + 1))};
    };
    EXPECT_EQ(bar_span(a), bar_span(b)) << "两行的进度条起止列必须相同（pacman 的固定版式）";
    EXPECT_EQ(bar_span(b), bar_span(c));
    for (const std::string& f : {a, b, c}) EXPECT_EQ(ui::visible_len(f), total) << f;
}

TEST(TermTest, ProgressFrameNeverOverflowsWhenBudgetLeavesNoRoomForEllipsis)
{
    // total=28 时 info_cols=16（被 total-kMinBarCols 钳住）、bar_cols=12。mid 较长则信息段
    // 留给左文本的预算 budget_for_left < 3（甚至为 0）——放不下 "..."。旧实现无条件补 "..."
    // 会把信息段撑过 info_cols ⇒ 整帧超宽（进度条那侧固定，多出的列全在左边）。钉住"绝不超宽"。
    const std::size_t total = 28;
    const std::string left = "==> Installing a-very-long-package-name";
    for (std::size_t mid_len : {13u, 14u, 15u, 16u}) {  // budget = 2/1/0/0
        const std::string mid(mid_len, 'x');
        const std::string f = ui::progress_frame(left, mid, 42, total);
        EXPECT_EQ(ui::visible_len(f), total) << "mid_len=" << mid_len << " -> " << f;
    }
}

// `Line::progress()` 的 `mid` **必须按值收**（2026-10-03 修，实测缺陷）。
//
// 缺陷形态：`finish_progress()` 会把自己的 `last_mid_` **当视图**再喂回 `progress()`，而
// `progress()` 的第一句是把 mid 存回 `last_mid_` —— 如果形参是 `string_view`，这一存就会清掉
// 它正在读的那块缓冲（首字节写 `\0`），随后 `progress_frame()` 读到的就是同一块已被改过的内存。
// 实测症状（真机 pty，同一个 filesystem 包）：
//     修前 `==> 正在解压 filesystem   <NUL>63.3 KiB / 863.3 KiB [...] 100%`   ← 57 帧里 1 帧
//     修后 57 帧全部完好（`863.3 KiB / 863.3 KiB`）
// 长的 mid 才中招（> SSO 阈值 15 字节才走堆缓冲）：解压/下载的 `863.3 KiB / 863.3 KiB` 被写坏，
// 而安装行的 `46/46 个文件`（恰好 15 字节，SSO 内）一直完好 —— 这也解释了"为什么只有某一类行花"。
//
// 这条 static_assert 钉的是**修复的实质**（按值收）。它能证明什么：签名一旦被改回
// `string_view`（= 把别名重新放回来），这里**编不过**。不能证明什么：它不覆盖渲染结果本身 ——
// 非 TTY 下 `render()` 直接返回，帧的内容在本套件里观察不到，所以帧那一层靠"真机 pty 复现"
// 验证（见上），而不是靠本套件。
static_assert(
    std::is_same_v<decltype(&ui::Line::progress), void (ui::Line::*)(double, std::string)>,
    "Line::progress 的 mid 必须按值收：按 string_view 收会让 finish_progress() 的"
    "自别名赋值改掉正在被读的缓冲（2026-10-03 实测的渲染损坏）");
