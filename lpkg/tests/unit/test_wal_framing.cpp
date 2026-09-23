/**
 * test_wal_framing.cpp — WAL 行分帧与哨兵的回归测试（TODO.md A1/A2）
 *
 * 背景（两个真实缺陷）：
 *   A1 旧 split_line() 只对 "A → B" 箭头形式保空格，NEW/NEW_DIR/DIR_RM/DB/DBNEW/DBRM
 *      一律按空格切 → 含空格路径被截断；DB 行被截断后 reverse_execute 算出的备份名
 *      不存在，于是"bak 不存在 → 跳过（幂等）"**静默跳过 DB 回滚**。
 *      必然触发：`lpkg --root "路径含空格" install X`（state_dir 重定基到 root 下）。
 *   A2 未知行被解析成 `type=BEGIN_PKGS` 哨兵 + `arg1="__INVALID__"`；而
 *      extract_current_batch_ops 的"找最后一个 BEGIN_PKGS"反向扫描漏了哨兵判断
 *      → 破损尾部行冒充批次起点 → 整批操作集被截断（甚至为空）。
 *
 * 修法：参数切分收进 parse_op（尾部固定字段从右往左切、剩余整段归 arg1），
 *       未知行改用独立的 WALOpType::INVALID（WALOp 默认即 INVALID）。
 * 本文件锁死：① 各 op 类型含空格路径的正确解析 ② 历史行格式完全兼容
 *          ③ 未知行是 INVALID 而**不是**任何真实类型 ④ 破损尾部不截断批次提取
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/i18n/localization.hpp"

namespace fs = std::filesystem;
using wal::WALOp;
using wal::WALOpType;

// ============================================================================
// 纯解析测试（不需要沙盒）
// ============================================================================

/// 解析测试也会经 log_warning 输出（破损行），需要先装好 l10n，否则输出 [MISSING_STRING]
class WalFramingTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        init_localization();
    }
};

TEST_F(WalFramingTest, SingleArgOpsKeepSpacesInPath)
{
    // NEW / NEW_DIR / CLEANUP 及 RESTORE_*_RM 都是单参数：整段剩余即 arg1
    const std::vector<std::pair<std::string, WALOpType>> cases = {
        {"NEW /usr/share/doc/a b.txt", WALOpType::NEW},
        {"NEW_DIR /usr/share/doc/a b/", WALOpType::NEW_DIR},
        {"CLEANUP /usr/lib/.lpkg_bak_x_1 with space", WALOpType::CLEANUP},
        {"RESTORE_FILE_RM /usr/bin/a b", WALOpType::RESTORE_FILE_RM},
        {"RESTORE_DIR /usr/share/a b", WALOpType::RESTORE_DIR},
        {"RESTORE_DIR_RM /usr/share/a b", WALOpType::RESTORE_DIR_RM},
        {"RESTORE_DB_RM /var/lib/lpkg/a b/pkgs", WALOpType::RESTORE_DB_RM},
    };
    for (const auto& [line, type] : cases) {
        const WALOp op = wal::parse_op(line);
        ASSERT_TRUE(op.is_valid()) << line;
        EXPECT_EQ(op.type, type) << line;
        EXPECT_EQ(op.arg1, line.substr(line.find(' ') + 1)) << line;
    }
}

TEST_F(WalFramingTest, DbMilestoneIsSplitFromTheRight)
{
    // A1 的致命形态：路径含空格时，里程碑必须从右往左切出来
    const std::vector<std::pair<std::string, WALOpType>> cases = {
        {"DB /var/lib/lpkg/a b/pkgs mypkg:installed", WALOpType::DB},
        {"DBNEW /var/lib/lpkg/a b/deps/mypkg mypkg:installed", WALOpType::DBNEW},
        {"DBRM /var/lib/lpkg/a b/needed_so/mypkg mypkg:installed", WALOpType::DBRM},
    };
    for (const auto& [line, type] : cases) {
        const WALOp op = wal::parse_op(line);
        ASSERT_TRUE(op.is_valid()) << line;
        EXPECT_EQ(op.type, type) << line;
        EXPECT_EQ(op.arg2, "mypkg:installed") << line;  // 里程碑完整
        // arg1 = 整条路径（含空格），而不是被截断的前缀
        EXPECT_NE(op.arg1.find(' '), std::string::npos) << line;
        EXPECT_EQ(op.arg1.back(), *line.substr(0, line.rfind(' ')).rbegin()) << line;
    }
    EXPECT_EQ(wal::parse_op("DB /var/lib/a b/pkgs /mnt/root x/pkgs::batch-start").arg2,
              "x/pkgs::batch-start");
}

TEST_F(WalFramingTest, DirRmSplitsModeUidGidFromTheRight)
{
    const WALOp op = wal::parse_op("DIR_RM /usr/share/my dir 0755 1000 1000");
    ASSERT_TRUE(op.is_valid());
    EXPECT_EQ(op.type, WALOpType::DIR_RM);
    EXPECT_EQ(op.arg1, "/usr/share/my dir");
    EXPECT_EQ(op.arg2, "0755");
    EXPECT_EQ(op.arg3, "1000");
    EXPECT_EQ(op.arg4, "1000");
}

TEST_F(WalFramingTest, PackageMetadataOpsKeepVersionSeparate)
{
    // 包名不含空格，版本号也不含空格：从右切出 1 个字段
    for (const auto& type :
         {WALOpType::BEGIN, WALOpType::COMMIT, WALOpType::ROLLBACK, WALOpType::END,
          WALOpType::RM_BEGIN, WALOpType::RM_COMMIT, WALOpType::RM_END}) {
        const std::string line = std::string(wal::walop_type_name(type)) + " mypkg 1.2.3+4";
        const WALOp op = wal::parse_op(line);
        ASSERT_TRUE(op.is_valid()) << line;
        EXPECT_EQ(op.type, type) << line;
        EXPECT_EQ(op.arg1, "mypkg") << line;
        EXPECT_EQ(op.arg2, "1.2.3+4") << line;
    }
}

TEST_F(WalFramingTest, PackageMetadataOpWithEmptyVersion)
{
    // batch_rollback 对"版本未知"的包写的是 "ROLLBACK <pkg> "（尾部带空格）
    const WALOp op = wal::parse_op("ROLLBACK mypkg ");
    ASSERT_TRUE(op.is_valid());
    EXPECT_EQ(op.arg1, "mypkg");
    EXPECT_EQ(op.arg2, "");
}

TEST_F(WalFramingTest, ArrowOpsKeepBothSidesIntact)
{
    // 箭头形式本来就是两段整取（git 历史里修过一次），必须保持
    const std::string line =
        "BACKUP /usr/lib/a b/libx.so \xe2\x86\x92 /usr/lib/.lpkg_bak_x_1/libx.so.lpkg_bak_x_ab12cd";
    const WALOp op = wal::parse_op(line);
    ASSERT_TRUE(op.is_valid());
    EXPECT_EQ(op.type, WALOpType::BACKUP);
    EXPECT_EQ(op.arg1, "/usr/lib/a b/libx.so");
    EXPECT_EQ(op.arg2, "/usr/lib/.lpkg_bak_x_1/libx.so.lpkg_bak_x_ab12cd");

    const WALOp copy = wal::parse_op("COPY /tmp/my pkg/x.lpkgtmp \xe2\x86\x92 /usr/bin/x y");
    EXPECT_EQ(copy.type, WALOpType::COPY);
    EXPECT_EQ(copy.arg1, "/tmp/my pkg/x.lpkgtmp");
    EXPECT_EQ(copy.arg2, "/usr/bin/x y");

    const WALOp rdb =
        wal::parse_op("RESTORE_DB /var/lib/a b/pkgs.bak \xe2\x86\x92 /var/lib/a b/pkgs");
    EXPECT_EQ(rdb.type, WALOpType::RESTORE_DB);
    EXPECT_EQ(rdb.arg1, "/var/lib/a b/pkgs.bak");
    EXPECT_EQ(rdb.arg2, "/var/lib/a b/pkgs");
}

TEST_F(WalFramingTest, HistoricalLineFormsStillParseIdentically)
{
    // 向后兼容：历史 WAL 行（字段内无空格）的解析结果必须与旧的逐空格切分一致
    struct Case {
        const char* line;
        WALOpType type;
        const char* a1;
        const char* a2;
        const char* a3;
        const char* a4;
    };
    const Case cases[] = {
        {"BEGIN_PKGS", WALOpType::BEGIN_PKGS, "", "", "", ""},
        {"BEGIN_PKGS 1", WALOpType::BEGIN_PKGS, "1", "", "", ""},
        {"COMMIT_PKGS", WALOpType::COMMIT_PKGS, "", "", "", ""},
        {"BEGIN curl 8.11.1", WALOpType::BEGIN, "curl", "8.11.1", "", ""},
        {"NEW /usr/share/doc/curl/README", WALOpType::NEW, "/usr/share/doc/curl/README", "", "",
         ""},
        {"NEW_DIR /usr/share/doc/curl/", WALOpType::NEW_DIR, "/usr/share/doc/curl/", "", "", ""},
        {"BACKUP /usr/bin/curl \xe2\x86\x92 /usr/bin/curl.lpkg_bak_curl_ab12cd", WALOpType::BACKUP,
         "/usr/bin/curl", "/usr/bin/curl.lpkg_bak_curl_ab12cd", "", ""},
        {"COPY /tmp/x.lpkgtmp \xe2\x86\x92 /usr/bin/curl", WALOpType::COPY, "/tmp/x.lpkgtmp",
         "/usr/bin/curl", "", ""},
        {"REMOVE_OLD /usr/bin/old \xe2\x86\x92 /usr/lib/.lpkg_bak_a_1/old.lpkg_bak_a_ab12cd",
         WALOpType::REMOVE_OLD, "/usr/bin/old", "/usr/lib/.lpkg_bak_a_1/old.lpkg_bak_a_ab12cd", "",
         ""},
        {"DIR_RM /usr/share/doc/old 0755 0 0", WALOpType::DIR_RM, "/usr/share/doc/old", "0755", "0",
         "0"},
        {"DB /var/lib/lpkg/pkgs A:installed", WALOpType::DB, "/var/lib/lpkg/pkgs", "A:installed",
         "", ""},
        {"DB /var/lib/lpkg/pkgs :batch-start", WALOpType::DB, "/var/lib/lpkg/pkgs", ":batch-start",
         "", ""},
        {"DBNEW /var/lib/lpkg/deps/A A:installed", WALOpType::DBNEW, "/var/lib/lpkg/deps/A",
         "A:installed", "", ""},
        {"DBRM /var/lib/lpkg/needed_so/A A:installed", WALOpType::DBRM, "/var/lib/lpkg/needed_so/A",
         "A:installed", "", ""},
        {"CLEANUP /usr/lib/.lpkg_bak_A_42", WALOpType::CLEANUP, "/usr/lib/.lpkg_bak_A_42", "", "",
         ""},
        {"RM_BEGIN A 1.0", WALOpType::RM_BEGIN, "A", "1.0", "", ""},
        {"ROLLBACK A 1.0", WALOpType::ROLLBACK, "A", "1.0", "", ""},
        {"END A 1.0", WALOpType::END, "A", "1.0", "", ""},
        {"RESTORE_FILE /a.lpkg_bak_A_1 \xe2\x86\x92 /a", WALOpType::RESTORE_FILE, "/a.lpkg_bak_A_1",
         "/a", "", ""},
        {"RESTORE_FILE_RM /usr/bin/a", WALOpType::RESTORE_FILE_RM, "/usr/bin/a", "", "", ""},
        {"REMOVE_FILE /usr/bin/legacy", WALOpType::REMOVE_FILE, "/usr/bin/legacy", "", "", ""},
    };
    for (const auto& c : cases) {
        const WALOp op = wal::parse_op(c.line);
        ASSERT_TRUE(op.is_valid()) << c.line;
        EXPECT_EQ(op.type, c.type) << c.line;
        EXPECT_EQ(op.arg1, c.a1) << c.line;
        EXPECT_EQ(op.arg2, c.a2) << c.line;
        EXPECT_EQ(op.arg3, c.a3) << c.line;
        EXPECT_EQ(op.arg4, c.a4) << c.line;
    }
}

TEST_F(WalFramingTest, LineWithTooFewFieldsFallsBackToWholeRest)
{
    // 字段不足（畸形/历史短行）：整段归 arg1，其余留空——与旧行为一致，不制造假字段
    const WALOp op = wal::parse_op("DB /var/lib/lpkg/pkgs");
    ASSERT_TRUE(op.is_valid());
    EXPECT_EQ(op.arg1, "/var/lib/lpkg/pkgs");
    EXPECT_TRUE(op.arg2.empty());

    const WALOp dir = wal::parse_op("DIR_RM /x 0755");
    EXPECT_EQ(dir.arg1, "/x 0755");
    EXPECT_TRUE(dir.arg2.empty());
}

TEST_F(WalFramingTest, UnknownLineIsInvalidSentinelNotBeginPkgs)
{
    // A2 的核心：未知行**绝不能**是任何真实类型（旧代码是 BEGIN_PKGS 哨兵，
    // 会让"找最后一个 BEGIN_PKGS"的反向扫描把它当成批次起点）
    for (const char* line : {"PY /tmp/x \xe2\x86\x92 /usr/bin/x",  // 半写：COPY → PY
                             "/tmp/x \xe2\x86\x92 /usr/bin/x",     // 半写：类型整个丢了
                             "THIS_LINE_IS_COMPLETELY_UNKNOWN_XXX_YYY", " DBNEW /x A:installed"}) {
        const WALOp op = wal::parse_op(line);
        EXPECT_FALSE(op.is_valid()) << line;
        EXPECT_EQ(op.type, WALOpType::INVALID) << line;
        EXPECT_NE(op.type, WALOpType::BEGIN_PKGS) << line;
        EXPECT_NE(op.type, WALOpType::COMMIT_PKGS) << line;
        EXPECT_TRUE(op.skip_in_reverse()) << line;  // 回放必须跳过它
        EXPECT_EQ(op.raw, line) << line;            // 原始文本仍保留（调试/警告）
    }
}

// ============================================================================
// 批次提取（沙盒）
// ============================================================================

class WalFramingBatchTest : public ::testing::Test
{
protected:
    fs::path suite_dir;
    fs::path test_root;

    void SetUp() override
    {
        suite_dir = fs::absolute("tmp_wal_framing_test");
        if (fs::exists(suite_dir)) fs::remove_all(suite_dir);
        test_root = suite_dir / "root";
        fs::create_directories(test_root);

        Config::instance().set_root_path(test_root.string());
        Config::instance().set_testing_mode(true);
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        Config::instance().set_non_interactive_mode(NonInteractiveMode::NO);
        fs::remove_all(suite_dir);
    }

    void append_wal(const std::string& content)
    {
        fs::create_directories(fs::path(wal::wal_log_path()).parent_path());
        std::ofstream f(wal::wal_log_path(), std::ios::app);
        f << content;
    }
};

TEST_F(WalFramingBatchTest, BrokenTailLineDoesNotTruncateBatchExtraction)
{
    // A2 回归：破损尾部行不得截断（更不能清空）批次操作集
    append_wal("BEGIN_PKGS\n");
    append_wal("BEGIN A 1.0\n");
    append_wal("NEW /usr/bin/a\n");
    append_wal("COPY /tmp/a \xe2\x86\x92 /usr/bin/a\n");
    append_wal("PY /tmp/b \xe2\x86\x92 /usr/bin/b\n");  // ← 半写行（COPY 被截成 PY）

    const auto ops = wal::extract_current_batch_ops(wal::wal_log_path());
    // 破损行之前的 4 条有效操作必须全部保留
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0].type, WALOpType::BEGIN_PKGS);
    EXPECT_EQ(ops[1].type, WALOpType::BEGIN);
    EXPECT_EQ(ops[2].type, WALOpType::NEW);
    EXPECT_EQ(ops[3].type, WALOpType::COPY);
    EXPECT_EQ(ops[3].arg2, "/usr/bin/a");
}

TEST_F(WalFramingBatchTest, BrokenTailLineDoesNotFakeBatchStart)
{
    // A2 的最坏形态：破损行出现在批次中间，旧代码会以它为起点、把之前的操作全丢掉
    append_wal("BEGIN_PKGS\n");
    append_wal("BEGIN A 1.0\n");
    append_wal("/tmp/a \xe2\x86\x92 /usr/bin/a\n");  // ← 类型丢失的半写行
    append_wal("NEW /usr/bin/c\n");

    const auto ops = wal::extract_current_batch_ops(wal::wal_log_path());
    ASSERT_EQ(ops.size(), 3u);  // BEGIN_PKGS + BEGIN + NEW（半写行被跳过）
    EXPECT_EQ(ops[0].type, WALOpType::BEGIN_PKGS);
    EXPECT_EQ(ops[1].type, WALOpType::BEGIN);
    EXPECT_EQ(ops[2].type, WALOpType::NEW);
    EXPECT_EQ(ops[2].arg1, "/usr/bin/c");
}

TEST_F(WalFramingBatchTest, CompletedBatchStillYieldsNoOps)
{
    // 已完成批次（尾部有 COMMIT_PKGS）→ 无未提交批次，提取必须为空
    append_wal("BEGIN_PKGS\n");
    append_wal("BEGIN A 1.0\n");
    append_wal("COMMIT_PKGS\n");
    append_wal("THIS_LINE_IS_UNKNOWN\n");  // 提交后的破损行不该改变结论
    EXPECT_TRUE(wal::extract_current_batch_ops(wal::wal_log_path()).empty());
}

TEST_F(WalFramingBatchTest, BatchRollbackReportsWhetherAnythingWasRolledBack)
{
    // A3：没有可回滚的批次时必须返回 false —— 调用方据此保留 DB 备份交给 rec 续传
    append_wal("THIS_LINE_IS_UNKNOWN\n");  // 无 BEGIN_PKGS
    EXPECT_FALSE(wal::batch_rollback({}));

    // 有未提交批次 → true（且写出 COMMIT_PKGS 收尾）
    append_wal("BEGIN_PKGS\n");
    append_wal("BEGIN A 1.0\n");
    EXPECT_TRUE(wal::batch_rollback({}));

    std::ifstream f(wal::wal_log_path());
    const std::string content((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("COMMIT_PKGS"), std::string::npos);
}

TEST_F(WalFramingBatchTest, SpacedPathBackupIsRestoredByReverseExecute)
{
    // A1 回归：含空格路径的 BACKUP 逆操作必须还原到原路径（旧解析器会把 dst 截断）
    const fs::path orig = test_root / "usr/share/doc/my pkg/README";
    const fs::path stash = test_root / "usr/lib/.lpkg_bak_mypkg_1";
    const fs::path bak = stash / "README.lpkg_bak_mypkg_ab12cd";
    fs::create_directories(stash);
    fs::create_directories(orig.parent_path());  // 还原目标目录（rename 需要父目录存在）
    std::ofstream(bak) << "old content\n";       // 现场：文件已被搬进 stash（等待还原）

    append_wal("BEGIN_PKGS\n");
    append_wal("BACKUP " + orig.string() + " \xe2\x86\x92 " + bak.string() + "\n");

    const auto ops = wal::extract_current_batch_ops(wal::wal_log_path());
    ASSERT_EQ(ops.size(), 2u);
    ASSERT_EQ(ops[1].arg1, orig.string());
    ASSERT_EQ(ops[1].arg2, bak.string());

    wal::reverse_execute(ops, /*write_audit=*/false);
    EXPECT_TRUE(fs::exists(orig)) << "含空格路径的备份未被还原";
    EXPECT_FALSE(fs::exists(bak));
}
