/**
 * test_undo_table.cpp — 撤销表（`reverse_execute` 的表驱动实现）的**可执行规格**
 *
 * 生产端的表在 `db/wal_op.cpp`（匿名命名空间，测试看不见），所以本文件用**行为**钉住它的
 * 每一格：每种可逆 op 的 ① 撤销动作的方向 ② 幂等口径（目标缺失/形态不符 → 跳过）
 * ③ `RESTORE_*` 审计行的字面（含**原样**还是**剥过尾斜杠**这种一眼看不出的差异）
 * ④ 计到 `RollbackStats` 的哪个量。
 *
 * 这些格子原先分散在 `reverse_execute` 的 48 个分支里、每种 op 各写一遍；现在它们是表里的
 * 几列。表里改坏任何一格都会在这里红 —— 例如：
 *   · 把 `UNSTASH` 的方向写反（它和 BACKUP 恰好互为逆，是最容易搞错的一格）；
 *   · 给 `RESTORE_DIR`（DIR_RM，记**规范化后**的路径）与 `RESTORE_DIR_RM`
 *     （NEW_DIR，记**原样**的 arg1）加上"顺手统一"的尾斜杠处理；
 *   · 让 `COPY` 的两支变成互斥（`also` 列），第二支（清残留 `.lpkgtmp`）就不再跑。
 *
 * 每种可逆 op 都至少有一条"必须动作"的用例：表里漏一行、或某行的 guard 恒假，
 * 对应用例就会红（`--gtest_filter` 里也点名了这些用例，见 lpkg/CLAUDE.md §2）。
 */

#include <gtest/gtest.h>
#include <sys/xattr.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../main/src/base/utils.hpp"  // base64_encode（手工构造 XATTR_SET 行）
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/wal_op.hpp"

namespace fs = std::filesystem;

namespace
{
constexpr const char* ARROW = " \xe2\x86\x92 ";  // WAL 箭头分帧用的 " → "
}

class UndoTableTest : public ::testing::Test
{
protected:
    fs::path suite_dir;
    fs::path test_root;

    void SetUp() override
    {
        suite_dir = fs::absolute("tmp_undo_table_test");
        if (fs::exists(suite_dir)) fs::remove_all(suite_dir);
        test_root = suite_dir / "root";
        fs::create_directories(test_root);

        Config::instance().set_root_path(test_root.string());
        Config::instance().set_testing_mode(true);
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_dir);
    }

    /// 沙盒内的真实路径（confinement 要求 WAL 行的路径都落在 root 内）
    fs::path in_root(const std::string& rel) const
    {
        return test_root / rel;
    }

    void touch(const fs::path& p, const std::string& content = "x")
    {
        fs::create_directories(p.parent_path());
        std::ofstream f(p);
        f << content;
    }

    std::string read_wal()
    {
        const std::string wpath = wal::wal_log_path();
        if (!fs::exists(wpath)) return "";
        std::ifstream f(wpath);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    /// 用一行的字面构造 op，并把路径字段换成沙盒内的绝对路径
    static wal::WALOp make_op(const std::string& type, const std::string& arg1,
                              const std::string& arg2 = "")
    {
        wal::WALOp op =
            wal::parse_op(arg2.empty() ? (type + " " + arg1) : (type + " " + arg1 + ARROW + arg2));
        op.arg1 = arg1;
        op.arg2 = arg2;
        return op;
    }

    wal::RollbackStats run(const std::vector<wal::WALOp>& ops, bool audit = true)
    {
        return wal::reverse_execute(ops, audit);
    }
};

// ============================================================================
// 备份三兄弟：BACKUP / REMOVE_OLD / SAVE_CONF —— 逆操作都是 rename(arg2 → arg1)
// ============================================================================

TEST_F(UndoTableTest, BackupRenamesStashBackToOriginAndAuditsArrow)
{
    const fs::path orig = in_root("usr/bin/tool");
    const fs::path bak = in_root(".lpkg_bak_pkg_1/tool.lpkg_bak_pkg_ab");
    fs::create_directories(orig.parent_path());  // 原位所在目录（真实系统里由基础包持有）
    touch(bak, "content");

    const auto stats = run({make_op("BACKUP", orig.string(), bak.string())});

    EXPECT_TRUE(fs::exists(orig)) << "BACKUP 的逆操作把 stash 里那份搬回原位";
    EXPECT_FALSE(fs::exists(bak));
    EXPECT_EQ(stats.files_restored, 1);
    EXPECT_EQ(stats.files_cleaned, 0);
    // 审计行记的是**实际动作**（bak → orig），两侧都是 WAL 里的原样字面
    EXPECT_NE(read_wal().find("RESTORE_FILE " + bak.string() + ARROW + orig.string() + "\n"),
              std::string::npos)
        << "审计行字面不对：" << read_wal();
}

TEST_F(UndoTableTest, RemoveOldHasItsOwnRowWithIdenticalUndo)
{
    const fs::path orig = in_root("usr/lib/libold.so");
    const fs::path bak = in_root(".lpkg_bak_pkg_1/libold.so.lpkg_bak_pkg_cd");
    fs::create_directories(orig.parent_path());
    touch(bak, "old");

    const auto stats = run({make_op("REMOVE_OLD", orig.string(), bak.string())});

    EXPECT_TRUE(fs::exists(orig)) << "REMOVE_OLD 的逆操作与 BACKUP 同形（这就是它单独一行的那格）";
    EXPECT_EQ(stats.files_restored, 1);
    EXPECT_NE(read_wal().find("RESTORE_FILE " + bak.string() + ARROW + orig.string() + "\n"),
              std::string::npos);
}

TEST_F(UndoTableTest, SaveConfRenamesLpkgsaveBackInPlace)
{
    const fs::path orig = in_root("etc/app.conf");
    const fs::path kept =
        in_root("etc/app.conf.lpkgsave");  // dst 不在 stash 里，是原位旁边的兄弟名
    touch(kept, "user edited");

    const auto stats = run({make_op("SAVE_CONF", orig.string(), kept.string())});

    EXPECT_TRUE(fs::exists(orig)) << "回滚要把 .lpkgsave 改名回配置原位";
    EXPECT_FALSE(fs::exists(kept));
    EXPECT_EQ(stats.files_restored, 1);
}

TEST_F(UndoTableTest, BackupMissingStashIsIdempotentAndWritesNoAudit)
{
    const fs::path orig = in_root("usr/bin/tool");
    const fs::path bak = in_root(".lpkg_bak_pkg_1/gone.lpkg_bak_pkg_ef");  // 不存在（已被消费）
    touch(orig, "still here");

    const auto stats = run({make_op("BACKUP", orig.string(), bak.string())});

    EXPECT_EQ(stats.files_restored, 0) << "bak 不存在 → 跳过（幂等），绝不因此报错";
    EXPECT_TRUE(fs::exists(orig));
    EXPECT_EQ(read_wal().find("RESTORE_FILE"), std::string::npos)
        << "什么都没做就不该写 RESTORE_* 行";
}

// ============================================================================
// UNSTASH：方向与上表**相反**（rename(orig → bak)）—— 最容易写反的一格
// ============================================================================

TEST_F(UndoTableTest, UnstashMovesOriginBackIntoStashAndAuditsActualDirection)
{
    const fs::path orig = in_root("etc/app.conf");
    const fs::path bak = in_root(".lpkg_bak_pkg_1/app.conf.lpkg_bak_pkg_12");
    touch(orig, "user edited");
    fs::create_directories(bak.parent_path());  // 守卫要求 stash 侧那份的父目录还在

    const auto stats = run({make_op("UNSTASH", bak.string(), orig.string())});

    EXPECT_FALSE(fs::exists(orig))
        << "UNSTASH 的逆操作 = 把原位那份**再搬进** stash（与 BACKUP 相反）";
    EXPECT_TRUE(fs::exists(bak));
    EXPECT_EQ(stats.files_restored, 1);
    // 审计记的是实际动作方向：orig → bak（字段仍是 WAL 里的原样字面）
    EXPECT_NE(read_wal().find("RESTORE_FILE " + orig.string() + ARROW + bak.string() + "\n"),
              std::string::npos)
        << "审计行记的必须是实际动作方向（orig → bak）：" << read_wal();
}

TEST_F(UndoTableTest, UnstashStripsTrailingSlashForTheMoveButAuditsRawPath)
{
    // 原位侧是**目录键形态**（带尾斜杠）：动作前必须剥（否则判定类调用落到链接目标上），
    // 而审计行仍记 WAL 里的原样字面 —— 这两件事在表里是**两格**，别"顺手统一"。
    const fs::path orig = in_root("etc/appdir");
    const fs::path bak = in_root(".lpkg_bak_pkg_1/appdir.lpkg_bak_pkg_34");
    fs::create_directories(orig);
    fs::create_directories(bak.parent_path());

    run({make_op("UNSTASH", bak.string(), orig.string() + "/")});

    EXPECT_FALSE(fs::exists(orig));
    EXPECT_TRUE(fs::exists(bak));  // 落在剥过尾斜杠的那个路径上
    EXPECT_NE(read_wal().find("RESTORE_FILE " + orig.string() + "/" + ARROW + bak.string() + "\n"),
              std::string::npos)
        << "审计行应按 WAL 里的原样字面渲染（带尾斜杠）：" << read_wal();
}

TEST_F(UndoTableTest, UnstashSkipsWhenOriginIsGone)
{
    const fs::path orig = in_root("etc/app.conf");  // 不存在
    const fs::path bak = in_root(".lpkg_bak_pkg_1/app.conf.lpkg_bak_pkg_56");
    touch(bak, "the only copy");
    fs::create_directories(bak.parent_path());

    const auto stats = run({make_op("UNSTASH", bak.string(), orig.string())});

    EXPECT_EQ(stats.files_restored, 0) << "原位没有可搬的 → 跳过（幂等），且不碰 stash 里那份";
    EXPECT_TRUE(fs::exists(bak)) << "跳过的语义是'原位保持不变'，绝不动 stash";
    EXPECT_EQ(read_wal().find("RESTORE_FILE"), std::string::npos);
}

// ============================================================================
// DIR_RM：按元数据重建目录（目录缺失正是它要修的状态）
// ============================================================================

TEST_F(UndoTableTest, DirRmRecreatesDirectoryWithMetadataAndAuditsStrippedPath)
{
    const fs::path dir = in_root("usr/share/pkgx");
    // 现场：目录已被 rmdir（不存在），WAL 里记着它的元数据。
    // mode 是 **十进制**（写入侧记的是 `st_mode & 07777` 的十进制值）：488 = 0750。
    // arg1 故意用**目录键形态**（带尾斜杠）。
    const auto stats = run({wal::parse_op("DIR_RM " + dir.string() + "/ 488 0 0")});

    EXPECT_TRUE(fs::is_directory(dir)) << "DIR_RM 的逆操作必须把目录重建出来（尾斜杠先剥掉）";
    EXPECT_EQ(stats.dirs_recreated, 1);
    // 审计记的是**规范化后**的路径（表里 DIR_RM 那格的 strip = true）——
    // 与 NEW_DIR 的 RESTORE_DIR_RM（记原样、保留尾斜杠）恰好相反，别去"统一"
    EXPECT_NE(read_wal().find("RESTORE_DIR " + dir.string() + "\n"), std::string::npos)
        << read_wal();
    // 元数据（mode）落到了重建出来的目录上
    EXPECT_EQ(
        static_cast<int>(fs::status(dir).permissions() & fs::perms::all),
        static_cast<int>(fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec));
}

TEST_F(UndoTableTest, DirRmKeepsExistingRealDirectoryAndRefreshesMetadata)
{
    // 幂等口径：**没有**"目标存在就跳过"这一说 —— 目录在位时仍刷元数据、仍计数
    // （表里 DIR_RM 那格的 guard = Always：目录缺失正是它要修的状态）
    const fs::path dir = in_root("usr/share/pkgx");
    fs::create_directories(dir);

    const auto stats = run({wal::parse_op("DIR_RM " + dir.string() + " 448 0 0")});

    EXPECT_TRUE(fs::is_directory(dir));
    EXPECT_EQ(stats.dirs_recreated, 1);
    EXPECT_EQ(static_cast<int>(fs::status(dir).permissions() & fs::perms::all),
              static_cast<int>(fs::perms::owner_all));
}

// ============================================================================
// NEW_DIR：删除安装时新建的**空**目录（审计记**原样**的 arg1）
// ============================================================================

TEST_F(UndoTableTest, NewDirRemovesEmptyDirectoryAndAuditsRawArgWithTrailingSlash)
{
    const fs::path dir = in_root("usr/share/newdir");
    fs::create_directories(dir);

    run({wal::parse_op("NEW_DIR " + dir.string() + "/")});

    EXPECT_FALSE(fs::exists(dir)) << "NEW_DIR 的逆操作删除安装时新建的空目录";
    // 与 DIR_RM 的 RESTORE_DIR 相反：这里记的是**原样**的 arg1（带尾斜杠）
    EXPECT_NE(read_wal().find("RESTORE_DIR_RM " + dir.string() + "/\n"), std::string::npos)
        << "NEW_DIR 的审计行应保留尾斜杠：" << read_wal();
}

TEST_F(UndoTableTest, NewDirKeepsNonEmptyDirectoryAndWritesNoAudit)
{
    const fs::path dir = in_root("usr/share/newdir");
    touch(dir / "someone-elses-file");

    const auto stats = run({wal::parse_op("NEW_DIR " + dir.string() + "/")});

    EXPECT_TRUE(fs::exists(dir)) << "非空 → 不动（无主内容绝不删）";
    EXPECT_EQ(stats.files_cleaned, 0);
    EXPECT_EQ(read_wal().find("RESTORE_DIR_RM"), std::string::npos)
        << "没删掉就不该写 RESTORE_DIR_RM（不能谎报）";
}

TEST_F(UndoTableTest, NewDirNeverTouchesSymlinkToEmptyDirectory)
{
    // 尾斜杠会让判定类调用**跟随**末段链接：链接目标是空目录时，判据会落在目标上，
    // 而路径对象是链接本身 —— 表里 NEW_DIR 那格先剥尾斜杠 + 判真目录，把这条堵死。
    const fs::path real_dir = in_root("opt/real-empty");
    const fs::path link = in_root("var/run");
    fs::create_directories(real_dir);
    fs::create_directories(link.parent_path());
    fs::create_symlink(real_dir, link);

    const auto stats = run({wal::parse_op("NEW_DIR " + link.string() + "/")});

    EXPECT_TRUE(fs::is_symlink(link)) << "符号链接不是'我们建的目录'，绝不能删";
    EXPECT_TRUE(fs::is_directory(real_dir)) << "链接目标（别人的目录）更不能删";
    EXPECT_EQ(stats.files_cleaned, 0);
    EXPECT_EQ(read_wal().find("RESTORE_DIR_RM"), std::string::npos);
}

// ============================================================================
// COPY：两支**互相独立**（删落点 + 清残留 .lpkgtmp）
// ============================================================================

TEST_F(UndoTableTest, CopyRemovesTargetAndLeftoverTmpWithOneAuditLine)
{
    const fs::path tmp = in_root("var/lpkg/app.lpkgtmp");
    const fs::path dst = in_root("usr/bin/app");
    touch(tmp);
    touch(dst);

    const auto stats = run({make_op("COPY", tmp.string(), dst.string())});

    EXPECT_FALSE(fs::exists(dst)) << "COPY 的逆操作删除落点";
    EXPECT_FALSE(fs::exists(tmp)) << "残留的 .lpkgtmp 也要清（第二支，不以第一支为条件）";
    EXPECT_EQ(stats.files_cleaned, 2) << "两支各计一次";
    // 只有落点那一支带审计行（tmp 不是 COPY 落位的对象）
    EXPECT_NE(read_wal().find("RESTORE_FILE_RM " + dst.string() + "\n"), std::string::npos);
    EXPECT_EQ(read_wal().find("RESTORE_FILE_RM " + tmp.string()), std::string::npos)
        << "不该为 .lpkgtmp 写审计行：" << read_wal();
}

TEST_F(UndoTableTest, CopyNeverRemovesRealDirectoryAtTargetButStillCleansTmp)
{
    // `fs::remove` 对**空目录**会 rmdir 成功 —— 表里 COPY 第一支的 guard 显式挡住真目录
    // （盘面/DB 脱节且没有 BACKUP 行可还原）。第二支照旧跑。
    const fs::path tmp = in_root("var/lpkg/app.lpkgtmp");
    const fs::path dst = in_root("usr/bin/app");
    touch(tmp);
    fs::create_directories(dst);

    const auto stats = run({make_op("COPY", tmp.string(), dst.string())});

    EXPECT_TRUE(fs::is_directory(dst)) << "真目录绝不能被 COPY 的逆操作 rmdir 掉";
    EXPECT_FALSE(fs::exists(tmp));
    EXPECT_EQ(stats.files_cleaned, 1) << "只有清残留那一支动了盘";
    EXPECT_EQ(read_wal().find("RESTORE_FILE_RM " + dst.string()), std::string::npos);
}

// ============================================================================
// NEW：删除安装时新建的文件
// ============================================================================

TEST_F(UndoTableTest, NewRemovesCreatedFileAndAuditsIt)
{
    const fs::path p = in_root("usr/bin/newfile");
    touch(p);

    const auto stats = run({make_op("NEW", p.string())});

    EXPECT_FALSE(fs::exists(p));
    EXPECT_EQ(stats.files_cleaned, 1);
    EXPECT_NE(read_wal().find("RESTORE_FILE_RM " + p.string() + "\n"), std::string::npos);
}

// ============================================================================
// DB 家族：DB / DBNEW / DBRM（备份侧是**派生**路径，不是 WAL 字段）
// ============================================================================

TEST_F(UndoTableTest, DbFamilyRestoresBackupOrDropsNewlyCreatedFile)
{
    // ── DB：有备份 → 还原 ──────────────────────────────────────────────────
    const fs::path db = in_root("var/lpkg/pkgs");
    const fs::path bak = in_root("var/lpkg/pkgs.lpkg_db_bak_before:pkg:installed");
    touch(db, "new content");
    touch(bak, "old content");

    auto stats = run({make_op("DB", db.string(), "pkg:installed")});
    EXPECT_EQ(stats.db_restored, 1);
    {
        std::ifstream f(db);
        std::stringstream ss;
        ss << f.rdbuf();
        EXPECT_EQ(ss.str(), "old content");
    }
    EXPECT_FALSE(fs::exists(bak)) << "备份被 rename 回正式名（不是复制）";
    EXPECT_NE(read_wal().find("RESTORE_DB " + bak.string() + ARROW + db.string() + "\n"),
              std::string::npos);

    // ── DBRM：有备份 → 同样还原 ────────────────────────────────────────────
    touch(db, "new content");
    touch(bak, "old content");
    stats = run({make_op("DBRM", db.string(), "pkg:installed")});
    EXPECT_EQ(stats.db_restored, 1);
    EXPECT_FALSE(fs::exists(bak));

    // ── DBNEW 无备份 → 删掉新建的文件（第二支，互斥）────────────────────────
    const fs::path new_db = in_root("var/lpkg/fresh.db");
    touch(new_db, "brand new");
    stats = run({make_op("DBNEW", new_db.string(), "pkg:installed")});
    EXPECT_FALSE(fs::exists(new_db));
    EXPECT_EQ(stats.files_cleaned, 1);
    EXPECT_EQ(stats.db_restored, 0);
    EXPECT_NE(read_wal().find("RESTORE_DB_RM " + new_db.string() + "\n"), std::string::npos);

    // ── DBNEW 有备份 → 还原（第一支，且**不**再删正式文件）──────────────────
    const fs::path new_db2 = in_root("var/lpkg/fresh2.db");
    const fs::path bak2 = in_root("var/lpkg/fresh2.db.lpkg_db_bak_before:pkg:installed");
    touch(new_db2, "new");
    touch(bak2, "old");
    stats = run({make_op("DBNEW", new_db2.string(), "pkg:installed")});
    EXPECT_EQ(stats.db_restored, 1);
    EXPECT_EQ(stats.files_cleaned, 0) << "第一支成立就不走第二支（互斥）";
    EXPECT_TRUE(fs::exists(new_db2));
    EXPECT_FALSE(fs::exists(bak2));
}

TEST_F(UndoTableTest, DbGuardOnSymlinkLoopBakDoesNotThrowAndSkips)
{
    // `Guard::DbBakExists` 原先是**抛**的 `fs::exists` —— 备份路径被**符号链接环**占着时它以
    // ELOOP 打断整条回滚，而本文件上方 `guard_ok()` 的纪律明写"回滚路径上的判定绝不能有能力
    // 打断事务"。实测（宿主最小复现）：`stat("self")` = **ELOOP**（抛版 `fs::exists` 就在这条
    // 路上抛），而**跟随语义的不抛**判据给 false。
    //
    // 判否的后果是**跳过这一行**（"备份解不开 ⇒ 当它不可用 ⇒ 原文件还在 ⇒ 幂等跳过"），
    // 不是把整批拖垮 —— 所以断言必须同时钉三件：**不抛** / 跳过（计数为 0）/ **原位一个字节
    // 没被动**且**没写审计行**。只钉"不抛"不够：一个"抛之前先改了盘"的实现也能过。
    const fs::path db = in_root("var/lpkg/pkgs");
    const fs::path bak = in_root("var/lpkg/pkgs.lpkg_db_bak_before:pkg:installed");
    touch(db, "official");
    fs::create_symlink(bak, bak);  // 自环：lstat 成功、解不开
    ASSERT_TRUE(fs::is_symlink(fs::symlink_status(bak))) << "现场没造出符号链接环";

    wal::RollbackStats stats{};
    EXPECT_NO_THROW(stats = run({make_op("DB", db.string(), "pkg:installed")}))
        << "回滚路径上的判定把异常抛出去了 —— 整条回滚会断在这里";
    EXPECT_EQ(stats.db_restored, 0) << "备份解不开 ⇒ 跳过（幂等）";
    {
        std::ifstream f(db);
        std::stringstream ss;
        ss << f.rdbuf();
        EXPECT_EQ(ss.str(), "official") << "跳过就不能动原位那份";
    }
    EXPECT_EQ(read_wal().find("RESTORE_DB "), std::string::npos) << "跳过就不该写审计行";

    // 对照（防"把跟随语义换成 lstat 语义"这种修法）：**备份是普通符号链接且目标可达**时，
    // 这一格必须照旧判"存在"并真的还原 —— `exists_no_follow` 也会给 true，所以这条不区分两者；
    // 它挡的是"改成恒 false / 索性不判"这类过度修正。
    const fs::path db2 = in_root("var/lpkg/provides.db");
    const fs::path bak2 = in_root("var/lpkg/provides.db.lpkg_db_bak_before:pkg:installed");
    const fs::path real_bak2 = in_root("var/lpkg/provides.db.real");
    touch(db2, "new");
    touch(real_bak2, "old");
    fs::create_symlink(real_bak2, bak2);
    stats = run({make_op("DB", db2.string(), "pkg:installed")});
    EXPECT_EQ(stats.db_restored, 1) << "跟随语义：链接可达就是要还原";
    {
        std::ifstream f(db2);
        std::stringstream ss;
        ss << f.rdbuf();
        EXPECT_EQ(ss.str(), "old");
    }
}

// ============================================================================
// 幂等：同一行跑两遍，第二遍是彻底的 no-op（不动盘、不再写审计行）
// ============================================================================

TEST_F(UndoTableTest, RestoreIsIdempotentOnSecondRunOfTheSameLine)
{
    const fs::path db = in_root("var/lpkg/pkgs");
    const fs::path bak = in_root("var/lpkg/pkgs.lpkg_db_bak_before:pkg:installed");
    touch(db, "official");
    touch(bak, "backup");

    const auto op = make_op("DB", db.string(), "pkg:installed");

    EXPECT_EQ(run({op}).db_restored, 1);
    const std::string after_first = read_wal();
    const auto second = run({op});  // 备份已在第一遍被消费

    EXPECT_EQ(second.db_restored, 0) << "bak 不存在 → 跳过（幂等）";
    EXPECT_EQ(read_wal(), after_first) << "什么都没做就不该再写审计行";
}

/**
 * `XATTR_SET` / `XATTR_NEW` 的逆操作只该落在**真实目录**上（2026-09-26 修）。
 *
 * 写入侧（`OpSink::set_xattr` / `unset_xattr`）的前置是 `lstat` + `S_ISDIR` ⇒ **这些行只描述
 * 真实目录**。而回滚侧原来用的是 `Guard::TakenNotSymlink` —— 它放行的范围宽得多
 * （普通文件 / FIFO / 设备都算）⇒ 回滚会 `lsetxattr` 在**写入侧从不写**的路径上造出一个键：
 * "写入侧刻意不造的状态，回滚侧造出来了"。`DIR_META` 那一格用的就是 `Guard::RealDir`
 * （与写入侧逐字对齐），这两格此前没跟上。
 *
 * **为什么直连 undo 层**：经正常事务到不了这里 —— XATTR 行只由真实目录产生，而"同一路径既
 * 是目录条目又是文件条目"会被冲突预检拒。这条守的正是"将来某条行类型被文件路径复用"那一天
 * （`wal_op.cpp` 里那两行的注释自己写了这句）。所以用**手工构造的行**把那个状态直接造出来。
 */
TEST_F(UndoTableTest, SetXattrUndoSkipsPlainFileTargets)
{
    const fs::path file = in_root("plain.txt");
    touch(file, "i am a plain file\n");
    ASSERT_TRUE(fs::is_regular_file(file));

    const std::string key = "user.k";
    const std::string val = "v";
    const std::string line = "XATTR_SET " + file.string() + " " +
                             base64_encode(std::vector<char>(key.begin(), key.end())) + " " +
                             base64_encode(std::vector<char>(val.begin(), val.end()));
    const wal::WALOp op = wal::parse_op(line);
    ASSERT_TRUE(op.is_valid()) << "构造的 WAL 行没解析成功：" << line;

    const wal::RollbackStats stats = wal::reverse_execute({op});
    (void)stats;

    // 逆操作必须是 no-op：写入侧从不会在**普通文件**上写这个键
    char buf[256];
    const ssize_t n = ::lgetxattr(file.c_str(), key.c_str(), buf, sizeof(buf));
    EXPECT_LT(n, 0) << "回滚在**普通文件**上造出了写入侧从不写的 xattr 键（`Guard` 比写入侧宽）:"
                    << " n=" << n;
    // 文件本身不该被动
    EXPECT_TRUE(fs::is_regular_file(file));
    std::ifstream f(file);
    const std::string got{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    EXPECT_EQ(got, "i am a plain file\n");
}
