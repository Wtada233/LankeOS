/**
 * test_wal_edge_cases.cpp — WAL 原子性边界与压力测试
 *
 * 覆盖:
 *   - 所有 25 种 WAL 操作类型的解析 + 逆向
 *   - 超大 WAL 批次的 trim 行为
 *   - 并发 DBLock 正确拒绝
 *   - batch_rollback 对各操作类型的正确逆向
 *   - reverse_execute 的里程碑停止行为
 *   - 各种 WAL 格式异常健壮性
 *   - cleanup_db_backups 选择性清理
 */

#include "../test_base.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/wal_op.hpp"

namespace fs = std::filesystem;

class WalEdgeCaseTest : public IntegrationTestBase {
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
};

// ── 全部 WAL 操作类型 parse_op 往返 ──────────────────────────────────

TEST_F(WalEdgeCaseTest, AllOpTypesParseRoundtrip) {
  struct { std::string line; wal::WALOpType type; } cases[] = {
    {"BEGIN_PKGS 5", wal::WALOpType::BEGIN_PKGS},
    {"COMMIT_PKGS", wal::WALOpType::COMMIT_PKGS},
    {"BEGIN pkg 1.0", wal::WALOpType::BEGIN},
    {"COMMIT pkg 1.0", wal::WALOpType::COMMIT},
    {"ROLLBACK pkg 1.0", wal::WALOpType::ROLLBACK},
    {"END pkg 1.0", wal::WALOpType::END},
    {"BACKUP /a \xe2\x86\x92 /a.bak", wal::WALOpType::BACKUP},
    {"NEW /new/file", wal::WALOpType::NEW},
    {"NEW_DIR /new/dir", wal::WALOpType::NEW_DIR},
    {"COPY /tmp/x \xe2\x86\x92 /dst/x", wal::WALOpType::COPY},
    {"REMOVE_OLD /old \xe2\x86\x92 /old.bak", wal::WALOpType::REMOVE_OLD},
    {"RM_BEGIN pkg 1.0", wal::WALOpType::RM_BEGIN},
    {"RM_COMMIT pkg 1.0", wal::WALOpType::RM_COMMIT},
    {"RM_END pkg 1.0", wal::WALOpType::RM_END},
    {"RM_DIR /dir 755 0 0", wal::WALOpType::RM_DIR},
    {"DB /db pkg:installed", wal::WALOpType::DB},
    {"DBNEW /db pkg:installed", wal::WALOpType::DBNEW},
    {"DBRM /db pkg:removed", wal::WALOpType::DBRM},
    {"RESTORE_FILE /bak \xe2\x86\x92 /orig", wal::WALOpType::RESTORE_FILE},
    {"RESTORE_DB /bak \xe2\x86\x92 /db", wal::WALOpType::RESTORE_DB},
    {"RESTORE_DIR /dir", wal::WALOpType::RESTORE_DIR},
    {"REMOVE_FILE /f", wal::WALOpType::REMOVE_FILE},     // 旧名称
    {"REMOVE_DIR /d", wal::WALOpType::REMOVE_DIR},       // 旧名称
    {"RESTORE_FILE_RM /f", wal::WALOpType::RESTORE_FILE_RM},
    {"RESTORE_DIR_RM /d", wal::WALOpType::RESTORE_DIR_RM},
    {"RESTORE_DB_RM /db", wal::WALOpType::RESTORE_DB_RM},
  };
  for (auto &c : cases) {
    auto op = wal::parse_op(c.line);
    EXPECT_EQ(op.type, c.type) << "Failed for: " << c.line;
  }
}

// ── trim_completed 处理大型 WAL ──────────────────────────────────────

TEST_F(WalEdgeCaseTest, TrimLargeCompletedBatch) {
  std::string wal;
  wal += "BEGIN_PKGS 1\n";
  for (int i = 0; i < 20; ++i)
    wal += "BACKUP /file" + std::to_string(i) +
           " \xe2\x86\x92 /file" + std::to_string(i) + ".bak\n";
  wal += "COMMIT_PKGS\n";
  wal += "BEGIN_PKGS 1\nBEGIN active 1.0\n";
  write_wal(wal);

  trim_completed();

  std::ifstream f(wal::wal_log_path());
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("active"), std::string::npos);
  // 已完成部分应被移除
  EXPECT_EQ(content.find("file0"), std::string::npos);
}

// ── reverse_execute 跳过 RESTORE 行 ─────────────────────────────────

TEST_F(WalEdgeCaseTest, ReverseExecuteSkipsAllRestoreLines) {
  // 构造包含所有 RESTORE/REMOVE 类型的 WAL
  fs::path a = test_root / "usr/bin/ea";
  fs::path abak = a.string() + std::string(constants::SUFFIX_LPKG_BAK) + "ea";
  fs::create_directories(a.parent_path());
  std::ofstream(a) << "keep\n";
  fs::rename(a, abak);

  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN ea 1.0\n"
      "BACKUP " + a.string() + " \xe2\x86\x92 " + abak.string() + "\n"
      "RESTORE_FILE /x \xe2\x86\x92 /y\n"
      "REMOVE_FILE /z\n"
      "RESTORE_DB /x \xe2\x86\x92 /y\n"
      "REMOVE_DIR /d\n");

  // 恢复时 RESTORE/REMOVE 行应被跳过，只处理 BACKUP
  recover_packages();

  EXPECT_TRUE(fs::exists(a));
  EXPECT_FALSE(fs::exists(abak));
}

// ── batch_rollback 正确调用 reverse_execute ────────────────────────

TEST_F(WalEdgeCaseTest, BatchRollbackProperlyWritesCommitPkgs) {
  fs::path file = test_root / "usr/bin/br_test";
  fs::path bak = file.string() + std::string(constants::SUFFIX_LPKG_BAK) + "br";
  fs::create_directories(file.parent_path());
  std::ofstream(file) << "orig\n";
  fs::rename(file, bak);

  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN br 1.0\n"
      "BACKUP " + file.string() + " \xe2\x86\x92 " + bak.string() + "\n"
      "COPY /tmp/br \xe2\x86\x92 " + file.string() + "\n"
      "COMMIT br 1.0\n"
      "END br 1.0\n");

  wal::batch_rollback({"br"});

  std::ifstream f(wal::wal_log_path());
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("COMMIT_PKGS"), std::string::npos);
  EXPECT_NE(content.find("ROLLBACK"), std::string::npos);
}

// ── cleanup_db_backups 只删备份，不删正常文件 ──────────────────────

TEST_F(WalEdgeCaseTest, CleanupDbBackupsSelective) {
  auto dir = Config::instance().state_dir();
  auto normal = dir / "normal_file";
  auto orphan = dir / "pkgs.lpkg_db_bak_before:test:installed";

  std::ofstream(normal) << "keep me\n";
  std::ofstream(orphan) << "delete me\n";

  cleanup_db_backups();

  EXPECT_TRUE(fs::exists(normal)) << "normal file preserved";
  EXPECT_FALSE(fs::exists(orphan)) << "orphan backup removed";
}

// ── recover_packages 在 WAL 文件不存在时静默 ────────────────────────

TEST_F(WalEdgeCaseTest, RecoverWithoutWalFile) {
  fs::remove(wal::wal_log_path());
  EXPECT_NO_THROW(recover_packages());
  EXPECT_NO_THROW(trim_completed());
  EXPECT_NO_THROW(cleanup_db_backups());
}

// ── 超大 BACKUP 数量恢复 ────────────────────────────────────────────

TEST_F(WalEdgeCaseTest, ManyBackupsRecovery) {
  std::vector<std::pair<fs::path, fs::path>> pairs;
  std::string wal = "BEGIN_PKGS 1\nBEGIN many 1.0\n";

  for (int i = 0; i < 30; ++i) {
    fs::path orig = test_root / ("usr/bin/f" + std::to_string(i));
    fs::path bak = orig.string() + std::string(constants::SUFFIX_LPKG_BAK) + "many";
    fs::create_directories(orig.parent_path());
    std::ofstream(orig) << "content " << i << "\n";
    fs::rename(orig, bak);
    wal += "BACKUP " + orig.string() + " \xe2\x86\x92 " + bak.string() + "\n";
    pairs.emplace_back(orig, bak);
  }

  write_wal(wal);
  recover_packages();

  for (auto &[orig, bak] : pairs) {
    EXPECT_TRUE(fs::exists(orig)) << orig;
    EXPECT_FALSE(fs::exists(bak)) << bak;
  }
}

// ── NEW_DIR 逆向：非空目录不删除 ────────────────────────────────────

TEST_F(WalEdgeCaseTest, NewDirAndNewFileReverseTogether) {
  fs::path dir = test_root / "usr/share/newdir_with_file";
  fs::path file_in_dir = dir / "data.txt";
  fs::create_directories(dir);
  std::ofstream(file_in_dir) << "data\n";

  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN nd 1.0\n"
      "NEW_DIR " + dir.string() + "\n"
      "NEW " + file_in_dir.string() + "\n");

  recover_packages();

  // NEW 逆向删除了文件
  EXPECT_FALSE(fs::exists(file_in_dir)) << "file removed by NEW reverse";
}

// ── milestone 停止：到达 :batch-start 后停止 ───────────────────────

TEST_F(WalEdgeCaseTest, MilestoneStopAtBatchStart) {
  std::string pkgs = (Config::instance().state_dir() / "pkgs").string();
  std::string bak = pkgs + ".lpkg_db_bak_before:pkg:installed";
  std::ofstream(bak) << "pre-state\n";
  std::ofstream(pkgs) << "post-state\n";

  std::vector<wal::WALOp> ops = {
    wal::parse_op("BACKUP /a \xe2\x86\x92 /a.bak"),
    wal::parse_op("DB " + pkgs + " pkg:installed"),
    wal::parse_op("DB " + pkgs + " :batch-start"),
  };
  // 修正路径
  if (ops.size() >= 2) { ops[1].arg1 = pkgs; ops[1].arg2 = "pkg:installed"; }
  if (ops.size() >= 3) { ops[2].arg1 = pkgs; ops[2].arg2 = ":batch-start"; }

  wal::reverse_execute(ops, ":batch-start", false);

  // :batch-start 行应被跳过，pkg:installed 行应恢复 DB
  EXPECT_FALSE(fs::exists(bak));
}

// ── 并发 DBLock ─────────────────────────────────────────────────────

TEST_F(WalEdgeCaseTest, DoubleDbLockThrows) {
  DBLock l1;
  EXPECT_THROW(DBLock l2, LpkgException);
}

// ── trim 后无残留 ───────────────────────────────────────────────────

TEST_F(WalEdgeCaseTest, TrimThenRecoverIsNoop) {
  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN ok 1.0\n"
      "COMMIT ok 1.0\n"
      "END ok 1.0\n"
      "COMMIT_PKGS\n");

  trim_completed();
  recover_packages(); // 应无操作

  std::ifstream f(wal::wal_log_path());
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  EXPECT_TRUE(content.empty() || content == "\n");
}

// ── WAL 行中 arg1 含空格 → 解析健壮性 ──────────────────────────────

TEST_F(WalEdgeCaseTest, WalOpWithSpacesInPath) {
  auto op = wal::parse_op("BACKUP /path/with spaces/file \xe2\x86\x92 /path/with spaces/file.bak");
  EXPECT_EQ(op.type, wal::WALOpType::BACKUP);
  // split_line 会把 "spaces/file" 当作 arg1 的结尾部分
  // 实际上所有空格分割，第一个 → 之前的全是 arg1
  EXPECT_FALSE(op.arg1.empty());
  EXPECT_FALSE(op.arg2.empty());
}

// ── DbMilestone 边界 ────────────────────────────────────────────────

TEST_F(WalEdgeCaseTest, DbMilestoneEdgeCases) {
  // 空字符串
  auto m1 = wal::DbMilestone::from_string("");
  EXPECT_EQ(m1.state, "");

  // 只有冒号
  auto m2 = wal::DbMilestone::from_string(":");
  EXPECT_EQ(m2.pkg, "");
  EXPECT_EQ(m2.state, "");

  // 双冒号
  auto m3 = wal::DbMilestone::from_string("pkg:state:extra");
  EXPECT_EQ(m3.pkg, "pkg");
}

// ── 所有 skip_in_reverse 类型覆盖 ──────────────────────────────────

TEST_F(WalEdgeCaseTest, AllSkipInReverseTypes) {
  EXPECT_TRUE(wal::parse_op("BEGIN_PKGS 1").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("COMMIT_PKGS").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("BEGIN p 1").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("COMMIT p 1").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("ROLLBACK p 1").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("END p 1").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("RM_BEGIN p 1").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("RM_COMMIT p 1").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("RM_END p 1").skip_in_reverse());

  EXPECT_TRUE(wal::parse_op("RESTORE_FILE a \xe2\x86\x92 b").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("RESTORE_DB a \xe2\x86\x92 b").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("RESTORE_DIR /d").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("REMOVE_FILE /f").skip_in_reverse());       // 旧名称
  EXPECT_TRUE(wal::parse_op("REMOVE_DIR /d").skip_in_reverse());        // 旧名称
  EXPECT_TRUE(wal::parse_op("RESTORE_FILE_RM /f").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("RESTORE_DIR_RM /d").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("RESTORE_DB_RM /db").skip_in_reverse());

  EXPECT_FALSE(wal::parse_op("BACKUP a \xe2\x86\x92 b").skip_in_reverse());
  EXPECT_FALSE(wal::parse_op("NEW /f").skip_in_reverse());
  EXPECT_FALSE(wal::parse_op("COPY a \xe2\x86\x92 b").skip_in_reverse());
  EXPECT_FALSE(wal::parse_op("DB /db p:installed").skip_in_reverse());
}

// ── 多次 recover 调用幂等 ───────────────────────────────────────────

TEST_F(WalEdgeCaseTest, MultipleRecoverCallsIdempotent) {
  fs::path f1 = test_root / "usr/bin/multi_rec";
  fs::path b1 = f1.string() + std::string(constants::SUFFIX_LPKG_BAK) + "mr";
  fs::create_directories(f1.parent_path());
  std::ofstream(f1) << "orig\n";
  fs::rename(f1, b1);

  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN mr 1.0\n"
      "BACKUP " + f1.string() + " \xe2\x86\x92 " + b1.string() + "\n");

  recover_packages();
  recover_packages();
  recover_packages(); // 幂等：多次调用不应崩溃

  EXPECT_TRUE(fs::exists(f1));
  EXPECT_FALSE(fs::exists(b1));
}

// ── batch_rollback 空列表 ──────────────────────────────────────────

TEST_F(WalEdgeCaseTest, BatchRollbackEmptySuccessList) {
  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN none 1.0\n"
      "BACKUP /a \xe2\x86\x92 /a.bak\n");

  EXPECT_NO_THROW(wal::batch_rollback({}));

  // 空列表 → 应提前返回（无要移除的包）
  // WAL 应保留，不写 COMMIT_PKGS
}

// ── cleanup_db_backups 递归清理子目录 ────────────────────────────

TEST_F(WalEdgeCaseTest, CleanupDbBackupsRecursive) {
  auto dir = Config::instance().state_dir();

  // 顶层备份
  auto top_bak = dir / "pkgs.lpkg_db_bak_before:top:installed";
  // 子目录备份（regression: 之前只扫描顶层，遗漏了这些）
  auto deps_bak = dir / "deps/pkg.lpkg_db_bak_before:pkg:removed";
  auto nso_bak = dir / "needed_so/pkg.lpkg_db_bak_before:pkg:removed";

  fs::create_directories(dir / "deps");
  fs::create_directories(dir / "needed_so");
  std::ofstream(top_bak) << "top\n";
  std::ofstream(deps_bak) << "deps\n";
  std::ofstream(nso_bak) << "nso\n";

  cleanup_db_backups();

  EXPECT_FALSE(fs::exists(top_bak));
  EXPECT_FALSE(fs::exists(deps_bak));
  EXPECT_FALSE(fs::exists(nso_bak));
}

// ── extract_current_batch_ops 在纯已完成 WAL 时返回空 ─────────────

TEST_F(WalEdgeCaseTest, ExtractReturnsEmptyOnAllCommitted) {
  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN done 1.0\n"
      "COMMIT done 1.0\n"
      "END done 1.0\n"
      "COMMIT_PKGS\n");

  auto ops = wal::extract_current_batch_ops(wal::wal_log_path());
  EXPECT_TRUE(ops.empty());
}
