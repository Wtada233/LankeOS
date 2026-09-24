/**
 * test_db_batch_start_recovery.cpp — 批次开头那个**不可自愈**的崩溃窗口
 *
 * `Cache::write(milestone)` 对每个 DB 文件的写入序列（cache.cpp 的 write_db_file_wal /
 * write_set_file_wal）是：
 *
 *     WAL 行 → fsync → rename(正式名 → <db>.lpkg_db_bak_before:<milestone>) → 写 .tmp →
 *     fsync → rename(.tmp → 正式名)
 *
 * 进程若在"正式名已经消失、.tmp 还没 rename 回来"这个窗口里死掉（SIGKILL/OOM/段错误/掉电），
 * 盘上就只剩那份备份。批次开头的 5 个 DB 写入都是 `:batch-start` 里程碑，而
 * `wal_op.cpp` 的 reverse_execute 对 `:batch-start` DB 行**无条件跳过**（它是"最终状态标记"，
 * 正常路径下后续各里程碑的逆操作已经把库带回那个状态）—— 于是这份唯一备份永远无人消费：
 *
 *   - `pkgs` / `holdpkgs`：`read_set_from_file` 对缺文件**抛异常**，异常从 recover_packages()
 *     逃出（那个 try 只包住 reverse_execute）→ 恢复本身失败。
 *   - `files.db` / `provides.db` / `confhashes.db`：`read_db_uncached` 把缺文件当**空表**、
 *     不报错 → 恢复"成功" → 紧接着 cleanup_db_backups() 把唯一备份删掉 → 文件归属静默归零、
 *     **不可逆**。
 *
 * 本文件用**盘面**精确复现这两种崩溃现场（手工构造"正式文件不存在 + 备份存在 + WAL 有
 * `:batch-start` 行"），不做 fork/信号注入 —— 与 test_cleanup.cpp / test_wal_edge_cases.cpp
 * 的既有做法一致。第三个用例复现**真实二进制**下的同一现场：`init_filesystem()`（启动时
 * 先跑，把消失的库按空文件重建）→ `recover_packages()`，此时受损的库是"存在但 0 字节"。
 * 第四个用例把同一现场搬到第 5 个库 confhashes.db，并钉住"它必须与兄弟库同一口径地预建"
 * （漏了 ensure_file_exists 时，恢复分支取决于它是第几个加进来的库）。
 *
 * 判据：正常路径下那份备份与正式文件**逐字节相同**吗？恰恰不是 —— 但**在处理到该行时**
 * 正式文件已经被后续各里程碑的逆操作带回了批次起点状态（WAL 里没有更晚的 DB 行时，
 * 正式文件压根没被动过 = 仍是批次起点状态），所以"文件在位 → 跳过"与"从备份还原"在
 * 正常路径上等价；而"文件缺失 + 备份存在"只可能来自上面那个崩溃窗口。
 * 故：**仅在正式文件不存在时**才从该里程碑的备份还原（见 wal_op.cpp 的注释）。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class DbBatchStartRecoveryTest : public IntegrationTestBase
{
protected:
    static constexpr const char* BAK_TAG = ".lpkg_db_bak_before:";

    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        trim_completed();
    }

    static std::string read_bytes(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    static void write_bytes(const fs::path& p, const std::string& data)
    {
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::trunc | std::ios::binary);
        f << data;
        f.flush();
        ASSERT_TRUE(f.good()) << "无法写入 " << p;
    }

    static fs::path bak_of(const fs::path& db, const std::string& milestone)
    {
        return fs::path(db.string() + BAK_TAG + milestone);
    }

    void write_wal(const std::string& content)
    {
        const auto p = wal::wal_log_path();
        fs::create_directories(fs::path(p).parent_path());
        std::ofstream f(p, std::ios::trunc);
        f << content;
        f.flush();
        ASSERT_TRUE(f.good()) << "无法写入 WAL " << p;
    }

    /** 盘上剩余的 .lpkg_db_bak_before:* 备份（base 名） */
    std::vector<std::string> leftover_db_backups() const
    {
        std::vector<std::string> out;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(Config::instance().state_dir(), ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->path().filename().string().find(BAK_TAG) != std::string::npos)
                out.push_back(it->path().filename().string());
        }
        return out;
    }
};

// ============================================================================
// pkgs：缺库 → 旧行为下 read_set_from_file 抛异常，整个恢复没能完成
// ============================================================================

TEST_F(DbBatchStartRecoveryTest, PkgsMissingIsRestoredFromBatchStartBackup)
{
    const fs::path pkgs = Config::instance().pkgs_file();
    // 崩溃前的库内容（注释行是**故意的**：逐字节比对要能抓住"重新序列化"的实现）
    const std::string pre_crash = "alpha:1.0\nbeta:2.3\n";

    // ── 崩溃盘面：进程死在 "pkgs 已 rename 成备份、.tmp 还没 rename 回正式名" 的窗口 ──
    write_bytes(bak_of(pkgs, ":batch-start"), pre_crash);
    fs::remove(pkgs);
    ASSERT_FALSE(fs::exists(pkgs));
    // 该窗口里 WAL 只写了批次开头第一条 DB 行（Cache::write 顺序：pkgs 第一）
    write_wal("BEGIN_PKGS 3\nDB " + pkgs.string() + " :batch-start\n");

    ASSERT_NO_THROW(recover_packages()) << "缺库不得让恢复整体抛出（pkgs 的备份必须被消费）";

    ASSERT_TRUE(fs::exists(pkgs)) << "pkgs 必须从 :batch-start 备份还原";
    EXPECT_EQ(read_bytes(pkgs), pre_crash) << "还原内容必须与崩溃前逐字节相同";

    EXPECT_TRUE(Cache::instance().is_installed("alpha"));
    EXPECT_EQ(Cache::instance().get_installed_version("alpha"), "1.0");
    EXPECT_EQ(Cache::instance().get_installed_version("beta"), "2.3");

    EXPECT_TRUE(leftover_db_backups().empty())
        << "备份已被消费，不该有残留（残留清单见下）: " << leftover_db_backups().size();

    // 恢复后工具必须能继续正常干活：装一个包、库能正常写入
    const std::string p = create_pkg("after_recovery", "1.0");
    ASSERT_NO_THROW(install_packages({p})) << "恢复后的一次 install 必须能正常跑";
    EXPECT_EQ(Cache::instance().get_installed_version("after_recovery"), "1.0");
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/after_recovery"));
    // 老记录仍在（没有因为恢复而丢）
    EXPECT_TRUE(Cache::instance().is_installed("alpha"));
}

// ============================================================================
// files.db：缺库 → 旧行为下"恢复成功"但库空、备份被 cleanup 删掉（不可逆）
// ============================================================================

TEST_F(DbBatchStartRecoveryTest, FilesDbMissingIsRestoredFromBatchStartBackup)
{
    const fs::path files_db = Config::instance().files_db();
    // 与真实序列化同形：键 = 逻辑路径，取值 = **属主包名**（多属主用 ',' 连接，见
    // cache.cpp 的 join_sorted + read_db_uncached）
    const std::string pre_crash =
        "/usr/bin/alpha\talpha\n"
        "/usr/bin/beta\talpha,beta\n";

    write_bytes(bak_of(files_db, ":batch-start"), pre_crash);
    fs::remove(files_db);
    ASSERT_FALSE(fs::exists(files_db));
    write_wal("BEGIN_PKGS 3\nDB " + files_db.string() + " :batch-start\n");

    ASSERT_NO_THROW(recover_packages());

    ASSERT_TRUE(fs::exists(files_db))
        << "files.db 必须从 :batch-start 备份还原（旧行为：库空且备份被删）";
    EXPECT_EQ(read_bytes(files_db), pre_crash) << "文件归属记录必须逐字节还原";

    EXPECT_TRUE(Cache::instance().is_file_owned_by("/usr/bin/alpha", "alpha"));
    EXPECT_TRUE(Cache::instance().is_file_owned_by("/usr/bin/beta", "beta"));

    EXPECT_TRUE(leftover_db_backups().empty())
        << "备份已被消费，不该有残留（残留 = 归属再也回不来的那种不可逆状态）";
}

// ============================================================================
// 防回归：正式文件在位（正常路径）时必须继续跳过 :batch-start 行
// ============================================================================

TEST_F(DbBatchStartRecoveryTest, BatchStartIsStillSkippedWhenOfficialFileExists)
{
    const fs::path pkgs = Config::instance().pkgs_file();
    // 正式文件在位 = 没落在崩溃窗口里；备份内容**故意与正式文件不同** ——
    // 若把"文件在位也还原"当成放宽，这里就会把正式库改成过期内容（静默回退状态）。
    const std::string official = "keeper:9.9\n";
    const std::string stale_backup = "ghost:1.1\n";
    write_bytes(pkgs, official);
    write_bytes(bak_of(pkgs, ":batch-start"), stale_backup);
    write_wal("BEGIN_PKGS 3\nDB " + pkgs.string() + " :batch-start\n");

    ASSERT_NO_THROW(recover_packages());

    EXPECT_EQ(read_bytes(pkgs), official) << "正式文件在位时，:batch-start 行必须继续跳过";
    EXPECT_EQ(read_bytes(pkgs).find("ghost"), std::string::npos)
        << "备份内容绝不能覆盖在位的正式库";
    EXPECT_TRUE(Cache::instance().is_installed("keeper"));
}

// ============================================================================
// 同一个崩溃窗口在**真实启动顺序**下的形状：库被 init_filesystem 按空文件重建
//
// main.cpp 的启动顺序是 init_filesystem()（约 397 行）→ recover_packages()（约 400 行），
// 而 init_filesystem 的 ensure_file_exists 会把"消失的"库**按 0 字节重建**（config.cpp）。
// 于是崩在窗口里的库到恢复时是"存在但空"而不是"不存在" —— 只看 fs::exists 就会把它当成
// "仍在位"跳过，后果与不修完全一样：空库 + 唯一备份被 cleanup_db_backups 删掉（不可逆）。
// 本用例把启动顺序原样搬进来（删文件 → init_filesystem() → recover_packages()）。
// ============================================================================

TEST_F(DbBatchStartRecoveryTest, RecreatedEmptyPkgsIsRestoredFromBatchStartBackup)
{
    const fs::path pkgs = Config::instance().pkgs_file();
    const std::string pre_crash = "alpha:1.0\nbeta:2.3\n";

    write_bytes(bak_of(pkgs, ":batch-start"), pre_crash);
    fs::remove(pkgs);
    write_wal("BEGIN_PKGS 3\nDB " + pkgs.string() + " :batch-start\n");
    // main.cpp 的启动顺序：init_filesystem（ensure_file_exists 把缺库建成空文件）→ 恢复
    Config::instance().init_filesystem();
    ASSERT_TRUE(fs::exists(pkgs));
    ASSERT_EQ(read_bytes(pkgs), std::string()) << "前置条件：库已被按空文件重建";

    ASSERT_NO_THROW(recover_packages());

    EXPECT_EQ(read_bytes(pkgs), pre_crash)
        << "空库 + 非空备份 = 内容丢了（不是'批次起点本来就是空库'），必须从备份还原";
    EXPECT_TRUE(Cache::instance().is_installed("alpha"));
    EXPECT_TRUE(leftover_db_backups().empty());
}

// ============================================================================
// 第 5 个库 confhashes.db：同一个崩溃窗口 + 它必须与兄弟库**同一个口径**地预建
//
// init_filesystem() 对 pkgs / holdpkgs / essential / files.db / provides.db 都
// ensure_file_exists，只有 confhashes.db 漏了。两个后果：
//   · 恢复判据的口径分歧：崩溃窗口里消失的库，兄弟库会被按 0 字节重建（于是走
//     batch_start_db_still_in_place 的"空文件 + 非空备份 → 还原"），它却停在"文件不存在"
//     那一支 —— 同一次崩溃、同一族文件，恢复路径不该取决于它是第几个加进来的；
//   · 备份链的算术（Cache::write 对一族逐个"备份 + 重写"）：文件不预建时首次写入走 DBNEW、
//     **不产生** :batch-start 备份，于是"每里程碑一份备份"对这一个库不成立
//     （tests/integration/test_db_backup_chain.cpp 的计数用例红）。
// 本用例把这两件事钉在一起：先断言预建（与兄弟库对齐），再走一遍真实启动顺序下的
// "崩溃窗口 → rec 还原"，确认预建**没有**把这条恢复路径弄坏。
// ============================================================================

TEST_F(DbBatchStartRecoveryTest, ConfHashesDbIsPrecreatedAndRestoredFromBatchStartBackup)
{
    const fs::path conf_db = Config::instance().conf_hashes_db();
    // 与 files.db / provides.db 同一口径：init_filesystem()（SetUp 里已调用）预建空文件
    ASSERT_TRUE(fs::exists(conf_db))
        << "confhashes.db 没被 init_filesystem 预建 —— 它与 files.db/provides.db 是同一族，"
           "口径不该不一样";
    ASSERT_EQ(read_bytes(conf_db), std::string()) << "预建的必须是空文件";

    // 与真实序列化同形：键 = 逻辑路径，取值 = "<pkg>:<sha256>"
    const std::string hash =
        "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";  // 64 位十六进制
    const std::string pre_crash = "/etc/foo.conf\tcurl:" + hash + "\n";

    // ── 崩溃盘面：confhashes.db 已 rename 成备份、.tmp 还没 rename 回正式名 ──
    write_bytes(bak_of(conf_db, ":batch-start"), pre_crash);
    fs::remove(conf_db);
    write_wal("BEGIN_PKGS 3\nDB " + conf_db.string() + " :batch-start\n");
    // main.cpp 的启动顺序：init_filesystem（把窗口里消失的库按空文件重建）→ recover_packages
    Config::instance().init_filesystem();
    ASSERT_TRUE(fs::exists(conf_db));
    ASSERT_EQ(read_bytes(conf_db), std::string()) << "前置条件：库已被按空文件重建";

    ASSERT_NO_THROW(recover_packages());

    EXPECT_EQ(read_bytes(conf_db), pre_crash)
        << "空库 + 非空备份 = 内容丢了（不是'批次起点本来就是空库'），必须从 :batch-start 备份"
           "还原 —— 预建空文件不得把这条恢复路径变成'文件在位 → 跳过'";
    // 还原的内容**真的**进了内存 Cache（不只是盘上的一串字节）
    EXPECT_EQ(Cache::instance().get_conf_hash("/etc/foo.conf", "curl"), hash);
    EXPECT_TRUE(leftover_db_backups().empty())
        << "备份已被消费，不该有残留（残留 = 这份记录再也回不来的那种不可逆状态）";
}

// ============================================================================
// 容错：连备份都没有（缺库且无处可还原）时，恢复自身不得被打挂；
//       但"库丢了"也不能退化成"静默空库" —— 正常操作路径仍然报错
// ============================================================================

TEST_F(DbBatchStartRecoveryTest, MissingSetFileWithoutBackupDoesNotAbortRecovery)
{
    const fs::path pkgs = Config::instance().pkgs_file();
    fs::remove(pkgs);  // 缺库 + 无备份（备份已被删/从未写出）
    ASSERT_FALSE(fs::exists(pkgs));
    write_wal("BEGIN_PKGS 3\nDB " + pkgs.string() + " :batch-start\n");

    // 恢复路径容忍"文件不存在"：不能让一条缺失记录把整个恢复（含 WAL 收尾、
    // 其余文件的还原、备份清理）全部作废
    EXPECT_NO_THROW(recover_packages()) << "缺库不得让整个 recover_packages() 打挂";

    // 但**没有**退化成"静默空库"：正常操作路径的加载仍然必须报错
    EXPECT_THROW(Cache::instance().load(), LpkgException)
        << "缺库在正常路径上必须报错 —— 不得退化成静默空库";
}
