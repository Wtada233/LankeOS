/**
 * test_durable_fsync_db.cpp — "DB/元数据写永远持久化"回归
 *
 * 缺陷（大设计缺陷）：`durable_fsync_enabled()` 默认关闭时，它门控的两个写入原语
 * （`fsync_and_rename` 里 .tmp 的 ::fsync、`fsync_dir_internal`）把 **DB 一族**也一起
 * 关掉了：`Cache::write` → `write_set_file_wal` / `write_db_file_wal` 走的正是
 * `fsync_and_rename`。于是"新库已经 rename 到位、内容还在页缓存"与
 * `finish_committed_batch()` 紧接着 `cleanup_db_backups()` 删掉
 * `.lpkg_db_bak_before:*` 构成一个**系统级不可恢复**窗口：断电落在窗口内 → DB 空/截断
 * 且唯一备份已删（files.db / pkgs 是单文件，丢了就是全库所有权归零）。
 *
 * 对照上游：libalpm 从不 fsync 任何东西、local DB 还是 `fopen(path,"w")` 原地覆盖、
 * 无 tmp+rename、无备份 —— lpkg 手里本来就有严格优于 pacman 的机制，是"默认关 fsync +
 * 提交即删备份"把它自己降到了 pacman 之下。所以把 DB 一族从开关里摘出来是零风险正收益
 * （每里程碑约 5 次 fsync）。
 *
 * 本组用 `durable_fsync_count_for_tests()` 直接数两个原语发出了多少次 ::fsync：
 * **在开关关闭的前提下**，DB/元数据写路径必须仍然 > 0（红→绿），而"批量文件数据"
 * （包内容 / .lpkgtmp / 它们的 rename 父目录）必须仍然受开关控制（不能被顺手一起打开）。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/db/wal_op.hpp"

namespace fs = std::filesystem;

class DurableFsyncDbTest : public ::testing::Test
{
protected:
    fs::path suite_dir;
    fs::path test_root;

    void SetUp() override
    {
        suite_dir = fs::absolute("tmp_durable_fsync_test");
        if (fs::exists(suite_dir)) fs::remove_all(suite_dir);
        test_root = suite_dir / "root";
        fs::create_directories(test_root);

        Config::instance().set_root_path(test_root.string());
        Config::instance().set_testing_mode(true);
        Config::instance().init_filesystem();
        Cache::instance().load();

        // 用例之间不得泄漏开关状态（默认 false）
        set_durable_fsync_enabled(false);
    }

    void TearDown() override
    {
        set_durable_fsync_enabled(false);
        Config::instance().set_root_path("/");
        fs::remove_all(suite_dir);
    }

    /** 数出 [f] 期间两个原语发出的 fsync 次数（WAL 行自己的 fsync 不经过它们，不计入） */
    template <typename F>
    size_t fsyncs_during(F&& f)
    {
        const size_t before = durable_fsync_count_for_tests();
        f();
        return durable_fsync_count_for_tests() - before;
    }

    std::string read_wal() const
    {
        std::ifstream f(wal::wal_log_path());
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
};

// ── DB 写（Cache::write → write_*_file_wal）在开关关闭时也必须持久化 ──────────
TEST_F(DurableFsyncDbTest, DbWriteFsyncsEvenWithSwitchOff)
{
    set_durable_fsync_enabled(false);
    Cache::instance().add_installed("fsyncpkg", "1.0");

    const size_t n = fsyncs_during([&] { Cache::instance().write("fsyncpkg:installed"); });

    EXPECT_GT(n, 0u)
        << "开关关闭时 DB 写路径一次 fsync 都没做：新库已 rename 到位、内容还在页缓存，"
           "而 finish_committed_batch 紧接着就删掉 .lpkg_db_bak_before 备份 —— "
           "断电落在窗口内即全库所有权归零且不可恢复";
}

// ── 同样走 fsync_and_rename 的元数据写（dep/needed_so/man）也不能被关掉 ──────
TEST_F(DurableFsyncDbTest, MetadataWriteFsyncsEvenWithSwitchOff)
{
    set_durable_fsync_enabled(false);
    const fs::path dep = Config::instance().dep_dir() / "metapkg";

    const size_t n = fsyncs_during(
        [&] { wal::write_string_file_wal(dep.string(), "glibc\nzlib\n", "metapkg:installed"); });

    EXPECT_TRUE(fs::exists(dep));
    EXPECT_GT(n, 0u) << "依赖/needed_so/man 元数据写没有 fsync：它们同样是单文件、同样由 "
                        "cleanup_db_backups 在提交后删备份，丢了就是依赖图归零";
}

// ── WAL 打开/首次创建：目录项 fsync 同样不受开关影响 ────────────────────────
//
// 上面两条守的是"DB/元数据**内容**"。WAL **文件本身**还需要一条独立的保证：
// POSIX 下 `fsync(文件)` 不覆盖**父目录的 dentry**，所以"WAL 行已经 fsync 过、而
// 文件整个不存在"是一个真实可能的断电后状态 —— 行是"某个操作已开始、可能未完成"
// 的唯一证据，行没了就没有回滚依据，而物理操作已经做完（不可恢复组合）。因此
// 创建 WAL 文件这条路径上的目录项 fsync 必须恒生效，与批量文件数据的开关无关。
TEST_F(DurableFsyncDbTest, WalLineOpenFsyncsParentDirEvenWithSwitchOff)
{
    set_durable_fsync_enabled(false);
    ASSERT_FALSE(fs::exists(wal::wal_log_path()))
        << "用例前提：WAL 文件尚不存在（首次创建才产生 dentry）";

    const size_t n = fsyncs_during([&] { wal::log_wal_line("BEGIN_PKGS"); });

    EXPECT_TRUE(fs::exists(wal::wal_log_path()));
    EXPECT_GT(n, 0u) << "开关关闭时创建 WAL 文件没有 fsync 父目录：fsync(文件) 管不到 dentry，"
                        "断电可能丢掉整个 WAL 文件，而它描述的物理操作已经做完 —— 无行可回滚";
}

TEST_F(DurableFsyncDbTest, WalWriterOpenFsyncsParentDirEvenWithSwitchOff)
{
    set_durable_fsync_enabled(false);
    ASSERT_FALSE(fs::exists(wal::wal_log_path()))
        << "用例前提：WAL 文件尚不存在（首次创建才产生 dentry）";

    const size_t n = fsyncs_during([&] { auto writer = wal::begin_batch(); });

    EXPECT_TRUE(fs::exists(wal::wal_log_path()));
    EXPECT_GT(n, 0u) << "WalWriter 的打开路径是同一个「WAL 首次创建」路径，必须一并恒持久化";
}

// ── 第三条 WAL 打开路径：`wal_append_raw`（回滚审计行）的目录项 fsync ──────────
//
// WAL 文件有三条打开/创建路径：`WalWriter` 构造（上面那条）、`wal::log_wal_line`、以及
// `db/wal_op.cpp` 里 `wal_append_raw`（`reverse_execute` 写 RESTORE_* 审计行与
// `batch_rollback` 写 ROLLBACK/END/COMMIT_PKGS）。第三条前两处已套 DurableFsyncGuard，
// 这一条曾经连 `fsync_parent_dir()` 都没调 —— 危害低（走到它时 WAL 必然已存在，dentry
// 早在别处落过盘），但"同一类保证只在两条路径上生效、第三条靠运气"不是可维护的形态：
// 判据必须是"这条路径**会不会**创建 WAL 文件"，而不是"今天它是不是恰好不会"。
//
// 注意这条判据的另一面：**只有真的创建了 WAL 文件才需要付父目录 fsync**。文件已存在
// 时再把父目录 fsync 一遍是纯粹的浪费（见下面两条 ExistingWal 用例）。
TEST_F(DurableFsyncDbTest, WalAuditAppendFsyncsParentDirOnlyWhenItCreatesTheFile)
{
    set_durable_fsync_enabled(false);

    // NEW 的逆操作 = 删除该文件；write_audit=true → 经 wal_append_raw 写 RESTORE_FILE_RM。
    // 这一步里没有任何别的 fsync 来源（safe_remove 是纯 unlink），所以数出来的次数
    // 恰好就是 wal_append_raw 自己那次目录项 fsync。
    const fs::path victim = test_root / "usr/lib/libaudit.so.1";
    fs::create_directories(victim.parent_path());
    std::ofstream(victim) << "x";

    wal::WALOp op;
    op.type = wal::WALOpType::NEW;
    op.arg1 = victim.string();

    // (a) WAL **不存在** → 本次调用创建它 → 必须恰好 fsync 一次父目录
    ASSERT_FALSE(fs::exists(wal::wal_log_path())) << "用例前提：WAL 尚不存在（首次创建）";
    const size_t created = fsyncs_during(
        [&] { wal::reverse_execute(std::vector<wal::WALOp>{op}, /*write_audit=*/true); });
    EXPECT_FALSE(fs::exists(victim)) << "用例前提：NEW 的逆操作真的删掉了目标（审计行才会写）";
    ASSERT_TRUE(fs::exists(wal::wal_log_path())) << "这次调用应该创建了 WAL 文件";
    EXPECT_EQ(created, 1u)
        << "创建 WAL 文件时没有 fsync 父目录：fsync(文件) 管不到 dentry，断电可能丢掉整个 "
           "WAL 文件，而它描述的物理操作已经做完 —— 无行可回滚";

    // (b) WAL **已存在** → 本次调用没有创建任何东西 → 父目录 fsync 次数必须是 0
    //     （否则每条审计行都白付一次父目录 fsync，实测占 fsync 总数 34%~40%）。
    //     "审计行真的写了"要一并钉住：否则 0 次可能只是"什么都没发生"。
    const fs::path victim2 = test_root / "usr/lib/libaudit2.so.1";
    std::ofstream(victim2) << "x";
    wal::WALOp op2;
    op2.type = wal::WALOpType::NEW;
    op2.arg1 = victim2.string();

    const size_t again = fsyncs_during(
        [&] { wal::reverse_execute(std::vector<wal::WALOp>{op2}, /*write_audit=*/true); });
    EXPECT_FALSE(fs::exists(victim2)) << "用例前提：第二次的逆操作也真的执行了";
    EXPECT_NE(read_wal().find("RESTORE_FILE_RM " + victim2.string()), std::string::npos)
        << "审计行没写进 WAL —— 那么'0 次 fsync'只是什么都没发生，不是本用例要证的事";
    EXPECT_EQ(again, 0u)
        << "WAL 文件已存在，这条审计行却仍然付了一次父目录 fsync（目录项只在**首次创建**"
           "时需要落盘）";
}

// ── 性能回归：已存在的 WAL 上连续追加 N 行，父目录 fsync 次数必须是 0 ──────────
//
// `wal::log_wal_line` 的注释写的是"**首次创建时**把目录项也落盘"，实现却在**每次调用**
// 都做一次 `fsync_parent_dir` —— 每条 WAL 行白付一次父目录 fsync（实测占 fsync 总数
// 34%~40%）。WAL 行自己的内容 fsync（::fsync(fd)）不受影响，恒生效；这里只砍掉"文件
// 本来就在、dentry 早就落过盘"时那次多余的目录 fsync。
TEST_F(DurableFsyncDbTest, ExistingWalAppendDoesNotFsyncParentDirPerLine)
{
    set_durable_fsync_enabled(false);

    // 首次创建那一次仍然必须有（保证这条保证没被顺手一起删掉）
    const size_t created = fsyncs_during([&] { wal::log_wal_line("BEGIN_PKGS"); });
    ASSERT_TRUE(fs::exists(wal::wal_log_path()));
    EXPECT_EQ(created, 1u) << "WAL **首次创建**那次必须 fsync 一次父目录（目录项落盘）";

    constexpr int N = 5;
    const size_t per_line = fsyncs_during([&] {
        for (int i = 0; i < N; ++i) wal::log_wal_line("BEGIN pkg" + std::to_string(i) + " 1.0");
    });
    // 行确实写进去了（"0 次 fsync"不能是因为什么都没做）
    EXPECT_NE(read_wal().find("BEGIN pkg4 1.0"), std::string::npos);
    EXPECT_EQ(per_line, 0u) << "已存在的 WAL 上追加 " << N << " 行付了 " << per_line
                            << " 次父目录 fsync（应当为 0：目录项只在首次创建时需要落盘）";
}

// ── 开关保留的语义：批量**文件数据**的 fsync 仍然由它控制 ────────────────────
TEST_F(DurableFsyncDbTest, BatchFileDataFsyncsStillFollowTheSwitch)
{
    const fs::path dir = test_root / "usr/lib";
    fs::create_directories(dir);

    set_durable_fsync_enabled(false);
    EXPECT_EQ(fsyncs_during([&] { fsync_parent_dir(dir / "libfoo.so.1"); }), 0u)
        << "开关关闭时文件数据（包内容 / .lpkgtmp / 它们的 rename 父目录）仍必须走快路径 —— "
           "把开关整个接成恒真会让两万文件规模的安装慢到分钟级";

    set_durable_fsync_enabled(true);
    EXPECT_GT(fsyncs_during([&] { fsync_parent_dir(dir / "libfoo.so.1"); }), 0u)
        << "--fsync 下目录项 fsync 必须真的发出";
}

// ── 守卫的 RAII 语义：嵌套与显式 --fsync 下都要还原成"进入前的值" ───────────
TEST_F(DurableFsyncDbTest, GuardRestoresPreviousState)
{
    set_durable_fsync_enabled(false);
    {
        DurableFsyncGuard durable;
        EXPECT_TRUE(durable_fsync_enabled());
        {
            DurableFsyncGuard inner;
            EXPECT_TRUE(durable_fsync_enabled());
        }
        EXPECT_TRUE(durable_fsync_enabled()) << "内层守卫析构把外层作用域也一起关了";
    }
    EXPECT_FALSE(durable_fsync_enabled());

    set_durable_fsync_enabled(true);
    {
        DurableFsyncGuard durable;
        EXPECT_TRUE(durable_fsync_enabled());
    }
    EXPECT_TRUE(durable_fsync_enabled()) << "守卫析构把 --fsync 的全局设置清掉了";
    set_durable_fsync_enabled(false);
}
