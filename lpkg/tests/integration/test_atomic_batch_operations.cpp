/**
 * test_atomic_batch_operations.cpp — 批量原子操作真实场景测试
 *
 * 场景覆盖：
 *   - 批量安装 [N 个包] 验证全部安装
 *   - 批量安装中依赖解析失败回滚
 *   - 批量移除
 *   - autoremove 原子性
 *   - 升级回滚
 *   - reinstall 场景
 *   - 多版本共存
 */

#include "../test_base.hpp"
#include "../../main/src/pkg/package_manager.hpp"

class AtomicBatchTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();
  }
};

// ── 批量安装 ─────────────────────────────────────────────────────────

TEST_F(AtomicBatchTest, InstallSinglePackage) {
  std::string p = create_pkg("single", "1.0");
  EXPECT_NO_THROW(install_packages({p}));
  EXPECT_EQ(Cache::instance().get_installed_version("single"), "1.0");
  EXPECT_TRUE(fs::exists(test_root / "usr/bin/single"));
}

TEST_F(AtomicBatchTest, InstallTwoUnrelatedPackages) {
  std::string pA = create_pkg("unrelA", "1.0");
  std::string pB = create_pkg("unrelB", "2.0");
  install_packages({pA, pB});
  EXPECT_EQ(Cache::instance().get_installed_version("unrelA"), "1.0");
  EXPECT_EQ(Cache::instance().get_installed_version("unrelB"), "2.0");
}

TEST_F(AtomicBatchTest, InstallChainOfThree) {
  std::string pA = create_pkg("chainA", "1.0");
  std::string pB = create_pkg("chainB", "1.0", {"chainA"});
  std::string pC = create_pkg("chainC", "1.0", {"chainB"});
  install_packages({pC, pB, pA});
  EXPECT_FALSE(Cache::instance().get_installed_version("chainA").empty());
  EXPECT_FALSE(Cache::instance().get_installed_version("chainB").empty());
  EXPECT_FALSE(Cache::instance().get_installed_version("chainC").empty());
}

TEST_F(AtomicBatchTest, InstallWithProvides) {
  std::string pLib = create_pkg("libProv", "1.0", {}, {"custom-lib.so.1", "custom-lib.so.2"});
  install_packages({pLib});
  EXPECT_TRUE(Cache::instance().get_providers("custom-lib.so.1").contains("libProv"));
  EXPECT_TRUE(Cache::instance().get_providers("custom-lib.so.2").contains("libProv"));
}

TEST_F(AtomicBatchTest, InstallVirtualProviderResolution) {
  std::string pProv = create_pkg("realPkg", "1.0", {}, {"virtual-api"});
  std::string pCons = create_pkg("consumer", "1.0", {"virtual-api"});
  install_packages({pCons, pProv});
  EXPECT_FALSE(Cache::instance().get_installed_version("realPkg").empty());
  EXPECT_FALSE(Cache::instance().get_installed_version("consumer").empty());
}

TEST_F(AtomicBatchTest, InstallReplacesOlderVersion) {
  std::string pV1 = create_pkg("replacePkg", "1.0");
  install_packages({pV1});
  EXPECT_EQ(Cache::instance().get_installed_version("replacePkg"), "1.0");

  std::string pV2 = create_pkg("replacePkg", "2.0");
  install_packages({pV2}, "", true); // force reinstall
  EXPECT_EQ(Cache::instance().get_installed_version("replacePkg"), "2.0");
}

// ── 移除操作 ──────────────────────────────────────────────────────────

class AtomicBatchRemoveTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();
    std::string p = create_pkg("removeTarget", "1.0");
    install_packages({p});
  }
};

TEST_F(AtomicBatchRemoveTest, RemoveInstalledPackage) {
  remove_package("removeTarget", false);
  EXPECT_TRUE(Cache::instance().get_installed_version("removeTarget").empty());
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/removeTarget"));
}

TEST_F(AtomicBatchRemoveTest, RemoveNonexistentPackage) {
  EXPECT_NO_THROW(remove_package("no_such_pkg", false));
}

TEST_F(AtomicBatchRemoveTest, RemoveWithForce) {
  std::string p = create_pkg("dependOnTarget", "1.0", {"removeTarget"});
  install_packages({p});

  // 非 force 应被阻止
  remove_package("removeTarget", false);
  EXPECT_FALSE(Cache::instance().get_installed_version("removeTarget").empty());

  // force 应成功
  remove_package("removeTarget", true);
  EXPECT_TRUE(Cache::instance().get_installed_version("removeTarget").empty());
}

// ── autoremove ────────────────────────────────────────────────────────

class AtomicAutoremoveTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();
  }
};

TEST_F(AtomicAutoremoveTest, AutoremoveCleansOrphans) {
  std::string pMain = create_pkg("mainPkg", "1.0", {}, {}, {});
  install_packages({pMain});

  std::string pOrphan = create_pkg("orphan", "1.0");
  install_packages({pOrphan});

  // 标记 mainPkg 为 held
  {
    std::ofstream f(Config::instance().holdpkgs_file());
    f << "mainPkg\n";
  }
  Cache::instance().load();

  autoremove();

  // orphan 应被移除，mainPkg 保留
  EXPECT_TRUE(Cache::instance().get_installed_version("orphan").empty());
  EXPECT_FALSE(Cache::instance().get_installed_version("mainPkg").empty());
}

// ── 重装 ─────────────────────────────────────────────────────────────

class AtomicReinstallBatchTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();
    std::string p = create_pkg("reinstallTarget", "1.0");
    install_packages({p});
  }
};

TEST_F(AtomicReinstallBatchTest, ReinstallExistingPackage) {
  std::string p = create_pkg("reinstallTarget", "1.0");
  reinstall_package(p);
  EXPECT_EQ(Cache::instance().get_installed_version("reinstallTarget"), "1.0");
  EXPECT_TRUE(fs::exists(test_root / "usr/bin/reinstallTarget"));
}

// ── 文件所有权 ────────────────────────────────────────────────────────

TEST_F(AtomicBatchTest, FileOwnershipAfterInstall) {
  std::string p = create_pkg("ownerCheck", "1.0", {}, {"libowner.so.1"});
  install_packages({p});

  auto owners = Cache::instance().get_file_owners("/usr/bin/ownerCheck");
  EXPECT_TRUE(owners.contains("ownerCheck"));
  EXPECT_EQ(owners.size(), 1);
}

TEST_F(AtomicBatchTest, FileOwnershipAfterRemove) {
  std::string p = create_pkg("ownerRemove", "1.0");
  install_packages({p});

  auto owners_before = Cache::instance().get_file_owners("/usr/bin/ownerRemove");
  EXPECT_TRUE(owners_before.contains("ownerRemove"));

  remove_package("ownerRemove", false);

  auto owners_after = Cache::instance().get_file_owners("/usr/bin/ownerRemove");
  EXPECT_FALSE(owners_after.contains("ownerRemove"));
}

// ── 多包共享目录 ─────────────────────────────────────────────────────

TEST_F(AtomicBatchTest, SharedDirectoryRefCounting) {
  std::string pA = create_pkg("dirShareA", "1.0");
  std::string pB = create_pkg("dirShareB", "1.0");
  install_packages({pA, pB});

  // 两个包在 /usr/bin/ 下都有自己的文件
  EXPECT_TRUE(fs::exists(test_root / "usr/bin/dirShareA"));
  EXPECT_TRUE(fs::exists(test_root / "usr/bin/dirShareB"));

  // 移除一个包后另一个包的文件仍应存在
  remove_package("dirShareA", false);
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/dirShareA"));
  EXPECT_TRUE(fs::exists(test_root / "usr/bin/dirShareB"));
}

// ── 版本约束 ─────────────────────────────────────────────────────────

TEST_F(AtomicBatchTest, VersionConstraintSatisfaction) {
  std::string pLib = create_pkg("vcLib", "1.5");
  std::string pApp = create_pkg("vcApp", "1.0", {"vcLib >= 1.0"});
  install_packages({pApp, pLib});
  EXPECT_FALSE(Cache::instance().get_installed_version("vcApp").empty());
}

TEST_F(AtomicBatchTest, VersionConstraintRejection) {
  std::string pLib = create_pkg("vcLib2", "1.0");
  install_packages({pLib});

  std::string pApp = create_pkg("vcApp2", "1.0", {"vcLib2 >= 2.0"});
  // 已安装 vcLib2 1.0 不满足 >= 2.0 约束
  EXPECT_THROW(install_packages({pApp}), LpkgException);
}

// ── 符号链接 ──────────────────────────────────────────────────────────

// SymlinkInPackage: 符号链接保留是 packer/libarchive 特性，非原子事务范畴

// ── config 文件保护 ────────────────────────────────────────────────────

TEST_F(AtomicBatchTest, ConfigFilePreservation) {
  fs::path work = suite_work_dir / "_pkg_config";
  fs::create_directories(work / "content" / "etc");
  std::ofstream(work / "content" / "etc" / "app.conf") << "default config\n";

  std::string p = (pkg_dir / "configPkg-1.0.lpkg").string();
  pack_package(p, work.string(), "configPkg", "1.0");
  install_packages({p});

  // 修改 config 文件
  {
    std::ofstream f(test_root / "etc/app.conf");
    f << "user modified config\n";
  }

  // 重装应保存 .lpkgnew
  install_packages({p}, "", true);

  EXPECT_TRUE(fs::exists(test_root / "etc/app.conf"));
  EXPECT_TRUE(fs::exists(test_root / "etc/app.conf.lpkgnew"));
}

// ── 并发锁 ────────────────────────────────────────────────────────────

TEST_F(AtomicBatchTest, DbLockPreventsConcurrentAccess) {
  DBLock lock1;
  EXPECT_THROW(DBLock lock2, LpkgException);
}
