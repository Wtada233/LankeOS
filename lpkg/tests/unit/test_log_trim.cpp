#include <gtest/gtest.h>
#include "../../main/src/pkg/transaction_log.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/i18n/localization.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

/**
 * 测试套件：TransactionLog::trim_completed 日志压缩
 *
 * 压缩安全性的核心原则：
 *   1. 已完结的事务（有完整的 BEGIN→END 或 COMMIT_PKGS）→ 可删除
 *   2. 未完结的事务（缺少 END/RM_END/COMMIT_PKGS）→ 必须保留全部上下文
 *   3. 批量事务内部即使某个包已 COMMIT，只要外层无 COMMIT_PKGS → 整个批量保留
 *   4. 压缩替换使用 .tmp + rename 原子操作，失败时不损坏原文件
 */
class LogTrimTest : public ::testing::Test {
protected:
    fs::path test_root;
    fs::path log_path;

    void SetUp() override {
        init_localization();
        test_root = fs::current_path() / "tmp_trim_test";
        fs::remove_all(test_root);
        fs::create_directories(test_root / "var" / "lpkg");
        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
        log_path = Config::instance().lock_dir() / "transaction.log";
    }

    void TearDown() override {
        fs::remove_all(test_root);
        Config::instance().set_root_path("/");
    }

    /** 将 raw lines（不含时间戳）写入日志文件 */
    void write_log(const std::vector<std::string>& lines) {
        std::ofstream f(log_path, std::ios::trunc);
        for (const auto& line : lines)
            f << "[2000-01-01 00:00:00] " << line << "\n";
        f.close();
    }

    /** 写出带时间戳和箭头的行 */
    void write_log_raw(const std::vector<std::string>& lines) {
        std::ofstream f(log_path, std::ios::trunc);
        for (const auto& line : lines)
            f << line << "\n";
        f.close();
    }

    /** 读取日志内容（去掉时间戳，方便断言） */
    std::vector<std::string> read_log() {
        if (!fs::exists(log_path)) return {};
        std::ifstream f(log_path);
        std::vector<std::string> result;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            if (line.back() == '\r') line.pop_back();
            // 去掉时间戳 "[...] "
            auto ts_end = line.find(']');
            if (ts_end != std::string::npos && ts_end + 2 < line.size())
                result.push_back(line.substr(ts_end + 2));
            else
                result.push_back(line);
        }
        return result;
    }

    int count_lines() {
        if (!fs::exists(log_path)) return 0;
        std::ifstream f(log_path);
        std::string line;
        int n = 0;
        while (std::getline(f, line)) {
            if (!line.empty()) ++n;
        }
        return n;
    }
};

// ═══════════════════════════════════════════════════════════════════════
// 第 0 组：边界——空文件 / 不存在
// ═══════════════════════════════════════════════════════════════════════

TEST_F(LogTrimTest, NoLogFile) {
    fs::remove(log_path);
    EXPECT_NO_THROW(TransactionLog::trim_completed());
    EXPECT_FALSE(fs::exists(log_path)) << "trim on missing file creates nothing";
}

TEST_F(LogTrimTest, EmptyLog) {
    write_log({});
    EXPECT_NO_THROW(TransactionLog::trim_completed());
    EXPECT_TRUE(fs::exists(log_path));
    auto c = read_log();
    EXPECT_TRUE(c.empty()) << "empty log stays empty after trim";
}

TEST_F(LogTrimTest, AllWhitespaceLines) {
    write_log_raw({"  ", "\t", "\n", ""});
    EXPECT_NO_THROW(TransactionLog::trim_completed());
    // trim 会过滤空行，最终写出空文件
    auto c = read_log();
    EXPECT_TRUE(c.empty()) << "whitespace-only log becomes empty after trim";
}

// ═══════════════════════════════════════════════════════════════════════
// 第 1 组：单包安装事务 (INSTALL)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(LogTrimTest, CompleteInstallTxnCleared) {
    write_log({
        "BEGIN pkg-a 1.0",
        "BACKUP /usr/bin/a → /usr/bin/a.lpkg_bak_pkg-a",
        "COMMIT pkg-a 1.0",
        "END pkg-a 1.0",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    EXPECT_TRUE(c.empty()) << "complete install txn cleared";
}

TEST_F(LogTrimTest, IncompleteInstallTxnPreserved) {
    write_log({
        "BEGIN_PKGS 1",
        "BEGIN pkg-a 1.0",
        "BACKUP /usr/bin/a â /usr/bin/a.lpkg_bak_pkg-a",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    ASSERT_FALSE(c.empty()) << "incomplete txn preserved";
    EXPECT_TRUE(c.size() > 0 && c[0].find("BEGIN_PKGS") != std::string::npos);
    EXPECT_TRUE(c.size() > 1 && c[1].find("BEGIN pkg-a") != std::string::npos);
}

TEST_F(LogTrimTest, RollbackInstallCleared) {
    write_log({
        "BEGIN pkg-a 1.0",
        "BACKUP /usr/bin/a → /usr/bin/a.lpkg_bak_pkg-a",
        "ROLLBACK pkg-a 1.0",
        "END pkg-a 1.0",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    EXPECT_TRUE(c.empty()) << "rollback+end is also complete";
}

// ═══════════════════════════════════════════════════════════════════════
// 第 2 组：移除事务 (REMOVE)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(LogTrimTest, CompleteRemoveCleared) {
    write_log({
        "RM_BEGIN pkg-x 1.0",
        "BACKUP /usr/bin/x → /usr/bin/x.lpkg_bak_pkg-x",
        "RM_DIR /usr/bin/",
        "RM_COMMIT pkg-x 1.0",
        "RM_END pkg-x 1.0",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    EXPECT_TRUE(c.empty()) << "complete remove txn cleared";
}

TEST_F(LogTrimTest, IncompleteRemovePreserved) {
    write_log({
        "BEGIN_PKGS 1",
        "RM_BEGIN pkg-x 1.0",
        "BACKUP /usr/bin/x â /usr/bin/x.lpkg_bak_pkg-x",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c.size() > 0 && c[0].find("BEGIN_PKGS") != std::string::npos);
}

TEST_F(LogTrimTest, RemoveOnlyBeginPreserved) {
    write_log({
        "BEGIN_PKGS 1",
        "RM_BEGIN pkg-x 1.0",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c[0].find("BEGIN_PKGS") != std::string::npos)
        << "BEGIN_PKGS + RM_BEGIN preserved";
}

// ═══════════════════════════════════════════════════════════════════════
// 第 3 组：批量事务 (BATCH)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(LogTrimTest, CompleteBatchCleared) {
    write_log({
        "BEGIN_PKGS 2",
        "BEGIN pkg-a 1.0",
        "BACKUP /usr/bin/a → /usr/bin/a.lpkg_bak_pkg-a",
        "COMMIT pkg-a 1.0",
        "END pkg-a 1.0",
        "BEGIN pkg-b 1.0",
        "BACKUP /usr/bin/b → /usr/bin/b.lpkg_bak_pkg-b",
        "COMMIT pkg-b 1.0",
        "END pkg-b 1.0",
        "COMMIT_PKGS",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    EXPECT_TRUE(c.empty()) << "complete batch (with COMMIT_PKGS) cleared";
}

TEST_F(LogTrimTest, IncompleteBatchPreserved_NoCommitPkgs) {
    write_log({
        "BEGIN_PKGS 2",
        "BEGIN pkg-a 1.0",
        "BACKUP /usr/bin/a → /usr/bin/a.lpkg_bak_pkg-a",
        "COMMIT pkg-a 1.0",
        "END pkg-a 1.0",
        "BEGIN pkg-b 1.0",
        "BACKUP /usr/bin/b → /usr/bin/b.lpkg_bak_pkg-b",
        // 无 COMMIT pkg-b, 无 COMMIT_PKGS
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    // 整个批量必须保留——因为未提交的批量事务需要完整上下文来回滚
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c[0].find("BEGIN_PKGS 2") != std::string::npos)
        << "entire batch preserved when no COMMIT_PKGS";
}

TEST_F(LogTrimTest, IncompleteBatchPreserved_NoSecondBegin) {
    write_log({
        "BEGIN_PKGS 2",
        "BEGIN pkg-a 1.0",
        "BACKUP /usr/bin/a → /usr/bin/a.lpkg_bak_pkg-a",
        "COMMIT pkg-a 1.0",
        "END pkg-a 1.0",
        // pkg-b 从未开始——但批量未提交
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c[0].find("BEGIN_PKGS 2") != std::string::npos)
        << "batch with only partial internal work preserved";
}

TEST_F(LogTrimTest, EmptyBatchComplete) {
    write_log({
        "BEGIN_PKGS 0",
        "COMMIT_PKGS",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    EXPECT_TRUE(c.empty()) << "empty batch (0 pkgs) cleared";
}

TEST_F(LogTrimTest, BatchWithNestedEndDoesNotClear) {
    // 关键边界：BATCH 状态下内部的 END 不清除 trim_line_idx
    write_log({
        "BEGIN_PKGS 1",
        "BEGIN pkg 1.0",
        "COMMIT pkg 1.0",
        "END pkg 1.0",
        // 无 COMMIT_PKGS
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c[0].find("BEGIN_PKGS") != std::string::npos)
        << "END inside BATCH does not exit batch";
}

TEST_F(LogTrimTest, BatchWithCompleteThenPending) {
    write_log({
        "BEGIN_PKGS 1",
        "BEGIN pkg-a 1.0",
        "COMMIT pkg-a 1.0",
        "END pkg-a 1.0",
        "COMMIT_PKGS",
        "BEGIN_PKGS 1",
        "BEGIN pkg-b 1.0",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c[0].find("BEGIN_PKGS") != std::string::npos)
        << "pending txn kept";
}

// ═══════════════════════════════════════════════════════════════════════
// 第 4 组：混合多个事务
// ═══════════════════════════════════════════════════════════════════════

TEST_F(LogTrimTest, MultipleCompleteTxnsCleared) {
    write_log({
        "BEGIN a 1.0", "COMMIT a 1.0", "END a 1.0",
        "BEGIN b 1.0", "COMMIT b 1.0", "END b 1.0",
        "RM_BEGIN c 1.0", "RM_COMMIT c 1.0", "RM_END c 1.0",
    });
    TransactionLog::trim_completed();
    EXPECT_TRUE(read_log().empty()) << "multiple complete txns cleared";
}

TEST_F(LogTrimTest, CompleteThenIncompleteKeepsSuffix) {
    write_log({
        "BEGIN_PKGS 1",
        "BEGIN pkg-a 1.0",
        "COMMIT pkg-a 1.0",
        "END pkg-a 1.0",
        "COMMIT_PKGS",
        "BEGIN_PKGS 1",
        "RM_BEGIN pkg-b 1.0",
        "BACKUP /usr/bin/b â /usr/bin/b.lpkg_bak_pkg-b",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c[0].find("BEGIN_PKGS") != std::string::npos)
        << "incomplete suffix preserved";
}

TEST_F(LogTrimTest, MixedInstallRemoveCompleteThenPendingInstall) {
    write_log({
        "BEGIN_PKGS 1", "BEGIN d 1.0", "COMMIT d 1.0", "END d 1.0", "COMMIT_PKGS",
        "BEGIN_PKGS 1", "RM_BEGIN e 1.0", "RM_COMMIT e 1.0", "RM_END e 1.0", "COMMIT_PKGS",
        "BEGIN_PKGS 1", "BEGIN f 1.0",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c[0].find("BEGIN_PKGS") != std::string::npos)
        << "pending txn preserved";
}

TEST_F(LogTrimTest, TripleMixedCompleteThenIncompleteRemove) {
    write_log({
        "BEGIN_PKGS 1", "BEGIN a 1.0", "COMMIT a 1.0", "END a 1.0", "COMMIT_PKGS",
        "BEGIN_PKGS 1", "BEGIN b 2.0", "COMMIT b 2.0", "END b 2.0", "COMMIT_PKGS",
        "BEGIN_PKGS 1", "BEGIN c 3.0", "COMMIT c 3.0", "END c 3.0", "COMMIT_PKGS",
        "BEGIN_PKGS 1", "RM_BEGIN p 1.0",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c[0].find("BEGIN_PKGS") != std::string::npos)
        << "pending txn preserved";
}

// ═══════════════════════════════════════════════════════════════════════
// 第 5 组：恢复标记 (recovery)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(LogTrimTest, RecoveryRollbackEndCleared) {
    write_log({
        "RM_BEGIN pkg 1.0",
        "BACKUP /usr/bin/x → /usr/bin/x.lpkg_bak_pkg",
        "ROLLBACK pkg (recovery)",
        "END pkg (recovery)",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    EXPECT_TRUE(c.empty()) << "recovery rollback+end clears txn";
}

TEST_F(LogTrimTest, RecoveryBeforeTrimThenNewTxn) {
    write_log({
        "BEGIN_PKGS 1",
        "RM_BEGIN old 1.0",
        "BACKUP /usr/bin/o â /usr/bin/o.lpkg_bak_old",
        "ROLLBACK old (recovery)",
        "END old (recovery)",
        "COMMIT_PKGS",
    });
    {
        std::ofstream f(log_path, std::ios::app);
        f << "[2000-01-01 00:00:05] BEGIN_PKGS 1\n";
        f << "[2000-01-01 00:00:05] BEGIN new 1.0\n";
    }
    TransactionLog::trim_completed();
    auto c = read_log();
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c[0].find("BEGIN_PKGS") != std::string::npos)
        << "new pending txn preserved";
}

// ═══════════════════════════════════════════════════════════════════════
// 第 6 组：幂等性（多次 trim 结果一致）
// ═══════════════════════════════════════════════════════════════════════

TEST_F(LogTrimTest, DoubleTrimIdempotent_Complete) {
    write_log({
        "BEGIN a 1.0", "COMMIT a 1.0", "END a 1.0",
    });
    TransactionLog::trim_completed();
    EXPECT_TRUE(read_log().empty());
    TransactionLog::trim_completed();
    EXPECT_TRUE(read_log().empty()) << "second trim on empty is no-op";
}

TEST_F(LogTrimTest, DoubleTrimIdempotent_Incomplete) {
    write_log({
        "BEGIN a 1.0",
    });
    TransactionLog::trim_completed();
    auto c1 = read_log();
    TransactionLog::trim_completed();
    auto c2 = read_log();
    ASSERT_EQ(c1.size(), c2.size());
    for (size_t i = 0; i < c1.size(); ++i)
        EXPECT_EQ(c1[i], c2[i]) << "line " << i << " unchanged after second trim";
}

// ═══════════════════════════════════════════════════════════════════════
// 第 7 组：IDEMPOTENT — 各种边界
// ═══════════════════════════════════════════════════════════════════════

TEST_F(LogTrimTest, TrimThenTrimSameResult) {
    write_log({
        "BEGIN_PKGS 1", "BEGIN a 1.0", "COMMIT a 1.0", "END a 1.0", "COMMIT_PKGS",
        "BEGIN_PKGS 1", "RM_BEGIN b 1.0",
    });
    TransactionLog::trim_completed();
    auto first = read_log();
    TransactionLog::trim_completed();
    auto second = read_log();
    ASSERT_EQ(first.size(), second.size());
    EXPECT_TRUE(first.size() > 0);
}

TEST_F(LogTrimTest, TrimOnlyAffectsLogFile) {
    // trim 不影响其他文件
    write_log({"BEGIN a 1.0", "COMMIT a 1.0", "END a 1.0"});
    EXPECT_NO_THROW(TransactionLog::trim_completed());
    EXPECT_TRUE(fs::exists(Config::instance().lock_dir()))
        << "lock dir still exists";
}

// ═══════════════════════════════════════════════════════════════════════
// 第 8 组：错误的日志行（乱序、残缺）
// ═══════════════════════════════════════════════════════════════════════

TEST_F(LogTrimTest, StrayOperationLines_NoActiveTxn) {
    // 没有 BEGIN 的 BACKUP 行——trim 不应崩溃
    write_log({
        "BACKUP /usr/bin/x → /usr/bin/x.lpkg_bak",
        "NEW /usr/share/f",
    });
    EXPECT_NO_THROW(TransactionLog::trim_completed());
    // 这些 stray 行属于"无活跃事务"状态 → 应被当成"已完结"清空
    auto c = read_log();
    EXPECT_TRUE(c.empty()) << "stray lines without BEGIN treated as complete";
}

TEST_F(LogTrimTest, StrayLinesBeforeActiveTxn) {
    write_log({
        "BACKUP /usr/bin/z â /usr/bin/z.lpkg_bak",
        "BEGIN_PKGS 1",
        "BEGIN pkg 1.0",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c[0].find("BEGIN_PKGS") != std::string::npos)
        << "BEGIN_PKGS line kept after trim";
}

TEST_F(LogTrimTest, CorruptedLineAfterBegin) {
    write_log({
        "BEGIN_PKGS 1",
        "BEGIN pkg 1.0",
        "blah blah not a valid op",
    });
    EXPECT_NO_THROW(TransactionLog::trim_completed());
    auto c = read_log();
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c[0].find("BEGIN_PKGS") != std::string::npos)
        << "corrupted line doesn't crash trim";
}

// ═══════════════════════════════════════════════════════════════════════
// 第 9 组：边界——日志文件恰好是临界大小
// ═══════════════════════════════════════════════════════════════════════

TEST_F(LogTrimTest, SingleBeginOnly) {
    write_log({"BEGIN_PKGS 1", "BEGIN lone 1.0"});
    TransactionLog::trim_completed();
    auto c = read_log();
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c[0].find("BEGIN_PKGS") != std::string::npos);
}

TEST_F(LogTrimTest, SingleEndOnlyStray) {
    write_log({"END stray 1.0"});
    TransactionLog::trim_completed();
    // stray END 没有对应的 BEGIN → 在 NONE 状态下 → 被视为无活跃事务
    // 但会被 NONE 状态 + 不是 BEGIN/RM_BEGIN/BEGIN_PKGS 的处理 → trim_line_idx 保持 -1
    auto c = read_log();
    EXPECT_TRUE(c.empty()) << "stray END without BEGIN is stray line, cleared";
}

// ═══════════════════════════════════════════════════════════════════════
// 第 10 组：重复调用 + 真实 install/remove 之后调 trim
// ═══════════════════════════════════════════════════════════════════════

TEST_F(LogTrimTest, TrimAfterMockInstallLeavesCleanState) {
    // 模拟完整安装流程的 WAL 轨迹
    write_log({
        "BEGIN_PKGS 1",
        "BEGIN curl 8.11.1",
        "BACKUP /usr/bin/curl → /usr/bin/curl.lpkg_bak_curl",
        "COPY /tmp/lpkg_tmp/curl.lpkgtmp → /usr/bin/curl",
        "COMMIT curl 8.11.1",
        "END curl 8.11.1",
        "COMMIT_PKGS",
    });
    EXPECT_NO_THROW(TransactionLog::trim_completed());
    EXPECT_TRUE(read_log().empty()) << "after complete install, trim clears log";

    // 再次 trim
    EXPECT_NO_THROW(TransactionLog::trim_completed());
    EXPECT_TRUE(read_log().empty()) << "second trim also fine";
}

TEST_F(LogTrimTest, TrimAfterMockRemoveLeavesCleanState) {
    write_log({
        "RM_BEGIN openssl 3.0.0",
        "BACKUP /usr/lib/libssl.so → /usr/lib/libssl.so.lpkg_bak_openssl",
        "RM_DIR /usr/lib/",
        "RM_COMMIT openssl 3.0.0",
        "RM_END openssl 3.0.0",
    });
    TransactionLog::trim_completed();
    EXPECT_TRUE(read_log().empty());
}

// ═══════════════════════════════════════════════════════════════════════
// 第 11 组：边界——BEGIN 后紧接着另一个 BEGIN（理论上不会发，但不崩溃）
// ═══════════════════════════════════════════════════════════════════════

TEST_F(LogTrimTest, NestedBeginInsideBegin) {
    write_log({
        "BEGIN outer 1.0",
        "  BEGIN inner 1.0",
        "  COMMIT inner 1.0",
        "  END inner 1.0",
        "COMMIT outer 1.0",
        "END outer 1.0",
    });
    // 外层 BEGIN 把状态设为 INSTALL，内部的 BEGIN 因状态非 NONE 不会重置 trim_line_idx
    // 但内部 BEGIN→END 在 INSTALL 状态下被积累——外层 END 回到 NONE 并清除 trim_line_idx
    TransactionLog::trim_completed();
    auto c = read_log();
    EXPECT_TRUE(c.empty()) << "nested begins with outer END cleared";
}

TEST_F(LogTrimTest, NestedBeginInsideBeginNoOuterEnd) {
    write_log({
        "BEGIN_PKGS 1",
        "BEGIN outer 1.0",
        "  BEGIN inner 1.0",
        "  COMMIT inner 1.0",
        "  END inner 1.0",
    });
    TransactionLog::trim_completed();
    auto c = read_log();
    ASSERT_FALSE(c.empty());
    EXPECT_TRUE(c[0].find("BEGIN_PKGS") != std::string::npos)
        << "outer incomplete preserved";
}
