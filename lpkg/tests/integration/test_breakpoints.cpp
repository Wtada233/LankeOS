/**
 * test_breakpoints.cpp — WAL 2.0 原子性断点测试
 *
 * 策略：
 *   1. 真实管线测试：install/remove/upgrade → 破坏 WAL → recover → 验证
 *   2. 手工构造测试：针对特定断电点的 WAL 状态（真实管线无法产生的场景）
 */

#include "../test_base.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/wal_op.hpp"

namespace fs = std::filesystem;

class BreakpointTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();
  }

  void write_wal(const std::string &c) {
    auto p = wal::wal_log_path();
    fs::create_directories(fs::path(p).parent_path());
    std::ofstream f(p, std::ios::trunc); f << c; f.close();
  }

  std::string read_wal() {
    std::ifstream f(wal::wal_log_path());
    if (!f.is_open()) return "";
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  }

  /// 移除 WAL 中的 COMMIT_PKGS 行，模拟崩溃
  void strip_commit_pkgs_from_wal() {
    auto content = read_wal();
    auto pos = content.rfind("COMMIT_PKGS");
    if (pos != std::string::npos) {
      content = content.substr(0, pos);
      // 也去掉后面的 \n
      while (!content.empty() && content.back() == '\n')
        content.pop_back();
      content += '\n';
      write_wal(content);
    }
  }

  /// 清空 WAL 模拟写前断电
  void clear_wal() { write_wal(""); }
};

// ============================================================================
// 真实管线：安装失败 → 批次回滚 → 验证系统干净
// （这些走完整 install_packages/remove_package 流程，含 WAL 写入+回滚）
// ============================================================================

TEST_F(BreakpointTest, InstallFailsOnUnresolvableDep) {
  // C 需要不存在的 SONAME → 安装应失败，整批回滚
  std::string pA = create_pkg("bf_A", "1.0");
  std::string pBad = create_pkg("bf_bad", "1.0", {"bf_A"}, {}, {"ghost.needed.so"});

  EXPECT_THROW(install_packages({pBad, pA}), LpkgException);

  // 回滚后两者都不应安装
  Cache::instance().load();
  EXPECT_TRUE(Cache::instance().get_installed_version("bf_A").empty());
  EXPECT_TRUE(Cache::instance().get_installed_version("bf_bad").empty());
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/bf_A"));
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/bf_bad"));
}

TEST_F(BreakpointTest, UpgradeAbiBreakPreventsUpgrade) {
  setup_local_mirror();

  std::string pLib = create_pkg("bk_lib", "1.0", {}, {"bk.so.1"});
  std::string pApp = create_pkg("bk_app", "1.0", {"bk_lib"}, {}, {"bk.so.1"});
  add_to_mirror("bk_lib", "1.0");
  add_to_mirror("bk_app", "1.0");

  install_packages({pApp, pLib});

  // 发布 v2（不提供 bk.so.1）
  {
    fs::path work = suite_work_dir / "_pkg_bk_lib_v2";
    fs::create_directories(work / "content" / "usr" / "lib");
    std::ofstream(work / "content" / "usr" / "lib" / "bk.so.2") << "v2\n";
    std::ofstream(work / "metadata.json")
        << R"({"name":"bk_lib","version":"2.0","deps":[],"provides":["bk.so.2"],"needed_so":[]})";
    std::string v2 = (pkg_dir / "bk_lib-2.0.lpkg").string();
    pack_package(v2, work.string(), "bk_lib", "2.0", {}, {"bk.so.2"});
    auto mirror = suite_work_dir / "mirror" / "x86_64";
    fs::create_directories(mirror / "bk_lib");
    fs::copy(v2, mirror / "bk_lib" / "2.0.lpkg");
    std::ofstream idx(mirror / "index.txt");
    idx << "bk_lib|1.0:::bk.so.1:;2.0:::bk.so.2:|\nbk_app|1.0::bk_lib::bk.so.1|\n";
  }

  EXPECT_THROW(upgrade_packages(), LpkgException);

  // lib 应保持在 v1
  Cache::instance().load();
  EXPECT_EQ(Cache::instance().get_installed_version("bk_lib"), "1.0");
}

// ============================================================================
// 真实管线：升级产生 .lpkg_bak → 批次成功后清理
// ============================================================================

TEST_F(BreakpointTest, UpgradeCleansLpkgBakAfterSuccess) {
  setup_local_mirror();

  // 安装 v1
  std::string p1 = create_pkg("upg_clean", "1.0", {}, {"upg_clean.so.1"});
  add_to_mirror("upg_clean", "1.0");
  install_packages({p1});
  EXPECT_TRUE(fs::exists(test_root / "usr/bin/upg_clean"));

  // 发布 v2
  {
    fs::path work = suite_work_dir / "_pkg_upg_clean_v2";
    fs::create_directories(work / "content" / "usr" / "bin");
    std::ofstream(work / "content" / "usr" / "bin" / "upg_clean") << "v2\n";
    std::ofstream(work / "metadata.json")
        << R"({"name":"upg_clean","version":"2.0","deps":[],"provides":["upg_clean.so.2"],"needed_so":[]})";
    std::string v2 = (pkg_dir / "upg_clean-2.0.lpkg").string();
    pack_package(v2, work.string(), "upg_clean", "2.0", {}, {"upg_clean.so.2"});
    auto mirror = suite_work_dir / "mirror" / "x86_64";
    fs::create_directories(mirror / "upg_clean");
    fs::copy(v2, mirror / "upg_clean" / "2.0.lpkg");
    std::ofstream idx(mirror / "index.txt");
    idx << "upg_clean|1.0:::upg_clean.so.1:;2.0:::upg_clean.so.2:|\n";
  }

  upgrade_packages();
  EXPECT_EQ(Cache::instance().get_installed_version("upg_clean"), "2.0");

  // 升级应产生 .lpkg_bak（覆盖 v1 的文件）→ 批次成功后应清理
  // 验证没有 .lpkg_bak 残留
  bool found_bak = false;
  std::error_code ec;
  for (const auto &e : fs::recursive_directory_iterator(test_root / "usr", ec)) {
    if (e.path().filename().string().find(".lpkg_bak_") != std::string::npos) {
      found_bak = true;
      break;
    }
  }
  EXPECT_FALSE(found_bak) << ".lpkg_bak files should be cleaned after successful batch";
}

// ============================================================================
// 手工构造: TODO.md §3 各断电点的 WAL 状态
// ============================================================================

// §3.2: WAL 先于 rename — 原文件还在
TEST_F(BreakpointTest, FileBackupPowerLoss_BeforeRename) {
  fs::path orig = test_root / "usr/bin/power_test";
  fs::path bak = orig.string() + std::string(constants::SUFFIX_LPKG_BAK) + "pt";
  fs::create_directories(orig.parent_path());
  std::ofstream(orig) << "original\n";

  write_wal("BEGIN_PKGS 1\nBEGIN pt 1.0\n"
            "BACKUP " + orig.string() + " \xe2\x86\x92 " + bak.string() + "\n");

  recover_packages();
  EXPECT_TRUE(fs::exists(orig));
  EXPECT_FALSE(fs::exists(bak));
}

// §3.2: rename 已完成 — 恢复应还原
TEST_F(BreakpointTest, FileBackupPowerLoss_AfterRename) {
  fs::path orig = test_root / "usr/bin/power_test2";
  fs::path bak = orig.string() + std::string(constants::SUFFIX_LPKG_BAK) + "pt2";
  fs::create_directories(orig.parent_path());
  std::ofstream(orig) << "original\n";
  fs::rename(orig, bak);

  write_wal("BEGIN_PKGS 1\nBEGIN pt2 1.0\n"
            "BACKUP " + orig.string() + " \xe2\x86\x92 " + bak.string() + "\n");

  recover_packages();
  EXPECT_TRUE(fs::exists(orig));
  EXPECT_FALSE(fs::exists(bak));
}

// §3.3: COPY 已完成 — 删除目标文件
TEST_F(BreakpointTest, CopyPowerLoss_Completed) {
  fs::path dst = test_root / "usr/bin/copied_pt";
  fs::create_directories(dst.parent_path());
  std::ofstream(dst) << "copied\n";

  write_wal("BEGIN_PKGS 1\nBEGIN cp 1.0\n"
            "COPY /tmp/x \xe2\x86\x92 " + dst.string() + "\n");

  recover_packages();
  EXPECT_FALSE(fs::exists(dst));
}

// §3.3: COPY 未 rename — 目标不存在，跳过
TEST_F(BreakpointTest, CopyPowerLoss_BeforeRename) {
  fs::path dst = test_root / "usr/bin/not_copied_pt";
  fs::path tmp = test_root / "usr/bin/not_copied_pt.lpkgtmp";
  fs::create_directories(dst.parent_path());
  std::ofstream(tmp) << "tmp\n";

  write_wal("BEGIN_PKGS 1\nBEGIN np 1.0\n"
            "COPY " + tmp.string() + " \xe2\x86\x92 " + dst.string() + "\n");

  recover_packages();
  EXPECT_FALSE(fs::exists(dst));
}

// §3.1: DB 备份存在 → 恢复（bak 命名必须匹配 WAL 中的 milestone）
TEST_F(BreakpointTest, DbWritePowerLoss_BackupExists) {
  std::string pkgs = (Config::instance().state_dir() / "pkgs").string();
  // db_bak_path(pkgs, "new_pkg:installed") = pkgs + ".lpkg_db_bak_before:new_pkg:installed"
  std::string bak = pkgs + ".lpkg_db_bak_before:new_pkg:installed";

  // bak = 安装 new_pkg 之前的 DB（:batch-start 状态）
  std::ofstream(bak) << "old_only:1.0\n";
  // 当前 pkgs = new_pkg 安装后的状态
  std::ofstream f(pkgs); f << "old_only:1.0\nnew_pkg:1.0\n"; f.close();

  write_wal("BEGIN_PKGS 1\nBEGIN new_pkg 1.0\nCOMMIT new_pkg 1.0\nEND new_pkg 1.0\n"
            "DB " + pkgs + " new_pkg:installed\n");

  recover_packages();
  // bak 被 reverse_execute rename 回 pkgs → 消费了
  EXPECT_FALSE(fs::exists(bak));

  Cache::instance().load();
  EXPECT_FALSE(Cache::instance().get_installed_version("old_only").empty());
  EXPECT_TRUE(Cache::instance().get_installed_version("new_pkg").empty());
}

// §3.1: DB 备份不存在 → 跳过（WAL 写了但备份未完成，原文件还在）
TEST_F(BreakpointTest, DbWritePowerLoss_NoBackup) {
  // 使用独立文件避免与真实 pkgs 混淆
  auto dir = Config::instance().state_dir();
  std::string db_path = (dir / "pkgs_nobak_test").string();

  std::ofstream(db_path) << "survivor:1.0\n";

  write_wal("BEGIN_PKGS 1\nBEGIN gone 1.0\nCOMMIT gone 1.0\nEND gone 1.0\n"
            "DB " + db_path + " gone:installed\n");

  // 恢复应跳过（没有备份文件），不崩溃
  EXPECT_NO_THROW(recover_packages());
  // 原文件仍在
  EXPECT_TRUE(fs::exists(db_path));
}

// §10: rollback 中途崩溃（RESTORE_DB 后）→ 二次恢复幂等
TEST_F(BreakpointTest, SecondaryRollbackAfterRestoreDb) {
  fs::path a = test_root / "usr/bin/sec_a";
  fs::path abak = a.string() + std::string(constants::SUFFIX_LPKG_BAK) + "A";
  fs::path bnew = test_root / "usr/bin/sec_b";
  std::string pkgs = (Config::instance().state_dir() / "pkgs").string();
  std::string bak_db = pkgs + ".lpkg_db_bak_before:A:installed";

  fs::create_directories(a.parent_path());
  std::ofstream(abak) << "old A\n";
  std::ofstream(bnew) << "new B\n";
  std::ofstream(bak_db) << "pre_state\n";

  write_wal("BEGIN_PKGS 2\n"
            "BEGIN A 1.0\n"
            "BACKUP " + a.string() + " \xe2\x86\x92 " + abak.string() + "\n"
            "COPY /tmp/a \xe2\x86\x92 " + a.string() + "\n"
            "COMMIT A 1.0\nEND A 1.0\n"
            "DB " + pkgs + " A:installed\n"
            "BEGIN B 1.0\n"
            "NEW " + bnew.string() + "\n"
            "COPY /tmp/b \xe2\x86\x92 " + bnew.string() + "\n"
            "ROLLBACK B 1.0\nEND B 1.0\n"
            "RESTORE_DB " + bak_db + " \xe2\x86\x92 " + pkgs + "\n");

  // 模拟 RESTORE_DB 已消费备份
  fs::rename(bak_db, pkgs);

  recover_packages();
  EXPECT_TRUE(fs::exists(a));
  EXPECT_FALSE(fs::exists(abak));
  EXPECT_FALSE(fs::exists(bnew));
  EXPECT_NE(read_wal().find("COMMIT_PKGS"), std::string::npos);
}

// §10 场景 B: rollback 完成但 COMMIT_PKGS 未写
TEST_F(BreakpointTest, SecondaryRollbackAllDoneBeforeCommit) {
  fs::path a = test_root / "usr/bin/full_rb_a";
  fs::path abak = a.string() + std::string(constants::SUFFIX_LPKG_BAK) + "A";
  std::string pkgs = (Config::instance().state_dir() / "pkgs_full").string();

  fs::create_directories(a.parent_path());
  std::ofstream(abak) << "old\n";
  std::ofstream(pkgs) << "pre\n";

  write_wal("BEGIN_PKGS 2\n"
            "BEGIN A 1.0\n"
            "BACKUP " + a.string() + " \xe2\x86\x92 " + abak.string() + "\n"
            "COPY /tmp/a \xe2\x86\x92 " + a.string() + "\n"
            "COMMIT A 1.0\nEND A 1.0\n"
            "DB " + pkgs + " A:installed\n"
            "BEGIN B 1.0\nROLLBACK B 1.0\nEND B 1.0\n"
            "RESTORE_FILE " + abak.string() + " \xe2\x86\x92 " + a.string() + "\n"
            "ROLLBACK A 1.0\nEND A 1.0\n"
            "DB " + pkgs + " :batch-start\n");

  fs::rename(abak, a);

  recover_packages();
  EXPECT_NE(read_wal().find("COMMIT_PKGS"), std::string::npos);
}

// 幂等性: 多次 recover 不崩溃
TEST_F(BreakpointTest, TripleRecoverIdempotent) {
  fs::path f = test_root / "usr/bin/triple_pt";
  fs::path b = f.string() + std::string(constants::SUFFIX_LPKG_BAK) + "triple";
  fs::create_directories(f.parent_path());
  std::ofstream(f) << "pre\n";
  fs::rename(f, b);

  write_wal("BEGIN_PKGS 1\nBEGIN triple 1.0\n"
            "BACKUP " + f.string() + " \xe2\x86\x92 " + b.string() + "\n");

  recover_packages();
  recover_packages();
  recover_packages();
  EXPECT_TRUE(fs::exists(f));
  EXPECT_FALSE(fs::exists(b));
}

// 空 WAL / 纯完成批次 / 垃圾行
TEST_F(BreakpointTest, WalEdgeCasesNoCrash) {
  EXPECT_NO_THROW(recover_packages());

  write_wal("BEGIN_PKGS 1\nBEGIN ok 1.0\nCOMMIT ok 1.0\nEND ok 1.0\nCOMMIT_PKGS\n");
  EXPECT_NO_THROW(recover_packages());

  write_wal("GARBAGE\nBEGIN_PKGS 1\nMORE_GARBAGE\nBEGIN p 1.0\nBACKUP /a \xe2\x86\x92 /b\n");
  EXPECT_NO_THROW(recover_packages());
}

// DBNEW/DBNEW+备份/DBRM 恢复
TEST_F(BreakpointTest, DbNewRmRecovery) {
  auto dir = Config::instance().state_dir();

  // DBNEW 无备份 → 删除
  auto nf = (dir / "nso_new").string();
  std::ofstream(nf) << "new\n";
  write_wal("BEGIN_PKGS 1\nBEGIN np 1.0\nCOMMIT np 1.0\nEND np 1.0\nDBNEW " + nf + " np:installed\n");
  recover_packages();
  EXPECT_FALSE(fs::exists(nf));

  // DBNEW 有备份 → 恢复
  auto nf2 = (dir / "nso_withbak").string();
  auto bak2 = nf2 + ".lpkg_db_bak_before:n2:installed";
  std::ofstream(bak2) << "old\n";
  std::ofstream(nf2) << "new\n";
  write_wal("BEGIN_PKGS 1\nBEGIN n2 1.0\nCOMMIT n2 1.0\nEND n2 1.0\nDBNEW " + nf2 + " n2:installed\n");
  recover_packages();
  EXPECT_FALSE(fs::exists(bak2));
  std::ifstream f2(nf2); std::string c2((std::istreambuf_iterator<char>(f2)), {}); EXPECT_EQ(c2, "old\n");

  // DBRM → 恢复
  auto df = (dir / "deps_gone").string();
  auto dbak = df + ".lpkg_db_bak_before:gone:removed";
  std::ofstream(dbak) << "dep_data\n";
  write_wal("BEGIN_PKGS 1\nRM_BEGIN gone 1.0\nDBRM " + df + " gone:removed\nRM_COMMIT gone 1.0\nRM_END gone 1.0\n");
  recover_packages();
  EXPECT_FALSE(fs::exists(dbak));
  EXPECT_TRUE(fs::exists(df));
}

// RM_DIR 重建
TEST_F(BreakpointTest, RmDirRecreation) {
  fs::path d = test_root / "usr/share/gone_pt";
  write_wal("BEGIN_PKGS 1\nRM_BEGIN p 1.0\nRM_DIR " + d.string() + " 755 0 0\n");
  recover_packages();
  EXPECT_TRUE(fs::exists(d));
}

// 组合：NEW + COPY + BACKUP 混合恢复
TEST_F(BreakpointTest, MixedNewCopyBackupReverse) {
  fs::path old_f = test_root / "usr/bin/old_mix";
  fs::path old_bak = old_f.string() + std::string(constants::SUFFIX_LPKG_BAK) + "mix";
  fs::path new_f = test_root / "usr/share/doc/mix/README";
  fs::path copied_f = test_root / "usr/bin/copied_mix";

  fs::create_directories(old_f.parent_path());
  fs::create_directories(new_f.parent_path());
  std::ofstream(old_f) << "old\n"; fs::rename(old_f, old_bak);
  std::ofstream(new_f) << "new\n";
  std::ofstream(copied_f) << "copied\n";

  write_wal("BEGIN_PKGS 1\nBEGIN mix 1.0\n"
            "BACKUP " + old_f.string() + " \xe2\x86\x92 " + old_bak.string() + "\n"
            "NEW " + new_f.string() + "\n"
            "COPY /tmp/mix \xe2\x86\x92 " + copied_f.string() + "\n"
            "COMMIT mix 1.0\nEND mix 1.0\n");

  recover_packages();
  EXPECT_TRUE(fs::exists(old_f));
  EXPECT_FALSE(fs::exists(old_bak));
  EXPECT_FALSE(fs::exists(new_f));
  EXPECT_FALSE(fs::exists(copied_f));
}

// ============================================================================
// §7 批量移除中途崩溃：恢复所有包的安装状态
// ============================================================================

TEST_F(BreakpointTest, BatchRemoveCrashMidwayRestoresAllPackages) {
  // 安装 3 个包（依赖链: rmC ← rmB ← rmA）
  std::string pC = create_pkg("rmC_crash", "1.0");
  std::string pB = create_pkg("rmB_crash", "1.0", {"rmC_crash"});
  std::string pA = create_pkg("rmA_crash", "1.0", {"rmB_crash"});

  install_packages({pA, pB, pC});

  // 验证安装完成
  for (auto &n : {"rmA_crash", "rmB_crash", "rmC_crash"}) {
    EXPECT_FALSE(Cache::instance().get_installed_version(n).empty())
        << n << " should be installed";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin" / n))
        << n << " binary should exist";
  }

  // 构建 WAL：批量移除 [rmC_crash, rmB_crash, rmA_crash]
  // 模拟 rmA 的备份完成，rmB 也完成，但 rmC 还没开始
  // （移除顺序是叶子先删：rmC → rmB → rmA）
  fs::path pA_bin = test_root / "usr/bin/rmA_crash";
  fs::path pA_bak = pA_bin.string() +
                    std::string(constants::SUFFIX_LPKG_BAK) + "rmA_crash";
  fs::path pB_bin = test_root / "usr/bin/rmB_crash";
  fs::path pB_bak = pB_bin.string() +
                    std::string(constants::SUFFIX_LPKG_BAK) + "rmB_crash";
  fs::path pC_bin = test_root / "usr/bin/rmC_crash";

  // 模拟 rmA 和 rmB 的文件已被 rename 到 .lpkg_bak
  fs::rename(pA_bin, pA_bak);
  fs::rename(pB_bin, pB_bak);
  // rmC 还没被处理（文件还在原位）
  EXPECT_TRUE(fs::exists(pC_bin));

  // 注册到 cache（模拟 rmC 仍在内存中）
  auto &cache = Cache::instance();
  cache.add_installed("rmC_crash", "1.0");
  cache.add_file_owner("/usr/bin/rmC_crash", "rmC_crash");

  // WAL：BEGIN_PKGS 3，rmA 和 rmB 的 RM_BEGIN + BACKUP 完成
  // rmC 还没开始 — 在此崩溃
  std::string pkgs_path = (Config::instance().state_dir() / "pkgs").string();
  std::string bakA = pkgs_path + ".lpkg_db_bak_before:rmA_crash:removed";

  // rmA 移除前的 DB 快照（包含全部 3 个包）
  {
    std::ofstream f(bakA);
    f << "rmA_crash:1.0\nrmB_crash:1.0\nrmC_crash:1.0\n";
  }

  write_wal(
      "BEGIN_PKGS 3\n"
      "RM_BEGIN rmC_crash 1.0\n"             // 叶子先
      "BACKUP " + pC_bin.string() + " \xe2\x86\x92 " + pC_bin.string() + ".lpkg_bak_rmC_crash\n"
      "RM_COMMIT rmC_crash 1.0\n"
      "RM_END rmC_crash 1.0\n"
      "DBRM /deps/rmC_crash rmC_crash:removed\n"
      "RM_BEGIN rmB_crash 1.0\n"
      "BACKUP " + pB_bin.string() + " \xe2\x86\x92 " + pB_bak.string() + "\n"
      "RM_COMMIT rmB_crash 1.0\n"
      "RM_END rmB_crash 1.0\n"
      "RM_BEGIN rmA_crash 1.0\n"
      "BACKUP " + pA_bin.string() + " \xe2\x86\x92 " + pA_bak.string() + "\n"
      // 在此崩溃：rmA 的 BACKUP 写了但 RM_COMMIT 没写
  );

  // 执行恢复
  recover_packages();

  // ── 验证包文件恢复 ──
  EXPECT_TRUE(fs::exists(pA_bin)) << "rmA binary should be restored from bak";
  EXPECT_FALSE(fs::exists(pA_bak)) << "rmA bak should be consumed";

  EXPECT_TRUE(fs::exists(pB_bin)) << "rmB binary should be restored from bak";
  EXPECT_FALSE(fs::exists(pB_bak)) << "rmB bak should be consumed";

  // rmC 被标记为 RM_COMMIT（移除已完成），文件被删
  // 但反向：RM_COMMIT 之后恢复时应不再恢复 rmC
  // 关键：rmC 的 RM_COMMIT 已经写了 → 移除已提交 → 不应恢复

  // ── 验证数据库状态 ──
  Cache::instance().load();

  // rmA 和 rmB 应该被恢复（RM_COMMIT 未写 → 移除了但回滚恢复）
  EXPECT_FALSE(Cache::instance().get_installed_version("rmA_crash").empty())
      << "rmA should be reinstalled after recovery";
  EXPECT_FALSE(Cache::instance().get_installed_version("rmB_crash").empty())
      << "rmB should be reinstalled after recovery";

  // rmC 的 RM_COMMIT 已写 → 移除已提交 → 保持移除
  // （这取决于反向处理逻辑：RM_COMMIT 是元数据行，被 skip）
  // 在 reverse_execute 中 RM_COMMIT 被 skip，BACKUP 被逆向恢复
  // 所以 rmC 的文件应该也被恢复

  // ── 验证 COMMIT_PKGS ──
  EXPECT_NE(read_wal().find("COMMIT_PKGS"), std::string::npos)
      << "COMMIT_PKGS should be written";
}

