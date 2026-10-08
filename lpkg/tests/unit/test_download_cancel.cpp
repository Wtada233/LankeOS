/**
 * test_download_cancel.cpp —— Ctrl+C 取消下载**不得**被当成可重试的失败
 *
 * ── 缺陷 ──────────────────────────────────────────────────────────────────
 * `download_with_retries()` 捕的是 `const LpkgException&`，而 `UserAbort` 正是它的子类
 * （`base/exception.hpp`）⇒ 用户在下载中按 Ctrl+C 抛出的 `UserAbort` 被当成一次**普通下载
 * 失败**：先 `fs::remove` 掉已落位的输出文件、打一条「正在重试」、然后**接着重试** ——
 * `installation_task` 传 5 次、`builder_executor` 传 3 次。而重试期间 `sigint_graceful`
 * 仍为真，进度回调立刻又返回 1、curl 立刻又被中止 ⇒ **空转若干次**，用户按了取消却看着
 * 程序"又在重试"，还收到一串误导性告警。取消被延迟到场（退出码最终仍正确）。
 *
 * 判据：取消必须**直接穿透** —— 与 `main_cli.cpp` 的 catch 顺序同款（`UserAbort` 单独先接住，
 * 别让它落进宽 `catch (const LpkgException&)`）。
 *
 * ── 为什么断言不是"异常类型" ────────────────────────────────────────────────
 * 两条路径（修前/修后）最终抛的**都是** `UserAbort`（重试到底后 `throw;` 重抛的就是它），
 * 所以 `EXPECT_THROW(..., UserAbort)` 区分不出来。真正能区分的是两个独立锚点：
 *   ① stdout 里不得出现「正在重试」；
 *   ② 输出文件还在盘上 —— 缺陷路径的第一步就是 `fs::remove(output_path)`。
 *
 * 用 `file://` 验（测试一律不碰网）：curl 对本地传输同样走进度回调，所以 `sigint_graceful`
 * 一置位就能触发 `CURLE_ABORTED_BY_CALLBACK`。
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

#include "../../main/src/archive/downloader.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/i18n/localization.hpp"

namespace fs = std::filesystem;

/** 定义在 main/src/main_cli.cpp（随 LPKG_OBJS 进测试二进制）；此处声明以便模拟 Ctrl+C */
extern std::atomic<bool> sigint_graceful;

class DownloadCancelTest : public ::testing::Test
{
protected:
    fs::path work;

    void SetUp() override
    {
        init_localization();
        work = fs::absolute("tmp_download_cancel_" + std::to_string(::getpid()));
        fs::remove_all(work);
        fs::create_directories(work);
        sigint_graceful.store(false);
    }

    void TearDown() override
    {
        // 进程级全局状态：绝不把"正在中断"漏给后续用例（本仓库踩过"单跑绿、全量红"）
        sigint_graceful.store(false);
        fs::remove_all(work);
    }
};

TEST_F(DownloadCancelTest, CancelIsNotRetried)
{
    // 源文件给足大小：进度回调是按块调的，太小的传输可能一次回调都不发生
    const fs::path src = work / "src.bin";
    {
        std::ofstream f(src, std::ios::binary);
        const std::string chunk(64 * 1024, 'x');
        for (int i = 0; i < 16; ++i) f << chunk;  // 1 MiB
    }
    const fs::path out = work / "out.bin";
    const std::string url = "file://" + src.string();

    // ⚠️ 这个键的模板是 `{}. Retrying...` —— **占位符在最前面**，所以"取 `{}` 之前的字面前缀"
    //    那种锚会得到**空串**，`find("")` 恒为 0、断言恒假。改用渲染后的完整文案（把占位符填成
    //    空串），并先自检它确实是个有区分力的非空串。
    const std::string retry_marker = string_format("info.retrying", "");
    ASSERT_FALSE(retry_marker.empty());
    ASSERT_EQ(retry_marker.find("MISSING_STRING"), std::string::npos)
        << "l10n 键缺失，下面的断言会退化成恒真：" << retry_marker;

    sigint_graceful.store(true);  // 用户已按 Ctrl+C
    // ⚠️ 捕的是 **stderr**：`log_warning` 写 `std::cerr`（`base/utils.cpp`）。捕 stdout 的话
    //    这条锚点会**恒真**（文案永远不在被捕获的流里）—— 踩到过：捕 stdout 的话，锚点①
    //    "通过"了，而缺陷明明打出了 4 条重试告警。
    testing::internal::CaptureStderr();
    EXPECT_THROW(download_with_retries(url, out, /*max_retries=*/5, /*show_progress=*/false),
                 UserAbort)
        << "取消必须以 UserAbort 穿透（注意：修前修后都抛 UserAbort，这条不区分，"
           "真正的判据是下面两个锚点）";
    const std::string logs = testing::internal::GetCapturedStderr();
    sigint_graceful.store(false);

    EXPECT_EQ(logs.find(retry_marker), std::string::npos)
        << "Ctrl+C 被当成了可重试的失败，打出了重试告警（用户按了取消却看到程序在重试）：\n"
        << logs;
    EXPECT_TRUE(fs::exists(out))
        << "重试循环把已落位的输出文件删掉了 —— `download_with_retries` 的重试分支第一步"
           "就是 `fs::remove(output_path)`，修后取消应当直接穿透、根本不走那一支";
}
