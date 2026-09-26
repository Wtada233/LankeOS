/**
 * test_db_backup_chain.cpp — DB 备份链是**每个里程碑一份**（保守选择，2026-09 复核后明确保留）
 *
 * `Cache::write(milestone)` 对 **DB 一族的 6 个文件**（见 test_base.hpp 的 db_family_files：
 * pkgs / files.db / provides.db / confhashes.db / xattrkeys.db / holdpkgs）各做一次"备份原文件 +
 * 全量重写"， 批次循环**每装完一个包**就调一次 → N 包批次落 6×(N+1) 份全量副本（`:batch-start` 一份
 * + 每包一份）。这是**有意保留的保守设计**：收益（省 IO、少几个崩溃窗口）不抵代价（改动落在
 * 最难测的"崩溃条件下的恢复"路径上），每里程碑一份还原点让任何一条 WAL DB 行都能就地恢复。
 *
 * 清单**不在这里硬编码**：一族里加 confhashes.db 时，硬编码的 4 元素清单就是
 * 各自漏掉它的地方 —— 本文件的三处（备份计数、逐字节快照、里程碑枚举）与
 * test_upgrade_rollback_fidelity.cpp 的 db_state() 现在都从 db_family_files() 取。
 *
 * 订正 2026-09-26：**同一处分歧又发生了一次** —— 加 `xattrkeys.db` 时那个唯一的清单没跟着
 * 加（清单在 test_base.hpp，不在本文件，所以本文件根本没机会发现）。已修。本文件的算术是
 * 从清单派生的，因此自动跟着从 5 变成 6 —— 这正是当初把清单抽出来的用意。
 *
 * 本文件钉住这条链的两个可观测性质 + 端到端不变式：
 *   ① **副本数随批次大小增长**：峰值 = 6×(1+N)（`:batch-start` + 每包一份），每装一个包
 *      多一份；提交后清干净（0）。
 *   ② **每个里程碑都有自己的备份文件**，且内容就是**该里程碑之前**的状态（链式语义：
 *      `:batch-start` 里没有 p1、`p1:installed` 里有 p1 没有 p2）——WAL 行与备份文件一一对应，
 *      这正是"任何一条 DB 行都能就地恢复"的依据。
 *   ③ **端到端不变式**（与备份链无关，改动不得破坏）：批次中途崩溃后 `rec` 收敛出与批次前
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
        pack_package(path, work.string(), name, ver, deps, {}, "man " + name, {});
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
// ① 副本数随批次大小增长：1 包峰值 4（:batch-start 一份），5 包峰值 4×5（每包再多一份）
// ============================================================================

TEST_F(DbBackupChainTest, PeakBackupCountGrowsWithBatchSize)
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

    // 5 包批次（依赖链保证顺序；最后一个包开始装时前 4 个包已各自写过 DB）
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
        // **每个**DB 文件此刻有 5 份：:batch-start + chain0..chain3 —— 含 confhashes.db
        // confhashes.db（少了它，这一族里就有一个库没人盯）
        for (const auto& [base, cnt] : bak_copies_per_file())
            EXPECT_EQ(cnt, 1 + 4) << base << " 在 5 包批次中途应有 5 份里程碑副本（实测 " << cnt
                                  << " 份）";
    });
    ASSERT_NO_THROW(install_packages(pkgs));
    BreakpointManager::instance().clear_all();

    ASSERT_GT(peak_5pkg, 0) << "断点没命中（取证无效）";
    EXPECT_EQ(peak_5pkg, db_file_count() * 5)
        << "5 包批次：每个 DB 文件 1 份 :batch-start + 4 份已装包的里程碑副本；实测 " << peak_5pkg;
    EXPECT_GT(peak_5pkg, peak_1pkg) << "每个里程碑各自一份备份 → 副本数必须随批次增长";
    EXPECT_EQ(total_baks(), 0) << "成功批次收尾必须把 DB 备份清干净";
}

// ============================================================================
// ② 每个里程碑都有自己的备份文件（名字 = <db>.lpkg_db_bak_before:<milestone>），
//    且与 WAL 的 DB 行一一对应（"任何一条 DB 行都能就地恢复"的依据）——
//    **整族 6 个库逐个枚举**（含 confhashes.db）
// ============================================================================

TEST_F(DbBackupChainTest, EveryMilestoneHasItsOwnBackupFile)
{
    std::vector<std::string> pkgs = {pack("mb1", "1.0")};
    pkgs.push_back(pack("mb2", "1.0", {"mb1"}));
    pkgs.push_back(pack("mb3", "1.0", {"mb2"}));

    bool fired = false;
    BreakpointManager::instance().set("install_after_begin_mb3", [&] {
        fired = true;
        const std::string wal = read_wal();
        for (const auto& db : db_files()) {
            // 批次开始那份
            EXPECT_TRUE(fs::exists(bak_of(db, ":batch-start")))
                << "缺 :batch-start 备份：" << bak_of(db, ":batch-start");
            // mb1 / mb2 各自的里程碑那份
            for (const char* p : {"mb1", "mb2"}) {
                const fs::path bak = bak_of(db, std::string(p) + ":installed");
                EXPECT_TRUE(fs::exists(bak)) << "缺里程碑备份：" << bak;
                EXPECT_NE(wal.find("DB " + db.string() + " " + p + ":installed"), std::string::npos)
                    << "WAL 里没有与备份对应的 DB 行：" << p;
            }
            // mb3 还没装完 → 它的里程碑备份不该存在
            EXPECT_FALSE(fs::exists(bak_of(db, "mb3:installed")));
        }
    });
    ASSERT_NO_THROW(install_packages(pkgs));
    BreakpointManager::instance().clear_all();
    ASSERT_TRUE(fired) << "断点没命中（取证无效）";
    EXPECT_EQ(total_baks(), 0) << "成功批次收尾必须把 DB 备份清干净";
}

// ============================================================================
// ③ 链的语义：每份备份的内容 = **该里程碑之前**的状态（逐级递进）
// ============================================================================

TEST_F(DbBackupChainTest, BackupContentsFormTheMilestoneChain)
{
    // 先装 ch0：让"批次前状态"非空，链的递进才看得出来
    ASSERT_NO_THROW(install_packages({pack("ch0", "1.0")}));
    const fs::path pkgs_db = Config::instance().pkgs_file();
    const std::string before_batch = read_text(pkgs_db);

    std::vector<std::string> batch = {pack("ch1", "1.0")};
    batch.push_back(pack("ch2", "1.0", {"ch1"}));

    bool fired = false;
    std::string start_bak;
    std::string ch1_bak;
    // ch2 开始装的那一刻：ch1 已写完 DB → :batch-start 与 ch1:installed 两份都在
    BreakpointManager::instance().set("install_after_begin_ch2", [&] {
        fired = true;
        start_bak = read_text(bak_of(pkgs_db, ":batch-start"));
        ch1_bak = read_text(bak_of(pkgs_db, "ch1:installed"));
    });

    ASSERT_NO_THROW(install_packages(batch));
    BreakpointManager::instance().clear_all();
    ASSERT_TRUE(fired) << "断点没命中（取证无效）";

    // 命名语义（ARCH §2.3）：`<pkg>:installed` 那份备份的内容 = 该里程碑**之前**的状态。
    // 于是链"错开一格"：:batch-start 那份 = 批次前；ch1:installed 那份 = 批次开始时
    // （ch1 还没写盘）的状态 —— 而"ch1 写盘前的状态"恰好就是 :batch-start 写下的内容。
    EXPECT_EQ(start_bak, before_batch) << ":batch-start 备份必须逐字节等于批次前的 DB";
    EXPECT_EQ(ch1_bak, before_batch)
        << "ch1:installed 那份备份 = ch1 写盘**之前**的状态 = 批次开始时的状态";
    EXPECT_NE(ch1_bak.find("ch0:1.0"), std::string::npos) << "批次前的包应保留在链里";
    EXPECT_EQ(ch1_bak.find("ch1:1.0"), std::string::npos)
        << "该里程碑之前的备份不该含有该里程碑自己的包";
    // 此刻盘上的 DB = 下一个里程碑（ch2:installed）那份备份将要保存的内容：ch0+ch1
    const std::string live = read_text(pkgs_db);
    EXPECT_NE(live.find("ch0:1.0"), std::string::npos);
    EXPECT_NE(live.find("ch1:1.0"), std::string::npos) << "ch1 的 DB 写必须已发生";
    // 最终状态：ch0/ch1/ch2 都在
    const std::string final_db = read_text(pkgs_db);
    for (const char* p : {"ch0:1.0", "ch1:1.0", "ch2:1.0"})
        EXPECT_NE(final_db.find(p), std::string::npos) << "最终 DB 里缺 " << p;
}

// ============================================================================
// ④ 升级批次同样每里程碑一份（整族 6 个 DB 文件都已存在 → 每包写完都落一份）
// ============================================================================

TEST_F(DbBackupChainTest, UpgradeBatchKeepsPerPackageBackups)
{
    std::vector<std::string> v1;
    for (int i = 0; i < 3; ++i) v1.push_back(pack(std::string("up") + std::to_string(i), "1.0"));
    ASSERT_NO_THROW(install_packages(v1));
    ASSERT_EQ(total_baks(), 0) << "成功批次收尾必须把 DB 备份清干净";

    std::vector<std::string> v2;
    for (int i = 0; i < 3; ++i) v2.push_back(pack(std::string("up") + std::to_string(i), "2.0"));

    // 升级顺序由求解器定，不假设哪个包最后 —— 取三处断点观测值的**最大值**（即峰值）
    bool fired = false;
    int peak = 0;
    int pkg_milestone_baks = 0;
    for (const char* n : {"up0", "up1", "up2"}) {
        BreakpointManager::instance().set(std::string("install_after_begin_") + n, [&, n] {
            fired = true;
            peak = std::max(peak, global_db_baks());
            // pkgs 这个文件此刻的副本数（1 份 :batch-start + 已装完包的里程碑）
            const auto counts = bak_copies_per_file();  // 先落地：别跨两个临时表比较迭代器
            if (const auto it = counts.find("pkgs"); it != counts.end())
                pkg_milestone_baks = it->second;
        });
    }
    ASSERT_NO_THROW(install_packages(v2));
    BreakpointManager::instance().clear_all();
    ASSERT_TRUE(fired) << "断点没命中（取证无效）";

    EXPECT_GT(peak, db_file_count())
        << "升级批次中途应已出现**包级里程碑**副本（:batch-start 之外的额外副本）；实测 " << peak;
    EXPECT_LE(peak, db_file_count() * 4) << "副本数不该超过 :batch-start + 每包一份；实测 " << peak;
    EXPECT_GE(pkg_milestone_baks, 2)
        << "pkgs 此刻应有 :batch-start + 至少一个 <pkg>:installed 副本；实测 "
        << pkg_milestone_baks;
    EXPECT_EQ(Cache::instance().get_installed_version("up0"), "2.0");
    EXPECT_EQ(total_baks(), 0) << "成功批次收尾必须把 DB 备份清干净";
}

// ============================================================================
// ⑤ 批次之间互不影响：前一批次的备份已清干净，后一批次重新从 :batch-start 起链
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

    // 崩溃现场：每个已装包的里程碑各留一份备份（链还在，`rec` 有依据可回退）
    for (const char* p : {"cr1", "cr2"}) {
        EXPECT_TRUE(
            fs::exists(bak_of(Config::instance().pkgs_file(), std::string(p) + ":installed")))
            << "崩溃现场缺少里程碑备份：" << p;
    }

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
