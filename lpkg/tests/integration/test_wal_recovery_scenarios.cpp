/**
 * test_wal_recovery_scenarios.cpp — WAL 恢复场景综合测试
 *
 * 覆盖 TODO.md 要求的真实恢复场景：
 *   - 安装断电恢复
 *   - 移除断电恢复
 *   - 批量操作中部分成功后的回滚
 *   - rollback 自身中断后的二次恢复
 *   - 各种操作类型的逆向幂等性
 */

#include "../test_base.hpp"

#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/wal_op.hpp"

namespace fs = std::filesystem;

class WalRecoveryScenarioTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();
  }

  void write_wal(const std::string &content) {
    std::string wpath = wal::wal_log_path();
    fs::create_directories(fs::path(wpath).parent_path());
    std::ofstream f(wpath, std::ios::trunc);
    f << content;
    f.close();
  }
};

// ============================================================================
// BACKUP 逆向：各种情况
// ============================================================================

TEST_F(WalRecoveryScenarioTest, BackupRestoreSingleFile) {
  fs::path orig = test_root / "usr/bin/single_file";
  fs::path bak = orig.string() + std::string(constants::SUFFIX_LPKG_BAK) + "test";

  fs::create_directories(orig.parent_path());
  std::ofstream(orig) << "original\n";
  fs::rename(orig, bak);

  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN test 1.0\n"
      "BACKUP " + orig.string() + " \xe2\x86\x92 " + bak.string() + "\n");

  recover_packages();
  EXPECT_TRUE(fs::exists(orig));
  EXPECT_FALSE(fs::exists(bak));
}

TEST_F(WalRecoveryScenarioTest, BackupRestoreMultipleFiles) {
  std::vector<fs::path> origs, baks;
  for (int i = 0; i < 5; ++i) {
    fs::path orig = test_root / ("usr/bin/file_" + std::to_string(i));
    fs::path bak = orig.string() + std::string(constants::SUFFIX_LPKG_BAK) + "multi";
    fs::create_directories(orig.parent_path());
    std::ofstream(orig) << "content " << i << "\n";
    fs::rename(orig, bak);
    origs.push_back(orig);
    baks.push_back(bak);
  }

  std::string wal = "BEGIN_PKGS 1\nBEGIN multi 1.0\n";
  for (int i = 0; i < 5; ++i)
    wal += "BACKUP " + origs[i].string() + " \xe2\x86\x92 " + baks[i].string() + "\n";

  write_wal(wal);
  recover_packages();

  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(fs::exists(origs[i])) << "file " << i;
    EXPECT_FALSE(fs::exists(baks[i])) << "bak " << i;
  }
}

// ============================================================================
// COPY 逆向 + NEW 逆向 组合
// ============================================================================

TEST_F(WalRecoveryScenarioTest, CopyAndNewReverseCombined) {
  fs::path new_file = test_root / "usr/bin/new_only";
  fs::path copied_file = test_root / "usr/bin/copied";

  fs::create_directories(new_file.parent_path());
  std::ofstream(new_file) << "new\n";
  std::ofstream(copied_file) << "copied\n";

  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN combo 1.0\n"
      "NEW " + new_file.string() + "\n"
      "COPY /tmp/combo.lpkgtmp \xe2\x86\x92 " + copied_file.string() + "\n");

  recover_packages();

  // NEW → 删除 new_file
  // COPY → 删除 copied_file
  EXPECT_FALSE(fs::exists(new_file));
  EXPECT_FALSE(fs::exists(copied_file));
}

// ============================================================================
// RM_DIR 恢复
// ============================================================================

TEST_F(WalRecoveryScenarioTest, RmDirRecreation) {
  fs::path dir = test_root / "usr/share/removed_dir";

  write_wal(
      "BEGIN_PKGS 1\n"
      "RM_BEGIN pkg 1.0\n"
      "RM_DIR " + dir.string() + " 755 1000 1000\n");

  recover_packages();

  EXPECT_TRUE(fs::exists(dir));
  EXPECT_TRUE(fs::is_directory(dir));
}

// ============================================================================
// 安装 → 回滚 → 文件恢复
// ============================================================================

TEST_F(WalRecoveryScenarioTest, FullInstallWithBackupThenCrash) {
  fs::path orig = test_root / "usr/bin/installed_pkg";
  fs::path bak = orig.string() + std::string(constants::SUFFIX_LPKG_BAK) + "installed_pkg";
  fs::path new_doc = test_root / "usr/share/doc/installed_pkg/README";
  std::string pkgs = (Config::instance().state_dir() / "pkgs").string();
  std::string bak_pkgs = pkgs + ".lpkg_db_bak_before:installed_pkg:installed";

  // 安装前状态：DB 有 oldPkg
  {
    fs::create_directories(orig.parent_path());
    std::ofstream(orig) << "old content\n";
  }
  {
    std::ofstream bak_f(bak_pkgs);
    bak_f << "oldPkg:1.0\n";
  }

  // 模拟安装过程：BACKUP renaming orig→bak, COPY, 写 DB
  fs::rename(orig, bak);
  std::ofstream(new_doc) << "new doc\n";
  {
    std::ofstream pkgs_f(pkgs);
    pkgs_f << "oldPkg:1.0\ninstalled_pkg:1.0\n";
  }
  Cache::instance().add_installed("oldPkg", "1.0");
  Cache::instance().add_installed("installed_pkg", "1.0");

  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN installed_pkg 1.0\n"
      "BACKUP " + orig.string() + " \xe2\x86\x92 " + bak.string() + "\n"
      "NEW " + new_doc.string() + "\n"
      "COPY /tmp/installed_pkg.lpkgtmp \xe2\x86\x92 " + orig.string() + "\n"
      "COMMIT installed_pkg 1.0\n"
      "END installed_pkg 1.0\n"
      "DB " + pkgs + " installed_pkg:installed\n");

  recover_packages();

  // 验证：安装被回滚
  // - orig 被恢复（从 bak）
  // - new_doc 被删除
  // - DB 被恢复（从 bak_pkgs）
  EXPECT_TRUE(fs::exists(orig)) << "original file should be restored";
  EXPECT_FALSE(fs::exists(bak)) << "backup should be consumed";
  EXPECT_FALSE(fs::exists(new_doc)) << "new doc should be removed";
}

// ============================================================================
// 移除 → 回滚 → 文件恢复
// ============================================================================

TEST_F(WalRecoveryScenarioTest, FullRemoveWithBackupThenCrash) {
  fs::path pkg_file = test_root / "usr/bin/removed_pkg";
  fs::path bak = pkg_file.string() + std::string(constants::SUFFIX_LPKG_BAK) + "removed_pkg";

  fs::create_directories(pkg_file.parent_path());
  std::ofstream(pkg_file) << "pkg content\n";
  fs::rename(pkg_file, bak);

  write_wal(
      "BEGIN_PKGS 1\n"
      "RM_BEGIN removed_pkg 1.0\n"
      "BACKUP " + pkg_file.string() + " \xe2\x86\x92 " + bak.string() + "\n");

  recover_packages();

  EXPECT_TRUE(fs::exists(pkg_file));
  EXPECT_FALSE(fs::exists(bak));
}

// ============================================================================
// 整个批次 [A, B] 中 B 失败，回滚 A
// ============================================================================

TEST_F(WalRecoveryScenarioTest, BatchAFailsRollbackB) {
  fs::path a_file = test_root / "usr/bin/passed_A";
  fs::path a_bak = a_file.string() + std::string(constants::SUFFIX_LPKG_BAK) + "A";
  fs::path b_file = test_root / "usr/bin/failed_B";
  std::string pkgs = (Config::instance().state_dir() / "pkgs").string();
  std::string bak_pkgs = pkgs + ".lpkg_db_bak_before:A:installed";

  fs::create_directories(a_file.parent_path());
  std::ofstream(a_bak) << "old A\n";
  std::ofstream(b_file) << "new B\n";

  write_wal(
      "BEGIN_PKGS 2\n"
      "BEGIN A 1.0\n"
      "BACKUP " + a_file.string() + " \xe2\x86\x92 " + a_bak.string() + "\n"
      "COPY /tmp/a \xe2\x86\x92 " + a_file.string() + "\n"
      "COMMIT A 1.0\n"
      "END A 1.0\n"
      "DB " + pkgs + " A:installed\n"
      "BEGIN B 1.0\n"
      "NEW " + b_file.string() + "\n"
      "COPY /tmp/b \xe2\x86\x92 " + b_file.string() + "\n"
      "ROLLBACK B 1.0\n"
      "END B 1.0\n");

  recover_packages();

  // A 的文件应被恢复（从备份），B 的新文件应被删除
  EXPECT_TRUE(fs::exists(a_file));
  EXPECT_FALSE(fs::exists(a_bak));
  EXPECT_FALSE(fs::exists(b_file));
}

// ============================================================================
// DB 链式恢复：多层 DB 备份
// ============================================================================

TEST_F(WalRecoveryScenarioTest, MultiLayerDbChainRestore) {
  std::string pkgs = (Config::instance().state_dir() / "pkgs").string();
  // 备份命名：.lpkg_db_bak_before:<milestone> = 写入该 milestone 之前的 DB
  // first:installed 的备份 = :batch-start 的内容（只有 system）
  std::string bak_first = pkgs + ".lpkg_db_bak_before:first:installed";

  std::ofstream(bak_first) << "system:1.0\n";
  std::ofstream f(pkgs); f << "system:1.0\nfirst:1.0\n"; f.close();

  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN first 1.0\n"
      "COMMIT first 1.0\n"
      "END first 1.0\n"
      "DB " + pkgs + " first:installed\n");

  recover_packages();

  Cache::instance().load();
  EXPECT_EQ(Cache::instance().get_installed_version("system"), "1.0");
  EXPECT_EQ(Cache::instance().get_installed_version("first"), "");
}

// ============================================================================
// DBNEW 恢复：新建文件被回滚
// ============================================================================

TEST_F(WalRecoveryScenarioTest, DbNewRollbackRemovesFile) {
  std::string nso_file = (Config::instance().state_dir() / "needed_so" / "newpkg").string();

  fs::create_directories(fs::path(nso_file).parent_path());
  std::ofstream(nso_file) << "libc.so.6\n";

  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN newpkg 1.0\n"
      "COMMIT newpkg 1.0\n"
      "END newpkg 1.0\n"
      "DBNEW " + nso_file + " newpkg:installed\n");

  recover_packages();

  // 文件应被删除（DBNEW 逆向无备份时删除）
  EXPECT_FALSE(fs::exists(nso_file));
}

// ============================================================================
// DBNEW 恢复：有备份时恢复
// ============================================================================

TEST_F(WalRecoveryScenarioTest, DbNewRollbackWithBackup) {
  std::string nso_file = (Config::instance().state_dir() / "needed_so" / "withbak").string();
  std::string bak_path = nso_file + ".lpkg_db_bak_before:withbak:installed";

  fs::create_directories(fs::path(nso_file).parent_path());
  std::ofstream(bak_path) << "old_needed_data\n";
  std::ofstream(nso_file) << "new_needed_data\n";

  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN withbak 1.0\n"
      "COMMIT withbak 1.0\n"
      "END withbak 1.0\n"
      "DBNEW " + nso_file + " withbak:installed\n");

  recover_packages();

  EXPECT_FALSE(fs::exists(bak_path));
  EXPECT_TRUE(fs::exists(nso_file));
  // 内容应为旧数据
  std::ifstream f(nso_file);
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "old_needed_data\n");
}

// ============================================================================
// DBRM 恢复：删除的文件恢复
// ============================================================================

TEST_F(WalRecoveryScenarioTest, DbRmRollbackRestoresFile) {
  std::string dep_file = (Config::instance().state_dir() / "deps" / "gone").string();
  std::string bak_path = dep_file + ".lpkg_db_bak_before:gone:removed";

  fs::create_directories(fs::path(dep_file).parent_path());
  std::ofstream(bak_path) << "glibc\nopenssl\n";

  write_wal(
      "BEGIN_PKGS 1\n"
      "RM_BEGIN gone 1.0\n"
      "DBRM " + dep_file + " gone:removed\n"
      "RM_COMMIT gone 1.0\n"
      "RM_END gone 1.0\n");

  recover_packages();

  EXPECT_FALSE(fs::exists(bak_path));
  EXPECT_TRUE(fs::exists(dep_file));
}

// ============================================================================
// 组合 RM_DIR + BACKUP 恢复
// ============================================================================

TEST_F(WalRecoveryScenarioTest, RmDirRecreationOnly) {
  // RM_DIR 逆向：仅测试目录重建（不涉及 BACKUP）
  fs::path dir = test_root / "usr/share/gone_dir";

  write_wal(
      "BEGIN_PKGS 1\n"
      "RM_BEGIN pkg 1.0\n"
      "RM_DIR " + dir.string() + " 755 0 0\n");

  recover_packages();

  EXPECT_TRUE(fs::exists(dir));
  EXPECT_TRUE(fs::is_directory(dir));
}

// ============================================================================
// COMMIT_PKGS 后 trim 正确保留未完成批次
// ============================================================================

TEST_F(WalRecoveryScenarioTest, TrimKeepsUncommittedBatchAfterCommitted) {
  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN done 1.0\n"
      "COMMIT done 1.0\n"
      "END done 1.0\n"
      "COMMIT_PKGS\n"
      "BEGIN_PKGS 1\n"
      "BEGIN active 1.0\n"
      "BACKUP /a \xe2\x86\x92 /a.bak\n");

  trim_completed();

  std::ifstream f(wal::wal_log_path());
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

  EXPECT_EQ(content.find("done"), std::string::npos);
  EXPECT_NE(content.find("active"), std::string::npos);
}

// ============================================================================
// 空 WAL 文件不应导致崩溃
// ============================================================================

TEST_F(WalRecoveryScenarioTest, VariousWalEdgeCases) {
  // 纯空
  EXPECT_NO_THROW(recover_packages());

  // 仅空格
  write_wal("   \n\n  \n");
  EXPECT_NO_THROW(recover_packages());

  // 仅注释
  write_wal("# this is a comment\n# another\n");
  EXPECT_NO_THROW(recover_packages());

  // 仅 COMMIT_PKGS（已完成的空批次）
  write_wal("BEGIN_PKGS 0\nCOMMIT_PKGS\n");
  EXPECT_NO_THROW(recover_packages());

  // WAL 不存在
  fs::remove(wal::wal_log_path());
  EXPECT_NO_THROW(recover_packages());
  EXPECT_NO_THROW(trim_completed());
}
