/**
 * test_active_rollback.cpp — 主动回滚测试（使用 BreakpointManager）
 *
 * 不同于手动构造 WAL 的被动 rec 测试，这些测试走真实的 install/remove
 * 管线，通过断点注入故障，验证 catch 块中的 batch_rollback 正确执行。
 */

#include "../test_base.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/db/wal_op.hpp"

namespace fs = std::filesystem;

class ActiveRollbackTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();
    BreakpointManager::instance().clear_all();
  }

  void TearDown() override {
    BreakpointManager::instance().clear_all();
    IntegrationTestBase::TearDown();
  }
};

// ============================================================================
// 安装中途断点 → 主动回滚 → 系统干净
// ============================================================================

TEST_F(ActiveRollbackTest, CopyBreakpointTriggersRollback) {
  // 断点：在 COPY WAL 写入后、rename 之前抛异常（fresh install 走 COPY 路径）
  BreakpointManager::instance().set("copy_after_wal_bp_cr_A",
      [] { throw LpkgException("injected failure: disk full during copy"); });

  std::string pA = create_pkg("bp_cr_A", "1.0", {}, {"bp_cr_A.so.1"});
  std::string pB = create_pkg("bp_cr_B", "1.0", {"bp_cr_A"});

  EXPECT_THROW(install_packages({pB, pA}), LpkgException);

  Cache::instance().load();
  EXPECT_TRUE(Cache::instance().get_installed_version("bp_cr_A").empty());
  EXPECT_TRUE(Cache::instance().get_installed_version("bp_cr_B").empty());
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/bp_cr_A"));
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/bp_cr_B"));
}

// ============================================================================
// 批量安装 [A, B]：A 成功，B 在 COMMIT 后抛异常 → 整批回滚
// ============================================================================

TEST_F(ActiveRollbackTest, AfterCommitBreakpointRollsBackWholeBatch) {
  // B 在 COMMIT 写入后、END 之前抛异常
  BreakpointManager::instance().set("after_commit_bp_B2",
      [] { throw LpkgException("injected crash after commit"); });

  std::string pA = create_pkg("bp_A2", "1.0", {}, {"bpA2.so.1"});
  std::string pB = create_pkg("bp_B2", "1.0", {"bp_A2"});

  EXPECT_THROW(install_packages({pB, pA}), LpkgException);

  // 整批应回滚
  Cache::instance().load();
  EXPECT_TRUE(Cache::instance().get_installed_version("bp_A2").empty())
      << "A should be rolled back even though it installed successfully";
  EXPECT_TRUE(Cache::instance().get_installed_version("bp_B2").empty());

  // 文件不应残留
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/bp_A2"));
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/bp_B2"));

  // WAL 应有 COMMIT_PKGS（回滚完结标记）
  std::ifstream f(wal::wal_log_path());
  std::string c((std::istreambuf_iterator<char>(f)), {});
  EXPECT_NE(c.find("COMMIT_PKGS"), std::string::npos);
}

// ============================================================================
// 移除中 BACKUP 后断点 → 主动回滚 → 包恢复
// ============================================================================

TEST_F(ActiveRollbackTest, RemoveBackupBreakpointRestoresPackage) {
  std::string p = create_pkg("bp_rm_x", "1.0");
  install_packages({p});
  EXPECT_TRUE(fs::exists(test_root / "usr/bin/bp_rm_x"));

  // 断点：移除过程中的 BACKUP WAL 写入后抛异常
  BreakpointManager::instance().set("rm_backup_after_wal_bp_rm_x",
      [] { throw LpkgException("injected failure during remove backup"); });

  EXPECT_THROW(remove_package("bp_rm_x", false), LpkgException);

  Cache::instance().load();
  EXPECT_FALSE(Cache::instance().get_installed_version("bp_rm_x").empty())
      << "package should still be installed after rollback";
  EXPECT_TRUE(fs::exists(test_root / "usr/bin/bp_rm_x"))
      << "binary should be restored";
}

// ============================================================================
// 升级中断点 → 回滚 → 旧版本保留
// ============================================================================

TEST_F(ActiveRollbackTest, UpgradeBreakpointPreservesOldVersion) {
  setup_local_mirror();

  std::string pV1 = create_pkg("bp_upg", "1.0", {}, {"bp_upg.so.1"});
  add_to_mirror("bp_upg", "1.0");
  install_packages({pV1});
  EXPECT_EQ(Cache::instance().get_installed_version("bp_upg"), "1.0");

  // 发布 v2
  {
    fs::path work = suite_work_dir / "_pkg_bp_upg_v2";
    fs::create_directories(work / "content" / "usr" / "bin");
    std::ofstream(work / "content" / "usr" / "bin" / "bp_upg") << "v2\n";
    std::ofstream(work / "metadata.json")
        << R"({"name":"bp_upg","version":"2.0","deps":[],"provides":["bp_upg.so.2"],"needed_so":[]})";
    std::string v2 = (pkg_dir / "bp_upg-2.0.lpkg").string();
    pack_package(v2, work.string(), "bp_upg", "2.0", {}, {"bp_upg.so.2"});
    auto m = suite_work_dir / "mirror" / "x86_64";
    fs::create_directories(m / "bp_upg");
    fs::copy(v2, m / "bp_upg" / "2.0.lpkg");
    std::ofstream idx(m / "index.txt");
    idx << "bp_upg|1.0:::bp_upg.so.1:;2.0:::bp_upg.so.2:|\n";
  }

  // 断点：升级 bp_upg 时 BACKUP 后抛异常
  BreakpointManager::instance().set("backup_after_wal_bp_upg",
      [] { throw LpkgException("injected upgrade failure"); });

  EXPECT_THROW(upgrade_packages(), LpkgException);

  // 应保持在 v1
  Cache::instance().load();
  EXPECT_EQ(Cache::instance().get_installed_version("bp_upg"), "1.0");
}

// ============================================================================
// 批量安装 [A,B,C]：C 的 COPY 后断点 → A,B 回滚
// ============================================================================

TEST_F(ActiveRollbackTest, ThirdPackageFailsRollsBackPreviousTwo) {
  // C 在 COPY WAL 写入后抛异常
  BreakpointManager::instance().set("copy_after_wal_bp_C3",
      [] { throw LpkgException("injected copy failure on 3rd pkg"); });

  std::string pA = create_pkg("bp_A3", "1.0", {}, {"bpA3.so.1"});
  std::string pB = create_pkg("bp_B3", "1.0", {"bp_A3"}, {"bpB3.so.1"});
  std::string pC = create_pkg("bp_C3", "1.0", {"bp_B3"}, {}, {"ghost_bp3.so"});

  EXPECT_THROW(install_packages({pC, pB, pA}), LpkgException);

  Cache::instance().load();
  EXPECT_TRUE(Cache::instance().get_installed_version("bp_A3").empty());
  EXPECT_TRUE(Cache::instance().get_installed_version("bp_B3").empty());
  EXPECT_TRUE(Cache::instance().get_installed_version("bp_C3").empty());

  EXPECT_FALSE(fs::exists(test_root / "usr/bin/bp_A3"));
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/bp_B3"));
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/bp_C3"));
}

// ============================================================================
// 断点不触发时（正常流程）不受影响
// ============================================================================

TEST_F(ActiveRollbackTest, NoBreakpointNormalFlowUnaffected) {
  // 不设断点 → 正常安装
  std::string p = create_pkg("bp_normal", "1.0");
  EXPECT_NO_THROW(install_packages({p}));
  EXPECT_FALSE(Cache::instance().get_installed_version("bp_normal").empty());
  EXPECT_TRUE(fs::exists(test_root / "usr/bin/bp_normal"));
}

// ============================================================================
// 断点抛出非 LpkgException → 仍能正确 unwind
// ============================================================================

TEST_F(ActiveRollbackTest, StdExceptionInBreakpointTriggersRollback) {
  BreakpointManager::instance().set("copy_after_wal_bp_std_x",
      [] { throw std::runtime_error("injected std exception"); });

  std::string p = create_pkg("bp_std_x", "1.0");
  EXPECT_THROW(install_packages({p}), std::runtime_error);

  Cache::instance().load();
  EXPECT_TRUE(Cache::instance().get_installed_version("bp_std_x").empty());
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/bp_std_x"));
}

// ============================================================================
// 验证回滚后的 WAL 包含 ROLLBACK + COMMIT_PKGS
// ============================================================================

TEST_F(ActiveRollbackTest, RollbackWritesRollbackAndCommitPkgs) {
  BreakpointManager::instance().set("copy_after_wal_bp_wal_x",
      [] { throw LpkgException("wal check"); });

  std::string p = create_pkg("bp_wal_x", "1.0");
  EXPECT_THROW(install_packages({p}), LpkgException);

  std::ifstream f(wal::wal_log_path());
  std::string c((std::istreambuf_iterator<char>(f)), {});
  EXPECT_NE(c.find("ROLLBACK"), std::string::npos)
      << "WAL must contain ROLLBACK marker";
  EXPECT_NE(c.find("COMMIT_PKGS"), std::string::npos)
      << "WAL must be closed with COMMIT_PKGS";
}

// ============================================================================
// 多个断点同时设置 → 第一个命中后清除
// ============================================================================

TEST_F(ActiveRollbackTest, MultipleBreakpointsOnlyFirstHits) {
  int hit_count = 0;
  BreakpointManager::instance().set("copy_after_wal_bp_multi1_x",
      [&] { hit_count++; throw LpkgException("first"); });
  BreakpointManager::instance().set("copy_after_wal_bp_multi1_x",
      [&] { hit_count++; throw LpkgException("duplicate"); });

  std::string p = create_pkg("bp_multi1_x", "1.0");
  EXPECT_THROW(install_packages({p}), LpkgException);

  EXPECT_EQ(hit_count, 1);
}

// ============================================================================
// remove -r 中途 SIGINT：已移除的包全部恢复（文件+DB）
// ============================================================================

TEST_F(ActiveRollbackTest, RecursiveRemoveSIGINTMidwayRestoresAll) {
  // 构建依赖链: leaf ← mid ← root
  std::string pLeaf = create_pkg("rr_leaf", "1.0");
  std::string pMid = create_pkg("rr_mid", "1.0", {"rr_leaf"});
  std::string pRoot = create_pkg("rr_root", "1.0", {"rr_mid"});

  install_packages({pRoot, pMid, pLeaf});

  // 确认全部安装
  for (auto &n : {"rr_root", "rr_mid", "rr_leaf"}) {
    EXPECT_FALSE(Cache::instance().get_installed_version(n).empty());
    EXPECT_TRUE(fs::exists(test_root / "usr/bin" / n));
  }

  // 断点：在 rr_root 的 BACKUP WAL 写入后抛异常
  // （root 是叶子插入顺序的最后，但 remove 顺序的中间）
  BreakpointManager::instance().set("rm_backup_after_wal_rr_root",
      [] { throw LpkgException("injected SIGINT during recursive remove"); });

  // 模拟递归移除 rr_leaf（所有 3 个都在受影响集中）
  EXPECT_THROW(remove_package_recursive("rr_leaf", true), LpkgException);

  // 回滚后全部 3 个包应恢复
  Cache::instance().load();
  for (auto &n : {"rr_root", "rr_mid", "rr_leaf"}) {
    EXPECT_FALSE(Cache::instance().get_installed_version(n).empty())
        << n << " should be reinstalled after rollback";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin" / n))
        << n << " binary should be restored from .lpkg_bak";
  }

  // WAL 应有 COMMIT_PKGS
  std::ifstream f(wal::wal_log_path());
  std::string c((std::istreambuf_iterator<char>(f)), {});
  EXPECT_NE(c.find("COMMIT_PKGS"), std::string::npos);

  // 不应有 .lpkg_bak 残留
  std::error_code ec_bak;
  for (const auto &e : fs::recursive_directory_iterator(
           test_root / "usr", ec_bak)) {
    EXPECT_EQ(e.path().filename().string().find(".lpkg_bak_"),
              std::string::npos)
        << "no .lpkg_bak should remain: " << e.path();
  }
}

// RecoveryAfterRollbackAllowsNormalReRemoval: 回滚后可重新正常移除（需要完整 remove -r 流程修复后启用）
