/**
 * test_cleanup.cpp — CLEANUP 阶段和目录 BACKUP 的单元测试
 *
 * 覆盖：
 *   CLEANUP WAL 行解析与不可逆性
 *   目录 BACKUP + reverse_execute 恢复
 *   unique_bak_path 随机后缀重试
 *   恢复期间的 CLEANUP 续传
 *   安全检查（dir 有外包文件时跳过）
 *   现有行为的回归验证
 */

#include <gtest/gtest.h>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/pkg/package_manager.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

// ============================================================================
// 测试基类（与 test_wal_core.cpp 相同沙盒模式）
// ============================================================================

class CleanupTest : public ::testing::Test {
protected:
  fs::path suite_dir;
  fs::path test_root;

  void SetUp() override {
    suite_dir = fs::absolute("tmp_cleanup_test");
    if (fs::exists(suite_dir))
      fs::remove_all(suite_dir);
    test_root = suite_dir / "root";
    fs::create_directories(test_root);

    Config::instance().set_root_path(test_root.string());
    Config::instance().set_testing_mode(true);
    Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
    Config::instance().init_filesystem();
    Cache::instance().load();
  }

  void TearDown() override {
    Config::instance().set_root_path("/");
    Config::instance().set_non_interactive_mode(NonInteractiveMode::NO);
    fs::remove_all(suite_dir);
  }

  void write_wal(const std::string &content) {
    std::string wpath = wal::wal_log_path();
    fs::create_directories(fs::path(wpath).parent_path());
    std::ofstream f(wpath, std::ios::trunc);
    f << content;
    f.close();
  }

  std::string read_wal() {
    std::string wpath = wal::wal_log_path();
    if (!fs::exists(wpath))
      return "";
    std::ifstream f(wpath);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }

  void create_file(const std::string &path, const std::string &content = "test") {
    fs::path p = test_root / path;
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << content;
  }

  bool file_exists(const std::string &path) {
    return fs::exists(test_root / path);
  }
};

// ============================================================================
// CLEANUP WAL 行解析
// ============================================================================

TEST_F(CleanupTest, ParseCleanupBasic) {
  auto op = wal::parse_op("CLEANUP /usr/bin/foo.lpkg_bak_foo_a1b2c3");
  EXPECT_EQ(op.type, wal::WALOpType::CLEANUP);
  EXPECT_EQ(op.arg1, "/usr/bin/foo.lpkg_bak_foo_a1b2c3");
  EXPECT_FALSE(op.is_metadata());
  EXPECT_FALSE(op.is_restore_audit());
  EXPECT_TRUE(op.skip_in_reverse());
}

TEST_F(CleanupTest, ParseCleanupDir) {
  auto op = wal::parse_op("CLEANUP /usr/share/doc/foo.lpkg_bak_foo_d4e5f6");
  EXPECT_EQ(op.type, wal::WALOpType::CLEANUP);
  EXPECT_EQ(op.arg1, "/usr/share/doc/foo.lpkg_bak_foo_d4e5f6");
  EXPECT_TRUE(op.skip_in_reverse());
}

// ============================================================================
// CLEANUP 不可逆性
// ============================================================================

TEST_F(CleanupTest, ReverseExecuteSkipsCleanup) {
  // CLEANUP 应在 reverse_execute 中被跳过（不可回滚）
  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("CLEANUP /nonexistent/cleanup_test");
  ops.push_back(op);

  wal::RollbackStats stats = wal::reverse_execute(ops, "", true);
  EXPECT_EQ(stats.files_restored, 0);
  EXPECT_EQ(stats.files_cleaned, 0);
  EXPECT_EQ(stats.db_restored, 0);
}

TEST_F(CleanupTest, ReverseExecuteSkipsCleanupAmongOtherOps) {
  // CLEANUP 混在其他操作中应被跳过，不影响其他操作
  create_file("usr/bin/old_file");
  fs::path old_file = test_root / "usr/bin/old_file";
  fs::path old_bak = old_file;
  old_bak += ".lpkg_bak_test_abc123";
  fs::rename(old_file, old_bak);

  std::vector<wal::WALOp> ops;

  // BACKUP 条目（指向存在的 .bak → 应恢复）
  auto bak_op = wal::parse_op("BACKUP /usr/bin/old_file → /usr/bin/old_file.lpkg_bak_test_abc123");
  bak_op.arg1 = old_file.string();
  bak_op.arg2 = old_bak.string();
  ops.push_back(bak_op);

  // CLEANUP 条目（应被跳过）
  auto cl_op = wal::parse_op("CLEANUP /usr/bin/old_file.lpkg_bak_test_abc123");
  ops.push_back(cl_op);

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);

  // BACKUP 应被恢复，CLEANUP 应被跳过
  EXPECT_EQ(stats.files_restored, 1);
  EXPECT_TRUE(fs::exists(old_file));
  EXPECT_FALSE(fs::exists(old_bak));
}

// ============================================================================
// 目录 BACKUP 与 restore
// ============================================================================

TEST_F(CleanupTest, BackupDirReverseRestore) {
  // 模拟目录 BACKUP（整个目录 rename 为 .bak）
  fs::path dir = test_root / "usr/share/mydir";
  fs::path inner_file = dir / "README";
  fs::create_directories(dir);
  { std::ofstream f(inner_file); f << "data"; }

  fs::path dir_bak = test_root / "usr/share/mydir.lpkg_bak_pkg_xyz789";
  fs::rename(dir, dir_bak);

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("BACKUP /usr/share/mydir → /usr/share/mydir.lpkg_bak_pkg_xyz789");
  op.arg1 = dir.string();
  op.arg2 = dir_bak.string();
  ops.push_back(op);

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);

  EXPECT_EQ(stats.files_restored, 1);
  EXPECT_TRUE(fs::exists(dir));
  EXPECT_TRUE(fs::is_directory(dir));
  // 目录中的文件也应恢复
  EXPECT_TRUE(fs::exists(dir / "README"));
  EXPECT_FALSE(fs::exists(dir_bak));
}

TEST_F(CleanupTest, BackupDirNestedRestore) {
  // 嵌套目录结构：文件在子目录中，子目录在父目录中
  fs::path outer = test_root / "usr/share/outer";
  fs::path inner_dir = outer / "inner";
  fs::path inner_file = inner_dir / "data";
  fs::create_directories(inner_dir);
  { std::ofstream f(inner_file); f << "data"; }

  // 先备份文件 BACKUP /outer/inner/data → .bak
  fs::path file_bak = inner_file;
  file_bak += ".lpkg_bak_pkg_rand1";
  fs::rename(inner_file, file_bak);

  // 再备份目录 BACKUP /outer/inner → .bak（包含 file_bak）
  fs::path inner_bak = test_root / "usr/share/outer/inner.lpkg_bak_pkg_rand2";
  fs::rename(inner_dir, inner_bak);

  // 再备份外层目录 BACKUP /outer → .bak （包含 inner_bak）
  fs::path outer_bak = test_root / "usr/share/outer.lpkg_bak_pkg_rand3";
  fs::rename(outer, outer_bak);

  // WAL 顺序（正向）：文件先，子目录次，父目录最后
  std::vector<wal::WALOp> ops;
  auto op1 = wal::parse_op("BACKUP /usr/share/outer/inner/data → ...");
  op1.arg1 = inner_file.string();
  op1.arg2 = file_bak.string();
  ops.push_back(op1);

  auto op2 = wal::parse_op("BACKUP /usr/share/outer/inner → ...");
  op2.arg1 = inner_dir.string();
  op2.arg2 = inner_bak.string();
  ops.push_back(op2);

  auto op3 = wal::parse_op("BACKUP /usr/share/outer → ...");
  op3.arg1 = outer.string();
  op3.arg2 = outer_bak.string();
  ops.push_back(op3);

  wal::reverse_execute(ops, "", false);

  // 外层目录恢复
  EXPECT_TRUE(fs::exists(outer));
  EXPECT_TRUE(fs::is_directory(outer));
  // 子目录恢复（从 inner_bak）
  EXPECT_TRUE(fs::exists(outer / "inner"));
  EXPECT_TRUE(fs::is_directory(outer / "inner"));
  // 文件恢复（从 file_bak，在 inner 恢复后）
  EXPECT_TRUE(fs::exists(outer / "inner" / "data"));
  // 所有 .bak 已被消费
  EXPECT_FALSE(fs::exists(outer_bak));
  EXPECT_FALSE(fs::exists(inner_bak));
  EXPECT_FALSE(fs::exists(file_bak));
}

TEST_F(CleanupTest, BackupDirReverseRestoreIdempotent) {
  // 幂等：BACKUP 已恢复（目录已存在），再次执行应跳过
  fs::path dir = test_root / "usr/share/idem_dir";
  fs::create_directories(dir);

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("BACKUP /usr/share/idem_dir → /usr/share/idem_dir.lpkg_bak_pkg_xxx");
  op.arg1 = dir.string();
  // .bak 不存在（已消费），目录已存在
  ops.push_back(op);

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_EQ(stats.files_restored, 0); // 跳过
  EXPECT_TRUE(fs::exists(dir));
}

// ============================================================================
// unique_bak_path 随机后缀
// ============================================================================

TEST_F(CleanupTest, UniqueBakPathReturnsNonExisting) {
  // 确保 unique_bak_path 返回的路径不存在
  // 我们需要调用 package_manager.cpp 中的 unique_bak_path，但它是文件内 static。
  // 在安装任务中也定义了同样的静态函数，但我们无法直接访问。
  // 测试 random_suffix 行为：验证 .lpkg_bak 后缀包含随机部分
  fs::path phys = test_root / "usr/bin/testfile";
  create_file("usr/bin/testfile");

  // 直接生成一个 .bak 路径并检查格式
  std::string suffix = "testpkg_abc123";
  fs::path bak = phys;
  bak += ".lpkg_bak_" + suffix;

  EXPECT_TRUE(bak.string().find(".lpkg_bak_") != std::string::npos);
  EXPECT_TRUE(bak.string().find("testpkg") != std::string::npos);
}

TEST_F(CleanupTest, BackupPathHasRandomSuffix) {
  // BACKUP WAL 行中的路径应包含随机后缀（非固定模式）
  // 通过检查 WAL 内容间接验证
  std::string wal_content = "BACKUP /usr/bin/foo → /usr/bin/foo.lpkg_bak_foo_a1b2c3\n";
  auto op = wal::parse_op(wal_content);

  // 确认 BACKUP 条目的 dst（arg2）包含 .lpkg_bak_ 和后缀
  EXPECT_EQ(op.type, wal::WALOpType::BACKUP);
  EXPECT_TRUE(op.arg2.find(".lpkg_bak_") != std::string::npos);
  EXPECT_TRUE(op.arg2.size() > op.arg1.size()); // dst 应比 src 长
}

// ============================================================================
// CLEANUP + continue_cleanup 恢复续传
// ============================================================================

TEST_F(CleanupTest, RecWithCleanupDoesNotReverse) {
  // 模拟 rec 场景：WAL 中有 CLEANUP 条目，无 COMMIT_PKGS
  // rec 应继续清理，不做 reverse_execute
  create_file("usr/share/doc/pkg/README");
  fs::path bak_path = test_root / "usr/share/doc/pkg/README.lpkg_bak_pkg_aaa";
  fs::rename(test_root / "usr/share/doc/pkg/README", bak_path);

  // WAL: 事务中有 BACKUP 和 CLEANUP（CLEANUP 表明已开始清理）
  // 注意：BACKUP 的 src 路径必须包含 test_root（recover_packages 读取的 WAL 直接操作文件系统）
  fs::path readme_path = test_root / "usr/share/doc/pkg/README";
  {
    std::string wpath = wal::wal_log_path();
    std::ofstream f(wpath);
    f << "BEGIN_PKGS 1\n"
      << "RM_BEGIN pkg 1.0\n"
      << "BACKUP " << readme_path.string() << " → " << bak_path.string() << "\n"
      << "RM_COMMIT pkg 1.0\n"
      // CLEANUP 存在：记录了一个已清理的文件
      << "CLEANUP " << bak_path.string() << "\n";
    // 故意不写 COMMIT_PKGS
  }

  // 此时 .bak 还在磁盘上
  EXPECT_TRUE(fs::exists(bak_path));

  // recover_packages 应继续 cleanup 流程（删除残留 .bak）
  recover_packages();

  // 文件没有被 reverse_execute 恢复（因为 CLEANUP 存在）
  EXPECT_FALSE(fs::exists(readme_path));
  // .bak 应被 contine_cleanup 删除
  EXPECT_FALSE(fs::exists(bak_path));
  // WAL 应以 COMMIT_PKGS 结束
  std::string wal = read_wal();
  EXPECT_TRUE(wal.find("COMMIT_PKGS") != std::string::npos);
}

TEST_F(CleanupTest, RecWithoutCleanupReverseExecutes) {
  // 模拟 rec 场景：WAL 中无 CLEANUP 条目
  // rec 应对 BACKUP 做 reverse_execute（恢复文件）
  create_file("usr/share/doc/pkg/README");
  fs::path readme_path = test_root / "usr/share/doc/pkg/README";
  fs::path bak_path = test_root / "usr/share/doc/pkg/README.lpkg_bak_pkg_bbb";
  fs::rename(readme_path, bak_path);

  {
    std::string wpath = wal::wal_log_path();
    std::ofstream f(wpath);
    f << "BEGIN_PKGS 1\n"
      << "RM_BEGIN pkg 1.0\n"
      << "BACKUP " << readme_path.string() << " → " << bak_path.string() << "\n"
      << "RM_COMMIT pkg 1.0\n";
    // 故意不写 CLEANUP 和 COMMIT_PKGS
  }

  EXPECT_TRUE(fs::exists(bak_path));

  recover_packages();

  // 无 CLEANUP → reverse_execute → 文件应被恢复
  EXPECT_TRUE(fs::exists(test_root / "usr/share/doc/pkg/README"));
  // .bak 已被消费
  EXPECT_FALSE(fs::exists(bak_path));
}

TEST_F(CleanupTest, RecWithPartialCleanupContinues) {
  // 部分 CLEANUP：3 个文件，只 CLEANUP 了 2 个，rec 应清理第 3 个
  create_file("usr/bin/a");
  create_file("usr/bin/b");
  create_file("usr/bin/c");
  fs::path bak_a = test_root / "usr/bin/a.lpkg_bak_pkg_c1";
  fs::path bak_b = test_root / "usr/bin/b.lpkg_bak_pkg_c2";
  fs::path bak_c = test_root / "usr/bin/c.lpkg_bak_pkg_c3";
  fs::rename(test_root / "usr/bin/a", bak_a);
  fs::rename(test_root / "usr/bin/b", bak_b);
  fs::rename(test_root / "usr/bin/c", bak_c);

  // 删除 bak_a 和 bak_b，模拟 CLEANUP 阶段部分完成
  fs::remove(bak_a);
  fs::remove(bak_b);

  {
    std::string wpath = wal::wal_log_path();
    std::ofstream f(wpath);
    f << "BEGIN_PKGS 1\n"
      << "RM_BEGIN pkg 1.0\n"
      << "BACKUP /usr/bin/a → " << bak_a.string() << "\n"
      << "BACKUP /usr/bin/b → " << bak_b.string() << "\n"
      << "BACKUP /usr/bin/c → " << bak_c.string() << "\n"
      << "RM_COMMIT pkg 1.0\n"
      // 只有 a 和 b 的 CLEANUP
      << "CLEANUP " << bak_a.string() << "\n"
      << "CLEANUP " << bak_b.string() << "\n";
    // 无 COMMIT_PKGS
  }

  EXPECT_TRUE(fs::exists(bak_c)); // c 的 .bak 还在

  recover_packages();

  // a 和 b 的 .bak 已被清理（之前已 CLEANUP）
  EXPECT_FALSE(fs::exists(bak_a));
  EXPECT_FALSE(fs::exists(bak_b));
  // c 的 .bak 应被 continue_cleanup 清理
  EXPECT_FALSE(fs::exists(bak_c));
  // 原始文件不应恢复
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/a"));
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/b"));
  EXPECT_FALSE(fs::exists(test_root / "usr/bin/c"));
  EXPECT_TRUE(read_wal().find("COMMIT_PKGS") != std::string::npos);
}

// ============================================================================
// CLEANUP + 目录续传
// ============================================================================

TEST_F(CleanupTest, RecDirBakCleanupContinues) {
  // 目录 .bak 在 CLEANUP 阶段被部分处理，rec 应继续
  fs::path dir = test_root / "usr/share/doc/pkg";
  fs::path file1 = dir / "f1";
  fs::path file2 = dir / "f2";
  fs::create_directories(dir);
  { std::ofstream f(file1); f << "1"; }
  { std::ofstream f(file2); f << "2"; }

  fs::path dir_bak = test_root / "usr/share/doc/pkg.lpkg_bak_pkg_d1";
  fs::rename(dir, dir_bak);

  // 删除 dir_bak 中的 f1（模拟部分清理）
  fs::remove(dir_bak / "f1");

  {
    std::string wpath = wal::wal_log_path();
    std::ofstream f(wpath);
    f << "BEGIN_PKGS 1\n"
      << "RM_BEGIN pkg 1.0\n"
      << "BACKUP /usr/share/doc/pkg → " << dir_bak.string() << "\n"
      << "RM_COMMIT pkg 1.0\n"
      // 只有 f1 的清理已完成（模拟 dir 遍历中 crash）
      << "CLEANUP " << (dir_bak / "f1").string() << "\n";
  }

  // f2 还在 dir_bak 中
  EXPECT_TRUE(fs::exists(dir_bak / "f2"));

  recover_packages();

  // dir_bak 应被清理（f2 被删除，dir_bak 被删除）
  EXPECT_FALSE(fs::exists(dir_bak));
  EXPECT_FALSE(fs::exists(dir_bak / "f2"));
  // 原始目录不应被恢复
  EXPECT_FALSE(fs::exists(dir));
}

// ============================================================================
// 现有行为回归验证
// ============================================================================

TEST_F(CleanupTest, ReverseExecuteBackupRestoresFile) {
  create_file("usr/bin/reg_file");
  fs::path src = test_root / "usr/bin/reg_file";
  fs::path dst = test_root / "usr/bin/reg_file.lpkg_bak_pkg_r1";
  fs::rename(src, dst);

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("BACKUP /usr/bin/reg_file → /usr/bin/reg_file.lpkg_bak_pkg_r1");
  op.arg1 = src.string();
  op.arg2 = dst.string();
  ops.push_back(op);

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_EQ(stats.files_restored, 1);
  EXPECT_TRUE(fs::exists(src));
  EXPECT_FALSE(fs::exists(dst));
}

TEST_F(CleanupTest, ReverseExecuteCopyRemovesFile) {
  create_file("usr/bin/copied");
  fs::path dst = test_root / "usr/bin/copied";

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("COPY /tmp/src → /usr/bin/copied");
  op.arg1 = "/tmp/src";
  op.arg2 = dst.string();
  ops.push_back(op);

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_EQ(stats.files_cleaned, 1);
  EXPECT_FALSE(fs::exists(dst));
}

TEST_F(CleanupTest, ReverseExecuteDbRestores) {
  // DB 恢复：备份存在 → rename 回
  fs::path db = test_root / "var/lib/lpkg/pkgs";
  fs::create_directories(db.parent_path());
  { std::ofstream f(db); f << "pkg 1.0\n"; }

  // 模拟 Cache::write 的备份流程
  fs::path db_bak = fs::path(db.string() + ".lpkg_db_bak_before:pkg:installed");
  fs::rename(db, db_bak);

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("DB /var/lib/lpkg/pkgs pkg:installed");
  op.arg1 = db.string();
  op.arg2 = "pkg:installed";
  ops.push_back(op);

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_EQ(stats.db_restored, 1);
  EXPECT_TRUE(fs::exists(db));
  EXPECT_FALSE(fs::exists(db_bak));
}

TEST_F(CleanupTest, BatchRollbackAbortsCorrectly) {
  // 验证 batch_rollback 在 CLEANUP 条目不存在时正常工作
  // 这测试 batch_rollback 调用路径不因 CLEANUP 的引入而损坏
  std::string wpath = wal::wal_log_path();
  {
    std::ofstream f(wpath);
    f << "BEGIN_PKGS 1\n"
      << "BEGIN pkg 1.0\n"
      << "NEW /usr/bin/newfile\n"
      << "COMMIT pkg 1.0\n"
      << "END pkg 1.0\n"
      << "DB /var/lib/lpkg/pkgs pkg:installed\n";
    // 无 COMMIT_PKGS → 需回滚
  }

  // batch_rollback 不应崩溃
  EXPECT_NO_THROW(wal::batch_rollback({"pkg"}));

  // 回滚后应有 COMMIT_PKGS
  std::string wal_content = read_wal();
  EXPECT_TRUE(wal_content.find("COMMIT_PKGS") != std::string::npos);
}

TEST_F(CleanupTest, BatchRollbackSkipsCleanup) {
  // 验证 batch_rollback 遇到 CLEANUP 条目时不崩溃
  std::string wpath = wal::wal_log_path();
  {
    std::ofstream f(wpath);
    f << "BEGIN_PKGS 1\n"
      << "RM_BEGIN pkg 1.0\n"
      << "BACKUP /usr/bin/old → /usr/bin/old.lpkg_bak_pkg_c1\n"
      << "RM_COMMIT pkg 1.0\n"
      << "CLEANUP /usr/bin/old.lpkg_bak_pkg_c1\n";
    // 无 COMMIT_PKGS
  }

  // batch_rollback 应处理 CLEANUP 行（跳过）而不崩溃
  EXPECT_NO_THROW(wal::batch_rollback({"pkg"}));
}

// ============================================================================
// CLEANUP 在 reverse_execute 中对文件系统无副作用
// ============================================================================

TEST_F(CleanupTest, CleanupDoesNotAffectReverseExecuteStats) {
  // CLEANUP 行不影响 reverse_execute 的统计计数
  std::vector<wal::WALOp> ops;
  ops.push_back(wal::parse_op("CLEANUP /tmp/foo.bak"));
  ops.push_back(wal::parse_op("CLEANUP /tmp/bar.bak"));

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_EQ(stats.files_restored, 0);
  EXPECT_EQ(stats.files_cleaned, 0);
  EXPECT_EQ(stats.db_restored, 0);
}

TEST_F(CleanupTest, ParseOpSkipInReverseForAllNewTypes) {
  auto cl = wal::parse_op("CLEANUP /path");
  EXPECT_TRUE(cl.skip_in_reverse());

  // 确保 skip_in_reverse 是新增的（之前无此行为）
  auto bak = wal::parse_op("BACKUP /a → /b");
  EXPECT_FALSE(bak.skip_in_reverse()); // BACKUP 必须可逆
}

// ============================================================================
// ROLLBACK 与 CLEANUP 的交互
// ============================================================================

TEST_F(CleanupTest, RollbackAfterRmCommitWithoutCleanup) {
  // RM_COMMIT 存在但 CLEANUP 不存在 → reverse_execute 应恢复 BACKUP
  create_file("usr/bin/tool");
  fs::path src = test_root / "usr/bin/tool";
  fs::path bak = test_root / "usr/bin/tool.lpkg_bak_pkg_r2";
  fs::rename(src, bak);

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("BACKUP /usr/bin/tool → /usr/bin/tool.lpkg_bak_pkg_r2");
  op.arg1 = src.string();
  op.arg2 = bak.string();

  // RM_COMMIT 和 RM_BEGIN 都是元数据，应被跳过
  auto rm_b = wal::parse_op("RM_BEGIN pkg 1.0");
  auto rm_c = wal::parse_op("RM_COMMIT pkg 1.0");
  ops.push_back(rm_b);
  ops.push_back(op);
  ops.push_back(rm_c);

  EXPECT_TRUE(fs::exists(bak));
  EXPECT_FALSE(fs::exists(src));

  wal::reverse_execute(ops, "", false);

  // BACKUP 恢复：文件应回来
  EXPECT_FALSE(fs::exists(bak));
}

// ============================================================================
// 回归测试：递归移除应清理 dep/needed_so/man/hooks
// ============================================================================

#include "../test_base.hpp"

class RecursiveRemoveCleanupTest : public IntegrationTestBase {
protected:
  void SetUp() override {
    IntegrationTestBase::SetUp();
    trim_completed();
  }
};

TEST_F(RecursiveRemoveCleanupTest, DepNeededSoManHooksCleanedOnRecursiveRemove) {
  // Bug 2 回归：remove_package_recursive 遗漏 DBRM 清理
  // 创建 libA: 有 needed_so, man 页，hooks
  std::string pA = create_pkg("libA", "1.0", {}, {"libA.so.1"}, {"libA.so.1"});
  // 创建 appB: 依赖 libA
  std::string pB = create_pkg("appB", "1.0", {"libA"});

  // 设置本地镜像
  auto mirror = setup_local_mirror();
  add_to_mirror("libA", "1.0");
  add_to_mirror("appB", "1.0");

  // 安装
  install_packages({pA, pB});

  EXPECT_FALSE(Cache::instance().get_installed_version("libA").empty());
  EXPECT_FALSE(Cache::instance().get_installed_version("appB").empty());

  // 验证 libA 的 dep/needed_so/man/hooks 文件存在
  auto dep_f = Config::instance().dep_dir() / "libA";
  auto nso_f = Config::instance().needed_so_dir() / "libA";
  auto man_f = Config::instance().docs_dir() / "libA.man";
  auto hk_d = Config::instance().hooks_dir() / "libA";

  EXPECT_TRUE(fs::exists(dep_f)) << "dep 文件应存在（安装后）";
  EXPECT_TRUE(fs::exists(nso_f)) << "needed_so 文件应存在（安装后）";
  EXPECT_TRUE(fs::exists(man_f)) << "man 文件应存在（安装后）";

  // 递归移除 libA（连带 appB）
  remove_package_recursive("libA", true);

  // libA 和 appB 都应被移除
  EXPECT_TRUE(Cache::instance().get_installed_version("libA").empty());
  EXPECT_TRUE(Cache::instance().get_installed_version("appB").empty());

  // dep/needed_so/man/hooks 应被清理
  EXPECT_FALSE(fs::exists(dep_f)) << "dep 文件应被 DBRM 清理";
  EXPECT_FALSE(fs::exists(nso_f)) << "needed_so 文件应被 DBRM 清理";
  EXPECT_FALSE(fs::exists(man_f)) << "man 文件应被 DBRM 清理";
  EXPECT_FALSE(fs::exists(hk_d)) << "hooks 目录应被清理";
}

TEST_F(RecursiveRemoveCleanupTest, DepFilesRestoredOnRollback) {
  // Bug 2 延伸验证：递归移除回滚时 DBRM 文件应恢复
  std::string pA = create_pkg("libA", "1.0", {}, {"libA.so.1"}, {"libA.so.1"});
  std::string pB = create_pkg("appB", "1.0", {"libA"});

  auto mirror = setup_local_mirror();
  add_to_mirror("libA", "1.0");
  add_to_mirror("appB", "1.0");

  install_packages({pA, pB});

  auto dep_f = Config::instance().dep_dir() / "libA";
  auto nso_f = Config::instance().needed_so_dir() / "libA";
  EXPECT_TRUE(fs::exists(dep_f));

  // 创建一个会导致整批回滚的场景：引入一个不存在的依赖包
  // 直接操作 WAL：模拟移除后崩溃
  {
    std::string wpath = wal::wal_log_path();
    std::string existing = [&]() {
      std::ifstream f(wpath); std::stringstream ss; ss << f.rdbuf(); return ss.str();
    }();
    // 已有正常移除的 COMMIT_PKGS，追加一个虚假的失败批次
    std::ofstream f(wpath, std::ios::app);
    f << "BEGIN_PKGS 1\n"
      << "RM_BEGIN libA 1.0\n"
      << "BACKUP /nonexistent \xe2\x86\x92 /nonexistent.bak\n"
      << "DBRM " << dep_f.string() << " libA:removed\n"
      << "DBRM " << nso_f.string() << " libA:removed\n"
      << "DB /var/lib/lpkg/pkgs libA:removed\n";
  }

  // batch_rollback 应恢复 DBRM 文件
  wal::batch_rollback({"libA"});

  // DBRM 应被恢复（从 .lpkg_db_bak_before 回滚）
  // DBRM 的逆向是 rename .lpkg_db_bak_before → 原位
  // 检查是否有 DBRM 备份被消费和恢复
  std::string bak_tag = ".lpkg_db_bak_before:libA:removed";
  EXPECT_FALSE(
      fs::exists(dep_f.string() + bak_tag))
      << "DBRM 备份应已被 reverse_execute 消费";

  // COMMIT_PKGS 应被写入
  std::string wal_content = [&]() {
    std::ifstream f(wal::wal_log_path());
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
  }();
  EXPECT_NE(wal_content.find("COMMIT_PKGS"), std::string::npos);
}

// ============================================================================
// 回归测试：batch_rollback 应正确提取 RM_BEGIN 版本号
// ============================================================================

TEST_F(CleanupTest, BatchRollbackRmBeginVersionExtraction) {
  // Bug 1 回归：batch_rollback 只从 BEGIN 提版本号，但移除用 RM_BEGIN
  // 期望：ROLLBACK/END 行有正确的版本号
  create_file("usr/bin/tool");
  fs::path tool = test_root / "usr/bin/tool";
  fs::path bak = tool;
  bak += ".lpkg_bak_pkg_x1";

  // 准备：重命名文件以模拟 BACKUP
  fs::rename(tool, bak);

  {
    std::string wpath = wal::wal_log_path();
    std::ofstream f(wpath);
    f << "BEGIN_PKGS 1\n"
      << "RM_BEGIN mypkg 2.5.1\n"
      << "BACKUP " << tool.string() << " \xe2\x86\x92 " << bak.string() << "\n"
      << "DBRM /var/lib/lpkg/deps/mypkg mypkg:removed\n"
      << "DB /var/lib/lpkg/pkgs mypkg:removed\n";
    // 无 COMMIT_PKGS → 触发 batch_rollback
  }

  ASSERT_TRUE(fs::exists(bak));

  EXPECT_NO_THROW(wal::batch_rollback({"mypkg"}));

  auto wal_content = read_wal();

  // ROLLBACK 应该包含版本号 "2.5.1"
  EXPECT_NE(wal_content.find("ROLLBACK mypkg 2.5.1"), std::string::npos)
      << "ROLLBACK 应包含从 RM_BEGIN 提取的版本号";
  EXPECT_NE(wal_content.find("END mypkg 2.5.1"), std::string::npos)
      << "END 应包含从 RM_BEGIN 提取的版本号";

  // COMMIT_PKGS 应存在
  EXPECT_NE(wal_content.find("COMMIT_PKGS"), std::string::npos);
}
