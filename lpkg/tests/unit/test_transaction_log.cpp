#include <gtest/gtest.h>
#include "../../main/src/pkg/transaction_log.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/i18n/localization.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

class TransactionLogTest : public ::testing::Test {
protected:
    fs::path test_root;
    fs::path lock_dir;

    void SetUp() override {
        init_localization();
        test_root = fs::current_path() / "tmp_txnlog_test";
        fs::remove_all(test_root);
        fs::create_directories(test_root / "var" / "lpkg");
        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
    }

    void TearDown() override {
        fs::remove_all(test_root);
        Config::instance().set_root_path("/");
    }

    std::string read_log() {
        fs::path p = Config::instance().lock_dir() / "transaction.log";
        std::ifstream f(p);
        if (!f.is_open()) return "";
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    int count_lines_containing(const std::string& content, const std::string& needle) {
        int count = 0;
        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line))
            if (line.find(needle) != std::string::npos) count++;
        return count;
    }
};

// ── 1. BEGIN 写入日志 ──
TEST_F(TransactionLogTest, BeginWritesToLog) {
    {
        TransactionLog log;
        log.begin("test-pkg", "1.0");
    }
    auto content = read_log();
    EXPECT_TRUE(content.find("BEGIN test-pkg 1.0") != std::string::npos);
}

// ── 2. BEGIN + COMMIT + END 完整事务 ──
TEST_F(TransactionLogTest, BeginCommitEnd) {
    {
        TransactionLog log;
        log.begin("test-pkg", "1.0");
        log.commit("test-pkg", "1.0");
        log.end("test-pkg", "1.0");
    }
    auto c = read_log();
    EXPECT_TRUE(c.find("BEGIN test-pkg 1.0") != std::string::npos);
    EXPECT_TRUE(c.find("COMMIT test-pkg 1.0") != std::string::npos);
    EXPECT_TRUE(c.find("END test-pkg 1.0") != std::string::npos);
}

// ── 3. BEGIN + ROLLBACK + END ──
TEST_F(TransactionLogTest, BeginRollbackEnd) {
    {
        TransactionLog log;
        log.begin("test-pkg", "1.0");
        log.rollback("test-pkg", "1.0");
        log.end("test-pkg", "1.0");
    }
    auto c = read_log();
    EXPECT_TRUE(c.find("ROLLBACK test-pkg 1.0") != std::string::npos);
}

// ── 4. check_pending: 完整事务返回空 ──
TEST_F(TransactionLogTest, CheckPending_EmptyForComplete) {
    {
        TransactionLog log;
        log.begin("pkg-a", "2.0");
        log.commit("pkg-a", "2.0");
        log.end("pkg-a", "2.0");
    }
    EXPECT_TRUE(TransactionLog::check_pending().empty());
}

// ── 5. check_pending: 未完成事务返回包名 ──
TEST_F(TransactionLogTest, CheckPending_ReturnsPkgName) {
    // 统一模型：check_pending 只识别 BEGIN_PKGS/COMMIT_PKGS
    TransactionLog::log_raw("BEGIN_PKGS 1");
    {
        TransactionLog log;
        log.begin("unfinished-pkg", "3.0");
        // 没有 commit 和 end
    }
    // check_pending 依赖 Config::instance().lock_dir()
    std::string pending = TransactionLog::check_pending();
    EXPECT_FALSE(pending.empty());
}

// ── 6. check_pending: ROLLBACK + END 不算未完成 ──
TEST_F(TransactionLogTest, CheckPending_RollbackIsComplete) {
    {
        TransactionLog log;
        log.begin("rolled-pkg", "1.0");
        log.rollback("rolled-pkg", "1.0");
        log.end("rolled-pkg", "1.0");
    }
    EXPECT_TRUE(TransactionLog::check_pending().empty());
}

// ── 7. BACKUP 日志 ──
TEST_F(TransactionLogTest, BackupLogsPath) {
    {
        TransactionLog log;
        log.backup("/usr/lib/libfoo.so", "/usr/lib/libfoo.so.lpkg_bak_pkg");
    }
    auto c = read_log();
    EXPECT_TRUE(c.find("BACKUP /usr/lib/libfoo.so → /usr/lib/libfoo.so.lpkg_bak_pkg") != std::string::npos);
}

// ── 8. COPY 日志 ──
TEST_F(TransactionLogTest, CopyLogsPath) {
    {
        TransactionLog log;
        log.copy("/tmp/pkg.lpkgtmp", "/usr/bin/tool");
    }
    auto c = read_log();
    EXPECT_TRUE(c.find("COPY /tmp/pkg.lpkgtmp → /usr/bin/tool") != std::string::npos);
}

// ── 9. NEW 日志 ──
TEST_F(TransactionLogTest, NewLogsPath) {
    {
        TransactionLog log;
        log.new_file("/usr/share/new_file.dat");
    }
    auto c = read_log();
    EXPECT_TRUE(c.find("NEW /usr/share/new_file.dat") != std::string::npos);
}

// ── 10. log_raw ──
TEST_F(TransactionLogTest, LogRawWrites) {
    TransactionLog::log_raw("CUSTOM event");
    auto c = read_log();
    EXPECT_TRUE(c.find("CUSTOM event") != std::string::npos);
}

// ── 11. 多事务──
TEST_F(TransactionLogTest, MultipleTransactions) {
    {
        TransactionLog log;
        log.begin("pkg1", "1.0");
        log.commit("pkg1", "1.0");
        log.end("pkg1", "1.0");
    }
    {
        TransactionLog log;
        log.begin("pkg2", "2.0");
        log.commit("pkg2", "2.0");
        log.end("pkg2", "2.0");
    }
    auto c = read_log();
    EXPECT_EQ(count_lines_containing(c, "BEGIN pkg1"), 1);
    EXPECT_EQ(count_lines_containing(c, "BEGIN pkg2"), 1);
}

// ── 12. check_pending: 多事务仅最后未完成 ──
TEST_F(TransactionLogTest, CheckPending_OnlyLastUnfinished) {
    {
        TransactionLog log;
        log.begin("done", "1.0");
        log.commit("done", "1.0");
        log.end("done", "1.0");
    }
    TransactionLog::log_raw("BEGIN_PKGS 1");
    TransactionLog::log_raw("BEGIN broken 2.0");
    // check_pending 现在只检查 BEGIN_PKGS/COMMIT_PKGS
    std::string pending = TransactionLog::check_pending();
    EXPECT_FALSE(pending.empty());
    EXPECT_NE(pending.find("BEGIN_PKGS 1"), std::string::npos);
}

// ── 13. 无日志文件时 check_pending 返回空 ──
TEST_F(TransactionLogTest, CheckPending_NoFile) {
    EXPECT_TRUE(TransactionLog::check_pending().empty());
}

// ── 14. COPY 在 COMMIT 前 ──
TEST_F(TransactionLogTest, CopyBeforeCommit) {
    {
        TransactionLog log;
        log.begin("p", "1");
        log.copy("/src", "/dst");
        log.commit("p", "1");
        log.end("p", "1");
    }
    auto c = read_log();
    auto begin_pos = c.find("BEGIN p 1");
    auto copy_pos = c.find("COPY /src → /dst");
    auto commit_pos = c.find("COMMIT p 1");
    EXPECT_LT(begin_pos, copy_pos);
    EXPECT_LT(copy_pos, commit_pos);
}
