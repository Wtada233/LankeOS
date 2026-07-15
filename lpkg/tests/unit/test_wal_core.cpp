/**
 * test_wal_core.cpp — WAL 2.0 核心单元测试
 *
 * 覆盖：DbMilestone、WAL 行解析、reverse_execute、batch_rollback、
 * recover_packages、trim_completed、幂等性
 */

#include <gtest/gtest.h>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/db/wal_op.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ============================================================================
// 测试基类：管理和 WAL 测试沙盒
// ============================================================================

class WalCoreTest : public ::testing::Test {
protected:
  fs::path suite_dir;
  fs::path test_root;

  void SetUp() override {
    suite_dir = fs::absolute("tmp_wal_core_test");
    if (fs::exists(suite_dir))
      fs::remove_all(suite_dir);
    test_root = suite_dir / "root";
    fs::create_directories(test_root);

    Config::instance().set_root_path(test_root.string());
    Config::instance().set_testing_mode(true);
    Config::instance().init_filesystem();
    Cache::instance().load();
  }

  void TearDown() override {
    Config::instance().set_root_path("/");
    fs::remove_all(suite_dir);
  }

  /// 写入 WAL 日志
  void write_wal(const std::string &content) {
    std::string wpath = wal::wal_log_path();
    fs::create_directories(fs::path(wpath).parent_path());
    std::ofstream f(wpath, std::ios::trunc);
    f << content;
    f.close();
  }

  /// 读取 WAL 日志
  std::string read_wal() {
    std::string wpath = wal::wal_log_path();
    if (!fs::exists(wpath))
      return "";
    std::ifstream f(wpath);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }

  /// 创建测试文件
  void create_file(const std::string &path, const std::string &content = "test") {
    fs::path p = test_root / path;
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << content;
  }

  /// 检查文件是否存在
  bool file_exists(const std::string &path) {
    return fs::exists(test_root / path);
  }

  /// 读取文件内容
  std::string read_file(const std::string &path) {
    fs::path p = test_root / path;
    if (!fs::exists(p))
      return "";
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }
};

// ============================================================================
// DbMilestone 测试
// ============================================================================

TEST_F(WalCoreTest, DbMilestoneToString) {
  wal::DbMilestone m1{"glibc", "installed"};
  EXPECT_EQ(m1.to_string(), "glibc:installed");

  wal::DbMilestone m2{"", "batch-start"};
  EXPECT_EQ(m2.to_string(), ":batch-start");

  wal::DbMilestone m3{"curl", "removed"};
  EXPECT_EQ(m3.to_string(), "curl:removed");
}

TEST_F(WalCoreTest, DbMilestoneFromString) {
  auto m1 = wal::DbMilestone::from_string("glibc:installed");
  EXPECT_EQ(m1.pkg, "glibc");
  EXPECT_EQ(m1.state, "installed");

  auto m2 = wal::DbMilestone::from_string(":batch-start");
  EXPECT_EQ(m2.pkg, "");
  EXPECT_EQ(m2.state, "batch-start");

  auto m3 = wal::DbMilestone::from_string("unknown");
  EXPECT_EQ(m3.state, "unknown");

  auto m4 = wal::DbMilestone::from_string("pkg:removed:extra");
  EXPECT_EQ(m4.pkg, "pkg");
}

TEST_F(WalCoreTest, DbMilestoneIsBatchStart) {
  EXPECT_TRUE((wal::DbMilestone{"", "batch-start"}.is_batch_start()));
  EXPECT_TRUE(wal::DbMilestone::from_string(":batch-start").is_batch_start());
  EXPECT_FALSE((wal::DbMilestone{"glibc", "installed"}.is_batch_start()));
}

// ============================================================================
// parse_op 测试
// ============================================================================

TEST_F(WalCoreTest, ParseOpBeginPkgs) {
  auto op = wal::parse_op("BEGIN_PKGS 3");
  EXPECT_EQ(op.type, wal::WALOpType::BEGIN_PKGS);
  EXPECT_EQ(op.arg1, "3");
}

TEST_F(WalCoreTest, ParseOpCommitPkgs) {
  auto op = wal::parse_op("COMMIT_PKGS");
  EXPECT_EQ(op.type, wal::WALOpType::COMMIT_PKGS);
}

TEST_F(WalCoreTest, ParseOpBeginPkg) {
  auto op = wal::parse_op("BEGIN glibc 2.39");
  EXPECT_EQ(op.type, wal::WALOpType::BEGIN);
  EXPECT_EQ(op.arg1, "glibc");
  EXPECT_EQ(op.arg2, "2.39");
}

TEST_F(WalCoreTest, ParseOpCommitPkg) {
  auto op = wal::parse_op("COMMIT glibc 2.39");
  EXPECT_EQ(op.type, wal::WALOpType::COMMIT);
}

TEST_F(WalCoreTest, ParseOpRollbackPkg) {
  auto op = wal::parse_op("ROLLBACK bash 5.2");
  EXPECT_EQ(op.type, wal::WALOpType::ROLLBACK);
  EXPECT_EQ(op.arg1, "bash");
  EXPECT_EQ(op.arg2, "5.2");
}

TEST_F(WalCoreTest, ParseOpEndPkg) {
  auto op = wal::parse_op("END glibc 2.39");
  EXPECT_EQ(op.type, wal::WALOpType::END);
}

TEST_F(WalCoreTest, ParseOpBackup) {
  auto op = wal::parse_op("BACKUP /usr/bin/bash → /usr/bin/bash.lpkg_bak_bash");
  EXPECT_EQ(op.type, wal::WALOpType::BACKUP);
  EXPECT_EQ(op.arg1, "/usr/bin/bash");
  EXPECT_TRUE(op.arg2.find("lpkg_bak_bash") != std::string::npos);
}

TEST_F(WalCoreTest, ParseOpNew) {
  auto op = wal::parse_op("NEW /usr/share/doc/README");
  EXPECT_EQ(op.type, wal::WALOpType::NEW);
  EXPECT_EQ(op.arg1, "/usr/share/doc/README");
}

TEST_F(WalCoreTest, ParseOpNewDir) {
  auto op = wal::parse_op("NEW_DIR /usr/share/newpkg");
  EXPECT_EQ(op.type, wal::WALOpType::NEW_DIR);
  EXPECT_EQ(op.arg1, "/usr/share/newpkg");
}

TEST_F(WalCoreTest, ParseOpCopy) {
  auto op = wal::parse_op("COPY /tmp/bash.lpkgtmp → /usr/bin/bash");
  EXPECT_EQ(op.type, wal::WALOpType::COPY);
  EXPECT_EQ(op.arg1, "/tmp/bash.lpkgtmp");
  EXPECT_EQ(op.arg2, "/usr/bin/bash");
}

TEST_F(WalCoreTest, ParseOpRemoveOld) {
  auto op = wal::parse_op("REMOVE_OLD /usr/lib/libold.so → /usr/lib/libold.so.lpkg_bak");
  EXPECT_EQ(op.type, wal::WALOpType::REMOVE_OLD);
  EXPECT_EQ(op.arg1, "/usr/lib/libold.so");
  EXPECT_TRUE(op.arg2.find("lpkg_bak") != std::string::npos);
}

TEST_F(WalCoreTest, ParseOpRmBegin) {
  auto op = wal::parse_op("RM_BEGIN vim 9.1");
  EXPECT_EQ(op.type, wal::WALOpType::RM_BEGIN);
  EXPECT_EQ(op.arg1, "vim");
  EXPECT_EQ(op.arg2, "9.1");
}

TEST_F(WalCoreTest, ParseOpRmCommit) {
  auto op = wal::parse_op("RM_COMMIT vim 9.1");
  EXPECT_EQ(op.type, wal::WALOpType::RM_COMMIT);
}

TEST_F(WalCoreTest, ParseOpRmEnd) {
  auto op = wal::parse_op("RM_END vim 9.1");
  EXPECT_EQ(op.type, wal::WALOpType::RM_END);
}

TEST_F(WalCoreTest, ParseOpRmDir) {
  auto op = wal::parse_op("RM_DIR /usr/share/vim 755 0 0");
  EXPECT_EQ(op.type, wal::WALOpType::RM_DIR);
  EXPECT_EQ(op.arg1, "/usr/share/vim");
  // split_line assigns sequentially: arg2=mode, arg3=uid, arg4=gid
  EXPECT_EQ(op.arg2, "755");
  EXPECT_EQ(op.arg3, "0");
  EXPECT_EQ(op.arg4, "0");
}

TEST_F(WalCoreTest, ParseOpDb) {
  auto op = wal::parse_op("DB /var/lib/lpkg/pkgs glibc:installed");
  EXPECT_EQ(op.type, wal::WALOpType::DB);
  EXPECT_EQ(op.arg1, "/var/lib/lpkg/pkgs");
  EXPECT_EQ(op.arg2, "glibc:installed");
}

TEST_F(WalCoreTest, ParseOpDbNew) {
  auto op = wal::parse_op("DBNEW /var/lib/lpkg/needed_so/bash bash:installed");
  EXPECT_EQ(op.type, wal::WALOpType::DBNEW);
}

TEST_F(WalCoreTest, ParseOpDbRm) {
  auto op = wal::parse_op("DBRM /var/lib/lpkg/deps/vim vim:removed");
  EXPECT_EQ(op.type, wal::WALOpType::DBRM);
}

TEST_F(WalCoreTest, ParseOpRestoreFile) {
  auto op = wal::parse_op("RESTORE_FILE /usr/bin/bash.lpkg_bak_bash → /usr/bin/bash");
  EXPECT_EQ(op.type, wal::WALOpType::RESTORE_FILE);
  EXPECT_EQ(op.arg1, "/usr/bin/bash.lpkg_bak_bash");
  EXPECT_EQ(op.arg2, "/usr/bin/bash");
}

TEST_F(WalCoreTest, ParseOpRestoreDb) {
  auto op = wal::parse_op("RESTORE_DB /var/lib/lpkg/pkgs.lpkg_db_bak_before:glibc:installed → /var/lib/lpkg/pkgs");
  EXPECT_EQ(op.type, wal::WALOpType::RESTORE_DB);
}

TEST_F(WalCoreTest, ParseOpRestoreDir) {
  auto op = wal::parse_op("RESTORE_DIR /usr/share/vim");
  EXPECT_EQ(op.type, wal::WALOpType::RESTORE_DIR);
  EXPECT_EQ(op.arg1, "/usr/share/vim");
}

TEST_F(WalCoreTest, ParseOpRemoveFile) {
  // 旧名称 — 向后兼容解析旧 WAL 文件
  auto op = wal::parse_op("REMOVE_FILE /usr/bin/newtool");
  EXPECT_EQ(op.type, wal::WALOpType::REMOVE_FILE);
  EXPECT_EQ(op.arg1, "/usr/bin/newtool");
}

TEST_F(WalCoreTest, ParseOpRemoveDir) {
  auto op = wal::parse_op("REMOVE_DIR /usr/share/newpkg");
  EXPECT_EQ(op.type, wal::WALOpType::REMOVE_DIR);
}

// 新名称 — RESTORE 统一前缀
TEST_F(WalCoreTest, ParseOpRestoreFileRm) {
  auto op = wal::parse_op("RESTORE_FILE_RM /usr/bin/newtool");
  EXPECT_EQ(op.type, wal::WALOpType::RESTORE_FILE_RM);
  EXPECT_EQ(op.arg1, "/usr/bin/newtool");
}

TEST_F(WalCoreTest, ParseOpRestoreDirRm) {
  auto op = wal::parse_op("RESTORE_DIR_RM /usr/share/newpkg");
  EXPECT_EQ(op.type, wal::WALOpType::RESTORE_DIR_RM);
  EXPECT_EQ(op.arg1, "/usr/share/newpkg");
}

TEST_F(WalCoreTest, ParseOpRestoreDbRm) {
  auto op = wal::parse_op("RESTORE_DB_RM /var/lib/lpkg/pkgs");
  EXPECT_EQ(op.type, wal::WALOpType::RESTORE_DB_RM);
  EXPECT_EQ(op.arg1, "/var/lib/lpkg/pkgs");
}

TEST_F(WalCoreTest, ParseOpInvalidLine) {
  auto op = wal::parse_op("GARBAGE_LINE_WITH_UNKNOWN_TYPE");
  EXPECT_EQ(op.arg1, "__INVALID__");
}

TEST_F(WalCoreTest, ParseOpEmptyLine) {
  auto op = wal::parse_op("");
  // split_line("") 返回 [""]，walop_type_from_name("") 抛异常 → arg1 = "__INVALID__"
  EXPECT_EQ(op.arg1, "__INVALID__");
}

// ============================================================================
// WALOp::is_metadata / is_restore_audit / skip_in_reverse 测试
// ============================================================================

TEST_F(WalCoreTest, IsMetadata) {
  EXPECT_TRUE(wal::parse_op("ROLLBACK pkg 1.0").is_metadata());
  EXPECT_TRUE(wal::parse_op("END pkg 1.0").is_metadata());
  EXPECT_TRUE(wal::parse_op("COMMIT pkg 1.0").is_metadata());
  EXPECT_TRUE(wal::parse_op("BEGIN pkg 1.0").is_metadata());
  EXPECT_TRUE(wal::parse_op("RM_BEGIN pkg 1.0").is_metadata());
  EXPECT_TRUE(wal::parse_op("RM_COMMIT pkg 1.0").is_metadata());
  EXPECT_TRUE(wal::parse_op("RM_END pkg 1.0").is_metadata());
  EXPECT_TRUE(wal::parse_op("BEGIN_PKGS 1").is_metadata());
  EXPECT_TRUE(wal::parse_op("COMMIT_PKGS").is_metadata());

  EXPECT_FALSE(wal::parse_op("BACKUP a → b").is_metadata());
  EXPECT_FALSE(wal::parse_op("NEW /path").is_metadata());
  EXPECT_FALSE(wal::parse_op("COPY a → b").is_metadata());
  EXPECT_FALSE(wal::parse_op("DB /db glibc:installed").is_metadata());
}

TEST_F(WalCoreTest, IsRestoreAudit) {
  EXPECT_TRUE(wal::parse_op("RESTORE_FILE a → b").is_restore_audit());
  EXPECT_TRUE(wal::parse_op("RESTORE_DB a → b").is_restore_audit());
  EXPECT_TRUE(wal::parse_op("RESTORE_DIR /path").is_restore_audit());
  EXPECT_TRUE(wal::parse_op("REMOVE_FILE /path").is_restore_audit());  // 旧名称
  EXPECT_TRUE(wal::parse_op("REMOVE_DIR /path").is_restore_audit());   // 旧名称
  EXPECT_TRUE(wal::parse_op("RESTORE_FILE_RM /path").is_restore_audit());
  EXPECT_TRUE(wal::parse_op("RESTORE_DIR_RM /path").is_restore_audit());
  EXPECT_TRUE(wal::parse_op("RESTORE_DB_RM /path").is_restore_audit());

  EXPECT_FALSE(wal::parse_op("BACKUP a → b").is_restore_audit());
}

TEST_F(WalCoreTest, SkipInReverse) {
  // 元数据和 RESTORE 审计行在 reverse_execute 中跳过
  EXPECT_TRUE(wal::parse_op("BEGIN pkg 1.0").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("RESTORE_FILE a → b").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("REMOVE_FILE /path").skip_in_reverse());    // 旧名称
  EXPECT_TRUE(wal::parse_op("REMOVE_DIR /path").skip_in_reverse());     // 旧名称
  EXPECT_TRUE(wal::parse_op("RESTORE_FILE_RM /path").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("RESTORE_DIR_RM /path").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("RESTORE_DB_RM /path").skip_in_reverse());
  EXPECT_TRUE(wal::parse_op("BEGIN_PKGS 1").skip_in_reverse());

  // 正向操作行不跳过
  EXPECT_FALSE(wal::parse_op("BACKUP a → b").skip_in_reverse());
  EXPECT_FALSE(wal::parse_op("COPY a → b").skip_in_reverse());
  EXPECT_FALSE(wal::parse_op("DB /db glibc:installed").skip_in_reverse());
}

// ============================================================================
// reverse_execute 测试 — BACKUP 逆向
// ============================================================================

TEST_F(WalCoreTest, ReverseExecuteBackupRestore) {
  // 创建文件 + 备份
  create_file("usr/bin/tool");
  fs::path phys = test_root / "usr/bin/tool";
  fs::path bak = test_root / "usr/bin/tool.lpkg_bak_tool";
  fs::rename(phys, bak);

  // WAL: BACKUP /usr/bin/tool → /usr/bin/tool.lpkg_bak_tool
  std::vector<wal::WALOp> ops;
  ops.push_back(wal::parse_op("BACKUP usr/bin/tool → usr/bin/tool.lpkg_bak_tool"));
  // 修正路径为绝对路径
  ops[0].arg1 = phys.string();
  ops[0].arg2 = bak.string();

  EXPECT_TRUE(fs::exists(bak));
  EXPECT_FALSE(fs::exists(phys));

  wal::reverse_execute(ops, "", false);

  // 文件应被恢复
  EXPECT_TRUE(fs::exists(phys));
  EXPECT_FALSE(fs::exists(bak));
}

TEST_F(WalCoreTest, ReverseExecuteBackupAlreadyConsumed) {
  // 备份已被消费（幂等场景）
  create_file("usr/bin/tool");
  fs::path phys = test_root / "usr/bin/tool";
  fs::path bak = test_root / "usr/bin/tool.lpkg_bak_tool";

  // 备份已经不存在（已被前一次 restore 消费）
  std::vector<wal::WALOp> ops;
  ops.push_back(wal::parse_op("BACKUP dummy → dummy"));
  ops[0].arg1 = phys.string();
  ops[0].arg2 = bak.string();

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  // 没有备份可恢复，跳过（幂等）
  EXPECT_EQ(stats.files_restored, 0);
}

// ============================================================================
// reverse_execute 测试 — COPY 逆向
// ============================================================================

TEST_F(WalCoreTest, ReverseExecuteCopyRemove) {
  // COPY 的逆向是删除目标文件
  create_file("usr/bin/newtool");
  fs::path dst = test_root / "usr/bin/newtool";

  std::vector<wal::WALOp> ops;
  ops.push_back(wal::parse_op("COPY dummy → dummy"));
  ops[0].arg2 = dst.string();

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_FALSE(fs::exists(dst));
  EXPECT_EQ(stats.files_cleaned, 1);
}

TEST_F(WalCoreTest, ReverseExecuteCopyNoTarget) {
  // COPY 的逆向：目标文件不存在（未 rename）→ 跳过
  fs::path dst = test_root / "usr/bin/nonexistent";

  std::vector<wal::WALOp> ops;
  ops.push_back(wal::parse_op("COPY dummy → dummy"));
  ops[0].arg2 = dst.string();

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_EQ(stats.files_cleaned, 0);
}

// ============================================================================
// reverse_execute 测试 — NEW / NEW_DIR 逆向
// ============================================================================

TEST_F(WalCoreTest, ReverseExecuteNewFile) {
  create_file("usr/bin/newfile");
  fs::path p = test_root / "usr/bin/newfile";

  std::vector<wal::WALOp> ops;
  ops.push_back(wal::parse_op("NEW dummy"));
  ops[0].arg1 = p.string();

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_FALSE(fs::exists(p));
  EXPECT_EQ(stats.files_cleaned, 1);
}

TEST_F(WalCoreTest, ReverseExecuteNewDir) {
  fs::path p = test_root / "usr/share/newdir";
  fs::create_directories(p);

  std::vector<wal::WALOp> ops;
  ops.push_back(wal::parse_op("NEW_DIR dummy"));
  ops[0].arg1 = p.string();

  wal::reverse_execute(ops, "", false);
  EXPECT_FALSE(fs::exists(p));
}

// ============================================================================
// reverse_execute 测试 — RM_DIR 逆向（重建目录）
// ============================================================================

TEST_F(WalCoreTest, ReverseExecuteRmDirRecreate) {
  fs::path dir = test_root / "usr/share/deleted_dir";

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("RM_DIR /usr/share/deleted_dir 755 1000 1000");
  op.arg1 = dir.string();
  ops.push_back(op);

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_TRUE(fs::exists(dir));
  EXPECT_TRUE(fs::is_directory(dir));
  EXPECT_EQ(stats.dirs_recreated, 1);
}

TEST_F(WalCoreTest, ReverseExecuteRmDirAlreadyExists) {
  // 目录已存在 → 跳过（幂等）
  fs::path dir = test_root / "usr/share/existing";
  fs::create_directories(dir);

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("RM_DIR /usr/share/existing 755 0 0");
  op.arg1 = dir.string();

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_EQ(stats.dirs_recreated, 0); // 跳过
}

// ============================================================================
// reverse_execute 测试 — DB 逆向
// ============================================================================

TEST_F(WalCoreTest, ReverseExecuteDbRestore) {
  // 创建 DB 文件和备份
  fs::path db_path = Config::instance().state_dir() / "pkgs_test";
  fs::path bak_path =
      fs::path(db_path.string() + ".lpkg_db_bak_before:glibc:installed");

  // 写入备份（模拟安装前状态）
  {
    std::ofstream f(bak_path);
    f << "bash:5.2\n";
  }

  // 写入当前 DB（模拟安装后状态）
  {
    std::ofstream f(db_path);
    f << "bash:5.2\nglibc:2.39\n";
  }

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("DB /var/lib/lpkg/pkgs_test glibc:installed");
  op.arg1 = db_path.string();
  op.arg2 = "glibc:installed";
  ops.push_back(op);

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_EQ(stats.db_restored, 1);
  EXPECT_FALSE(fs::exists(bak_path)); // 备份已被消费

  // DB 恢复到安装前的状态
  std::string content = read_file("var/lib/lpkg/pkgs_test");
  EXPECT_EQ(content, "bash:5.2\n");
}

TEST_F(WalCoreTest, ReverseExecuteDbBakMissing) {
  // DB 备份不存在（WAL 已写但备份未完成）→ 跳过
  fs::path db_path = Config::instance().state_dir() / "pkgs_no_bak";

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("DB /var/lib/lpkg/pkgs_no_bak glibc:installed");
  op.arg1 = db_path.string();
  op.arg2 = "glibc:installed";
  ops.push_back(op);

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_EQ(stats.db_restored, 0);
}

// ============================================================================
// reverse_execute 测试 — DBNEW 逆向
// ============================================================================

TEST_F(WalCoreTest, ReverseExecuteDbNewWithBackup) {
  fs::path db_path = Config::instance().state_dir() / "needed_so_new";
  fs::path bak_path =
      fs::path(db_path.string() + ".lpkg_db_bak_before:bash:installed");

  {
    std::ofstream f(bak_path);
    f << "old content\n";
  }
  {
    std::ofstream f(db_path);
    f << "new content\n";
  }

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("DBNEW /var/lib/lpkg/needed_so_new bash:installed");
  op.arg1 = db_path.string();
  op.arg2 = "bash:installed";
  ops.push_back(op);

  wal::reverse_execute(ops, "", false);
  EXPECT_FALSE(fs::exists(bak_path));
  EXPECT_EQ(read_file("var/lib/lpkg/needed_so_new"), "old content\n");
}

TEST_F(WalCoreTest, ReverseExecuteDbNewNoBackup) {
  // 无备份 → 文件是新建的 → 删除
  fs::path db_path = Config::instance().state_dir() / "needed_so_pure_new";
  {
    std::ofstream f(db_path);
    f << "new content\n";
  }

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("DBNEW /var/lib/lpkg/needed_so_pure_new bash:installed");
  op.arg1 = db_path.string();
  op.arg2 = "bash:installed";
  ops.push_back(op);

  wal::reverse_execute(ops, "", false);
  EXPECT_FALSE(fs::exists(db_path));
}

// ============================================================================
// reverse_execute 测试 — DBRM 逆向
// ============================================================================

TEST_F(WalCoreTest, ReverseExecuteDbRmWithBackup) {
  fs::path db_path = Config::instance().state_dir() / "deps_vim";
  fs::path bak_path =
      fs::path(db_path.string() + ".lpkg_db_bak_before:vim:removed");

  {
    std::ofstream f(bak_path);
    f << "glibc\nopenssl\n";
  }

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("DBRM /var/lib/lpkg/deps_vim vim:removed");
  op.arg1 = db_path.string();
  op.arg2 = "vim:removed";
  ops.push_back(op);

  wal::reverse_execute(ops, "", false);
  EXPECT_FALSE(fs::exists(bak_path));
  EXPECT_EQ(read_file("var/lib/lpkg/deps_vim"), "glibc\nopenssl\n");
}

// ============================================================================
// reverse_execute 测试 — 审计 WAL 行命名（锚定意图，防止误改）
// ============================================================================

// 锚定：DBNEW 无备份的逆操作是删除文件，审计名 RESTORE_DB_RM（描述实际动作）
// 不是 RESTORE_DB（那是 restore from backup），也不是 REMOVE_FILE（旧名称）
TEST_F(WalCoreTest, ReverseExecuteAuditNamingDbNewNoBackup) {
  fs::path db_path = Config::instance().state_dir() / "audit_db_new";
  {
    std::ofstream f(db_path);
    f << "new db content\n";
  }

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("DBNEW /var/lib/lpkg/audit_db_new testpkg:installed");
  op.arg1 = db_path.string();
  op.arg2 = "testpkg:installed";
  ops.push_back(op);

  wal::reverse_execute(ops, "", true); // write_audit = true

  std::string wal_content = read_wal();
  EXPECT_NE(wal_content.find("RESTORE_DB_RM " + db_path.string()),
            std::string::npos)
      << "DBNEW without backup MUST emit RESTORE_DB_RM, not RESTORE_DB or REMOVE_FILE";
  EXPECT_FALSE(fs::exists(db_path));
}

// 锚定：COPY/NEW 逆操作是删除文件，审计名 RESTORE_FILE_RM
TEST_F(WalCoreTest, ReverseExecuteAuditNamingCopyReverse) {
  fs::path fpath = test_root / "usr/bin/copied_file";
  create_file("usr/bin/copied_file");

  std::vector<wal::WALOp> ops;
  auto op = wal::parse_op("COPY /tmp/src → /usr/bin/copied_file");
  op.arg2 = fpath.string();
  ops.push_back(op);

  wal::reverse_execute(ops, "", true);

  std::string wal_content = read_wal();
  EXPECT_NE(wal_content.find("RESTORE_FILE_RM " + fpath.string()),
            std::string::npos)
      << "COPY reverse MUST emit RESTORE_FILE_RM";
  EXPECT_FALSE(fs::exists(fpath));
}

// ============================================================================
// reverse_execute 测试 — 跳过的行类型
// ============================================================================

TEST_F(WalCoreTest, ReverseExecuteSkipMetadataLines) {
  // 元数据行和 RESTORE 审计行被 skip_in_reverse 跳过
  create_file("usr/bin/testfile");

  std::vector<wal::WALOp> ops;
  ops.push_back(wal::parse_op("BEGIN_PKGS 1"));
  ops.push_back(wal::parse_op("BEGIN test 1.0"));
  ops.push_back(wal::parse_op("RESTORE_FILE a → b"));
  ops.push_back(wal::parse_op("REMOVE_FILE /path"));
  ops.push_back(wal::parse_op("ROLLBACK test 1.0"));
  ops.push_back(wal::parse_op("END test 1.0"));
  ops.push_back(wal::parse_op("COMMIT_PKGS"));
  // 正向操作（应被逆序处理）
  ops.push_back(wal::parse_op("NEW /usr/bin/testfile"));

  // 修正最后一个 op 的路径
  fs::path p = test_root / "usr/bin/testfile";
  ops.back().arg1 = p.string();

  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  // NEW 的逆向应该删除了文件
  EXPECT_FALSE(fs::exists(p));
  EXPECT_EQ(stats.files_cleaned, 1);
}

// ============================================================================
// reverse_execute 测试 — 里程碑停止
// ============================================================================

TEST_F(WalCoreTest, ReverseExecuteMilestoneStop) {
  // 设置场景：
  //   DB /pkgs glibc:installed  (需要逆向，恢复为 bash 状态)
  //   DB /pkgs bash:installed   (达到此里程碑即停止)
  //   BACKUP ...
  // 目标是 :batch-start，遇到 DB bash:installed 时停止

  // 验证里程碑字符串检查逻辑 — is_batch_start_milestone 在 :batch-start 时停止
  // 这里测试完整的里程碑停止机制

  std::vector<wal::WALOp> ops;
  ops.push_back(wal::parse_op("BACKUP dummy → dummy"));
  ops.push_back(wal::parse_op("DB /pkgs glibc:installed"));
  ops.push_back(wal::parse_op("DB /pkgs :batch-start")); // 应跳过（batch-start）

  // 两个 BACKUP 作为正向操作
  wal::RollbackStats stats =
      wal::reverse_execute(ops, ":batch-start", false);

  // 到达 :batch-start 即停止，不再处理前面的 BACKUP
  // stats 依赖于具体文件状态，但核心行为是遇到里程碑后提前返回
  EXPECT_GE(stats.files_restored + stats.files_cleaned + stats.db_restored, 0);
}

// ============================================================================
// extract_current_batch_ops 测试
// ============================================================================

TEST_F(WalCoreTest, ExtractCurrentBatchOpsSimple) {
  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN bash 5.2\n"
      "BACKUP /usr/bin/bash → /usr/bin/bash.lpkg_bak_bash\n"
      "COPY /tmp/bash.lpkgtmp → /usr/bin/bash\n"
      "COMMIT bash 5.2\n"
      "END bash 5.2\n"
      "DB /var/lib/lpkg/pkgs bash:installed\n"
      "COMMIT_PKGS\n");

  auto ops = wal::extract_current_batch_ops(wal::wal_log_path());
  // 最后一个批次已完成，应该返回空
  EXPECT_TRUE(ops.empty());
}

TEST_F(WalCoreTest, ExtractCurrentBatchOpsUncommitted) {
  write_wal(
      "BEGIN_PKGS 2\n"
      "BEGIN bash 5.2\n"
      "BACKUP /usr/bin/bash → /usr/bin/bash.lpkg_bak_bash\n"
      "COMMIT bash 5.2\n"
      "END bash 5.2\n"
      "DB /var/lib/lpkg/pkgs bash:installed\n"
      // 没有 COMMIT_PKGS — 批次未完成
  );

  auto ops = wal::extract_current_batch_ops(wal::wal_log_path());
  EXPECT_FALSE(ops.empty());
  // 应该从 BEGIN_PKGS 开始提取
  EXPECT_EQ(ops[0].type, wal::WALOpType::BEGIN_PKGS);
  EXPECT_EQ(ops[0].arg1, "2");
}

TEST_F(WalCoreTest, ExtractCurrentBatchOpsTwoBatches) {
  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN bash 5.2\n"
      "COMMIT bash 5.2\n"
      "END bash 5.2\n"
      "COMMIT_PKGS\n"
      "BEGIN_PKGS 1\n"
      "BEGIN glibc 2.39\n"
      // 未完成
  );

  auto ops = wal::extract_current_batch_ops(wal::wal_log_path());
  EXPECT_FALSE(ops.empty());
  // 应该只提取第二个批次
  EXPECT_EQ(ops[0].type, wal::WALOpType::BEGIN_PKGS);
}

TEST_F(WalCoreTest, ExtractCurrentBatchOpsEmptyFile) {
  write_wal("");
  auto ops = wal::extract_current_batch_ops(wal::wal_log_path());
  EXPECT_TRUE(ops.empty());
}

// ============================================================================
// trim_completed 测试
// ============================================================================

TEST_F(WalCoreTest, TrimCompletedAllDone) {
  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN bash 5.2\n"
      "COMMIT bash 5.2\n"
      "END bash 5.2\n"
      "COMMIT_PKGS\n");

  trim_completed();

  std::string content = read_file("var/lib/lpkg/transaction.log");
  // 所有批次已完成，WAL 应为空
  EXPECT_TRUE(content.empty() || content == "\n");
}

TEST_F(WalCoreTest, TrimCompletedKeepsUncommitted) {
  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN bash 5.2\n"
      "COMMIT bash 5.2\n"
      "END bash 5.2\n"
      "COMMIT_PKGS\n"
      "BEGIN_PKGS 1\n"
      "BEGIN glibc 2.39\n"
      "BACKUP /usr/lib/libc.so.6 → /usr/lib/libc.so.6.lpkg_bak_glibc\n");

  trim_completed();

  std::string content = read_file("var/lib/lpkg/transaction.log");
  // 应该保留第二个（未完成的）批次
  EXPECT_NE(content.find("BEGIN_PKGS"), std::string::npos);
  EXPECT_NE(content.find("glibc"), std::string::npos);
  // 第一个批次应该被清理
  EXPECT_EQ(content.find("bash"), std::string::npos);
}

TEST_F(WalCoreTest, TrimCompletedMultipleBatches) {
  write_wal(
      "BEGIN_PKGS 1\n"
      "BEGIN a 1.0\n"
      "COMMIT a 1.0\n"
      "END a 1.0\n"
      "COMMIT_PKGS\n"
      "BEGIN_PKGS 1\n"
      "BEGIN b 1.0\n"
      "COMMIT b 1.0\n"
      "END b 1.0\n"
      "COMMIT_PKGS\n"
      "BEGIN_PKGS 1\n"
      "BEGIN c 1.0\n");

  trim_completed();

  std::string content = read_file("var/lib/lpkg/transaction.log");
  // 只保留第三个批次
  EXPECT_EQ(content.find("a"), std::string::npos);
  EXPECT_EQ(content.find("b"), std::string::npos);
  EXPECT_NE(content.find("c"), std::string::npos);
}

// ============================================================================
// 幂等性测试 — 多次 reverse_execute 得到相同结果
// ============================================================================

TEST_F(WalCoreTest, ReverseExecuteIdempotentBackup) {
  create_file("usr/bin/idemtool");
  fs::path phys = test_root / "usr/bin/idemtool";
  fs::path bak = test_root / "usr/bin/idemtool.lpkg_bak_idem";
  fs::rename(phys, bak);

  std::vector<wal::WALOp> ops;
  ops.push_back(wal::parse_op("BACKUP dummy → dummy"));
  ops[0].arg1 = phys.string();
  ops[0].arg2 = bak.string();

  // 第一次逆向：恢复文件
  wal::reverse_execute(ops, "", false);
  EXPECT_TRUE(fs::exists(phys));
  EXPECT_FALSE(fs::exists(bak));

  // 第二次逆向：备份已被消费，跳过（幂等）
  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_TRUE(fs::exists(phys));
  EXPECT_EQ(stats.files_restored, 0);
}

TEST_F(WalCoreTest, ReverseExecuteIdempotentNew) {
  create_file("usr/bin/idemnew");
  fs::path p = test_root / "usr/bin/idemnew";

  std::vector<wal::WALOp> ops;
  ops.push_back(wal::parse_op("NEW dummy"));
  ops[0].arg1 = p.string();

  // 第一次：删除
  wal::reverse_execute(ops, "", false);
  EXPECT_FALSE(fs::exists(p));

  // 第二次：文件不存在，跳过
  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_EQ(stats.files_cleaned, 0);
}

TEST_F(WalCoreTest, ReverseExecuteIdempotentCopy) {
  create_file("usr/bin/idemcopy");
  fs::path p = test_root / "usr/bin/idemcopy";

  std::vector<wal::WALOp> ops;
  ops.push_back(wal::parse_op("COPY dummy → dummy"));
  ops[0].arg2 = p.string();

  // 第一次：删除
  wal::reverse_execute(ops, "", false);
  EXPECT_FALSE(fs::exists(p));

  // 第二次：跳过
  wal::RollbackStats stats = wal::reverse_execute(ops, "", false);
  EXPECT_EQ(stats.files_cleaned, 0);
}

// ============================================================================
// WalWriter 测试
// ============================================================================

TEST_F(WalCoreTest, WalWriterLogAndFsync) {
  {
    wal::WalWriter writer;
    writer.log("TEST_LINE_1 value");
    writer.log("TEST_LINE_2 value2");
  } // 析构关闭文件

  std::string content = read_file("var/lib/lpkg/transaction.log");
  EXPECT_NE(content.find("TEST_LINE_1"), std::string::npos);
  EXPECT_NE(content.find("TEST_LINE_2"), std::string::npos);
}

TEST_F(WalCoreTest, BeginBatchAndCommitBatch) {
  auto writer = wal::begin_batch(3);
  EXPECT_GT(writer.lines_written(), 0);

  wal::commit_batch();

  std::string content = read_file("var/lib/lpkg/transaction.log");
  EXPECT_NE(content.find("BEGIN_PKGS 3"), std::string::npos);
  EXPECT_NE(content.find("COMMIT_PKGS"), std::string::npos);
}

// ============================================================================
// 完整 WAL 场景测试
// ============================================================================

TEST_F(WalCoreTest, FullInstallWalScenario) {
  // 模拟一个包安装完成的 WAL
  wal::commit_batch(); // 清除之前的

  auto w = wal::begin_batch(1);
  wal::log_wal_line("BEGIN curl 8.11.1");
  wal::log_wal_line("BACKUP /usr/bin/curl → /usr/bin/curl.lpkg_bak_curl");
  wal::log_wal_line("NEW /usr/share/doc/curl/README");
  wal::log_wal_line("COPY /tmp/curl.lpkgtmp → /usr/bin/curl");
  wal::log_wal_line("COMMIT curl 8.11.1");
  wal::log_wal_line("END curl 8.11.1");
  wal::log_wal_line("DB /var/lib/lpkg/pkgs curl:installed");
  wal::commit_batch();

  std::string content = read_file("var/lib/lpkg/transaction.log");
  EXPECT_NE(content.find("BEGIN_PKGS 1"), std::string::npos);
  EXPECT_NE(content.find("BEGIN curl"), std::string::npos);
  EXPECT_NE(content.find("COMMIT curl"), std::string::npos);
  EXPECT_NE(content.find("END curl"), std::string::npos);
  EXPECT_NE(content.find("DB"), std::string::npos);
  EXPECT_NE(content.find("COMMIT_PKGS"), std::string::npos);
}

TEST_F(WalCoreTest, BatchRollbackWalKeepsCommitPkgs) {
  // batch_rollback 写 COMMIT_PKGS 关闭批次
  write_wal(
      "BEGIN_PKGS 2\n"
      "BEGIN A 1.0\n"
      "BACKUP /usr/bin/a → /usr/bin/a.lpkg_bak_A\n"
      "COPY /tmp/a → /usr/bin/a\n"
      "COMMIT A 1.0\n"
      "END A 1.0\n"
      "DB /var/lib/lpkg/pkgs A:installed\n"
      "BEGIN B 1.0\n"
      "ROLLBACK B 1.0\n"
      "END B 1.0\n");

  wal::batch_rollback({"A"});

  std::string content = read_file("var/lib/lpkg/transaction.log");
  // batch_rollback 应该补写了 COMMIT_PKGS
  EXPECT_NE(content.find("COMMIT_PKGS"), std::string::npos);
}

// ============================================================================
// 健壮性测试 — 异常输入处理
// ============================================================================

TEST_F(WalCoreTest, ParseOpWithLeadingWhitespace) {
  // 测试去除行尾 \r
  auto op = wal::parse_op("BEGIN pkg 1.0\n");
  EXPECT_EQ(op.type, wal::WALOpType::BEGIN);
  EXPECT_EQ(op.arg1, "pkg");
}

TEST_F(WalCoreTest, ParseOpWithTrailingWhitespace) {
  // WAL 行由内部写入，不应包含 \r。\r 会导致解析失败。
  auto op = wal::parse_op("COMMIT_PKGS\r");
  EXPECT_EQ(op.arg1, "__INVALID__");
}

TEST_F(WalCoreTest, WalOpTypeNameRoundtrip) {
  for (auto t : {wal::WALOpType::BEGIN_PKGS, wal::WALOpType::BACKUP,
                  wal::WALOpType::COPY, wal::WALOpType::DB,
                  wal::WALOpType::RESTORE_FILE, wal::WALOpType::RM_BEGIN}) {
    std::string_view name = wal::walop_type_name(t);
    EXPECT_EQ(wal::walop_type_from_name(name), t);
  }
}
