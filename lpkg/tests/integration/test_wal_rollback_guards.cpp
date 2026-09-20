/**
 * test_wal_rollback_guards.cpp — 含空格 root 下的批次回滚 + 回滚守卫（TODO.md A1/A2/A3）
 *
 * 三个真实缺陷的行为级回归：
 *   A1 WAL 非箭头行按空格切 → 路径含空格时 DB 里程碑被截断 → reverse_execute 算出的
 *      备份名不存在 → **静默跳过 DB 回滚**，文件回退了而 DB 仍写着"已安装"。
 *      本文件把 root 建成含空格的路径（`.../root with space`），使**所有** state/WAL
 *      路径天然含空格 —— 这正是 `lpkg --root "路径含空格"` 的真实形态。
 *   A2 破损尾部行冒充批次起点 → extract_current_batch_ops 返回空 → 整批不回滚。
 *   A3 回滚没做成时却无条件 cleanup_db_backups() → DB 备份被删，rec 永远还原不回来。
 *
 * 断言都落在**用户可见状态**上：文件是否还原、DB 是否还原、备份是否还在。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"

namespace fs = std::filesystem;

class WalRollbackGuardTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        Config::instance().set_no_hooks_mode(true);
        init_localization();
        BreakpointManager::instance().clear_all();

        suite_work_dir = fs::absolute("tmp_wal_guards_test");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
        // 关键：root 路径含空格 → state_dir/WAL/DB 全部带空格（重现 A1 的必然触发条件）
        test_root = suite_work_dir / "root with space";
        pkg_dir = suite_work_dir / "pkgs";
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    /** 打一个只含单个 bin 文件的虚拟包 */
    std::string create_pkg(const std::string& name, const std::string& version,
                           const std::vector<std::string>& deps = {})
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name);
        fs::create_directories(work / "content" / "usr" / "bin");
        std::ofstream(work / "content" / "usr" / "bin" / name) << "#!/bin/sh\necho " << name << "\n";
        const std::string path = (pkg_dir / (name + "-" + version + ".lpkg")).string();
        pack_package(path, work.string(), name, version, deps, {}, "man " + name, {});
        return path;
    }

    std::string read_file(const fs::path& p) const
    {
        std::ifstream f(p);
        if (!f.is_open()) return "";
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    std::string read_wal() const
    {
        return read_file(wal::wal_log_path());
    }

    /** 数出仍未清理的 DB 备份（回滚已消费 + 清理过的批次应为 0） */
    int count_db_backups() const
    {
        int n = 0;
        const fs::path state = Config::instance().state_dir();
        if (!fs::exists(state)) return n;
        for (const auto& e : fs::recursive_directory_iterator(state))
            if (e.path().filename().string().find(".lpkg_db_bak_before:") != std::string::npos) ++n;
        return n;
    }
};

// ============================================================================
// 前置自检：确认这个 fixture 真的在"含空格路径"下运行
// ============================================================================

TEST_F(WalRollbackGuardTest, SandboxPathsReallyContainSpaces)
{
    const auto has_space = [](const std::string& s) {
        return s.find(' ') != std::string::npos;
    };
    EXPECT_TRUE(has_space(Config::instance().pkgs_file().string()));
    EXPECT_TRUE(has_space(Config::instance().state_dir().string()));
    EXPECT_TRUE(has_space(wal::wal_log_path()));
    EXPECT_TRUE(has_space(Config::instance().files_db().string()));
}

// ============================================================================
// A1：含空格路径下的批次回滚必须把文件与 DB 一起还原
// ============================================================================

TEST_F(WalRollbackGuardTest, FailedBatchRestoresBothFilesAndDbWhenRootHasSpaces)
{
    const std::string a = create_pkg("wa", "1.0");
    const std::string b = create_pkg("wb", "1.0", {"wa"});  // 依赖 wa → 顺序确定 [wa, wb]

    // wa 装完并写入 DB 里程碑（cache.write("wa:installed")）之后，在 wb 的安装起点打断
    BreakpointManager::instance().set("install_after_begin_wb", [] {
        throw LpkgException("interrupt before wb files land");
    });
    EXPECT_THROW(install_packages({a, b}), LpkgException);
    BreakpointManager::instance().clear_all();

    // 文件已回退
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/wa")) << "wa 的文件未被回滚";
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/wb"));

    // 关键：DB 也必须回到批次开始前 —— 旧解析器会因备份名被截断而静默跳过 DB 回滚，
    // 留下"DB 说 wa 已安装、磁盘上却没有"的永久不一致
    const std::string pkgs = read_file(Config::instance().pkgs_file());
    EXPECT_EQ(pkgs.find("wa:1.0"), std::string::npos)
        << "DB 未回滚，仍记录 wa:1.0（含空格路径下 DB 备份名被截断）：" << pkgs;
    EXPECT_FALSE(Cache::instance().is_installed("wa"));

    // 批次已收尾 → trim_completed() 会把整个已完成批次（含 COMMIT_PKGS）裁掉，
    // 所以"收尾"的可观测形态是：WAL 里不再有本批次的 BEGIN_PKGS，且备份清干净
    EXPECT_EQ(read_wal().find("BEGIN_PKGS"), std::string::npos) << "批次未收尾（WAL 未被 trim）";
    EXPECT_EQ(count_db_backups(), 0) << "批次已收尾却残留 DB 备份";
}

TEST_F(WalRollbackGuardTest, SuccessfulBatchKeepsInstalledStateWhenRootHasSpaces)
{
    // 正向对照：同样的含空格 root，成功批次必须真的装上（防止上面的断言被"什么都没做"蒙过）
    const std::string a = create_pkg("wa", "1.0");
    install_packages({a});

    EXPECT_TRUE(fs::exists(test_root / "usr/bin/wa"));
    EXPECT_NE(read_file(Config::instance().pkgs_file()).find("wa:1.0"), std::string::npos);
    EXPECT_TRUE(Cache::instance().is_installed("wa"));
    EXPECT_EQ(count_db_backups(), 0);
}

// ============================================================================
// A2：破损尾部行不得让整批回滚失效
// ============================================================================

TEST_F(WalRollbackGuardTest, BrokenWalTailStillRollsBackWholeBatch)
{
    const std::string a = create_pkg("wa", "1.0");
    const std::string b = create_pkg("wb", "1.0", {"wa"});

    BreakpointManager::instance().set("install_after_begin_wb", [] {
        // 模拟"批次中途 WAL 尾部出现半写行"（断电/磁盘满的典型残留）：
        // 旧代码会把这类行解析成 BEGIN_PKGS 哨兵，反向扫描从它开始 → 操作集被截断
        std::ofstream(wal::wal_log_path(), std::ios::app) << "PY /tmp/x \xe2\x86\x92 /usr/bin/x\n";
        throw LpkgException("interrupt after torn wal tail");
    });
    EXPECT_THROW(install_packages({a, b}), LpkgException);
    BreakpointManager::instance().clear_all();

    // 整批仍必须完整回滚（含 wa 已装的包），且批次正常收尾（WAL 被 trim，不含 BEGIN_PKGS）
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/wa")) << "破损尾部行导致 wa 未被回滚";
    EXPECT_EQ(read_file(Config::instance().pkgs_file()).find("wa:1.0"), std::string::npos);
    EXPECT_EQ(read_wal().find("BEGIN_PKGS"), std::string::npos) << "破损尾部行导致批次未能收尾";
}

// ============================================================================
// A3：回滚没能执行时，绝不清理 DB 备份（否则 rec 永远还原不回来）
// ============================================================================

TEST_F(WalRollbackGuardTest, UnclosableRollbackKeepsDbBackupsForRec)
{
    const std::string a = create_pkg("wa", "1.0");

    BreakpointManager::instance().set("install_after_begin_wa", [] {
        // 模拟"WAL 记录整段丢失"（state 目录被误删/文件被截断）：此时提取不到任何
        // 可回滚的批次，batch_rollback 必须返回 false，调用方据此保留 DB 备份
        std::ofstream(wal::wal_log_path(), std::ios::trunc);
        throw LpkgException("wal lost mid-batch");
    });
    EXPECT_THROW(install_packages({a}), LpkgException);
    BreakpointManager::instance().clear_all();

    // 批次开始时的 DB 备份必须还在（回滚没做成时它是唯一没被销毁的恢复依据；
    // 若被 cleanup_db_backups() 删掉，连人工恢复 DB 的余地都没有了）
    const fs::path start_bak =
        fs::path(Config::instance().pkgs_file().string() + ".lpkg_db_bak_before::batch-start");
    EXPECT_TRUE(fs::exists(start_bak))
        << "回滚未完成却清理了 DB 备份 → 再也没有还原 DB 的依据";
    EXPECT_GT(count_db_backups(), 0);

    // 且没有被补写一个假的收尾标记（WAL 是被清空的，这里断言的是"没有谎报已提交"）
    EXPECT_EQ(read_wal().find("COMMIT_PKGS"), std::string::npos);
}

TEST_F(WalRollbackGuardTest, NormalFailureConsumesAndCleansDbBackups)
{
    // 正向对照：正常失败的批次必须真的把备份消费掉并清理（守卫不能过宽，
    // 否则备份会在每次失败后无限累积）
    const std::string a = create_pkg("wa", "1.0");

    BreakpointManager::instance().set("install_after_begin_wa", [] {
        throw LpkgException("plain interrupt");
    });
    EXPECT_THROW(install_packages({a}), LpkgException);
    BreakpointManager::instance().clear_all();

    EXPECT_EQ(read_file(Config::instance().pkgs_file()).find("wa:1.0"), std::string::npos);
    EXPECT_EQ(read_wal().find("BEGIN_PKGS"), std::string::npos) << "批次未收尾";
    EXPECT_EQ(count_db_backups(), 0) << "回滚已收尾，DB 备份应被清理干净";
}

// ============================================================================
// X6：两个未提交批次时，恢复区域必须只覆盖**最近**一批
// ============================================================================

TEST_F(WalRollbackGuardTest, TwoUncommittedBatchesConvergeInOnePass)
{
    // Z5 端到端：两个未提交批次（前一批回滚失败后同进程又开了新批次）+ 生产布局
    // （stash 落在**文件系统顶层** = root_dir 直接子目录，正是孤儿回收的扫描范围，
    //  而被延迟批次的 pid 必然已死）。
    //
    // 曾有的错误：恢复区域起点取"**最后一个**未配对 BEGIN_PKGS"，于是每轮都在处理最近那批
    // （其行仍在 WAL 里），更早那批永远轮不到 —— 实测两轮下来文件一次都没被还原。
    // 现在（区域起点 = 第一个未配对）一次逆序回滚整个未提交区域，**一轮收敛**。
    const fs::path orig = test_root / "usr/bin/from_batch1";
    fs::create_directories(orig.parent_path());
    const fs::path stash1 = test_root / ".lpkg_bak_A_999999";
    const fs::path bak1 = stash1 / "from_batch1.lpkg_bak_A_ab12cd";
    const fs::path stash2 = test_root / ".lpkg_bak_B_999999";
    fs::create_directories(stash1);
    fs::create_directories(stash2);
    std::ofstream(bak1) << "batch1 content\n";
    std::ofstream(stash2 / "junk.lpkg_bak_B_zz") << "batch2 leftover\n";
    {
        std::ofstream w(wal::wal_log_path());
        w << "BEGIN_PKGS\n";
        w << "BEGIN A 1.0\n";
        w << "BACKUP " << orig.string() << " \xe2\x86\x92 " << bak1.string() << "\n";
        w << "BEGIN_PKGS\n";
        w << "BEGIN B 1.0\n";
        w << "BACKUP " << (test_root / "usr/bin/b2").string() << " \xe2\x86\x92 "
          << (stash2 / "b2.lpkg_bak_B_yy").string() << "\n";
        w << "CLEANUP " << stash2.string() << "\n";
    }

    // 真实启动序列（main.cpp 顺序）两轮：第一轮收敛，第二轮必须幂等 ——
    // 且中途的孤儿回收**不得**碰 WAL 仍引用的 stash（否则前一批再无还原依据）
    for (int pass = 0; pass < 2; ++pass) {
        ASSERT_NO_THROW(recover_packages()) << "pass " << pass;
        trim_completed();
        cleanup_orphan_stashes(wal::referenced_stash_roots());
    }

    EXPECT_TRUE(fs::exists(orig)) << "前一批的文件没有被还原（收敛失败）";
    EXPECT_FALSE(fs::exists(bak1)) << "前一批的备份已被消费，不该还在";
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(test_root, ec)) {
        if (ec) break;
        EXPECT_EQ(e.path().filename().string().find(".lpkg_bak_"), std::string::npos)
            << "残留 stash: " << e.path();
    }
}

// （原 RecoveryOfLaterBatchKeepsEarlierBatchsBackups 已删除：它断言"恢复区域只覆盖
//   最近一批、前一批保留待后续 pass"——那是区域起点取错的产物，现行语义是
//   "一次逆序回滚整个未提交区域、一轮收敛"，由下方 TwoUncommittedBatchesConvergeInOnePass 覆盖。）

