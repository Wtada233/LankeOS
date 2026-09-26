/**
 * test_op_sink_dir_rm_warning.cpp — rmdir 失败**不许静默**
 *
 * `OpSink::remove_empty_dir()` 的顺序是 write-ahead 的：先写 `DIR_RM <path> <mode> <uid> <gid>`
 * 再 `fs::remove`。旧实现在 `ec` 非空时只 `return NotRemoved`，而两个调用方
 * （installation_task.cpp / package_manager.cpp）都只看 `SkippedMountPoint` —— rmdir 失败
 * 于是**完全静默**：WAL 里 DIR_RM 行声称"已删"、DB 归属已摘、目录还在盘上，三方脱节，
 * 回滚侧还会照行里的元数据把它"重建"出来。挂载点已被函数内的守卫拦掉（EBUSY，另有用例），
 * 走到 `ec` 的是 EROFS / EACCES / is_empty 与 rmdir 之间的 ENOTEMPTY 竞态这类**真错误**。
 *
 * 本用例用"非空目录"制造 rmdir 失败（ENOTEMPTY）：这是**与权限无关**的手段 —— 沙盒里
 * chmod 父目录 0500 那类造法对 root 无效（root 不受权限位限制，用例会恒真/恒假），
 * 而 ENOTEMPTY 对 root 与普通用户同样发生。
 *
 * 注意 DIR_RM 行**仍然会写**，这是有意的、不是漏修：行的作用是"回滚时把目录重建出来"，
 * 若改成"先 rmdir 后写行"，崩在中间就是"目录没了、WAL 无记录"，盘面与 DB 脱节 —— 那是
 * 比"行与盘面不一致"更坏的不可恢复状态。所以修法是**让失败可见**（告警），而不是反悔写行。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/db/wal_op.hpp"  // wal::wal_log_path()
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/op_sink.hpp"

namespace fs = std::filesystem;
using detail::DirRemoval;
using detail::OpSink;

class OpSinkDirRmWarningTest : public ::testing::Test
{
protected:
    fs::path suite_dir;
    fs::path root;
    std::vector<fs::path> stashes;

    void SetUp() override
    {
        init_localization();
        Config::instance().set_testing_mode(true);
        suite_dir = fs::absolute("tmp_op_sink_dir_rm_warning_test");
        fs::remove_all(suite_dir);
        root = suite_dir / "root";
        fs::create_directories(root);
        Config::instance().set_root_path(root.string());
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        Config::instance().set_testing_mode(false);
        fs::remove_all(suite_dir);
    }

    std::string read_wal() const
    {
        std::ifstream f(wal::wal_log_path());
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    /** 告警消息的**字面前缀**（l10n 模板在第一个占位符处切断）——与语言无关 */
    static std::string warning_head()
    {
        const std::string tmpl = get_string("warning.dir_remove_failed");
        const auto p = tmpl.find("{}");
        return tmpl.substr(0, p);
    }

    /** `{}` 与 `{}` 之间的字面分隔（拿它拼出"前缀 + 路径 + 分隔"的期望片段） */
    static std::string warning_sep()
    {
        const std::string tmpl = get_string("warning.dir_remove_failed");
        const auto p1 = tmpl.find("{}");
        const auto p2 = tmpl.find("{}", p1 + 2);
        return tmpl.substr(p1 + 2, p2 - p1 - 2);
    }
};

// ============================================================================
// rmdir 失败（ENOTEMPTY）→ 必须留下一条点名路径的告警
// ============================================================================

TEST_F(OpSinkDirRmWarningTest, FailedRmdirIsWarnedWithPathAndReason)
{
    // 非空目录：remove_empty_dir 的前置只判"是不是真实目录"，空不空由调用方守卫 ——
    // 这里直接喂非空目录，等价于"调用方的 is_empty 与 rmdir 之间出现了内容"（竞态）。
    const fs::path target = root / "usr" / "share" / "notempty";
    fs::create_directories(target);
    std::ofstream(target / "racer.txt") << "appeared between is_empty and rmdir\n";

    OpSink sink("t", &stashes);
    DirRemoval r = DirRemoval::Removed;
    std::string err;

    testing::internal::CaptureStderr();
    r = sink.remove_empty_dir(target);
    err = testing::internal::GetCapturedStderr();

    EXPECT_EQ(r, DirRemoval::NotRemoved) << "rmdir 失败必须报告为 NotRemoved";
    EXPECT_TRUE(fs::exists(target)) << "rmdir 失败了，目录却不见了？（用例前提被破坏）";

    // 关键断言：**不再静默** —— 输出里要有这条告警，且点名路径
    EXPECT_NE(err.find(warning_head() + target.string() + warning_sep()), std::string::npos)
        << "rmdir 失败没有告警（或告警没点名路径）。实际输出：\n"
        << err;

    // 行仍然在（write-ahead 不允许反悔，理由见文件头）：用例把它钉住，免得后人"顺手"改成
    // 先 rmdir 后写行 —— 那会让崩溃窗口从"行与盘面不一致"恶化成"目录没了、WAL 无记录"。
    EXPECT_NE(read_wal().find("DIR_RM " + target.string() + " "), std::string::npos)
        << "DIR_RM 行必须照写（write-ahead）：";
}

// ============================================================================
// 正面对照：rmdir 真的成功时**不得**告警（防止修成"每次 remove_empty_dir 都刷一行"）
// ============================================================================

TEST_F(OpSinkDirRmWarningTest, SuccessfulRmdirStaysSilent)
{
    const fs::path target = root / "usr" / "share" / "empty";
    fs::create_directories(target);

    OpSink sink("t", &stashes);
    std::string err;
    DirRemoval r = DirRemoval::NotRemoved;

    testing::internal::CaptureStderr();
    r = sink.remove_empty_dir(target);
    err = testing::internal::GetCapturedStderr();

    EXPECT_EQ(r, DirRemoval::Removed);
    EXPECT_FALSE(fs::exists(target));
    EXPECT_EQ(err.find(warning_head()), std::string::npos)
        << "删除成功却打了告警（告警会因此变成噪音、被用户忽略）：\n"
        << err;
    EXPECT_NE(read_wal().find("DIR_RM " + target.string() + " "), std::string::npos);
}
