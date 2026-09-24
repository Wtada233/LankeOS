/**
 * test_op_sink.cpp — 写入层原语 `detail::OpSink` 的契约
 *
 * 头文件把"路径规范化在这一层兜底"写成不变量，但当前 9 个调用点**全部**已各自剥过尾斜杠，
 * 所以那条路径在新代码里没有任何用例覆盖 —— 一旦谁按契约字面去删掉调用点的自剥（那正是
 * 这次重构的目标之一），防线就剩一段从没被执行过的代码。本文件直接把带尾斜杠的路径喂给
 * sink，把这条不变量钉住。
 *
 * 钉的到底是什么（ARCH.md §3.6.1 第 1 条）：尾斜杠会让 lstat / rename / rmdir 落到**末尾
 * 符号链接的目标**上。所以
 *   · `backup("<root>/link/")`           必须搬走**链接本身**，绝不能把链接指向的目录搬走；
 *   · `remove_empty_dir("<root>/link2/")` 必须**什么都不做**（lstat 语义下它是非目录），
 *                                          绝不能 rmdir 掉链接指向的空目录；
 *   · `commit_copy(tmp, "<root>/x/")`     落点必须是剥掉斜杠的 `<root>/x`（否则 rename(2)
 *                                          会因尾斜杠报 ENOTDIR）。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"  // mount_points()（挂载点用例的取证目标）
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/db/wal_op.hpp"  // wal::wal_log_path()
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/op_sink.hpp"

namespace fs = std::filesystem;
using detail::OpSink;

/**
 * 两个 OpSink fixture 的公共基类 —— 它们在 SetUp/TearDown 上**必须一致**。
 *
 * 改前 `OpSinkNormalizationTest` 既没设 `testing_mode(true)` 也不清断点表，而
 * `OpSinkSaveConfigTest` 设了 `testing_mode(true)` 却**从不复原**（TearDown 只还原
 * root_path）。两处都是**全局**状态：
 *   · `testing_mode` 是 `BreakpointManager::enabled()` 的判据（`db/test_breakpoints.cpp`）
 *     —— 不置 true 断点静默不命中（断言恒真），不复位则把"测试模式"泄漏给本文件之后
 *     的套件（且泄漏的是 `true` 还是 `false` 取决于文件内套件顺序）；
 *   · 断点表跨用例残留 → 上一个用例注入的异常在下一个用例里触发。
 * 所以这里统一：SetUp 建立（含 `clear_all()`）、TearDown 复原**到默认值**。
 *
 * 两个 fixture 仍各立其名（sandbox 目录名与是否需要 `<root>/etc` 不同），只是把公共
 * 部分收进基类 —— 不是靠"两边都记得写"。
 */
class OpSinkTestBase : public ::testing::Test
{
protected:
    fs::path suite_dir;
    fs::path root;
    std::vector<fs::path> stashes;

    /** sandbox 目录名（两个 fixture 不能共用：`remove_all` 会互相踩） */
    virtual fs::path suite_dir_name() const = 0;
    /** 需要 `<root>/etc` 的 fixture（save_config 要往那里放配置文件） */
    virtual bool needs_etc() const
    {
        return false;
    }

    void SetUp() override
    {
        init_localization();
        Config::instance().set_testing_mode(true);  // 断点表的开关，默认 false
        suite_dir = fs::absolute(suite_dir_name());
        fs::remove_all(suite_dir);
        root = suite_dir / "root";
        fs::create_directories(root);
        if (needs_etc()) fs::create_directories(root / "etc");
        Config::instance().set_root_path(root.string());
        Config::instance().init_filesystem();
        Cache::instance().load();
        BreakpointManager::instance().clear_all();
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        Config::instance().set_root_path("/");
        Config::instance().set_testing_mode(false);  // 复位到 Config 的默认值
        fs::remove_all(suite_dir);
    }

    std::string read_wal() const
    {
        std::ifstream f(wal::wal_log_path());
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
};

class OpSinkNormalizationTest : public OpSinkTestBase
{
protected:
    fs::path suite_dir_name() const override
    {
        return "tmp_op_sink_test";
    }

    /**
     * 挑一个可安全拿来当 rmdir 目标的**目录型挂载点**：优先当前为空的那一个
     * （= 调用方的 is_empty 守卫会放行的形态，也就是真实会走到 rmdir 的那条路）。
     *
     * rmdir 对挂载点恒 EBUSY，所以哪怕守卫缺失、这个用例也删不掉系统上的任何东西
     * —— 它测的是"有没有为它写 DIR_RM / 有没有报告跳过"，不是"删没删掉"。
     */
    static fs::path pick_mount_point_dir()
    {
        fs::path fallback;
        for (const auto& mp : mount_points()) {
            std::error_code ec;
            if (mp == "/" || !fs::is_directory(mp, ec)) continue;
            std::error_code empty_ec;
            if (fs::is_empty(mp, empty_ec)) return mp;
            if (fallback.empty()) fallback = mp;
        }
        return fallback;
    }
};

// ============================================================================
// backup：尾斜杠必须被剥掉，搬走的是链接本身而不是它的目标
// ============================================================================

TEST_F(OpSinkNormalizationTest, BackupMovesLinkItselfNotItsTarget)
{
    fs::create_directories(root / "realdir");
    std::ofstream(root / "realdir" / "keep.txt") << "keep\n";
    fs::create_directory_symlink("realdir", root / "link");

    OpSink sink("t", &stashes);
    const fs::path bak = sink.backup((root / "link").string() + "/");

    EXPECT_TRUE(fs::is_symlink(bak))
        << "搬进 stash 的应当是符号链接本身（尾斜杠没剥会去 rename 链接的目标）";
    EXPECT_FALSE(fs::exists(root / "link"));
    EXPECT_TRUE(fs::is_directory(root / "realdir")) << "链接目标目录被搬走/删掉了";
    EXPECT_TRUE(fs::exists(root / "realdir" / "keep.txt")) << "目标目录里的内容丢了";
    ASSERT_EQ(stashes.size(), 1u);

    const std::string w = read_wal();
    EXPECT_NE(w.find("BACKUP " + (root / "link").string() + " "), std::string::npos)
        << "WAL 行没有记录被搬走的对象：" << w;
    EXPECT_EQ(w.find((root / "link").string() + "/ "), std::string::npos)
        << "WAL 行里的路径带了尾斜杠（回滚侧会按它去找不存在的东西）：" << w;
}

// ============================================================================
// remove_empty_dir：symlink→目录 必须整体跳过（哪怕传进来带尾斜杠）
// ============================================================================

TEST_F(OpSinkNormalizationTest, RemoveEmptyDirSkipsDirSymlinkEvenWithTrailingSlash)
{
    fs::create_directories(root / "target");  // 空目录：老毛病会把 rmdir 穿过去删掉它
    fs::create_directory_symlink("target", root / "link2");

    OpSink sink("t", &stashes);
    EXPECT_NO_THROW(sink.remove_empty_dir((root / "link2").string() + "/"));

    EXPECT_TRUE(fs::is_symlink(root / "link2")) << "符号链接被删/被替换";
    EXPECT_TRUE(fs::is_directory(root / "target")) << "rmdir 穿过了尾斜杠，把链接目标删了";
    EXPECT_EQ(read_wal().find("DIR_RM"), std::string::npos) << "不该为它记录 DIR_RM";
}

// ============================================================================
// remove_empty_dir：**目录型挂载点**必须整体跳过（不写 WAL、不 rmdir）
//
// rmdir(2) 对挂载点一律返回 EBUSY（VFS 的 may_delete）。旧实现把这个错误码用
// `fs::remove(target, ec)` 静默吞掉，于是 DIR_RM 行已经写了、目录所有权也已经摘了，
// 盘面却什么都没变 —— 行与盘面不一致（回滚会把"本该被删掉的"目录重建出来，而它其实
// 一直还在）。pacman 对目录型 mountpoint 是**保留 + 不报错**，这里对齐；
// **文件型** mountpoint 不在此列（pacman 同样是 unlink→EBUSY 失败，不假装修得更好）。
// ============================================================================

TEST_F(OpSinkNormalizationTest, RemoveEmptyDirSkipsMountPoint)
{
    const fs::path mp = pick_mount_point_dir();
    if (mp.empty()) GTEST_SKIP() << "本环境的挂载表里没有可用作目标的目录型挂载点";

    OpSink sink("t", &stashes);
    EXPECT_EQ(sink.remove_empty_dir(mp), detail::DirRemoval::SkippedMountPoint)
        << "挂载点必须被报告为'跳过'（而不是'已删除'）：" << mp;

    EXPECT_TRUE(fs::is_directory(mp)) << "挂载点目录不见了：" << mp;
    EXPECT_EQ(read_wal().find("DIR_RM"), std::string::npos)
        << "为挂载点写了 DIR_RM：行声称目录已删，rmdir 却因 EBUSY 什么都没做（行与盘面不一致）："
        << read_wal();
}

TEST_F(OpSinkNormalizationTest, RemoveEmptyDirStripsTrailingSlashForRealDir)
{
    fs::create_directories(root / "emptydir");

    OpSink sink("t", &stashes);
    sink.remove_empty_dir((root / "emptydir").string() + "/");

    EXPECT_FALSE(fs::exists(root / "emptydir"));

    const std::string w = read_wal();
    EXPECT_NE(w.find("DIR_RM " + (root / "emptydir").string() + " "), std::string::npos) << w;
    EXPECT_EQ(w.find((root / "emptydir").string() + "/ "), std::string::npos)
        << "DIR_RM 行里的路径带了尾斜杠（逆操作会重建到别的路径上去）：" << w;
}

// ============================================================================
// commit_copy：目的路径带尾斜杠也要落到剥掉斜杠的那个名字上
// ============================================================================

TEST_F(OpSinkNormalizationTest, CommitCopyStripsTrailingSlashOnDestination)
{
    fs::create_directories(root / "sub");
    const fs::path tmp = root / "sub" / "x.lpkgtmp";
    std::ofstream(tmp) << "content\n";

    OpSink sink("t", &stashes);
    sink.commit_copy(tmp, (root / "sub" / "x").string() + "/");

    EXPECT_TRUE(fs::is_regular_file(root / "sub" / "x"))
        << "尾斜杠让 rename(2) 落到了别处（或直接失败）";
    EXPECT_FALSE(fs::exists(tmp));
    EXPECT_NE(read_wal().find("COPY " + tmp.string()), std::string::npos);
}

// ============================================================================
// save_config：配置文件改名保留（WAL SAVE_CONF），**不进 stash**
// ============================================================================

class OpSinkSaveConfigTest : public OpSinkTestBase
{
protected:
    fs::path suite_dir_name() const override
    {
        return "tmp_op_sink_save_test";
    }
    bool needs_etc() const override
    {
        return true;
    }

    std::string read_file(const fs::path& p) const
    {
        std::ifstream f(p);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
};

TEST_F(OpSinkSaveConfigTest, RenamesToLpkgsaveAndDoesNotRegisterStash)
{
    const fs::path conf = root / "etc" / "app.conf";
    std::ofstream(conf) << "USER-CONF\n";

    OpSink sink("t", &stashes);
    const fs::path saved = sink.save_config(conf);

    EXPECT_EQ(saved, fs::path(conf.string() + ".lpkgsave"));
    EXPECT_FALSE(fs::exists(conf));
    EXPECT_EQ(read_file(saved), "USER-CONF\n");
    // **不进 stash**：stash 在批次提交后会被 remove_all —— 记进去就等于把配置真删了，
    // 那是 --purge-config 的语义，不是"保留"。
    EXPECT_TRUE(stashes.empty()) << "save_config 不得记账 stash（提交后会被 remove_all 删掉）";
    EXPECT_NE(read_wal().find("SAVE_CONF " + conf.string() + " \xe2\x86\x92 " + saved.string()),
              std::string::npos);
}

TEST_F(OpSinkSaveConfigTest, ShiftsExistingSaveAndKeepsItsContent)
{
    const fs::path conf = root / "etc" / "app.conf";
    std::ofstream(conf) << "NEW-CONF\n";
    const fs::path saved = fs::path(conf.string() + ".lpkgsave");
    std::ofstream(saved) << "OLD-SAVE-1\n";
    std::ofstream(fs::path(saved.string() + ".1")) << "OLD-SAVE-2\n";

    OpSink sink("t", &stashes);
    sink.save_config(conf);

    EXPECT_EQ(read_file(saved), "NEW-CONF\n") << "本次配置应落在 .lpkgsave";
    EXPECT_EQ(read_file(fs::path(saved.string() + ".1")), "OLD-SAVE-2\n")
        << "第一个空闲后缀是 .2，.1 不得被覆盖";
    EXPECT_EQ(read_file(fs::path(saved.string() + ".2")), "OLD-SAVE-1\n")
        << "旧 .lpkgsave 必须被移位而不是被覆盖";
    const std::string w = read_wal();
    EXPECT_NE(w.find("SAVE_CONF " + saved.string() + " \xe2\x86\x92 " + saved.string() + ".2"),
              std::string::npos)
        << "移位本身也要 write-ahead（否则崩在移位与改名之间无法回滚）：" << w;
}

TEST_F(OpSinkSaveConfigTest, WriteAheadWindowLeavesOriginalInPlace)
{
    const fs::path conf = root / "etc" / "app.conf";
    std::ofstream(conf) << "USER-CONF\n";
    const fs::path saved = fs::path(conf.string() + ".lpkgsave");

    BreakpointManager::instance().set(
        "after_wal_probe", [] { throw LpkgException("injected failure: WAL 已写、rename 未做"); });

    OpSink sink("t", &stashes);
    EXPECT_THROW(sink.save_config(conf, "after_wal_probe"), LpkgException);

    EXPECT_EQ(read_file(conf), "USER-CONF\n") << "原文件必须还在原位";
    EXPECT_FALSE(fs::exists(saved));
    EXPECT_NE(read_wal().find("SAVE_CONF " + conf.string() + " \xe2\x86\x92 " + saved.string()),
              std::string::npos)
        << "行必须**先于** rename 落盘（write-ahead）";
    EXPECT_TRUE(stashes.empty());
}
