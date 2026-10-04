/**
 * test_db_backup_chain.cpp — DB 批次内**只落盘一次**，快照恒两份（2026-10-03 改）
 *
 * `Cache::write(milestone)` 对 **DB 一族的 6 个文件**（见 test_base.hpp 的 db_family_files：
 * pkgs / files.db / provides.db / confhashes.db / xattrkeys.db / holdpkgs）各做一次"备份原文件 +
 * 全量重写"。它现在每批次只被调**两次**：`BEGIN_PKGS` 之后的 `:batch-start`，以及提交之前的
 * `:batch-end` ⇒ N 包批次恒为 6×2 份副本，**与 N 无关**。
 *
 * ⚠️ **本文件 2026-10-03 有意推翻了自己原来的钉法**。原文把"每里程碑一份、副本数随批次增长"
 * 当**保守设计正面钉住**，理由是"收益（省 IO、少几个崩溃窗口）不抵代价（改动落在最难测的
 * 恢复路径上）"。推翻的不是**保证**，是**表示**：查实了两件事 ——
 *   ① 批次进行中**没有任何读取器**读盘上的 DB（`Cache::load()` 的调用点全在批次之外，
 *      循环内一律走内存 `Cache`）；
 *   ② 未提交批次**一律整体回滚**，所以中途的盘上状态既不可观测、也不可能成为最终状态。
 * 于是"每包一个还原点"换不来任何可观测的东西，却让 100 包批次在 `/var/lib/lpkg` 落下
 * ~2 GB 临时备份（本机 `files.db` 实测 19.8 MB / 286k 行，整仓升级 ~15 GB）。
 * 现在唯一的还原点是 `:batch-start`，加一份提交前的 `:batch-end`。
 *
 * 清单**不在这里硬编码**：一族里加 confhashes.db / xattrkeys.db 时硬编码清单就是各自漏掉它
 * 的地方 —— 本文件三处计数/快照与 test_upgrade_rollback_fidelity.cpp 的 db_state() 都从
 * db_family_files() 取。
 *
 * 本文件钉住：
 *   ① **副本数恒为 6×2**，批次中途只有 `:batch-start` 一份，**不随批次大小增长**；
 *      提交后清干净（0）。
 *   ② **批次中途盘上的 DB 停在批次前**，WAL 里**没有**包级 DB 行 —— "逐包写入已取消"的
 *      可观测形式。
 *   ③ **新窗口**：`:batch-end` 行已落、`COMMIT_PKGS` 未写时崩溃 ⇒ 整批（含这次 DB 写）
 *      逐字节退回批次前。这个窗口在逐包写入时代**不存在**，此前没有用例走过它。
 *   ④ **端到端不变式**（与备份表示无关，改动不得破坏）：批次中途崩溃后 `rec` 收敛出与批次前
 *      **逐字节相同**的 DB（**全部 6 个库**，含 confhashes.db）；正常失败回滚、单包失败、
 *      升级失败（含 deps/man 元数据与被 DBRM 删空的文件）同样逐字节还原。
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class DbBackupChainTest : public IntegrationTestBase
{
protected:
    static constexpr const char* BAK_TAG = ".lpkg_db_bak_before:";

    /** DB 一族的 6 个文件（清单只在 test_base.hpp 的 db_family_files 里有一份） */
    std::vector<fs::path> db_files() const
    {
        return db_family_files();
    }
    /** 同族文件数 —— "每里程碑一份"的算术全基于它，不写死 4 */
    int db_file_count() const
    {
        return static_cast<int>(db_family_files().size());
    }

    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        Config::instance().set_no_hooks_mode(true);
        BreakpointManager::instance().clear_all();
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        IntegrationTestBase::TearDown();
    }

    static std::string read_text(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    std::string read_wal() const
    {
        return read_text(wal::wal_log_path());
    }

    /**
     * 打一个包：内容是一个 `usr/bin/<name>` + 一个 `/etc/<name>.conf`，man 页固定。
     *
     * `/etc` 条目是**故意**加的：它让 `confhashes.db` 在整个文件里始终有**真实内容**
     * （每次安装/升级都写一条记录）。少了它，本文件那几条"DB 逐字节回到批次前"的不变量对
     * confhashes.db 就退化成"空文件 == 空文件"的恒真断言 —— 覆盖是假的。
     */
    std::string pack(const std::string& name, const std::string& ver,
                     const std::vector<std::string>& deps = {})
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::remove_all(work);
        fs::create_directories(work / "content/usr/bin");
        fs::create_directories(work / "content/etc");
        std::ofstream(work / "content/usr/bin" / name) << name << " " << ver << "\n";
        // 内容带版本号：升级批次里这份配置**真的会变**（走 ① 静默替换），不是恒等不动
        std::ofstream(work / "content/etc" / (name + ".conf")) << name << "-conf " << ver << "\n";
        const std::string path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(path, work.string(), name, ver, deps, {}, {}, "man " + name, {});
        return path;
    }

    /** 盘上所有 `.lpkg_db_bak_before:*` 的 base 名 → 副本数 */
    std::map<std::string, int> bak_copies_per_file() const
    {
        std::map<std::string, int> counts;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(test_root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const std::string name = it->path().filename().string();
            const auto pos = name.find(BAK_TAG);
            if (pos == std::string::npos) continue;
            ++counts[name.substr(0, pos)];
        }
        return counts;
    }

    int total_baks() const
    {
        int n = 0;
        for (const auto& [base, cnt] : bak_copies_per_file()) n += cnt;
        return n;
    }

    /** DB 一族（6 个文件）此刻的副本总数 */
    int global_db_baks() const
    {
        const auto counts = bak_copies_per_file();
        int n = 0;
        for (const auto& p : db_files()) {
            if (const auto it = counts.find(p.filename().string()); it != counts.end())
                n += it->second;
        }
        return n;
    }

    /** 某个 DB 文件、某个里程碑的备份路径 */
    static fs::path bak_of(const fs::path& db, const std::string& milestone)
    {
        return fs::path(db.string() + BAK_TAG + milestone);
    }

    /** DB 一族的逐字节快照（含"不存在"） */
    std::map<std::string, std::string> db_bytes() const
    {
        std::map<std::string, std::string> out;
        for (const auto& p : db_files()) {
            std::ifstream f(p, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            out[p.string()] = f.is_open() ? ss.str() : std::string("<不存在>");
        }
        return out;
    }

    void expect_db_unchanged(const std::map<std::string, std::string>& before,
                             const std::string& ctx) const
    {
        for (const auto& [path, bytes] : before)
            EXPECT_EQ(bytes, db_bytes().at(path))
                << ctx << "：DB 文件与批次前不逐字节相同：" << path;
    }
};

// ============================================================================
// ① 副本数**与批次大小无关**：DB 每批次只落盘两次，峰值恒为 6×2。
//    批次**中途**只有 `:batch-start` 一份（`:batch-end` 还没写），所以此刻是 6。
//    （原用例名 PeakBackupCountGrowsWithBatchSize 断言峰值 = 6×(1+N) 且必须随批次增长 ——
//      2026-10-03 有意推翻，理由见文件头。）
// ============================================================================

TEST_F(DbBackupChainTest, PeakBackupCountIsIndependentOfBatchSize)
{
    // 1 包批次
    int peak_1pkg = -1;
    BreakpointManager::instance().set("install_after_begin_solo",
                                      [&] { peak_1pkg = global_db_baks(); });
    ASSERT_NO_THROW(install_packages({pack("solo", "1.0")}));
    BreakpointManager::instance().clear_all();
    ASSERT_GT(peak_1pkg, 0) << "断点没命中（取证无效）";
    EXPECT_EQ(peak_1pkg, db_file_count())
        << "1 包批次：" << db_file_count() << " 个 DB 文件各一份 :batch-start 备份";
    EXPECT_EQ(total_baks(), 0) << "成功批次收尾必须把 DB 备份清干净";

    // 5 包批次（依赖链保证顺序）
    std::vector<std::string> pkgs;
    for (int i = 0; i < 5; ++i) {
        const std::string name = std::string("chain") + std::to_string(i);
        pkgs.push_back(
            pack(name, "1.0",
                 i == 0 ? std::vector<std::string>{}
                        : std::vector<std::string>{std::string("chain") + std::to_string(i - 1)}));
    }
    int peak_5pkg = -1;
    BreakpointManager::instance().set("install_after_begin_chain4", [&] {
        peak_5pkg = global_db_baks();
        // **每个**DB 文件此刻只有 :batch-start 一份 —— 含 confhashes.db
        // （少了它，这一族里就有一个库没人盯）
        for (const auto& [base, cnt] : bak_copies_per_file())
            EXPECT_EQ(cnt, 1) << base << " 在 5 包批次中途只该有 :batch-start 一份（实测 " << cnt
                              << " 份）—— 包级里程碑副本已取消";
    });
    ASSERT_NO_THROW(install_packages(pkgs));
    BreakpointManager::instance().clear_all();

    ASSERT_GT(peak_5pkg, 0) << "断点没命中（取证无效）";
    EXPECT_EQ(peak_5pkg, db_file_count())
        << "5 包批次中途同样只有每个 DB 文件的一份 :batch-start；实测 " << peak_5pkg;
    EXPECT_EQ(peak_5pkg, peak_1pkg)
        << "1 包与 5 包的峰值必须**相同** —— 副本数不随批次大小增长（这是 2026-10-03 的改动点）";
    EXPECT_EQ(total_baks(), 0) << "成功批次收尾必须把 DB 备份清干净";
}

// ============================================================================
// ② 批次中途**没有**包级里程碑备份，也没有对应的 WAL DB 行
//    （原用例 EveryMilestoneHasItsOwnBackupFile 钉的是"每个里程碑一份、与 WAL 行一一对应"
//      —— 2026-10-03 有意推翻，理由见文件头）—— **整族 6 个库逐个枚举**（含 confhashes.db）
// ============================================================================

TEST_F(DbBackupChainTest, NoPerPackageBackupOrWalRowMidBatch)
{
    std::vector<std::string> pkgs = {pack("mb1", "1.0")};
    pkgs.push_back(pack("mb2", "1.0", {"mb1"}));
    pkgs.push_back(pack("mb3", "1.0", {"mb2"}));

    bool fired = false;
    BreakpointManager::instance().set("install_after_begin_mb3", [&] {
        fired = true;
        const std::string wal = read_wal();
        for (const auto& db : db_files()) {
            // 批次开始那份必须在（它是唯一的还原点）
            EXPECT_TRUE(fs::exists(bak_of(db, ":batch-start")))
                << "缺 :batch-start 备份：" << bak_of(db, ":batch-start");
            // mb1 / mb2 已装完，但**不该**留下各自的里程碑备份，也不该有对应的 WAL DB 行
            for (const char* p : {"mb1", "mb2"}) {
                EXPECT_FALSE(fs::exists(bak_of(db, std::string(p) + ":installed")))
                    << "批次中途不该有包级里程碑备份：" << p;
                EXPECT_EQ(wal.find("DB " + db.string() + " " + p + ":installed"), std::string::npos)
                    << "WAL 里不该有包级 DB 行（逐包落盘已取消）：" << p;
            }
        }
    });
    ASSERT_NO_THROW(install_packages(pkgs));
    BreakpointManager::instance().clear_all();
    ASSERT_TRUE(fired) << "断点没命中（取证无效）";
    EXPECT_EQ(total_baks(), 0) << "成功批次收尾必须把 DB 备份清干净";
}

// ============================================================================
// ②' 批次中途**盘上的 DB 停在批次前** —— 这是"逐包写入已取消"最直接的可观测形式：
//     已装完的 mb1/mb2 此刻**不在**盘上的 pkgs 里（而它们在**内存** Cache 里）
// ============================================================================

TEST_F(DbBackupChainTest, OnDiskDbStaysAtBatchStartUntilCommit)
{
    ASSERT_NO_THROW(install_packages({pack("ob0", "1.0")}));
    const fs::path pkgs_db = Config::instance().pkgs_file();
    const std::string before_batch = read_text(pkgs_db);

    std::vector<std::string> batch = {pack("ob1", "1.0")};
    batch.push_back(pack("ob2", "1.0", {"ob1"}));

    bool fired = false;
    BreakpointManager::instance().set("install_after_begin_ob2", [&] {
        fired = true;
        const std::string live = read_text(pkgs_db);
        EXPECT_EQ(live, before_batch)
            << "批次中途盘上的 DB 必须**逐字节等于批次前** —— 改变了说明又有人在逐包落盘了";
        EXPECT_NE(live.find("ob0:1.0"), std::string::npos)
            << "ob0 应在；下面那条断言才有区分力（否则是空文件比空文件）";
        EXPECT_EQ(live.find("ob1:1.0"), std::string::npos)
            << "ob1 已经装完，但盘上的 DB 里不该有它（批次未提交）";
        // 内存 Cache 是对的 —— 批次内的判定一律走它，不走盘
        EXPECT_EQ(Cache::instance().get_installed_version("ob1"), "1.0")
            << "内存里必须已经记上 ob1（否则批次内的依赖判定会瞎）";
    });
    ASSERT_NO_THROW(install_packages(batch));
    BreakpointManager::instance().clear_all();
    ASSERT_TRUE(fired) << "断点没命中（取证无效）";

    // 提交之后才落盘：最终 DB 里三个包都在
    const std::string final_db = read_text(pkgs_db);
    for (const char* p : {"ob0:1.0", "ob1:1.0", "ob2:1.0"})
        EXPECT_NE(final_db.find(p), std::string::npos) << "最终 DB 里缺 " << p;
}

// ============================================================================
// ③ `:batch-start` 那份备份的内容 = **批次前**的状态（唯一的还原点）
//    （原用例 BackupContentsFormTheMilestoneChain 钉的是链式递进语义 —— 2026-10-03 有意
//      推翻，理由见文件头）
// ============================================================================

TEST_F(DbBackupChainTest, BatchStartBackupHoldsThePreBatchState)
{
    // 先装 ch0：让"批次前状态"非空，"等于批次前"这条断言才有区分力
    ASSERT_NO_THROW(install_packages({pack("ch0", "1.0")}));
    const fs::path pkgs_db = Config::instance().pkgs_file();
    const std::string before_batch = read_text(pkgs_db);

    std::vector<std::string> batch = {pack("ch1", "1.0")};
    batch.push_back(pack("ch2", "1.0", {"ch1"}));

    bool fired = false;
    std::string start_bak;
    // ch2 开始装的那一刻：ch1 已装完（内存里），但盘上不该有任何包级里程碑备份
    BreakpointManager::instance().set("install_after_begin_ch2", [&] {
        fired = true;
        start_bak = read_text(bak_of(pkgs_db, ":batch-start"));
        EXPECT_FALSE(fs::exists(bak_of(pkgs_db, "ch1:installed")))
            << "批次中途不该出现包级里程碑备份";
    });

    ASSERT_NO_THROW(install_packages(batch));
    BreakpointManager::instance().clear_all();
    ASSERT_TRUE(fired) << "断点没命中（取证无效）";

    // `:batch-start` 是**唯一**的还原点，它的内容 = 批次**前**的状态
    EXPECT_EQ(start_bak, before_batch) << ":batch-start 备份必须逐字节等于批次前的 DB";
    EXPECT_NE(start_bak.find("ch0:1.0"), std::string::npos)
        << "取证无效：批次前的状态是空的，下面那条就成了恒真废话";
    EXPECT_EQ(start_bak.find("ch1:1.0"), std::string::npos)
        << "批次**前**的备份里不该有本批次才装的包";

    // 最终状态：ch0/ch1/ch2 都在
    const std::string final_db = read_text(pkgs_db);
    for (const char* p : {"ch0:1.0", "ch1:1.0", "ch2:1.0"})
        EXPECT_NE(final_db.find(p), std::string::npos) << "最终 DB 里缺 " << p;
}

// ============================================================================
// ④ 升级批次同样每里程碑一份（整族 6 个 DB 文件都已存在 → 每包写完都落一份）
// ============================================================================

// ============================================================================
// ④ 升级批次同样**不**留包级里程碑备份（原用例 UpgradeBatchKeepsPerPackageBackups
//    断言相反的事 —— 2026-10-03 有意推翻）
// ============================================================================

TEST_F(DbBackupChainTest, UpgradeBatchLeavesNoPerPackageBackups)
{
    std::vector<std::string> v1;
    v1.reserve(3);
    for (int i = 0; i < 3; ++i) v1.push_back(pack(std::string("up") + std::to_string(i), "1.0"));
    ASSERT_NO_THROW(install_packages(v1));
    ASSERT_EQ(total_baks(), 0) << "成功批次收尾必须把 DB 备份清干净";

    std::vector<std::string> v2;
    v2.reserve(3);
    for (int i = 0; i < 3; ++i) v2.push_back(pack(std::string("up") + std::to_string(i), "2.0"));

    // 升级顺序由求解器定，不假设哪个包最后 —— 取三处断点观测值的**最大值**（即峰值）
    bool fired = false;
    int peak = 0;
    int pkgs_baks = 0;
    for (const char* n : {"up0", "up1", "up2"}) {
        // 不捕获 `n`：闭包里没用到它（捕获了却不引用的，clang 会报 -Wunused-lambda-capture）
        BreakpointManager::instance().set(std::string("install_after_begin_") + n, [&] {
            fired = true;
            peak = std::max(peak, global_db_baks());
            const auto counts = bak_copies_per_file();  // 先落地：别跨两个临时表比较迭代器
            if (const auto it = counts.find("pkgs"); it != counts.end()) pkgs_baks = it->second;
        });
    }
    ASSERT_NO_THROW(install_packages(v2));
    BreakpointManager::instance().clear_all();
    ASSERT_TRUE(fired) << "断点没命中（取证无效）";

    EXPECT_EQ(peak, db_file_count())
        << "升级批次中途也只该有每个 DB 文件的一份 :batch-start；实测 " << peak;
    EXPECT_EQ(pkgs_baks, 1) << "pkgs 此刻只该有 :batch-start 一份；实测 " << pkgs_baks;
    // 内存里升级确实发生了（DB 落盘在批次末尾，见 write_batch_db）
    EXPECT_EQ(Cache::instance().get_installed_version("up0"), "2.0");
    EXPECT_EQ(total_baks(), 0) << "成功批次收尾必须把 DB 备份清干净";
}

// ============================================================================
// ⑤ 批次之间互不影响：**前一次操作的**备份已清干净，**下一次操作**从自己的 :batch-start 起。
//
// 注意"批次"的口径：**一条命令 = 一个批次**（`run_batch_transaction` 全项目只有三个调用点 ——
// `install_packages` / `remove_packages_in_one_batch` / `upgrade_packages`，互不嵌套）。
// 本用例是在**同一个测试进程里连调三次 `install_packages()`**，等于模拟**三条命令**，
// 所以这里说的是"三个批次之间"，不是"一条命令内的两批"。
// ============================================================================

TEST_F(DbBackupChainTest, BatchesDoNotShareBackupChain)
{
    ASSERT_NO_THROW(install_packages({pack("seq1", "1.0")}));
    ASSERT_EQ(total_baks(), 0);

    int baks_mid = -1;
    BreakpointManager::instance().set("install_after_begin_seq2",
                                      [&] { baks_mid = global_db_baks(); });
    ASSERT_NO_THROW(install_packages({pack("seq2", "1.0")}));
    BreakpointManager::instance().clear_all();
    EXPECT_EQ(baks_mid, db_file_count())
        << "第二个批次的链必须从自己的 :batch-start 重新开始（不该带上前一批的副本）";

    // 第三个批次失败 → DB 逐字节回到它开始前（即只有 seq1+seq2）
    const auto before = db_bytes();
    const std::string s3 = pack("seq3", "1.0");
    BreakpointManager::instance().set("install_after_begin_seq3",
                                      [] { throw LpkgException("injected third-batch failure"); });
    EXPECT_THROW(install_packages({s3}), LpkgException);
    BreakpointManager::instance().clear_all();
    expect_db_unchanged(before, "第三批失败回滚后");
    EXPECT_EQ(total_baks(), 0);
}

// ============================================================================
// ⑥ 端到端不变式：批次中途**崩溃**（不走 catch 的 batch_rollback）后 rec 收敛，
//    DB 必须逐字节等于批次前
// ============================================================================

TEST_F(DbBackupChainTest, RecoverAfterMidBatchCrashRestoresByteIdenticalDb)
{
    ASSERT_NO_THROW(install_packages({pack("base1", "1.0")}));
    ASSERT_NO_THROW(install_packages({pack("base2", "1.0", {"base1"})}));

    const auto before = db_bytes();
    ASSERT_EQ(total_baks(), 0);

    std::vector<std::string> batch = {pack("cr1", "1.0")};
    batch.push_back(pack("cr2", "1.0", {"cr1"}));
    batch.push_back(pack("cr3", "1.0", {"cr2"}));

    // 崩溃注入：cr3 的 WAL BEGIN 之后抛一个**非 std::exception** 类型 ——
    // run_batch_transaction 的 catch (const std::exception&) 接不住 → 整批不回滚，
    // 进程状态等同"断电死在这里"（WAL 里留下未提交批次 + 前两个包已完整装完）
    BreakpointManager::instance().set("install_after_begin_cr3", [] { throw 42; });
    bool crashed = false;
    try {
        install_packages(batch);
    } catch (int) {
        crashed = true;
    }
    BreakpointManager::instance().clear_all();
    ASSERT_TRUE(crashed) << "崩溃注入没生效（断点未命中？）";
    ASSERT_EQ(read_wal().find("COMMIT_PKGS"), std::string::npos) << "批次必须仍处于未提交状态";
    ASSERT_EQ(Cache::instance().get_installed_version("cr1"), "1.0")
        << "cr1 确实装完了（崩溃点之前）";

    // 崩溃现场：每个 DB 文件都留着 `:batch-start` 那份 —— 它是 `rec` 唯一的回退依据。
    // （2026-10-03 前这里断言的是"每个已装包的里程碑各留一份"；逐包落盘取消后不再有那些，
    //   但**回退依据仍然在**，所以下面"逐字节回到批次前"的断言照旧成立。）
    for (const auto& db : db_files()) {
        EXPECT_TRUE(fs::exists(bak_of(db, ":batch-start")))
            << "崩溃现场缺少 :batch-start 备份（rec 就没有回退依据了）：" << db;
    }
    // 崩溃点在 cr3 的 BEGIN 之后、批次末尾的 DB 落盘之前 ⇒ 连 :batch-end 都还没写
    EXPECT_FALSE(fs::exists(bak_of(Config::instance().pkgs_file(), ":batch-end")))
        << "崩溃发生在批次末尾落盘之前，不该有 :batch-end";

    ASSERT_NO_THROW(recover_packages());
    trim_completed();
    cleanup_db_backups();

    // 端到端不变式：DB 逐字节回到批次前 + 文件/所有权都退回去
    expect_db_unchanged(before, "崩溃恢复后");
    EXPECT_EQ(total_baks(), 0);
    for (const auto& n : {"cr1", "cr2", "cr3"})
        EXPECT_TRUE(Cache::instance().get_installed_version(n).empty())
            << n << " 未回滚干净（批次是全或无）";
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/cr1"));
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/base1")) << "批次前的包被误删";
}

// ============================================================================
// ⑥' 同一不变式的"主动回滚"侧：正常失败（catch → batch_rollback）后 DB 同样逐字节相同
// ============================================================================

TEST_F(DbBackupChainTest, ActiveRollbackAlsoRestoresByteIdenticalDb)
{
    ASSERT_NO_THROW(install_packages({pack("abase", "1.0")}));
    const auto before = db_bytes();

    std::vector<std::string> batch = {pack("ar1", "1.0")};
    batch.push_back(pack("ar2", "1.0", {"ar1"}));
    batch.push_back(pack("ar3", "1.0", {"ar2"}));

    BreakpointManager::instance().set("install_after_begin_ar3",
                                      [] { throw LpkgException("injected mid-batch failure"); });
    EXPECT_THROW(install_packages(batch), LpkgException);
    BreakpointManager::instance().clear_all();

    expect_db_unchanged(before, "主动回滚后");
    EXPECT_EQ(read_wal().find("BEGIN_PKGS"), std::string::npos) << "批次未收尾";
    EXPECT_TRUE(Cache::instance().get_installed_version("ar1").empty());
    EXPECT_EQ(total_baks(), 0) << "回滚已收尾，DB 备份应被消费并清理干净";
}

// ============================================================================
// ⑥'' **新窗口**：`:batch-end` 行已落、`COMMIT_PKGS` 未写时失败 ⇒ 整批（含这次 DB 写）
//      逐字节退回批次前。
//
// 这个窗口是 2026-10-03 的改动**新引入**的：DB 改成批次末尾写一次之前，最后一个包的
// COMMIT 之后就不再有 DB 写要撤。现在批次末尾多了一次 DB 落盘，而它**必须在 COMMIT_PKGS
// 之前**完成 —— 否则崩溃留下"批次已提交、DB 还是旧的"，而**已提交批次不会被回滚**，
// 没有任何机制能修回来。本用例把"它确实发生在提交之前（= 可回滚）"钉住：
// 断点命中说明那次落盘真的执行到了，随后的关断说明**它被完整撤回了**。
// ============================================================================

TEST_F(DbBackupChainTest, FailureAfterBatchDbWriteRollsBackByteIdentically)
{
    ASSERT_NO_THROW(install_packages({pack("nb0", "1.0")}));
    const auto before = db_bytes();

    std::vector<std::string> batch = {pack("nb1", "1.0")};
    batch.push_back(pack("nb2", "1.0", {"nb1"}));

    bool fired = false;
    BreakpointManager::instance().set("batch_db_before_commit", [&] {
        fired = true;
        throw LpkgException("injected failure right after the batch DB write");
    });
    EXPECT_THROW(install_packages(batch), LpkgException);
    BreakpointManager::instance().clear_all();
    ASSERT_TRUE(fired) << "断点 `batch_db_before_commit` 没命中 —— 批次末尾那次 DB 落盘不见了？"
                          "或它被挪到了 COMMIT_PKGS **之后**（那才是真问题：已提交批次不回滚）";

    expect_db_unchanged(before, "批次末尾 DB 落盘之后失败、回滚后");
    for (const auto& n : {"nb1", "nb2"})
        EXPECT_TRUE(Cache::instance().get_installed_version(n).empty())
            << n << " 未回滚干净（批次是全或无）";
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/nb1")) << "批次回滚必须把文件也退回去";
    EXPECT_EQ(total_baks(), 0) << "回滚已收尾，DB 备份应被消费并清理干净";
}

// ============================================================================
// ⑦ 单包批次失败：这个包自己失败时 DB 也要回到批次前
// ============================================================================

TEST_F(DbBackupChainTest, SinglePackageFailureRestoresDb)
{
    ASSERT_NO_THROW(install_packages({pack("sbase", "1.0")}));
    const auto before = db_bytes();

    BreakpointManager::instance().set("install_after_begin_sonly",
                                      [] { throw LpkgException("injected single-pkg failure"); });
    EXPECT_THROW(install_packages({pack("sonly", "1.0")}), LpkgException);
    BreakpointManager::instance().clear_all();

    expect_db_unchanged(before, "单包批次失败回滚后");
    EXPECT_EQ(total_baks(), 0);
    EXPECT_TRUE(Cache::instance().get_installed_version("sonly").empty());
}

// ============================================================================
// ⑧ 升级失败回滚：旧版本的 per-package 元数据（deps / man）逐字节回来
// ============================================================================

TEST_F(DbBackupChainTest, FailedUpgradeRestoresPerPackageMetadataBytes)
{
    ASSERT_NO_THROW(install_packages({pack("libdep", "1.0")}));
    ASSERT_NO_THROW(install_packages({pack("meta1", "1.0")}));

    const fs::path dep = Config::instance().dep_dir() / "meta1";
    const fs::path man = Config::instance().docs_dir() / "meta1.man";
    const std::string dep_before = read_text(dep);
    const std::string man_before = read_text(man);
    const auto db_before = db_bytes();
    ASSERT_TRUE(fs::exists(man)) << "fixture 自检：man 元数据文件应已存在";
    ASSERT_EQ(dep_before, "") << "fixture 自检：v1 无依赖 → deps 文件是空内容";

    // v2 新增依赖 → deps/meta1 被改写（DB 分支：先备份后覆盖）
    const std::string v2 = pack("meta1", "2.0", {"libdep"});
    bool fired = false;
    std::string dep_mid;
    BreakpointManager::instance().set("after_commit_meta1", [&] {
        fired = true;
        dep_mid = read_text(dep);
        throw LpkgException("injected upgrade failure");
    });
    EXPECT_THROW(install_packages({v2}), LpkgException);
    BreakpointManager::instance().clear_all();

    ASSERT_TRUE(fired) << "断点没命中（取证无效）";
    EXPECT_NE(dep_mid, dep_before) << "取证无效：断点时 deps 元数据还没被改写过";
    EXPECT_EQ(read_text(dep), dep_before) << "升级失败后 deps 元数据没有逐字节恢复";
    EXPECT_EQ(read_text(man), man_before) << "升级失败后 man 元数据没有逐字节恢复";
    expect_db_unchanged(db_before, "升级失败回滚后");
    EXPECT_EQ(Cache::instance().get_installed_version("meta1"), "1.0");
    EXPECT_EQ(total_baks(), 0);
}

// ============================================================================
// ⑧' 升级把某个元数据文件**清空**（DBRM：备份后删除）时，回滚同样逐字节还原
// ============================================================================

TEST_F(DbBackupChainTest, FailedUpgradeRestoresMetadataRemovedByDependencyDrop)
{
    ASSERT_NO_THROW(install_packages({pack("libdep2", "1.0")}));
    ASSERT_NO_THROW(install_packages({pack("meta2", "1.0", {"libdep2"})}));

    const fs::path dep = Config::instance().dep_dir() / "meta2";
    const std::string dep_before = read_text(dep);
    ASSERT_EQ(dep_before, "libdep2\n") << "fixture 自检：v1 的依赖应写进 deps 文件";

    const std::string v2 = pack("meta2", "2.0");
    bool fired = false;
    BreakpointManager::instance().set("after_commit_meta2", [&] {
        fired = true;
        EXPECT_FALSE(fs::exists(dep)) << "取证无效：断点时 deps 文件应已被 DBRM 删除";
        throw LpkgException("injected upgrade failure");
    });
    EXPECT_THROW(install_packages({v2}), LpkgException);
    BreakpointManager::instance().clear_all();

    ASSERT_TRUE(fired) << "断点没命中（取证无效）";
    EXPECT_EQ(read_text(dep), dep_before) << "升级失败后（依赖被丢掉的）deps 元数据没有还原";
    EXPECT_EQ(Cache::instance().get_installed_version("meta2"), "1.0");
    EXPECT_EQ(total_baks(), 0);
}
