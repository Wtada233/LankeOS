/**
 * test_cli_dispatch.cpp — **CLI 分派层**的端到端用例（退出码 + 消息）
 *
 * ## 这个文件补的是什么缺口
 * `Makefile` 里 `LPKG_OBJS = $(filter-out $(BUILD_DIR)/main/main.o, $(MAIN_OBJS))` ——
 * 测试二进制（`build/run_tests`）**不链接 main.o**（它有 tests/main.cpp 自己的 main()）。
 * 于是"解析 argv → 应用全局选项 → 校验 → 分派到 13 个 handler"这整条路径此前**零覆盖**：
 * 退出码、错误消息、`--yes/--no` 冲突、位置参数个数校验全靠人工 CLI 冒烟。
 * 现在分派路径在 main/src/main_cli.cpp（不是 main.o ⇒ 自动进 LPKG_OBJS），
 * 本文件调 `run_cli({...})` 把它钉成永久用例。
 *
 * ## 边界（明确不测，别当已覆盖）
 * 只测**分派层**：不真装包 / 不真卸包 / 不写 DB。会落盘的命令（install 带真包名、
 * remove、upgrade、rec、scan）归集成测试。这里用到的 `depend` / `build` / `pack` /
 * `query` 几条都会在参数校验阶段就失败，**不触碰软件包**（`depend remove alpha beta`
 * 只读仓库反向依赖图并打印，不落盘）。
 *
 * ## 断言纪律：每条都落**两个锚点**（退出码 + 消息关键字）
 * 只有一个锚点时，"用例自己把参数写错了"（比如把 `--no` 写成 `--nope`，于是 cxxopts
 * 报解析错误、退出码同样是 1）与"分派真的坏了"不可区分。
 *
 * ## 消息捕获
 * 消息经 `log_info`/`log_error`/`print_usage` 走 `std::cout`/`std::cerr`，把 `rdbuf`
 * 换成 `std::ostringstream` 即可捕获。**唯一捕获不到的是 `sigint_handler` 里那句
 * `write(STDERR_FILENO, ...)`** —— 它绕开 iostream 直接写 fd 2（信号处理函数里不能做
 * 加锁的流操作），所以本文件**不**断言它；真要测那句得在子进程里真的送一次 SIGINT
 * （见 tests/integration/test_sigint*.cpp 的做法）。
 *
 * ## 语言
 * 容器里 `LANG` 为空 ⇒ 生效的是 `en.txt`（与 tests/integration/test_phase_log_messages.cpp
 * 的说明一致），所以这里直接断言英文原文。用例不会自己 setenv —— 分派层本就不该依赖语言。
 */

#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../../main/src/config/config.hpp"
#include "../../main/src/main_cli.hpp"

namespace
{

/** 一次 `run_cli` 的退出码与两个流的内容 */
struct CliRun {
    int code = 0;
    std::string out;
    std::string err;

    /** 两个流拼起来：消息走 stdout 还是 stderr 多属实现细节，断言通常只关心"打出来了" */
    std::string all() const
    {
        return out + err;
    }
};

/** 跑一次 `run_cli`，期间把 `std::cout`/`std::cerr` 换到内存缓冲区 */
CliRun run_cli_captured(const std::vector<std::string>& argv)
{
    std::ostringstream out_buf;
    std::ostringstream err_buf;
    std::streambuf* saved_out = std::cout.rdbuf(out_buf.rdbuf());
    std::streambuf* saved_err = std::cerr.rdbuf(err_buf.rdbuf());

    int code = 0;
    try {
        code = run_cli(argv);
    } catch (...) {
        // run_cli 的三个 catch 覆盖了全部异常路径（各自 return 1）——走到这里说明那个契约
        // 破了。先还原流再让异常往上抛，否则后续用例会一直往这个临时缓冲区里写。
        std::cout.rdbuf(saved_out);
        std::cerr.rdbuf(saved_err);
        throw;
    }
    std::cout.rdbuf(saved_out);
    std::cerr.rdbuf(saved_err);

    CliRun r;
    r.code = code;
    r.out = out_buf.str();
    r.err = err_buf.str();
    return r;
}

/** 两个锚点：退出码必须等于 want_code，且输出里必须出现 needle */
void expect_code_and_message(const CliRun& r, int want_code, const std::string& needle)
{
    EXPECT_EQ(r.code, want_code);
    EXPECT_NE(r.all().find(needle), std::string::npos)
        << "输出里找不到关键字 [" << needle << "]\n--- stdout ---\n"
        << r.out << "\n--- stderr ---\n"
        << r.err;
}

}  // namespace

/**
 * 夹具：只把 `Config` 的 root_path 钉回 `/`。
 *
 * 为什么需要：`run_cli` 对非 `man` 命令会 `init_database_for()`（check_root →
 * init_filesystem → 取 DB 锁 → WAL 恢复），全过程按 `Config` 的**当前** root_path
 * 解析路径。前一个套件若把 root_path 留在已被 `remove_all` 掉的沙盒里，这里的
 * `DBLock` 就会以"锁文件建不出来"失败 —— 那是夹具污染，不是分派坏了。
 * 与 test_cleanup.cpp 同一手法（TearDown 里也写回 "/"，不自留污染）。
 */
class CliDispatchTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Config::instance().set_root_path("/");
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
    }
};

// ── 无副作用的两条：在数据库初始化**之前**就返回 ────────────────────────────

TEST_F(CliDispatchTest, HelpExitsZeroAndPrintsUsage)
{
    const CliRun r = run_cli_captured({"lpkg", "--help"});
    // 锚点 1 = 退出码 0；锚点 2 = 用法正文真的打了（`info.commands` = "Commands:"）
    expect_code_and_message(r, 0, "Commands:");
    // 帮助走 stderr（`print_usage` 全用 std::cerr）——顺带钉住"没有跑到 stdout"
    EXPECT_NE(r.err.find("Commands:"), std::string::npos);
    EXPECT_EQ(r.out.find("Commands:"), std::string::npos);
}

TEST_F(CliDispatchTest, VersionExitsZeroAndPrintsToStdout)
{
    const CliRun r = run_cli_captured({"lpkg", "--version"});
    // 锚点 1 = 退出码 0；锚点 2 = `info.version` = "lpkg version {}"
    expect_code_and_message(r, 0, "lpkg version");
    EXPECT_NE(r.out.find("lpkg version"), std::string::npos);
    EXPECT_EQ(r.err.find("lpkg version"), std::string::npos);
}

// ── 用法与参数错误：全部退出码 1，且都要打用法/具体原因 ──────────────────────

TEST_F(CliDispatchTest, NoCommandPrintsUsageAndExitsOne)
{
    const CliRun r = run_cli_captured({"lpkg"});
    // 锚点 1 = 退出码 1；锚点 2 = 打了用法正文
    expect_code_and_message(r, 1, "Commands:");
}

TEST_F(CliDispatchTest, UnknownCommandPrintsUsageAndExitsOne)
{
    const CliRun r = run_cli_captured({"lpkg", "definitely-not-a-command"});
    // 锚点 1 = 退出码 1；锚点 2 = 走的是"未知命令 → 打用法"那条分支（不是别处抛的错）
    expect_code_and_message(r, 1, "Commands:");
    // 未知命令**不**是异常路径：`error.lpkg_error` 的 "Lpkg error:" 前缀不该出现
    EXPECT_EQ(r.all().find("Lpkg error:"), std::string::npos);
}

TEST_F(CliDispatchTest, InstallRejectsConflictingYesAndNo)
{
    // `--yes` 与 `--no` 同给：曾静默取后者（= 猜用户意图），现在必须报错。
    // 这一步在数据库初始化**之前**（apply_non_interactive_mode），所以不需要 root/DB。
    const CliRun r = run_cli_captured({"lpkg", "install", "--yes", "--no", "foo"});
    // 锚点 1 = 退出码 1；锚点 2 = error.conflicting_yes_no 原文
    expect_code_and_message(r, 1, "--yes and --no are mutually exclusive");
    // 是 LpkgException 路径（被 catch 后带前缀打印），不是解析层错误
    EXPECT_NE(r.all().find("Lpkg error:"), std::string::npos);
    EXPECT_EQ(r.all().find("Command line parsing error:"), std::string::npos);
}

TEST_F(CliDispatchTest, DependWithoutSubcommandIsRejected)
{
    const CliRun r = run_cli_captured({"lpkg", "depend"});
    // 锚点 1 = 退出码 1；锚点 2 = error.depend_need_subcmd 原文
    expect_code_and_message(r, 1, "depend command requires a subcommand");
}

TEST_F(CliDispatchTest, DependRemoveWithoutPackageNameIsRejected)
{
    const CliRun r = run_cli_captured({"lpkg", "depend", "remove"});
    // 锚点 1 = 退出码 1；锚点 2 = error.depend_need_pkg 原文
    expect_code_and_message(r, 1, "depend requires at least one package name");
}

TEST_F(CliDispatchTest, DependUnknownSubcommandIsRejected)
{
    const CliRun r = run_cli_captured({"lpkg", "depend", "bogus", "foo"});
    // 锚点 1 = 退出码 1；锚点 2 = error.depend_unknown_subcmd 原文（含被点名的子命令）
    expect_code_and_message(r, 1, "Unknown depend subcommand: bogus");
}

TEST_F(CliDispatchTest, BuildRejectsMoreThanOneDirectory)
{
    // `lpkg build a b c` 曾静默只编 a —— 多余参数必须报错，别假装成功
    const CliRun r = run_cli_captured({"lpkg", "build", "a", "b"});
    // 锚点 1 = 退出码 1；锚点 2 = error.build_single_dir 原文
    expect_code_and_message(r, 1, "build takes exactly one directory");
}

TEST_F(CliDispatchTest, PackWithoutOutputIsRejected)
{
    const CliRun r = run_cli_captured({"lpkg", "pack"});
    // 锚点 1 = 退出码 1；锚点 2 = error.pack_no_output 原文
    expect_code_and_message(r, 1, "Please specify output file name with -o");
}

TEST_F(CliDispatchTest, QueryRejectsWrongArgumentCount)
{
    // query 恰好一个参数：0 个与 2 个都必须拒（`pre_operation_check(..., 1, 1)`）
    const CliRun r = run_cli_captured({"lpkg", "query", "a", "b"});
    // 锚点 1 = 退出码 1；锚点 2 = error.invalid_arg_count 原文
    expect_code_and_message(r, 1, "Invalid number of arguments.");
    // 顺带钉住 pre_operation_check 的"先打用法再报错"（这条路径的第二个可辨识特征）
    EXPECT_NE(r.all().find("Commands:"), std::string::npos);
}

// ── 唯一一条走到底的：depend remove 打两棵依赖树 ─────────────────────────────

TEST_F(CliDispatchTest, DependRemovePrintsOneTreePerArgument)
{
    // 不存在的包名是有意的：`scan_remove_tree` 对"仓库里没有"的包返回一个
    // "(not found)" 的叶子节点而**不抛**，于是这条能稳定走到"两个参数 = 两棵树 + 中间空行"
    // 这条成功路径，且全程不落盘、不依赖仓库里有任何包。
    const CliRun r = run_cli_captured({"lpkg", "depend", "remove", "alpha", "beta"});
    // 锚点 1 = 退出码 0；锚点 2 = 两棵树的标题都打了（info.depend_remove_header 带包名）
    expect_code_and_message(r, 0, "Dependency removal scan for 'alpha':");
    EXPECT_NE(r.all().find("Dependency removal scan for 'beta':"), std::string::npos)
        << "只打了第一棵树（或第二棵被跳过）—— print_depend_trees 的循环坏了\n"
        << r.all();
}
