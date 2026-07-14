/**
 * test_wal_atomic_transaction.cpp — WAL 2.0 原子事务集成测试
 *
 * 模拟真实用户场景：安装、移除、升级、中断恢复。
 * 遵循与现有集成测试相同的模式：使用 create_pkg() 返回的本地文件路径。
 */

#include "../test_base.hpp"

#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/db/wal_op.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ============================================================================
// 原子安装测试
// ============================================================================

class AtomicInstallTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();
  }
};

TEST_F(AtomicInstallTest, SinglePackageInstall) {
  std::string p = create_pkg("testpkg", "1.0", {}, {"libtest.so.1"});
  install_packages({p});
  EXPECT_FALSE(Cache::instance().get_installed_version("testpkg").empty());
}

TEST_F(AtomicInstallTest, BatchInstallWithDepsViaLocalFiles) {
  std::string pA = create_pkg("pkgA", "1.0", {}, {"libA.so.1"});
  std::string pB = create_pkg("pkgB", "1.0", {"pkgA"}, {"libB.so.1"});
  std::string pC = create_pkg("pkgC", "1.0", {"pkgB"}, {"libC.so.1"});

  install_packages({pC, pB, pA});

  EXPECT_FALSE(Cache::instance().get_installed_version("pkgC").empty());
  EXPECT_FALSE(Cache::instance().get_installed_version("pkgB").empty());
  EXPECT_FALSE(Cache::instance().get_installed_version("pkgA").empty());
}

TEST_F(AtomicInstallTest, FileOwnershipTracking) {
  std::string p = create_pkg("filepkg", "1.0");
  install_packages({p});

  auto owners = Cache::instance().get_file_owners("/usr/bin/filepkg");
  EXPECT_TRUE(owners.contains("filepkg"));
}

TEST_F(AtomicInstallTest, ProvidesRegistration) {
  std::string p = create_pkg("libpkg", "1.0", {}, {"libtest.so.1", "libtest.so.2"});
  install_packages({p});

  auto providers = Cache::instance().get_providers("libtest.so.1");
  EXPECT_TRUE(providers.contains("libpkg"));
}

// ============================================================================
// 原子移除测试
// ============================================================================

class AtomicRemoveTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();

    std::string p = create_pkg("removepkg", "1.0", {}, {"libremove.so.1"});
    install_packages({p});
  }
};

TEST_F(AtomicRemoveTest, SinglePackageRemove) {
  EXPECT_FALSE(Cache::instance().get_installed_version("removepkg").empty());
  remove_package("removepkg", false);
  EXPECT_TRUE(Cache::instance().get_installed_version("removepkg").empty());

  auto owners = Cache::instance().get_file_owners("/usr/bin/removepkg");
  EXPECT_FALSE(owners.contains("removepkg"));
}

TEST_F(AtomicRemoveTest, RemoveWithDependentsBlocked) {
  std::string pDep = create_pkg("depender", "1.0", {"removepkg"}, {});
  install_packages({pDep});

  // 尝试移除被依赖的包应该被阻止
  remove_package("removepkg", false);
  EXPECT_FALSE(Cache::instance().get_installed_version("removepkg").empty());
}

TEST_F(AtomicRemoveTest, ForceRemoveOverridesDeps) {
  std::string pDep = create_pkg("depender", "1.0", {"removepkg"}, {});
  install_packages({pDep});

  remove_package("removepkg", true);
  EXPECT_TRUE(Cache::instance().get_installed_version("removepkg").empty());
}

// ============================================================================
// 原子重装测试
// ============================================================================

class AtomicReinstallTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();

    std::string p = create_pkg("reinstpkg", "1.0", {}, {"libreinst.so.1"});
    install_packages({p});
  }
};

TEST_F(AtomicReinstallTest, ReinstallPreservesFiles) {
  EXPECT_EQ(Cache::instance().get_installed_version("reinstpkg"), "1.0");

  // 重装（使用原始包文件）
  std::string p = create_pkg("reinstpkg", "1.0", {}, {"libreinst.so.1"});
  install_packages({p}, "", true);

  EXPECT_EQ(Cache::instance().get_installed_version("reinstpkg"), "1.0");
  EXPECT_TRUE(fs::exists(test_root / "usr" / "bin" / "reinstpkg"));
}

// ============================================================================
// 升级回滚测试
// ============================================================================

class AtomicUpgradeTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();
  }
};

TEST_F(AtomicUpgradeTest, UpgradeToNewerVersion) {
  setup_local_mirror();

  // 安装旧版本
  std::string p1 = create_pkg("upgpkg", "1.0", {}, {"libupg.so.1"});
  add_to_mirror("upgpkg", "1.0");
  install_packages({p1});
  EXPECT_EQ(Cache::instance().get_installed_version("upgpkg"), "1.0");

  // 创建并发布 v2.0
  {
    fs::path work = suite_work_dir / "_pkg_upgpkg_v2";
    fs::create_directories(work / "content" / "usr" / "bin");
    std::ofstream(work / "content" / "usr" / "bin" / "upgpkg") << "#!/bin/sh\necho v2.0\n";
    std::ofstream(work / "metadata.json")
        << R"({"name":"upgpkg","version":"2.0","deps":[],"provides":["libupg.so.2"],"needed_so":[]})";

    std::string v2path = (pkg_dir / "upgpkg-2.0.lpkg").string();
    pack_package(v2path, work.string(), "upgpkg", "2.0", {}, {"libupg.so.2"});

    fs::path mirror = suite_work_dir / "mirror" / "x86_64";
    fs::create_directories(mirror / "upgpkg");
    fs::copy(v2path, mirror / "upgpkg" / "2.0.lpkg");
    {
      std::ofstream idx(mirror / "index.txt");
      idx << "upgpkg|1.0:::libupg.so.1:;2.0:::libupg.so.2:|\n";
    }
  }

  upgrade_packages();
  EXPECT_EQ(Cache::instance().get_installed_version("upgpkg"), "2.0");
}

// ============================================================================
// 依赖冲突与一致性测试
// ============================================================================

TEST_F(AtomicUpgradeTest, PlanConsistencyCheckBlocksConflict) {
  setup_local_mirror();

  // lib v1 + app（依赖 lib >= 1.0 < 2.0）
  std::string pLib = create_pkg("lib2", "1.0", {}, {"libfoo.so.1"});
  add_to_mirror("lib2", "1.0");
  install_packages({pLib});

  std::string pApp = create_pkg("app2", "1.0", {"lib2 >= 1.0 < 2.0"}, {});
  add_to_mirror("app2", "1.0");
  install_packages({pApp});

  // 发布 lib2 v2.0（不兼容）
  {
    fs::path work = suite_work_dir / "_pkg_lib2_v2";
    fs::create_directories(work / "content" / "usr" / "lib");
    std::ofstream(work / "content" / "usr" / "lib" / "libfoo.so.2") << "dummy";
    std::ofstream(work / "metadata.json")
        << R"({"name":"lib2","version":"2.0","deps":[],"provides":["libfoo.so.2"],"needed_so":[]})";

    std::string v2path = (pkg_dir / "lib2-2.0.lpkg").string();
    pack_package(v2path, work.string(), "lib2", "2.0", {}, {"libfoo.so.2"});

    fs::path mirror = suite_work_dir / "mirror" / "x86_64";
    fs::create_directories(mirror / "lib2");
    fs::copy(v2path, mirror / "lib2" / "2.0.lpkg");
    {
      std::ofstream idx(mirror / "index.txt");
      idx << "lib2|1.0:::libfoo.so.1:;2.0:::libfoo.so.2:|\n"
          << "app2|1.0::lib2 >= 1.0 < 2.0::|\n";
    }
  }

  EXPECT_THROW(upgrade_packages(), LpkgException);
}

// ============================================================================
// 恢复测试 — 模拟崩溃场景
// ============================================================================

class RecoveryTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();
  }
};

TEST_F(RecoveryTest, RecoverRestoresBackedUpFiles) {
  fs::path orig = test_root / "usr/bin/crashed_pkg";
  fs::path bak(orig.string() + std::string(constants::SUFFIX_LPKG_BAK) + "crashed_pkg");

  fs::create_directories(bak.parent_path());
  {
    std::ofstream f(bak);
    f << "original content";
  }

  Cache::instance().add_installed("crashed_pkg", "1.0");
  Cache::instance().add_file_owner("/usr/bin/crashed_pkg", "crashed_pkg");

  {
    std::ofstream w(wal::wal_log_path());
    w << "BEGIN_PKGS 1\n"
      << "BEGIN crashed_pkg 1.0\n"
      << "BACKUP " << orig.string() << " \xe2\x86\x92 " << bak.string() << "\n"
      << "COPY /tmp/crashed_pkg.lpkgtmp \xe2\x86\x92 " << orig.string() << "\n"
      << "COMMIT crashed_pkg 1.0\n"
      << "END crashed_pkg 1.0\n"
      << "DB " << (Config::instance().state_dir() / "pkgs").string()
      << " crashed_pkg:installed\n";
  }

  recover_packages();

  std::ifstream wf(wal::wal_log_path());
  std::string wal_content((std::istreambuf_iterator<char>(wf)),
                          std::istreambuf_iterator<char>());
  EXPECT_NE(wal_content.find("COMMIT_PKGS"), std::string::npos);
}

TEST_F(RecoveryTest, RecoverWithRestoreAuditLines) {
  fs::path orig = test_root / "usr/bin/partial_rb";
  fs::path bak(orig.string() + std::string(constants::SUFFIX_LPKG_BAK) + "partial_rb");

  fs::create_directories(bak.parent_path());
  std::ofstream(bak) << "backup data";

  {
    std::ofstream w(wal::wal_log_path());
    w << "BEGIN_PKGS 2\n"
      << "BEGIN A 1.0\n"
      << "BACKUP " << orig.string() << " \xe2\x86\x92 " << bak.string() << "\n"
      << "COPY /tmp/a \xe2\x86\x92 " << orig.string() << "\n"
      << "COMMIT A 1.0\n"
      << "END A 1.0\n"
      << "BEGIN B 1.0\n"
      << "ROLLBACK B 1.0\n"
      << "END B 1.0\n"
      << "RESTORE_DB /bak \xe2\x86\x92 /db\n";
  }

  EXPECT_NO_THROW(recover_packages());
}

TEST_F(RecoveryTest, RecoverHandlesEmptyWal) {
  EXPECT_NO_THROW(recover_packages());
}

TEST_F(RecoveryTest, RecoverHandlesNoWalFile) {
  fs::remove(wal::wal_log_path());
  EXPECT_NO_THROW(recover_packages());
}

// ============================================================================
// 二次回滚幂等性测试
// ============================================================================

class SecondaryRollbackTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();
  }
};

TEST_F(SecondaryRollbackTest, DoubleRecoverIsIdempotent) {
  fs::path orig = test_root / "usr/bin/dbl_recover";
  fs::path bak(orig.string() + std::string(constants::SUFFIX_LPKG_BAK) + "dbl_recover");

  fs::create_directories(bak.parent_path());
  std::ofstream(bak) << "pre-install data\n";

  {
    std::ofstream w(wal::wal_log_path());
    w << "BEGIN_PKGS 1\n"
      << "BEGIN dbl_recover 1.0\n"
      << "BACKUP " << orig.string() << " \xe2\x86\x92 " << bak.string() << "\n";
  }

  recover_packages();
  EXPECT_NO_THROW(recover_packages()); // 第二次幂等
}

TEST_F(SecondaryRollbackTest, CleanupDbBackupsClearsOrphans) {
  fs::path db_dir = Config::instance().state_dir();
  fs::path orphan1 = db_dir / "pkgs.lpkg_db_bak_before:test:installed";
  fs::path orphan2 = db_dir / "files.db.lpkg_db_bak_before:test:removed";

  std::ofstream(orphan1) << "orphan1";
  std::ofstream(orphan2) << "orphan2";

  cleanup_db_backups();

  EXPECT_FALSE(fs::exists(orphan1));
  EXPECT_FALSE(fs::exists(orphan2));
}

TEST_F(SecondaryRollbackTest, InterruptedInstallFileRecovery) {
  fs::path orig_file = test_root / "usr/bin/half_installed";
  {
    fs::create_directories(orig_file.parent_path());
    std::ofstream(orig_file) << "pre-existing content\n";
  }

  fs::path bak_file(orig_file.string() + std::string(constants::SUFFIX_LPKG_BAK) + "half_installed");

  {
    std::ofstream w(wal::wal_log_path());
    w << "BEGIN_PKGS 1\n"
      << "BEGIN half_installed 1.0\n"
      << "BACKUP " << orig_file.string() << " \xe2\x86\x92 " << bak_file.string() << "\n";
  }

  fs::rename(orig_file, bak_file);
  EXPECT_FALSE(fs::exists(orig_file));

  recover_packages();
  EXPECT_TRUE(fs::exists(orig_file));
  EXPECT_FALSE(fs::exists(bak_file));
}

TEST_F(SecondaryRollbackTest, InterruptedRemoveFileRecovery) {
  std::string p = create_pkg("rm_recover", "1.0");
  install_packages({p});

  fs::path pkg_file = test_root / "usr/bin/rm_recover";
  fs::path bak_file(pkg_file.string() + std::string(constants::SUFFIX_LPKG_BAK) + "rm_recover");

  {
    std::ofstream w(wal::wal_log_path());
    w << "BEGIN_PKGS 1\n"
      << "RM_BEGIN rm_recover 1.0\n"
      << "BACKUP " << pkg_file.string() << " \xe2\x86\x92 " << bak_file.string() << "\n";
  }

  fs::rename(pkg_file, bak_file);
  EXPECT_FALSE(fs::exists(pkg_file));

  recover_packages();
  EXPECT_TRUE(fs::exists(pkg_file));
}

TEST_F(SecondaryRollbackTest, BatchWithRollbackFullRecovery) {
  fs::path a_file = test_root / "usr/bin/a_installed";
  fs::path a_bak(a_file.string() + std::string(constants::SUFFIX_LPKG_BAK) + "A");
  fs::path b_new = test_root / "usr/bin/b_new";

  {
    fs::create_directories(a_file.parent_path());
    std::ofstream(a_bak) << "old a\n";
  }
  std::ofstream(b_new) << "new b\n";

  {
    std::ofstream w(wal::wal_log_path());
    w << "BEGIN_PKGS 3\n"
      << "BEGIN A 1.0\n"
      << "BACKUP " << a_file.string() << " \xe2\x86\x92 " << a_bak.string() << "\n"
      << "COPY /tmp/a \xe2\x86\x92 " << a_file.string() << "\n"
      << "COMMIT A 1.0\n"
      << "END A 1.0\n"
      << "DB /var/lib/lpkg/pkgs A:installed\n"
      << "BEGIN B 1.0\n"
      << "NEW " << b_new.string() << "\n"
      << "COPY /tmp/b \xe2\x86\x92 " << b_new.string() << "\n"
      << "ROLLBACK B 1.0\n"
      << "END B 1.0\n"
      << "RESTORE_FILE " << a_bak.string() << " \xe2\x86\x92 " << a_file.string() << "\n";
  }

  EXPECT_NO_THROW(recover_packages());
}

TEST_F(SecondaryRollbackTest, WALWithOnlyBeginPkgs) {
  std::ofstream(wal::wal_log_path()) << "BEGIN_PKGS 1\n";
  EXPECT_NO_THROW(recover_packages());
}

TEST_F(SecondaryRollbackTest, CorruptedWalLine) {
  fs::path orig = test_root / "usr/bin/after_garbage";
  fs::path bak(orig.string() + std::string(constants::SUFFIX_LPKG_BAK) + "after_garbage");

  fs::create_directories(bak.parent_path());
  std::ofstream(bak) << "data\n";

  {
    std::ofstream w(wal::wal_log_path());
    w << "BEGIN_PKGS 1\n"
      << "THIS_LINE_IS_COMPLETELY_UNKNOWN_XXX_YYY\n"
      << "BEGIN after_garbage 1.0\n"
      << "BACKUP " << orig.string() << " \xe2\x86\x92 " << bak.string() << "\n";
  }

  EXPECT_NO_THROW(recover_packages());
}

TEST_F(SecondaryRollbackTest, EmptyWalWithOnlyCommittedBatches) {
  {
    std::ofstream w(wal::wal_log_path());
    w << "BEGIN_PKGS 1\n"
      << "BEGIN done 1.0\n"
      << "COMMIT done 1.0\n"
      << "END done 1.0\n"
      << "COMMIT_PKGS\n";
  }

  EXPECT_NO_THROW(recover_packages());
}

TEST_F(SecondaryRollbackTest, MultipleTrimInvocationsAreIdempotent) {
  {
    std::ofstream w(wal::wal_log_path());
    w << "BEGIN_PKGS 1\n"
      << "BEGIN done 1.0\n"
      << "COMMIT done 1.0\n"
      << "END done 1.0\n"
      << "COMMIT_PKGS\n"
      << "BEGIN_PKGS 1\n"
      << "BEGIN active 1.0\n";
  }

  trim_completed();
  trim_completed();

  std::ifstream f(wal::wal_log_path());
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

  EXPECT_NE(content.find("active"), std::string::npos);
  EXPECT_EQ(content.find("done"), std::string::npos);
}
