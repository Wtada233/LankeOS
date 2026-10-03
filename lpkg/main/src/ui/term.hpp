#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/**
 * ui/term — 终端输出（进度条 / 单行状态 / 阶段分隔条）。
 *
 * 与 `base/utils.hpp` 的 `log_info/log_warning/log_error` 分工：
 *   · 那三个是**逐条落行**的通用日志（一行一条，机器可读优先）；
 *   · 本模块管**会原地刷新的动态输出**（进度条、`[OK]` 状态）与**阶段分隔条**。
 *
 * 铁律：**非 TTY（管道 / 日志 / CI / farm 不带 -t 的 docker exec）一律降级为纯文本**——
 * 不写 `\r`、不写 ANSI、不写字符画进度条。理由：lpkg 的输出会被 farm 流式转发进日志，
 * `\r` 与色码会污染日志且不可解析。降级后每条 step 仍是**一行**，只是没有原地刷新。
 *
 * 颜色判据与 `farm/src/ux.rs` 一致：**是 TTY 且未设 `NO_COLOR`**。
 *
 * ── 原地刷新的正确性（对齐 pacman 的 `fill_progress` / `cb_progress`）─────────────
 * pacman 的做法是：**每一帧都写满整行**——信息段用 `%-*s` 补齐到 `infolen`（= max(60% 列数,
 * 50)），进度条段再补齐到 `cols - infolen`，于是任意两帧**长度相同**，旧帧不会被留下残影；
 * 行尾再打 `\r` 让光标回到列 0。行太长时它先截断信息段（加 `...`）也**绝不缩短整行**。
 *
 * 本模块照此实现：`Line::render()` **恒把内容补齐到 `width()-1` 个显示列**，并在末尾补一个
 * `ESC[K`（擦到行尾）兜住"终端宽度变化 / COLUMNS 说谎 / 行本身超宽"这几种情况。
 * **显示列**（不是字节、也不是码点）由 `visible_len()` 给出——中文等宽字符占 2 列，
 * 按字节或码点补空格会**少补**（zh 文案下就是这一条会露残影）。详见 `visible_len`。
 */
namespace ui
{

/// 颜色是否启用（TTY 且未设 `NO_COLOR`）。
bool colors();
/// 是否可做"单行原地刷新"（`\r`）：stdout 是 TTY。
bool redraw();
/// 终端宽度：`COLUMNS` 环境变量 → `ioctl(TIOCGWINSZ)` → 80。下限 20。
int width();

/// ANSI 上色；`colors()==false` 时原样返回。所有色码自带 reset（绝不泄漏配色）。
std::string paint(std::string_view ansi_code, std::string_view text);

/// 动作行前缀（`"==> "`）。`ui::Line` 会自动加；**手工拼行**的地方（如 git 传输进度用
/// `\r` 自绘）用它，免得前缀散落成字面量、与 Line 的形态漂移。
std::string_view step_prefix();

/// 人类可读字节数（1024 进制 + 1 位小数，如 `968.5 KiB`）。
std::string human_size(std::uint64_t bytes);
/// 人类可读速率（如 `4.57 MiB/s`）。
std::string human_rate(double bytes_per_sec);
/// `mm:ss`（pacman 的 ETA 写法）。
std::string human_time(double seconds);

/**
 * 文本占用的**显示列数**：跳过 ANSI 转义序列，按 `wcwidth()` 计宽字符
 * （CJK 占 2 列、组合字符占 0 列；未知码点按 1 列兜底）。
 *
 * 右对齐补空格必须按它算 —— 按 `size()`（字节）或码点数会**少补**（zh 文案全是宽字符），
 * 少补的后果正是"旧帧更长的部分留在屏上"。
 */
std::size_t visible_len(std::string_view text);

/// 截断到最多 @p max_cols 个显示列（不劈开 UTF-8 序列 / 不切半个宽字符）。
std::string truncate_width(std::string_view text, std::size_t max_cols);

/**
 * 进度条的**字符画**形态（对齐 pacman 的 `fill_progress`：`" [" + 填充 + "] %3d%%"`，
 * 填充用 `#`（已完成）/ `-`（未完成））。**总宽恰好 @p width_cols 个显示列**；宽度不足
 * 8 列时只输出百分比。纯函数（不含 TTY 判断），供 `bar()` 与测试共用。
 */
std::string bar_text(double percent, int width_cols);

/// 对外：可重绘终端 → `bar_text`；否则退化为 `100%`（日志里不刷字符画）。
std::string bar(double percent, int width_cols);

/**
 * 组装**一整帧**：左文本（放不下就按显示列截断 + `...`）与右端 @p tail（贴右端），
 * **整行补齐到恰好 @p total_cols 个显示列**。
 *
 * 这是原地刷新不花屏的**不变量**：pacman 的 `cb_progress` 同样"每帧等宽"（信息段按
 * `%-*s` 补齐、进度条段再补齐到剩余列）——只要每帧都等宽，前几帧更长的部分**不可能**
 * 留在屏上。反过来（"新帧比旧帧短就补空格"）在宽字符下会**少补**（中文占 2 列），
 * 于是出现"MiB MiB"这类叠字。纯函数，供 `Line::render()` 与测试共用。
 *
 * @param total_cols 整行可用的显示列数（调用方通常传 `ui::width() - 1`）
 */
std::string compose_frame(std::string left, std::string_view tail, std::size_t total_cols);

/**
 * 进度行的**全文**（纯函数，恒用字符画进度条，与 TTY 无关）：`信息段 + 进度条`。
 *
 * 版式取自 pacman 的 `draw_pacman_progress_bar`：**信息段固定占 `total_cols` 的 60%**
 * （下限 50 列），**进度条固定占剩下的部分** —— 于是同一终端上**每一行的进度条起止列完全相同**，
 * 多行叠在一起是齐的。信息段内：左文本靠左、@p mid 靠右（pacman 是"文件名左、size/rate/eta 右"），
 * 放不下先截左文本（加 `...`）；条宽不足 8 时退化成只出百分比。
 */
std::string progress_frame(std::string left, std::string_view mid, double percent,
                           std::size_t total_cols);

/// `[OK]` / `[FAILED]`（着色）。
std::string ok(bool success);
/// `[SKIPPED]`（着色）。
std::string skipped();

/**
 * 一条"左文本 + 右状态"的**单行**（systemd/pacman 风格，`[OK]` 靠终端最右）。
 *
 * 构造时自动加 `==> ` 前缀（与 `::` 开头的普通信息行一眼可分）。
 * TTY：构造时先把左文本打出来（长操作期间看得见"在干什么"），右侧随后原地刷新；
 * 非 TTY：构造什么都不打，`finish()` 时输出**一行** `左文本 右状态`。
 *
 * 右侧两种形态（同一个左侧文本只用其一）：
 *   · `progress(pct, mid)` —— 进度条（下载 / 解压 / 安装）；
 *   · `right(text)`        —— 任意状态文本（`[OK]` 之类）。
 *
 * 析构时若未 `finish`，补一个换行（异常展开时自动收成 `[FAILED]`）。
 */
class Line
{
public:
    /// 惰性行：**什么都不输出**（用于"可能用不上"的场合，如按条件才显示的下载进度）。
    Line();
    explicit Line(std::string left);
    Line(Line&& other) noexcept;
    Line& operator=(Line&& other) noexcept;
    Line(const Line&) = delete;
    Line& operator=(const Line&) = delete;
    ~Line();

    /// 右侧画进度条；`mid` 是条左边的小字（字节 / 速率 / ETA），非 TTY 下忽略。
    ///
    /// `mid` **按值收**（不是 `string_view`）是有意的：`finish_progress()` 会把 `last_mid_`
    /// 本身当 mid 再喂一次，而本函数要把它存回 `last_mid_` —— 按引用收就会在"存"的过程中
    /// 改掉自己正在读的那块内存（实测会把首字节写成 `\0`，见 `term.cpp` 的实现注释）。
    void progress(double percent, std::string mid = {});
    /// 右侧画任意文本。
    void right(std::string_view text);
    /**
     * 收尾：以 @p right 作为最终右侧状态并换行。
     *
     * 进度条那种"跑到 100%"的语义请用 `finish_progress()`（它会保留最后一帧的 mid）。
     */
    void finish(std::string_view right);
    /// 进度条收尾：画到 100%，**保留最后一次 `progress()` 的 mid**（否则最终帧会突然少掉
    /// 字节/速率，看起来像被截断）。非 TTY 下退化成 `左文本 100%`。
    void finish_progress();

private:
    /// 组装并输出一帧：`left` + 空格补齐 + `tail`，总宽恒为 `width()-1` 列（TTY 才输出）。
    // `tail` 只读（透传给 `compose_frame` 的 `string_view`），按值收会白拷一次；
    // `left` 按值是有意的 —— 函数内 `std::move` 进 `compose_frame`。
    void render(std::string left, std::string_view tail);

    std::string left_;
    std::string last_mid_;  ///< 最近一次 progress() 的 mid（finish_progress 复用）
    bool tty_ = false;      ///< 可否原地刷新（构造时取一次，避免每条都做 ioctl）
    bool drawn_ = false;    ///< TTY 下左文本是否已输出
    bool done_ = false;
};

/// 阶段分隔：**pacman 风格** `:: <标题>`（与普通信息行同形同色，走 `log_info`）。
void section(std::string_view title);

}  // namespace ui
