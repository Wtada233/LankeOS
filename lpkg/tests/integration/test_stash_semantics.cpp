/**
 * test_stash_semantics.cpp — stash + DIR_RM 的专属语义测试（TODO.md §6 测试补位）
 *
 * 覆盖（WAL 级 + 行为级）：
 *   1. DIR_RM 行解析（mode/uid/gid）
 *   2. reverse 重建目录（含元数据、父链）
 *   3. reverse 幂等（目录已存在）
 *   4. reverse 顺序：先重建被删目录、再把其内文件从 stash 还原
 *   5. purge_consumed_stashes：reverse 完成后清空 stash 根
 *   6. cleanup_orphan_stashes：只删 pid 已死的 stash
 *   7. remove 一次写**单条** CLEANUP（stash 根），不逐文件
 *   8. stash 挂在设备顶层（=root），绝不落在会被删除的包子树里
 */

#include <gtest/gtest.h>
#include <sys/types.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/pkg/install_common.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class StashSemanticsTest : public IntegrationTestBase
{
protected:
    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        Config::instance().set_no_hooks_mode(true);
        BreakpointManager::instance().clear_all();
        setup_local_mirror();
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        IntegrationTestBase::TearDown();
    }

    static void touch(const fs::path& p, const std::string& data = "x")
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << data;
    }

    std::string read_wal() const
    {
        std::ifstream f(wal::wal_log_path());
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
};

// ============================================================================
// 1) DIR_RM 解析
// ============================================================================

TEST_F(StashSemanticsTest, ParseDirRmOpFields)
{
    auto op = wal::parse_op("DIR_RM /usr/lib/acl 493 0 0");
    EXPECT_EQ(op.type, wal::WALOpType::DIR_RM);
    EXPECT_EQ(op.arg1, "/usr/lib/acl");
    EXPECT_EQ(op.arg2, "493");  // 0755
    EXPECT_EQ(op.arg3, "0");
    EXPECT_EQ(op.arg4, "0");
    EXPECT_FALSE(op.is_metadata());
    EXPECT_FALSE(op.skip_in_reverse());  // 可逆（回滚要重建目录）
}

// ============================================================================
// 2) reverse 重建目录（含父链 + mode）
// ============================================================================

TEST_F(StashSemanticsTest, ReverseDirRmRecreatesDirWithMeta)
{
    const fs::path p = test_root / "usr" / "lib" / "acl" / "deep";
    // p 不存在（已被删除）；reverse 应整链重建并赋 mode
    std::vector<wal::WALOp> ops;
    auto op = wal::parse_op("DIR_RM " + p.string() + " 493 0 0");
    ops.push_back(op);

    wal::RollbackStats st = wal::reverse_execute(ops, /*write_audit=*/false);

    EXPECT_GE(st.dirs_recreated, 1);
    EXPECT_TRUE(fs::is_directory(p));
    std::error_code ec;
    auto perms = fs::status(p, ec).permissions();
    EXPECT_EQ(perms & fs::perms::owner_all, fs::perms::owner_all) << "目录应按记录 mode(0755) 重建";
}

// ============================================================================
// 3) reverse 幂等：目录已存在 → 不报错、不重复建坏
// ============================================================================

TEST_F(StashSemanticsTest, ReverseDirRmIdempotentWhenDirExists)
{
    const fs::path p = test_root / "usr" / "share" / "pkg";
    fs::create_directories(p);

    std::vector<wal::WALOp> ops;
    auto op = wal::parse_op("DIR_RM " + p.string() + " 493 0 0");
    ops.push_back(op);

    EXPECT_NO_THROW(wal::reverse_execute(ops, false));
    EXPECT_TRUE(fs::is_directory(p));
}

// ============================================================================
// 4) reverse 顺序：目录先重建，其内文件再从 stash 还原
// ============================================================================

TEST_F(StashSemanticsTest, ReverseRestoresFileAfterDirRmRecreate)
{
    const fs::path dir = test_root / "usr" / "lib" / "pkgx";
    const fs::path orig = dir / "data.so";
    // 现场：目录已被 rmdir（不存在），文件备份躺在 stash 里
    const fs::path stash = test_root / ".lpkg_bak_pkgx_123";
    const fs::path bak = stash / "data.so.lpkg_bak_pkgx_abc";
    touch(bak);

    // forward：先 BACKUP 文件、再 DIR_RM 目录
    std::vector<wal::WALOp> ops;
    auto b = wal::parse_op("BACKUP " + orig.string() + " \xe2\x86\x92 " + bak.string());
    auto d = wal::parse_op("DIR_RM " + dir.string() + " 493 0 0");
    ops.push_back(b);
    ops.push_back(d);

    wal::reverse_execute(ops, false);  // 逆序：d 先（重建目录），b 后（还原文件）

    EXPECT_TRUE(fs::exists(orig)) << "文件应从 stash 还原进重建的目录";
    EXPECT_FALSE(fs::exists(bak));
    EXPECT_TRUE(fs::is_directory(dir));
}

// ============================================================================
// 5) purge_consumed_stashes：reverse 后清空 stash 根
// ============================================================================

TEST_F(StashSemanticsTest, PurgeConsumedStashesRemovesStashRoots)
{
    const fs::path stash = test_root / ".lpkg_bak_pkgx_123";
    const fs::path bak1 = stash / "a.lpkg_bak_pkgx_1";
    const fs::path bak2 = stash / "b.lpkg_bak_pkgx_2";
    touch(bak1);
    touch(bak2);

    std::vector<wal::WALOp> ops;
    auto o1 = wal::parse_op("BACKUP /x/a \xe2\x86\x92 " + bak1.string());
    auto o2 = wal::parse_op("BACKUP /y/b \xe2\x86\x92 " + bak2.string());
    ops.push_back(o1);
    ops.push_back(o2);

    wal::purge_consumed_stashes(ops);

    EXPECT_FALSE(fs::exists(stash)) << "purge 应整目录 remove_all stash 根";
    EXPECT_FALSE(fs::exists(bak1));
    EXPECT_FALSE(fs::exists(bak2));
}

// ============================================================================
// 6) cleanup_orphan_stashes：只删 pid 已死，不碰自己/存活
// ============================================================================

TEST_F(StashSemanticsTest, CleanupOrphanStashesReapsOnlyDeadPid)
{
    const fs::path dead = test_root / ".lpkg_bak_pkgx_999999";  // 无此进程
    const fs::path self = test_root / (std::string(".lpkg_bak_pkgx_") + std::to_string(getpid()));
    const fs::path alive = test_root / (std::string(".lpkg_bak_pkgx_") + std::to_string(getppid()));
    touch(dead / "f");
    touch(self / "f");
    touch(alive / "f");

    cleanup_orphan_stashes();

    EXPECT_FALSE(fs::exists(dead)) << "pid 已死的 stash 应被回收";
    EXPECT_TRUE(fs::exists(self)) << "当前进程的 stash 绝不能动";
    EXPECT_TRUE(fs::exists(alive)) << "存活进程（父进程）的 stash 绝不能动";
}

// ============================================================================
// 7) remove 单条 CLEANUP（stash 根），不逐文件
// ============================================================================

TEST_F(StashSemanticsTest, RemoveWritesSingleCleanupPerStash)
{
    // 三文件小包
    const fs::path work = suite_work_dir / "_pkg_tiny";
    for (const char* f : {"usr/lib/tiny/1", "usr/lib/tiny/2", "usr/bin/tiny"}) {
        fs::path p = work / "content" / f;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "x";
    }
    const fs::path pkgf = pkg_dir / "tiny-1.0.lpkg";
    pack_package(pkgf.string(), work.string(), "tiny", "1.0");
    install_packages({pkgf.string()});

    // 在"首条 CLEANUP 已写、删除前"打断。注意：异常后 run_batch_transaction 会
    // continue_cleanup + trim_completed 清空日志，所以必须在断点动作里当场抓 WAL 快照。
    std::string snap;
    BreakpointManager::instance().set("cleanup_after_wal", [this, &snap] {
        snap = read_wal();
        throw LpkgException("interrupt right after CLEANUP row written");
    });
    EXPECT_THROW(remove_package("tiny", false), LpkgException);
    BreakpointManager::instance().clear_all();

    int n_cleanup = 0;
    std::string stash_path;
    std::istringstream in(snap);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("CLEANUP ", 0) == 0) {
            ++n_cleanup;
            stash_path = line.substr(8);
        }
    }
    EXPECT_EQ(n_cleanup, 1) << "remove 应恰好写一条 CLEANUP（stash 根），不逐文件：\n" << snap;
    EXPECT_TRUE(stash_path.find(".lpkg_bak_tiny_") != std::string::npos)
        << "CLEANUP 应指向 stash 根，got: " << stash_path;
    // 后续 continue_cleanup 已把 stash 根清掉（快照之外），磁盘上应无残留
    EXPECT_FALSE(fs::exists(fs::path(stash_path)));
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(test_root, ec)) {
        if (ec) break;
        EXPECT_EQ(e.path().filename().string().find(".lpkg_bak"), std::string::npos)
            << "不得残留 .lpkg_bak/stash: " << e.path();
    }
}

// ============================================================================
// 8) stash 挂在设备顶层（= root），不落在会被删除的包子树内
// ============================================================================

TEST_F(StashSemanticsTest, StashParentIsDeviceTopNotInsidePackageSubtree)
{
    const fs::path file = test_root / "usr" / "lib" / "acl" / "x.so";
    touch(file);

    // 备份目标在 root 顶层，而不是文件所在/将删除的子树里
    const fs::path parent = detail::stash_parent_dir(file);
    EXPECT_EQ(parent.lexically_normal(), test_root.lexically_normal());
    const fs::path stash = detail::ensure_stash_dir(file, "acl");
    EXPECT_TRUE(stash.filename().string().rfind(".lpkg_bak_acl_", 0) == 0);
    EXPECT_EQ(stash.parent_path(), parent);
    // 顶层 stash 存在且 0700（root-only，备份残留隔离）
    std::error_code ec;
    auto perms = fs::status(stash, ec).permissions();
    EXPECT_EQ(perms & fs::perms::others_all, fs::perms::none);
}
