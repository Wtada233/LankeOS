/**
 * test_recovery_batch_pairing.cpp — 恢复的"批次配对记账"必须只回滚**未提交**的批次
 *
 * 缺陷（五路评审里事务那一路报的高危，本节把它钉成回归）：
 * `scan_batch_pairing()` 用**前向 depth 记账**找"第一个未配对 BEGIN_PKGS"，回滚区域取
 * `[第一个未配对 BEGIN_PKGS, EOF)`。这个记账隐含一个前提 —— **未配对 BEGIN_PKGS 至多一个**
 * （`wal_op.hpp` 的 `extract_current_batch_ops` 注释里白纸黑字写着这条前提）。但该前提只覆盖
 * "同一批不可重入"，**没覆盖"上一次恢复留下的未封口批 + 本次新开的批"**：
 *
 *   1. 某次恢复里有一条撤销动作**真的失败了**（`RollbackStats::failures > 0`，如 EROFS /
 *      immutable / 权限 / ENOTDIR）→ `rollback_uncommitted_region` **故意不 seal**（留给下次
 *      rec 幂等重做）。这是**有意的**、且有无特权构造法（见 test_db_backup_retention.cpp）。
 *   2. 同一进程接着执行用户的命令（`init_database_for` 不返回恢复成败），该命令**成功提交**
 *      → WAL 里出现 `BEGIN₁ …(未封口) BEGIN₂ … COMMIT₂`。
 *   3. 下一次任何 lpkg 命令启动时，depth 记账停在 1 ⇒ `unpaired_begin` 永远指向 BEGIN₁ ⇒
 *      回滚 `[BEGIN₁, EOF)` 会把**已提交那批**的 COPY/NEW（删文件）、BACKUP（搬回旧文件）、
 *      DB `:batch-end`（还原数据库）一并撤掉 —— **上一轮明明装成功的包被静默卸载**，且结果
 *      自洽（盘面与 DB 一起回到批次前）。
 *
 * 本文件覆盖四条：
 *   ① 嵌套在未提交区域里的**已提交**批次，其行**不得**被回滚（核心缺陷）；
 *   ② 同一形状下**未提交**批次的行**照样**回滚（别改成一刀切不动盘）；
 *   ③ seal 时**补足**与未配对 BEGIN 条数相等的 COMMIT_PKGS —— 只写一条会让"未提交区域"
 *      永远存在（下一轮再回滚一遍、trim 永不裁剪、WAL 无限增长）；
 *   ④ 入口守卫：WAL 里还留着未封口批次时，`run_batch_transaction` **拒绝**在其上开新批次
 *      （fail-closed），而不是把已提交批次的风险留给下一轮。
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/batch_transaction.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class RecoveryBatchPairingTest : public IntegrationTestBase
{
protected:
    void write_wal(const std::string& content)
    {
        const std::string path = wal::wal_log_path();
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream f(path, std::ios::trunc);
        f << content;
    }

    std::string read_wal() const
    {
        std::ifstream f(wal::wal_log_path());
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    /** `NEW <path>` 的逆操作是 RemoveFile ⇒ 用它当"这批装出来的东西"的可观察代理。 */
    static std::string new_line(const fs::path& p)
    {
        return "NEW " + p.string() + "\n";
    }
};

// ── ① + ② 嵌套在未提交区域里的已提交批次，不得被回滚 ───────────────────────
TEST_F(RecoveryBatchPairingTest, CommittedBatchNestedInUncommittedRegionIsNotRolledBack)
{
    const fs::path uncommitted_file = test_root / "uncommitted.txt";
    const fs::path committed_file = test_root / "committed.txt";
    std::ofstream(uncommitted_file) << "上一批未提交\n";
    std::ofstream(committed_file) << "这一批已提交\n";

    // BEGIN₁ 没有配对的 COMMIT（模拟"回滚失败后故意不 seal"），其后 BEGIN₂…COMMIT₂ 是
    // **成功提交**的批次 —— 正是缺陷形状。
    write_wal(std::string("BEGIN_PKGS\n") + "BEGIN pkgA 1.0\n" + new_line(uncommitted_file) +
              "BEGIN_PKGS\n" + "BEGIN pkgB 2.0\n" + new_line(committed_file) + "COMMIT_PKGS\n");

    recover_packages();

    // ② 未提交的那批照样回滚
    EXPECT_FALSE(fs::exists(uncommitted_file))
        << "未提交批次装出来的文件必须被回滚掉（别把这条修成一刀切不动盘）";
    // ① 已提交的那批绝不能被牵连
    EXPECT_TRUE(fs::exists(committed_file))
        << "已提交批次装出来的文件被回滚掉了 —— 上一轮成功的安装被静默撤销";
}

// ── ③ seal 补足 COMMIT：一轮恢复之后 WAL 必须不再有未封口批次 ──────────────
TEST_F(RecoveryBatchPairingTest, TwoUnpairedBatchesAreSealedInOneRecoveryRound)
{
    const fs::path a = test_root / "a.txt";
    const fs::path b = test_root / "b.txt";
    std::ofstream(a) << "a\n";
    std::ofstream(b) << "b\n";

    // **两个**未配对 BEGIN（前一批回滚失败后同进程又开了新批的形态，崩溃在提交前）。
    write_wal(std::string("BEGIN_PKGS\n") + "BEGIN pkgA 1.0\n" + new_line(a) + "BEGIN_PKGS\n" +
              "BEGIN pkgB 2.0\n" + new_line(b));

    recover_packages();

    EXPECT_FALSE(fs::exists(a));
    EXPECT_FALSE(fs::exists(b));
    // 两个 BEGIN ⇒ 必须补两条 COMMIT。只补一条时 depth 停在 1，"未提交区域"永远存在。
    EXPECT_FALSE(wal_has_unpaired_batch())
        << "恢复后 WAL 仍有未配对 BEGIN_PKGS —— seal 没补足 COMMIT，恢复永远收敛不了";
}

// ── ④ 入口守卫：还有未封口批次时，拒绝在其上开新批次 ──────────────────────
TEST_F(RecoveryBatchPairingTest, NewBatchIsRefusedWhileAnUnpairedBatchRemains)
{
    // 只留一个未配对 BEGIN（模拟"恢复没能撤销某一行"之后的状态）。
    write_wal(std::string("BEGIN_PKGS\n") + "BEGIN pkgA 1.0\n");

    EXPECT_THROW(
        { run_batch_transaction([](std::vector<std::string>&) {}); }, LpkgException)
        << "在未封口的 WAL 上开新批次会把'已提交批次被连带回滚'的风险留给下一轮，必须 fail-closed";
}

// ── 对照：干净的 WAL（无未封口批次）照样能开新批次 ────────────────────────
TEST_F(RecoveryBatchPairingTest, CleanWalStillAllowsANewBatch)
{
    write_wal(std::string("BEGIN_PKGS\n") + "BEGIN pkgA 1.0\n" + "COMMIT_PKGS\n");

    EXPECT_NO_THROW({
        auto ok = run_batch_transaction([](std::vector<std::string>&) {});
        EXPECT_TRUE(ok.empty());
    });
}
