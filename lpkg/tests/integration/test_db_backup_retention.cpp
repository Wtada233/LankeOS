/**
 * test_db_backup_retention.cpp — 未提交批次的 DB 备份必须留住
 *
 * 缺陷：`*.lpkg_db_bak_before:*` 是**重试还原 DB 的唯一依据**（WAL 里未配对区域的
 * DB/DBNEW 行只记了"备份文件名"，备份被删 → 那一行再也回滚不了），而
 * `cleanup_db_backups()` 此前只看"文件在不在"，三个调用点里只有一个（recover_packages
 * 自己判 any_batch_failed 后跳过）带守卫：
 *   ① `lpkg rec`（main_cli.cpp 的 `run_rec_command`）在 recover_packages() 之后**无条件**
 *      再调一次 → 恢复失败时刚被
 *      特意保留的还原点立刻被删光，CLI 还照打"恢复完成"；
 *   ② finish_committed_batch()：任何一次成功提交都会把**上一个未配对批次**的重试依据一并扫掉
 *      （trim_completed 只删已配对块，未配对区域仍在，备份却没了）。
 * 结果是"文件被逆向还原了、DB 却停在已安装"的不可恢复状态。
 *
 * 修复把守卫下沉进 `cleanup_db_backups()` 本身：`wal_has_unpaired_batch()` 为真则**直接
 * return**（该函数是 recover.cpp 里的文件内静态函数，只能从 cleanup_db_backups 的行为侧面钉）。
 *
 * 本文件钉住四条：
 *   ① WAL 里有未配对 BEGIN_PKGS → 一处备份都不删（state_dir 与 docs_dir 两个扫描根同办）；
 *   ② 批次已配对（BEGIN_PKGS + COMMIT_PKGS）→ **必须放行**（否则备份永远清不掉、state_dir
 *      无限膨胀）—— 与 ① 成对，专门拆穿"WAL 非空就跳过"这种过宽实现；
 *   ③ WAL 文件不存在（正常路径：trim 之后）→ 照常清理；
 *   ④ 深度记账：先一批已配对、再一批未配对 → 仍是"有未提交批次"（不能只看 WAL 最后一行）。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class DbBackupRetentionTest : public IntegrationTestBase
{
protected:
    static constexpr const char* BAK_TAG = ".lpkg_db_bak_before:";

    /** 两个扫描根各放一份备份：state_dir（DB/依赖/metadata）与 docs_dir（man 页） */
    fs::path bak_in_state;
    fs::path bak_in_docs;

    void SetUp() override
    {
        IntegrationTestBase::SetUp();

        const fs::path state_dir = Config::instance().state_dir();
        const fs::path docs_dir = Config::instance().docs_dir();
        fs::create_directories(state_dir);
        fs::create_directories(docs_dir);

        // 备份名就是 DBRM/cache 的产物形态：<原文件>.lpkg_db_bak_before:<里程碑>
        bak_in_state = state_dir / ("pkgs" + std::string(BAK_TAG) + "crash:installed");
        bak_in_docs = docs_dir / ("manpage" + std::string(BAK_TAG) + "crash:installed");
    }

    /** 直接铺 WAL（与 tests/unit/test_cleanup.cpp 同款手法） */
    void write_wal(const std::string& content)
    {
        const std::string path = wal::wal_log_path();
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream f(path, std::ios::trunc);
        f << content;
    }

    void remove_wal()
    {
        std::error_code ec;
        fs::remove(wal::wal_log_path(), ec);
        ASSERT_FALSE(fs::exists(wal::wal_log_path()));
    }

    void seed_backups()
    {
        std::ofstream(bak_in_state) << "pkgs 崩溃前状态\n";
        std::ofstream(bak_in_docs) << "man 页崩溃前状态\n";
        ASSERT_TRUE(fs::exists(bak_in_state));
        ASSERT_TRUE(fs::exists(bak_in_docs));
    }

    void expect_backups_kept(const std::string& why) const
    {
        EXPECT_TRUE(fs::exists(bak_in_state)) << why << "（state_dir 里的还原点被删了）";
        EXPECT_TRUE(fs::exists(bak_in_docs)) << why << "（docs_dir 里的 man 还原点被删了）";
    }

    void expect_backups_cleaned(const std::string& why) const
    {
        EXPECT_FALSE(fs::exists(bak_in_state)) << why << "（state_dir 里的备份没清掉）";
        EXPECT_FALSE(fs::exists(bak_in_docs)) << why << "（docs_dir 里的 man 备份没清掉）";
    }
};

// ── ① 未提交批次：一个备份都不许删 ────────────────────────────────────────
TEST_F(DbBackupRetentionTest, UncommittedBatchRetainsDbBackups)
{
    write_wal("BEGIN_PKGS 1\nBEGIN crashed 1.0\nDB /var/lib/lpkg/pkgs crashed:installed\n");
    seed_backups();

    cleanup_db_backups();

    expect_backups_kept("WAL 里还有未配对的 BEGIN_PKGS（批次未提交）");
}

// ── ② 批次已配对：门控必须放行 ────────────────────────────────────────────
TEST_F(DbBackupRetentionTest, PairedBatchStillCleansDbBackups)
{
    // 关键对照：WAL 同样**非空**且含 BEGIN_PKGS，只是它已被 COMMIT_PKGS 配对
    write_wal(
        "BEGIN_PKGS 1\n"
        "BEGIN crashed 1.0\n"
        "DB /var/lib/lpkg/pkgs crashed:installed\n"
        "COMMIT_PKGS\n");
    seed_backups();

    cleanup_db_backups();

    expect_backups_cleaned("批次已提交，备份是该清掉的孤儿");
}

// ── ③ WAL 不存在（正常路径）：照常清理 ────────────────────────────────────
TEST_F(DbBackupRetentionTest, MissingWalStillCleansDbBackups)
{
    remove_wal();
    seed_backups();

    cleanup_db_backups();

    expect_backups_cleaned("没有 WAL = 没有未提交批次");
}

// ── ⑤ CRLF WAL：守卫必须与 recover/trim 给出**同一个**答案（留备份） ──────
TEST_F(DbBackupRetentionTest, CrlfWalStillRetainsDbBackups)
{
    // 缺陷（2026-09-26 实测）：`BEGIN_PKGS`/`COMMIT_PKGS` 是**裸行**（`begin_batch()` →
    // `w.log("BEGIN_PKGS")`，不带载荷），于是 CRLF 的 \r 粘在**类型 token** 上：
    // `parse_op("BEGIN_PKGS\r")` 按第一个空格切类型 —— 这里根本没有空格 —— 得到未知类型
    // → INVALID ⇒ 配对扫描一个批次都看不见 ⇒ 守卫判"没有未提交批次" ⇒ 备份被删光。
    // 而 recover / trim / continue_post_commit_cleanup 走 read_wal_lines（剥 \r），看到的是
    // "有未提交批次"。**同一份 WAL，两个相反的答案**，且守卫那一侧是危险的那一侧。
    //
    // 形态必须是**裸的**：写成 `BEGIN_PKGS 1` 时 \r 落在 arg1、类型照样解析成功，测不出来。
    write_wal("BEGIN_PKGS\r\nBEGIN crashed 1.0\r\nDB /var/lib/lpkg/pkgs crashed:installed\r\n");
    seed_backups();

    cleanup_db_backups();

    expect_backups_kept("CRLF WAL 里仍有未配对批次（守卫必须与 recover/trim 同答案）");
}

// ── ⑥ 对照：CRLF 且批次**已配对** → 仍须放行（防"见到 CRLF 就一律保留"） ──
TEST_F(DbBackupRetentionTest, CrlfPairedBatchStillCleansDbBackups)
{
    write_wal(
        "BEGIN_PKGS\r\n"
        "BEGIN crashed 1.0\r\n"
        "DB /var/lib/lpkg/pkgs crashed:installed\r\n"
        "COMMIT_PKGS\r\n");
    seed_backups();

    cleanup_db_backups();

    expect_backups_cleaned("CRLF 但批次已配对 → 备份仍是该清的孤儿");
}

TEST_F(DbBackupRetentionTest, SecondUnpairedBatchAfterCommittedOneStillRetains)
{
    // 一次崩溃可能留下两个未提交批次（前一批回滚失败后又开了新批次）；只看"最后一行"或
    // "有过 COMMIT_PKGS"都会误判成"没有未提交批次" → 把重试依据删掉
    write_wal(
        "BEGIN_PKGS 2\n"
        "DB /var/lib/lpkg/pkgs done:installed\n"
        "COMMIT_PKGS\n"
        "BEGIN_PKGS 1\n"
        "BEGIN pending 1.0\n");
    seed_backups();

    cleanup_db_backups();

    expect_backups_kept("第二批仍未配对（深度记账不能只看最后一行/最后一次 COMMIT_PKGS）");
}
