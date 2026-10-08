/**
 * test_prompt_interrupt.cpp — **输入等待期间 Ctrl+C 必须有效**
 *
 * 现象：`lpkg force-solve-conflict` 的确认短语、`lpkg remove -r` 的三轮验证码
 * 用**裸** `std::getline(std::cin, …)` / `std::cin >> x` 读输入 —— 而 glibc 下 `std::signal()`
 * 装的 handler 带 `SA_RESTART`，被打断的 `read(2)` 会被**自动重启**（iostreams 也会重试）。
 * 于是 Ctrl+C 只把 `sigint_graceful` 置位、打印一句提示，进程**仍旧卡在输入上**：
 * 用户看到的就是"**Ctrl+C 无效，只能 kill -9**"。
 *
 * 修法：唯一的读取实现 `read_line_interruptible()`（base/utils.cpp）——100ms 轮询，
 * 每轮先看 `sigint_graceful`。本文件覆盖它，**并且覆盖"必须会红"的那一条**：
 * 若退回裸 `std::cin`，`CtrlC...ReturnsInsteadOfHanging` 会**永久挂起**（不是变红那么简单）。
 *
 * 用 dup2 把管道接到 fd 0 上造输入；用线程在读取过程中置标志 —— 这正是"读取中途按 Ctrl+C"
 * 的等价物，比真发 SIGINT 更确定（不依赖信号投递时机与 SA_RESTART）。
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "../test_hygiene.hpp"
#include "base/utils.hpp"

namespace
{
using namespace std::chrono_literals;
}

class PromptInterruptTest : public ::testing::Test
{
protected:
    int saved_stdin_ = -1;
    int pipe_r_ = -1;
    int pipe_w_ = -1;

    void SetUp() override
    {
        sigint_graceful.store(false);
        saved_stdin_ = ::dup(STDIN_FILENO);
        ASSERT_NE(saved_stdin_, -1);
        int fds[2] = {-1, -1};
        ASSERT_EQ(::pipe(fds), 0);
        pipe_r_ = fds[0];
        pipe_w_ = fds[1];
        ASSERT_NE(::dup2(pipe_r_, STDIN_FILENO), -1);
    }

    void TearDown() override
    {
        if (saved_stdin_ != -1) {
            ::dup2(saved_stdin_, STDIN_FILENO);
            ::close(saved_stdin_);
        }
        if (pipe_r_ != -1) ::close(pipe_r_);
        if (pipe_w_ != -1) ::close(pipe_w_);
        sigint_graceful.store(false);
    }

    /// 往"stdin"里喂数据（不关写端 ⇒ 后续读会阻塞等更多输入）
    void feed(const std::string& s) const
    {
        ASSERT_EQ(::write(pipe_w_, s.data(), s.size()), static_cast<ssize_t>(s.size()));
    }
};

TEST_F(PromptInterruptTest, NormalLineIsReadAndStripped)
{
    feed("hello\n");
    std::string out;
    ASSERT_TRUE(read_line_interruptible(out));
    EXPECT_EQ(out, "hello");
}

TEST_F(PromptInterruptTest, CarriageReturnAlsoTerminates)
{
    feed("abc\r\n");  // CRLF：`\r` 即行尾，`\n` 留在管道里（下一行会读到空串）
    std::string out;
    ASSERT_TRUE(read_line_interruptible(out));
    EXPECT_EQ(out, "abc");
}

TEST_F(PromptInterruptTest, EofReturnsFalseInsteadOfHanging)
{
    ::close(pipe_w_);
    pipe_w_ = -1;
    std::string out;
    EXPECT_FALSE(read_line_interruptible(out)) << "stdin 到 EOF 必须返回 false（调用方放弃操作）";
}

TEST_F(PromptInterruptTest, FlagAlreadySetReturnsImmediately)
{
    // 没有数据、标志已置位：必须立刻返回，而不是先等输入
    sigint_graceful.store(true);
    std::string out;
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(read_line_interruptible(out));
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count(),
        500);
}

TEST_F(PromptInterruptTest, CtrlCDuringABlockedReadReturnsInsteadOfHanging)
{
    // **本文件的核心用例**：管道里一个字节都没有（读会阻塞），150ms 后置标志 ——
    // 等价于"用户卡在提示符上按了 Ctrl+C"。
    // 退回裸 `std::getline(std::cin, …)` 的话，这里会**永久挂起**（SA_RESTART 让 read 重启）。
    std::atomic<bool> done{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(150ms);
        sigint_graceful.store(true);
        done.store(true);
    });

    std::string out;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got_line = read_line_interruptible(out);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    canceller.join();

    EXPECT_TRUE(done.load()) << "控制线程没跑完（用例本身写错了）";
    EXPECT_FALSE(got_line) << "Ctrl+C 之后必须返回 false（放弃当前操作）";
    EXPECT_LT(ms, 2000) << "读取被 Ctrl+C 打断后应当很快返回，实测 " << ms << " ms";
}

TEST_F(PromptInterruptTest, UserConfirmsTreatsCtrlCAsCancelNotAsAHang)
{
    // `user_confirms` 走同一个原语（重构后共用一份实现）—— 置位后必须返回 false 而不是阻塞
    Config::instance().set_non_interactive_mode(NonInteractiveMode::INTERACTIVE);
    sigint_graceful.store(true);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(user_confirms("continue?"));
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count(),
        500);
}
