/**
 * test_db_wal.cpp
 *
 * 数据库 WAL 事务保护测试。
 *
 * 测试 Cache::write() 产生的 WAL 条目在 recovery 中的回滚语义。
 * WAL 格式（三个条目，每个带 /path 和 tag）：
 *   DB /path tag     — 修改已有 DB 文件（备份到 .lpkg_db_bak_<tag> 后写新内容）
 *   DBNEW /path tag  — 新建 DB 文件
 *   DBRM /path tag   — 删除 DB 文件（先备份到 .lpkg_db_bak_<tag>）
 *
 * 每个 .lpkg_db_bak_<tag> 与 WAL 条目精确对应，tag 通常为包名。
 */

#include <gtest/gtest.h>
#include "../../main/src/pkg/transaction_log.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/base/constants.hpp"
#include "../test_base.hpp"

#include <filesystem>
#include <fstream>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

class DbWalTest : public IntegrationTestBase {
protected:
    fs::path pkgs_path;

    void SetUp() override {
        IntegrationTestBase::SetUp();
        pkgs_path = Config::instance().pkgs_file();
        write_pkgs({"old-pkg:1.0"});
        Cache::instance().load();
    }

    void write_pkgs(const std::vector<std::string>& entries) {
        std::ofstream f(pkgs_path, std::ios::trunc);
        for (const auto& e : entries) f << e << "\n";
    }

    std::vector<std::string> read_pkgs() {
        std::ifstream f(pkgs_path);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(f, line))
            if (!line.empty()) lines.push_back(line);
        std::sort(lines.begin(), lines.end());
        return lines;
    }

    fs::path db_bak(const fs::path& db_file, const std::string& tag) {
        return fs::path(db_file.string() + ".lpkg_db_bak_" + tag);
    }

    void raw_log(const std::string& line) {
        TransactionLog::log_raw(line);
    }
};


// ═════════════════════════════════════════════════════════════════════════
//  1. DB /path tag 后未 COMMIT → .lpkg_db_bak_<tag> 被恢复
// ═════════════════════════════════════════════════════════════════════════

TEST_F(DbWalTest, DbModifyRollbackRestoresFile) {
    ASSERT_EQ(read_pkgs(), std::vector<std::string>({"old-pkg:1.0"}));

    fs::path bak = db_bak(pkgs_path, "new-pkg");
    raw_log("BEGIN_PKGS 1");
raw_log("BEGIN new-pkg 2.0");
    fs::rename(pkgs_path, bak);
    write_pkgs({"old-pkg:1.0", "new-pkg:2.0"});
    raw_log("DB " + pkgs_path.string() + " new-pkg");
    // 没有 COMMIT

    ASSERT_TRUE(fs::exists(bak));
    ASSERT_TRUE(fs::exists(pkgs_path));

    recover_packages();

    EXPECT_EQ(read_pkgs(), std::vector<std::string>({"old-pkg:1.0"}));
    EXPECT_FALSE(fs::exists(bak));
}


// ═════════════════════════════════════════════════════════════════════════
//  2. DBNEW /path tag 后未 COMMIT → 文件被删除
// ═════════════════════════════════════════════════════════════════════════

TEST_F(DbWalTest, DbNewRollbackDeletesFile) {
    fs::path new_db = Config::instance().state_dir() / "test_new.db";
    ASSERT_FALSE(fs::exists(new_db));

    raw_log("BEGIN_PKGS 1");
raw_log("BEGIN newdb-pkg 1.0");
    { std::ofstream f(new_db); f << "new data"; }
    raw_log("DBNEW " + new_db.string() + " newdb-pkg");
    // 没有 COMMIT

    ASSERT_TRUE(fs::exists(new_db));
    recover_packages();
    EXPECT_FALSE(fs::exists(new_db));
}

TEST_F(DbWalTest, DbNewWithCommitPreservesFile) {
    fs::path new_db = Config::instance().state_dir() / "test_new_commit.db";
    ASSERT_FALSE(fs::exists(new_db));

    raw_log("BEGIN_PKGS 1");
raw_log("BEGIN newdb-pkg 1.0");
    { std::ofstream f(new_db); f << "new data"; }
    raw_log("DBNEW " + new_db.string() + " newdb-pkg");
    raw_log("COMMIT newdb-pkg 1.0");
    raw_log("END newdb-pkg 1.0");
    raw_log("COMMIT_PKGS");

    recover_packages();

    EXPECT_TRUE(fs::exists(new_db));
    { std::ifstream f(new_db); std::string c; std::getline(f, c); EXPECT_EQ(c, "new data"); }
    fs::remove(new_db);
}


// ═════════════════════════════════════════════════════════════════════════
//  3. DBRM /path tag 后未 COMMIT → .lpkg_db_bak_<tag> 被恢复
// ═════════════════════════════════════════════════════════════════════════

TEST_F(DbWalTest, DbRmRollbackRestoresFile) {
    fs::path bak = db_bak(pkgs_path, "rm-pkg");

    raw_log("BEGIN_PKGS 1");
raw_log("BEGIN rm-pkg 1.0");
    fs::rename(pkgs_path, bak);
    raw_log("DBRM " + pkgs_path.string() + " rm-pkg");
    // 没有 COMMIT

    ASSERT_FALSE(fs::exists(pkgs_path));
    ASSERT_TRUE(fs::exists(bak));

    recover_packages();

    EXPECT_TRUE(fs::exists(pkgs_path));
    EXPECT_EQ(read_pkgs(), std::vector<std::string>({"old-pkg:1.0"}));
    EXPECT_FALSE(fs::exists(bak));
}


// ═════════════════════════════════════════════════════════════════════════
//  4. 完整 COMMIT 事务：文件 + DB 操作全部保留
// ═════════════════════════════════════════════════════════════════════════

TEST_F(DbWalTest, FullCommitPreservesDbAndFileState) {
    fs::path bak = db_bak(pkgs_path, "commit-pkg");
    fs::path test_file = test_root / "usr/bin/commit_test";
    fs::create_directories(test_file.parent_path());

    raw_log("BEGIN_PKGS 1");
raw_log("BEGIN commit-pkg 1.0");
    raw_log("NEW " + test_file.string());
    { std::ofstream f(test_file); f << "new file"; }

    fs::rename(pkgs_path, bak);
    write_pkgs({"old-pkg:1.0", "commit-pkg:1.0"});
    raw_log("DB " + pkgs_path.string() + " commit-pkg");

    raw_log("COMMIT commit-pkg 1.0");
    raw_log("END commit-pkg 1.0");
    raw_log("COMMIT_PKGS");

    recover_packages();

    auto pkgs = read_pkgs();
    EXPECT_NE(std::find(pkgs.begin(), pkgs.end(), "commit-pkg:1.0"), pkgs.end());
    EXPECT_TRUE(fs::exists(test_file));
    EXPECT_FALSE(fs::exists(bak));
}


// ═════════════════════════════════════════════════════════════════════════
//  5. 无 COMMIT → 文件 + DB 全部回滚
// ═════════════════════════════════════════════════════════════════════════

TEST_F(DbWalTest, NoCommitRollsBackBothFilesAndDb) {
    fs::path bak = db_bak(pkgs_path, "roll-pkg");
    fs::path test_file = test_root / "usr/bin/roll_test";
    fs::create_directories(test_file.parent_path());

    raw_log("BEGIN_PKGS 1");
raw_log("BEGIN roll-pkg 1.0");
    raw_log("NEW " + test_file.string());
    { std::ofstream f(test_file); f << "orphan"; }

    fs::rename(pkgs_path, bak);
    write_pkgs({"old-pkg:1.0", "roll-pkg:1.0"});
    raw_log("DB " + pkgs_path.string() + " roll-pkg");
    // 没有 COMMIT

    recover_packages();

    EXPECT_EQ(read_pkgs(), std::vector<std::string>({"old-pkg:1.0"}));
    EXPECT_FALSE(fs::exists(test_file));
    EXPECT_FALSE(fs::exists(bak));
}


// ═════════════════════════════════════════════════════════════════════════
//  6. 批量事务：每个包有独立 .lpkg_db_bak_<tag>，整体无 COMMIT_PKGS → 全回滚
// ═════════════════════════════════════════════════════════════════════════

TEST_F(DbWalTest, BatchNoCommitRollsBackAllDbOps) {
    fs::path bak_a = db_bak(pkgs_path, "batch-a");
    fs::path bak_b = db_bak(pkgs_path, "batch-b");

    raw_log("BEGIN_PKGS 2");

    // 包 A
    raw_log("BEGIN batch-a 1.0");
    fs::rename(pkgs_path, bak_a);
    write_pkgs({"old-pkg:1.0", "batch-a:1.0"});
    raw_log("DB " + pkgs_path.string() + " batch-a");
    raw_log("COMMIT batch-a 1.0");
    raw_log("END batch-a 1.0");

    // 包 B（无 COMMIT，触发整体回滚）
    raw_log("BEGIN batch-b 1.0");
    fs::rename(pkgs_path, bak_b);
    write_pkgs({"old-pkg:1.0", "batch-b:1.0"});
    raw_log("DB " + pkgs_path.string() + " batch-b");
    // 没有 COMMIT 和 COMMIT_PKGS

    recover_packages();

    // 两个备份应各自恢复，最终回到初始状态
    EXPECT_EQ(read_pkgs(), std::vector<std::string>({"old-pkg:1.0"}));
    EXPECT_FALSE(fs::exists(bak_a));
    EXPECT_FALSE(fs::exists(bak_b));
}


// ═════════════════════════════════════════════════════════════════════════
//  7. 三个包批量修改同一 DB → 唯一 tag 确保每层备份可独立恢复
// ═════════════════════════════════════════════════════════════════════════

TEST_F(DbWalTest, BatchThreePackagesSameDbReturnsToOriginal) {
    // 三个包依次修改 pkgs，每个有自己的 .lpkg_db_bak_<tag>
    // 反向恢复时应逐层还原，最终回到 batch 前的状态
    fs::path bak_a = db_bak(pkgs_path, "pkg-a");
    fs::path bak_b = db_bak(pkgs_path, "pkg-b");
    fs::path bak_c = db_bak(pkgs_path, "pkg-c");

    raw_log("BEGIN_PKGS 3");

    // 初始 {old-pkg} → 加 pkg-a
    raw_log("BEGIN pkg-a 1.0");
    fs::rename(pkgs_path, bak_a);
    write_pkgs({"old-pkg:1.0", "pkg-a:1.0"});
    raw_log("DB " + pkgs_path.string() + " pkg-a");
    raw_log("COMMIT pkg-a 1.0");
    raw_log("END pkg-a 1.0");

    // {old, pkg-a} → 加 pkg-b
    raw_log("BEGIN pkg-b 1.0");
    fs::rename(pkgs_path, bak_b);
    write_pkgs({"old-pkg:1.0", "pkg-a:1.0", "pkg-b:1.0"});
    raw_log("DB " + pkgs_path.string() + " pkg-b");
    raw_log("COMMIT pkg-b 1.0");
    raw_log("END pkg-b 1.0");

    // {old, pkg-a, pkg-b} → 加 pkg-c（无 COMMIT）
    raw_log("BEGIN pkg-c 1.0");
    fs::rename(pkgs_path, bak_c);
    write_pkgs({"old-pkg:1.0", "pkg-a:1.0", "pkg-b:1.0", "pkg-c:1.0"});
    raw_log("DB " + pkgs_path.string() + " pkg-c");
    // pkg-c 无 COMMIT + 无 COMMIT_PKGS

    recover_packages();

    EXPECT_EQ(read_pkgs(), std::vector<std::string>({"old-pkg:1.0"}));
    EXPECT_FALSE(fs::exists(bak_a));
    EXPECT_FALSE(fs::exists(bak_b));
    EXPECT_FALSE(fs::exists(bak_c));
}


// ═════════════════════════════════════════════════════════════════════════
//  8. DB 条目但 .lpkg_db_bak_<tag> 缺失（备份前即 crash）→ 跳过
// ═════════════════════════════════════════════════════════════════════════

TEST_F(DbWalTest, DbEntryNoBakDoesNothing) {
    raw_log("BEGIN_PKGS 1");
raw_log("BEGIN ghost 1.0");
    // WAL 条目已写但 .bak 从未创建（crash 在 rename 之前）
    raw_log("DB " + pkgs_path.string() + " ghost");
    // 没有 COMMIT

    ASSERT_EQ(read_pkgs(), std::vector<std::string>({"old-pkg:1.0"}));
    recover_packages();
    EXPECT_EQ(read_pkgs(), std::vector<std::string>({"old-pkg:1.0"}));
}


// ═════════════════════════════════════════════════════════════════════════
//  9. 多 DB 文件同时修改后回滚（各文件有独立 tag）
// ═════════════════════════════════════════════════════════════════════════

TEST_F(DbWalTest, MultipleDbFilesRollback) {
    fs::path bak_pkgs = db_bak(pkgs_path, "multi-pkg");
    fs::path bak_provides = db_bak(Config::instance().provides_db(), "multi-pkg");
    fs::path test_file = test_root / "usr/share/multi_test";
    fs::create_directories(test_file.parent_path());
    Config::instance().init_filesystem();

    raw_log("BEGIN_PKGS 1");
raw_log("BEGIN multi-pkg 1.0");
    raw_log("NEW " + test_file.string());
    { std::ofstream f(test_file); f << "multi test"; }

    fs::rename(pkgs_path, bak_pkgs);
    write_pkgs({"old-pkg:1.0", "multi-pkg:1.0"});
    raw_log("DB " + pkgs_path.string() + " multi-pkg");

    fs::rename(Config::instance().provides_db(), bak_provides);
    { std::ofstream f(Config::instance().provides_db()); f << "libx.so.1\tmulti-pkg"; }
    raw_log("DB " + Config::instance().provides_db().string() + " multi-pkg");

    // 没有 COMMIT

    recover_packages();

    EXPECT_EQ(read_pkgs(), std::vector<std::string>({"old-pkg:1.0"}));
    EXPECT_TRUE(fs::exists(Config::instance().provides_db()));
    EXPECT_FALSE(fs::exists(test_file));
    EXPECT_FALSE(fs::exists(bak_pkgs));
    EXPECT_FALSE(fs::exists(bak_provides));
}


// ═════════════════════════════════════════════════════════════════════════
//  10. 移除事务 + DB 回滚
// ═════════════════════════════════════════════════════════════════════════

TEST_F(DbWalTest, RemoveTransactionRollbackDb) {
    write_pkgs({"old-pkg:1.0", "remove-me:1.0"});
    Cache::instance().load();

    fs::path bak = db_bak(pkgs_path, "remove-me");

    raw_log("BEGIN_PKGS 1");
raw_log("RM_BEGIN remove-me 1.0");
    fs::rename(pkgs_path, bak);
    write_pkgs({"old-pkg:1.0"});
    raw_log("DBRM " + pkgs_path.string() + " remove-me");
    // 没有 RM_COMMIT

    ASSERT_TRUE(fs::exists(bak));
    ASSERT_EQ(read_pkgs(), std::vector<std::string>({"old-pkg:1.0"}));

    recover_packages();

    auto pkgs = read_pkgs();
    EXPECT_NE(std::find(pkgs.begin(), pkgs.end(), "remove-me:1.0"), pkgs.end());
}
