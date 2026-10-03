/**
 * test_regression_fixes.cpp — 代码审计修复的回归测试
 *
 * 覆盖：
 *  - 升级回滚后旧版 dep/needed_so/man 元数据必须恢复（write_string_file_wal）
 *  - 符号链接不能静默替换目录（无 WAL 记录的破坏）
 *  - provides 只能精确匹配，子串匹配不再误满足依赖
 *  - 安装虚拟能力名不因 "virtual" 版本号抛异常
 *  - force-solve-conflict 非交互模式不阻塞 stdin
 *  - --hash 不能用于多个本地包
 *  - query_file 对 root 前缀兄弟路径不误判
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class RegressionFixTest : public IntegrationTestBase
{
protected:
    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        // 空本地镜像：让 install/force_solve 的 repo 加载快速且不联网
        setup_local_mirror();
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        IntegrationTestBase::TearDown();
    }

    std::string read_file(const fs::path& p)
    {
        std::ifstream f(p);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
};

// ============================================================================
// 升级回滚恢复旧版 dep 元数据（write_string_file_wal 回归）
// ============================================================================

TEST_F(RegressionFixTest, UpgradeRollbackRestoresOldDepMetadata)
{
    auto pH = create_pkg("fxa_hlp", "1.0");
    install_packages({pH});

    auto pA1 = create_pkg("fxa_up", "1.0", {"fxa_hlp"});
    install_packages({pA1});

    const fs::path depA = Config::instance().dep_dir() / "fxa_up";
    ASSERT_TRUE(fs::exists(depA));
    EXPECT_EQ(read_file(depA), "fxa_hlp\n");

    // v2.0 无依赖：升级时 dep 文件被 DBRM 备份删除（曾因无备份在回滚时丢失旧元数据）
    auto pA2 = create_pkg("fxa_up", "2.0");
    auto pC = create_pkg("fxa_c", "1.0");

    // 第二个包复制中途失败 → 整批回滚（A2 已升级完成）
    BreakpointManager::instance().set("copy_after_wal_fxa_c", [] {
        throw LpkgException("injected: force batch rollback after A upgraded");
    });

    EXPECT_THROW(install_packages({pA2, pC}), LpkgException);

    BreakpointManager::instance().clear_all();
    trim_completed();
    Cache::instance().load();

    // A 回滚回 v1.0
    EXPECT_EQ(Cache::instance().get_installed_version("fxa_up"), "1.0");
    // dep 文件恢复为 v1.0 内容
    EXPECT_TRUE(fs::exists(depA));
    EXPECT_EQ(read_file(depA), "fxa_hlp\n");
    // C 未安装
    EXPECT_TRUE(Cache::instance().get_installed_version("fxa_c").empty());
}

// ============================================================================
// 符号链接不能静默替换目录
// ============================================================================

TEST_F(RegressionFixTest, SymlinkOverDirectoryRejected)
{
    const fs::path dir = test_root / "usr" / "lib" / "fxsym";
    fs::create_directories(dir);

    // 构造包：content/usr/lib/fxsym 是符号链接，而目标上该路径是目录
    fs::path work = suite_work_dir / "_pkg_fxsym";
    fs::create_directories(work / "content" / "usr" / "lib");
    fs::create_symlink("/usr/lib/sometarget", work / "content" / "usr" / "lib" / "fxsym");
    std::string pkg_path = (pkg_dir / "fxsym-1.0.lpkg").string();
    pack_package(pkg_path, work.string(), "fxsym", "1.0", {}, {"fxsym"}, "Man page for fxsym", {});

    // 必须作为文件冲突拒绝，而不是静默删除目录
    EXPECT_THROW(install_packages({pkg_path}), LpkgException);
    // 目录必须原样保留
    EXPECT_TRUE(fs::is_directory(dir));
}

// ============================================================================
// provides 精确匹配：子串不再误满足依赖
// ============================================================================

TEST_F(RegressionFixTest, ProvidesSubstringNoLongerSatisfiesDep)
{
    auto pP = create_pkg("fxp_prov", "1.0", {}, {"libfoo-dev"});
    auto pB = create_pkg("fxp_depb", "1.0", {"foo"});

    // 依赖 "foo" 只能由提供 "foo" 的包满足；"libfoo-dev" 含子串但不应匹配
    EXPECT_THROW(install_packages({pB, pP}), LpkgException);
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().get_installed_version("fxp_depb").empty());
    EXPECT_TRUE(Cache::instance().get_installed_version("fxp_prov").empty());
}

// ============================================================================
// 安装虚拟能力名：不因 "virtual" 版本号调用 version_compare 抛异常
// ============================================================================

TEST_F(RegressionFixTest, InstallVirtualCapabilityNameDoesNotCrash)
{
    auto pP = create_pkg("fxv_prov", "1.0", {}, {"myvirt"});
    install_packages({pP});

    // "myvirt" 是已装包的虚拟能力：解析时 installed_version == "virtual"，
    // 曾因此对 "virtual" 调用 version_compare 抛 invalid_version_format。
    // 修复后应干净地结束（仓库为空 → 无提供者 → 全部已装）。
    EXPECT_NO_THROW(install_packages({"myvirt"}));
}

// ============================================================================
// force-solve-conflict 非交互模式直接拒绝，不阻塞 stdin
// ============================================================================

TEST_F(RegressionFixTest, ForceSolveInNonInteractiveModeThrows)
{
    // 构造一个 needed_so 在**仓库**中无人提供的已装包
    auto pP = create_pkg("fxs_prov", "1.0", {}, {"libprov.so.1"});
    auto pQ = create_pkg("fxs_need", "1.0", {}, {}, {"libprov.so.1"});
    install_packages({pP, pQ});

    // 索引必须**非空**：force_solve_conflict 现在有一条"索引为空 → 拒绝"的守卫
    // （否则它会把每个包都判成 broken、提议删光）。那条守卫是另一条用例的范畴；
    // 本用例要走到的是"非交互模式"那道检查，所以给一个非空、但不提供 libprov.so.1 的索引。
    {
        std::ofstream idx(suite_work_dir / "mirror" / "x86_64" / "index.txt");
        idx << "fxs_unrelated|1.0:::|\n";
    }

    // 非交互模式（IntegrationTestBase 已设 YES）→ 直接抛错而非读 stdin
    try {
        force_solve_conflict();
        FAIL() << "force_solve_conflict should throw in non-interactive mode";
    } catch (const LpkgException& e) {
        EXPECT_STREQ(e.what(), get_string("error.force_solve_requires_interactive").c_str());
    }
}

// ============================================================================
// force-solve-conflict：索引为空时必须拒绝，绝不把全部包判成 broken
// ============================================================================

TEST_F(RegressionFixTest, ForceSolveRefusesWhenRepoIndexEmpty)
{
    auto pP = create_pkg("fxs2_prov", "1.0", {}, {"libprov2.so.1"});
    auto pQ = create_pkg("fxs2_need", "1.0", {}, {}, {"libprov2.so.1"});
    install_packages({pP, pQ});

    // 镜像里没有 index.txt（→ 解析出 0 个包）。此前会把两个包都判成 broken 并提议删除。
    try {
        force_solve_conflict();
        FAIL() << "force_solve_conflict must refuse when the repository index is empty";
    } catch (const LpkgException& e) {
        EXPECT_EQ(std::string(e.what()), string_format("error.repo_index_empty", 0));
    }

    // 两个包都必须还在
    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().get_installed_version("fxs2_prov").empty());
    EXPECT_FALSE(Cache::instance().get_installed_version("fxs2_need").empty());
}

// ============================================================================
// upgrade：索引为空/不可用 + 有已装包 → 拒绝，不得报"所有包都是最新"
// ============================================================================

TEST_F(RegressionFixTest, UpgradeRefusesWhenRepoIndexEmpty)
{
    auto p = create_pkg("fxu_one", "1.0");
    install_packages({p});  // 有已装包，但镜像里没有 index.txt（解析出 0 个包）

    try {
        upgrade_packages();
        FAIL() << "upgrade must refuse when the repository index is empty, not claim 'up to date'";
    } catch (const LpkgException& e) {
        EXPECT_EQ(std::string(e.what()), string_format("error.repo_index_empty", std::size_t{0}));
    }
}

// ============================================================================
// 移除"声明了 provides、却没有任何文件"的包 → provider 记录必须一起撤掉
// ============================================================================

TEST_F(RegressionFixTest, RemovingFilelessPackageClearsItsProvides)
{
    // 不能走 create_pkg：它总会在 content/usr/bin/ 下放一个文件，那样
    // `remove_package_files` 的 `owned_entries.empty()` 早退就碰不到。手工造一个**空 content/**。
    const fs::path work = suite_work_dir / "_pkg_fxp_meta";
    fs::create_directories(work / "content");
    const std::string pkg_path = (pkg_dir / "fxp_meta-1.0.lpkg").string();
    pack_package(pkg_path, work.string(), "fxp_meta", "1.0", {}, {"capX"}, "", {});

    install_packages({pkg_path});
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("fxp_meta"));
    ASSERT_TRUE(Cache::instance().get_providers("capX").contains("fxp_meta"));

    remove_package("fxp_meta", /*force=*/true, /*wrap_in_txn=*/false, /*purge_config=*/false);

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("fxp_meta"));
    EXPECT_TRUE(Cache::instance().get_providers("capX").empty())
        << "provider 记录必须随包移除一起撤 —— 此前它会残留（早退在撤 provider 之前），"
           "而 dep_satisfied_on_disk() 只看 providers 非空、不看 is_installed";
}

// ============================================================================
// --hash 不能用于多个本地包
// ============================================================================

TEST_F(RegressionFixTest, HashWithMultipleLocalPackagesRejected)
{
    const fs::path hf = suite_work_dir / "hash.txt";
    {
        std::ofstream f(hf);
        f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    }

    auto p1 = create_pkg("fxh_a", "1.0");
    auto p2 = create_pkg("fxh_b", "1.0");

    try {
        install_packages({p1, p2}, hf.string());
        FAIL() << "install with --hash and two local packages should be rejected";
    } catch (const LpkgException& e) {
        EXPECT_STREQ(e.what(), get_string("error.hash_requires_single_local").c_str());
    }
}

// ============================================================================
// 移除包时清理 needed_so 派生的反向依赖边
// ============================================================================

TEST_F(RegressionFixTest, RemoveCleansNeededSoReverseDeps)
{
    auto pP = create_pkg("m2_prov", "1.0", {}, {"libm2.so.1"});
    auto pQ = create_pkg("m2_need", "1.0", {}, {}, {"libm2.so.1"});
    install_packages({pP, pQ});

    // 安装后：m2_need 的 needed_so 派生边存在（m2_prov 的反向依赖含 m2_need）
    EXPECT_TRUE(Cache::instance().get_reverse_deps("m2_prov").contains("m2_need"));

    remove_package("m2_need", false);

    // 移除后：边必须被清理，否则同一进程内 get_reverse_deps 返回已移除的包
    EXPECT_FALSE(Cache::instance().get_reverse_deps("m2_prov").contains("m2_need"));
}

// ============================================================================
// query_file：root 前缀兄弟路径不误判（冒烟测试，不崩溃）
// ============================================================================

TEST_F(RegressionFixTest, QueryFileSiblingPrefixIsNotMistakenForOwned)
{
    auto p = create_pkg("fxq_a", "1.0");
    install_packages({p});

    // 根内绝对路径正常：**已安装**的文件照常查得到，不抛
    EXPECT_NO_THROW(query_file((test_root / "usr" / "bin" / "fxq_a").string()));
    // root 前缀匹配但实际在外的路径（rootE/...）**既不能误归因**，也必须**出声**：
    // 2026-10-03 起 `query <文件>` 查不到属主与 `query -p <未安装>` 口径统一 —— 抛
    // `LpkgException`（退出码 1），不再是"打条 info 然后退 0"。
    const std::string evil = test_root.string() + "E/usr/bin/fxq_a";
    EXPECT_THROW(query_file(evil), LpkgException);
}

// ============================================================================
// reinstall 路径也必须走元数据一致性校验（三条路径共用 verify_package_metadata）
// ============================================================================

TEST_F(RegressionFixTest, ReinstallAlsoRefusesMetadataMismatch)
{
    setup_local_mirror();
    // 镜像里 app 的真实 metadata 依赖 libz；索引却（过时）声明它没有依赖。
    create_pkg("libz", "1.0");
    create_pkg("fxi_app", "1.0", {"libz"});
    add_to_mirror("libz", "1.0");
    add_to_mirror("fxi_app", "1.0");
    {
        std::ofstream idx(suite_work_dir / "mirror" / "x86_64" / "index.txt");
        idx << "fxi_app|1.0:::|\n";  // 故意漏掉 deps=libz
        idx << "libz|1.0:::|\n";
    }

    try {
        reinstall_packages({"fxi_app"});  // 内部走 install_packages(force_reinstall=true)
        FAIL() << "reinstall 也必须拒绝 metadata 与索引不一致的包";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("fxi_app"), std::string::npos) << msg;
        EXPECT_NE(msg.find("deps"), std::string::npos) << msg;
    }

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("fxi_app"));
}

/**
 * 约束违反的批次必须**拒绝**：`vdapp` 要求 `vdlib <= 1.0`，而同一批次又显式要求装 `vdlib 1.0+1`。
 *
 * **订正 2026-10-03（版本桥接修好后）**：此前 libsolv 的 EVR 匹配把"要求侧缺 release"当通配，
 * 会**接受**这个计划，靠安装期的版本复核（`error.dep_version_mismatch`）才拦下来 —— 本用例
 * 原先断言的就是那条报错。桥接修好后（`to_libsolv_evr` 改用 caret 分隔 release，见
 * `tests/unit/test_vercmp_libsolv_bridge.cpp` 的等价性矩阵）求解器**自己**就拒绝这个批次，
 * 报错换成"依赖无解"。⇒ 这里只钉"必须拒绝 + 整批回滚"这条不变量，**不再钉具体文案**；
 * 那道复核判据本身改由 `tests/unit/test_plan_dep_version_check.cpp` 直接喂手搓计划来钉
 * （否则它就成了"永远走不到的分支上写的假绿用例"）。
 */
TEST_F(RegressionFixTest, PlanVersionViolatingDependencyConstraintIsRejected)
{
    const std::string lib = create_pkg("vdlib", "1.0+1");
    const std::string app = create_pkg("vdapp", "1.0", {"vdlib<=1.0"});

    try {
        install_packages({app, lib});
        FAIL() << "计划把依赖解析到违反约束的版本（1.0+1 不满足 <= 1.0），必须拒绝";
    } catch (const LpkgException& e) {
        EXPECT_NE(std::string(e.what()).find("vdlib"), std::string::npos)
            << "报错必须点名那个依赖：" << e.what();
    }

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("vdapp"));
    EXPECT_FALSE(Cache::instance().is_installed("vdlib"));
}

/**
 * 版本复核**不能被"盘上那份满足"短路**：约束被**已装版本**满足、但计划要把该依赖换成
 * 违反约束的版本时，也必须拦下 —— 批次后生效的是**计划版本**，不是盘上那份。
 *
 * **订正 2026-10-03**：桥接修好后求解器自己就会拒这个批次（不再产出"计划版本违规"的方案），
 * 所以这里同样只钉"必须拒绝 + 整批回滚"；那道复核判据的每一格（含"计划版本 vs 盘上版本"
 * 的取舍）改由 `tests/unit/test_plan_dep_version_check.cpp` 直接喂手搓计划钉。
 */
TEST_F(RegressionFixTest, VersionRecheckIsNotShortCircuitedBySatisfiedOnDiskVersion)
{
    // ① 先装 vslib 1.0（满足 <= 1.0）。
    const std::string lib10 = create_pkg("vslib", "1.0");
    ASSERT_NO_THROW(install_packages({lib10}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("vslib"));
    ASSERT_EQ(Cache::instance().get_installed_version("vslib"), "1.0");

    // ② 同一批次里把 vslib 换成 1.0+1、并装 vsapp（dep: vslib<=1.0）。盘上那份（1.0）满足，
    //    但计划版本（1.0+1）不满足 —— 必须拒绝，且整批回滚。
    const std::string libNew = create_pkg("vslib", "1.0+1");
    const std::string app = create_pkg("vsapp", "1.0", {"vslib<=1.0"});
    EXPECT_THROW(install_packages({app, libNew}), LpkgException)
        << "计划把「已装且满足约束」的依赖升级到违反约束的版本，必须拒绝";

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("vslib")) << "整批回滚：vslib 应仍是 1.0";
    EXPECT_EQ(Cache::instance().get_installed_version("vslib"), "1.0");
    EXPECT_FALSE(Cache::instance().is_installed("vsapp"));
}
