/**
 * test_cancel_semantics.cpp — **取消 ≠ 完成**（退出码与消息都要说清楚）
 *
 * 现象（2026-10-03 报）：`lpkg remove -r <pkg>` 输错验证码之后
 *
 *     :: 验证码错误，递归移除已中止。
 *     :: 安装被用户中止。          ← ①措辞串了（这是**卸载**操作）
 *     :: 卸载完成。                ← ②还报了"完成"、退出码 0（脚本以为删成功了）
 *
 * 三处一起收：
 *   ① 文案与操作无关（`info.sigint_aborted` / `info.user_aborted` 被 install / upgrade /
 *      remove 共用，不能再写死"安装"）；
 *   ② 取消走 `UserAbort`（`run_cli` 单独接它 ⇒ 用 `log_info` 打消息、**不套 `Error:`**）；
 *   ③ **退出码非零、且不打完成消息** —— 脚本/farm 必须能区分"用户取消（什么都没做）"
 *      与"做好了"。原先那两条路是 `log_info(aborted); return;`，于是命令**继续走到完成行**。
 *
 * 断言落在**两个锚点**上（退出码 + 消息），并额外断言"没有完成行"与"盘上确实没变"。
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/main_cli.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class CancelSemanticsTest : public IntegrationTestBase
{
protected:
    int saved_stdin_ = -1;
    int pipe_r_ = -1;
    int pipe_w_ = -1;

    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        // 要看到"交互提示"就必须是 INTERACTIVE（全局 listener 复位成 YES）
        Config::instance().set_non_interactive_mode(NonInteractiveMode::INTERACTIVE);
        saved_stdin_ = ::dup(STDIN_FILENO);
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
        IntegrationTestBase::TearDown();
    }

    void feed(const std::string& s) const
    {
        ASSERT_EQ(::write(pipe_w_, s.data(), s.size()), static_cast<ssize_t>(s.size()));
    }

    /** 跑一次 CLI，捕获两个流（消息走 stdout/stderr 都是实现细节） */
    struct CliRun {
        int code = 0;
        std::string out;
        std::string err;
        std::string all() const
        {
            return out + err;
        }
    };

    static CliRun run_cli_captured(const std::vector<std::string>& argv)
    {
        std::ostringstream out_buf;
        std::ostringstream err_buf;
        auto* old_out = std::cout.rdbuf(out_buf.rdbuf());
        auto* old_err = std::cerr.rdbuf(err_buf.rdbuf());
        int code = 0;
        try {
            code = run_cli(argv);
        } catch (...) {
            std::cout.rdbuf(old_out);
            std::cerr.rdbuf(old_err);
            throw;
        }
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
        return {code, out_buf.str(), err_buf.str()};
    }
};

TEST_F(CancelSemanticsTest, RemoveClaimsCompletionOnlyWhenSomethingWasActuallyRemoved)
{
    // 2026-10-03 修：`lpkg remove <从未安装过的包>` 此前**照样**打印 `info.uninstall_complete`
    // （"卸载完成"）—— 与 install 侧早就删掉的那条假消息同款，脚本/farm 会以为删掉了。
    // 两半都钉：没删东西时**不许**出现这句，真删了必须出现（否则"从未打印"也能让上半条绿）。
    Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);

    const std::string never = "cs_never_installed";
    const CliRun r1 = run_cli_captured({"lpkg", "--root", test_root.string(), "remove", never});
    const std::string done_msg = get_string("info.uninstall_complete");
    EXPECT_EQ(r1.all().find(done_msg), std::string::npos) << "没删任何东西却报了完成: " << r1.all();
    // 锚点：它确实走到了移除路径、并给出了"这个包没装"的原因（否则"什么都没打"也能让上面绿）
    EXPECT_NE(r1.all().find(never), std::string::npos) << r1.all();

    const std::string installed = "cs_removed_for_real";
    ASSERT_NO_THROW(install_packages({create_pkg(installed, "1.0")}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed(installed));

    const CliRun r2 = run_cli_captured({"lpkg", "--root", test_root.string(), "remove", installed});
    EXPECT_EQ(r2.code, 0) << r2.all();
    EXPECT_NE(r2.all().find(done_msg), std::string::npos) << "真删了却不报完成: " << r2.all();
}

TEST_F(CancelSemanticsTest, RecursiveRemoveWrongCodeIsACancelNotACompletion)
{
    const std::string pkg = "cs_pkg";
    const std::string pkg_path = create_pkg(pkg, "1.0");
    // 先装一个包（SetUp 把模式设成了 INTERACTIVE —— 那是为下面那句 `remove -r` 的验证码提示
    // 准备的；装包这一步得显式走 YES，否则它会**在 stdin 上等确认**，把用例自己挂住）。
    Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
    ASSERT_NO_THROW(install_packages({pkg_path}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed(pkg));
    Config::instance().set_non_interactive_mode(NonInteractiveMode::INTERACTIVE);

    feed("XXXXXX\n");  // 验证码一定不对（6 位随机大写字母数字）

    const CliRun r = run_cli_captured({"lpkg", "--root", test_root.string(), "remove", "-r", pkg});

    // ① 退出码非零（取消不是成功）
    EXPECT_EQ(r.code, 1) << r.all();
    // ② 消息是"取消"（UserAbort → log_info，无 `Error:` 前缀），且**与操作无关**
    EXPECT_NE(r.all().find(get_string("info.user_aborted")), std::string::npos) << r.all();
    EXPECT_EQ(r.all().find(get_string("error.prefix")), std::string::npos)
        << "取消不是错误，不该套 Error: 前缀：\n"
        << r.all();
    // ③ **不许出现完成行**（这正是报的缺陷）
    EXPECT_EQ(r.all().find(get_string("info.uninstall_complete")), std::string::npos)
        << "取消之后不能报「卸载完成」：\n"
        << r.all();
    // ④ 盘上没变
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed(pkg)) << "取消 = 什么都没做";
}

TEST_F(CancelSemanticsTest, UserSaysNoToInstallIsACancelNotACompletion)
{
    const std::string pkg = "cs_no";
    const std::string pkg_path = create_pkg(pkg, "1.0");

    const CliRun r =
        run_cli_captured({"lpkg", "--root", test_root.string(), "--no", "install", pkg_path});

    EXPECT_EQ(r.code, 1) << r.all();
    EXPECT_NE(r.all().find(get_string("info.user_aborted")), std::string::npos) << r.all();
    // 这里原本还有一条 `EXPECT_EQ(r.all().find(get_string("info.install_complete")), npos)` ——
    // 那个键**根本不存在**（两份 l10n 里都没有），`get_string` 对未知键返回
    // `[MISSING_STRING: info.install_complete]`，而 CLI 输出里永远不可能出现这个占位符
    // ⇒ `find` 恒为 npos ⇒ 是一条**恒真断言**（0 区分力，看起来在测"取消后没有完成提示"，
    // 实际什么都没测）。真正钉住同一件事的是下面那条：用**存在**的键
    // `info.install_summary`（成功安装时的汇总行）断言它不出现。
    EXPECT_EQ(r.all().find(string_format("info.install_summary", 1, "")), std::string::npos)
        << "取消之后不该出现安装 summary：\n"
        << r.all();
    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed(pkg));
}

TEST_F(CancelSemanticsTest, LibraryLevelCancelThrowsUserAbort)
{
    // 库层的判据：取消抛 `UserAbort`（而不是"正常返回"）—— run_cli 据此翻成退出码 1。
    const std::string pkg = "cs_lib";
    const std::string pkg_path = create_pkg(pkg, "1.0");
    Config::instance().set_non_interactive_mode(NonInteractiveMode::NO);
    EXPECT_THROW(install_packages({pkg_path}), UserAbort);
    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed(pkg));
}

TEST_F(CancelSemanticsTest, RecursiveRemoveWithNoIsACancelNotACompletion)
{
    const std::string pkg = "cs_rm_no";
    const std::string pkg_path = create_pkg(pkg, "1.0");
    Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
    ASSERT_NO_THROW(install_packages({pkg_path}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed(pkg));

    // `--no` = 对所有提问自动答否。递归删除是破坏性操作，非交互下**不得**跳过 3 轮验证码
    // 径直执行（修复前 `confirmed` 初值即 true、只在 INTERACTIVE 分支被改写 ⇒ `--no` 与
    // `-y` 一样放行）。非交互就不会读 stdin，所以这里不需要喂任何输入。
    const CliRun r =
        run_cli_captured({"lpkg", "--root", test_root.string(), "--no", "remove", "-r", pkg});

    EXPECT_EQ(r.code, 1) << r.all();
    EXPECT_NE(r.all().find(get_string("info.user_aborted")), std::string::npos) << r.all();
    EXPECT_EQ(r.all().find(get_string("info.uninstall_complete")), std::string::npos) << r.all();
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed(pkg)) << "--no 下递归删除必须什么都不做";
}
