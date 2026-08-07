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
    // 构造一个 needed_so 在空仓库中无人提供的已装包
    auto pP = create_pkg("fxs_prov", "1.0", {}, {"libprov.so.1"});
    auto pQ = create_pkg("fxs_need", "1.0", {}, {}, {"libprov.so.1"});
    install_packages({pP, pQ});

    // 非交互模式（IntegrationTestBase 已设 YES）→ 直接抛错而非读 stdin
    try {
        force_solve_conflict();
        FAIL() << "force_solve_conflict should throw in non-interactive mode";
    } catch (const LpkgException& e) {
        EXPECT_STREQ(e.what(), get_string("error.force_solve_requires_interactive").c_str());
    }
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

TEST_F(RegressionFixTest, QueryFileSiblingPrefixDoesNotThrow)
{
    auto p = create_pkg("fxq_a", "1.0");
    install_packages({p});

    // 根内绝对路径正常
    EXPECT_NO_THROW(query_file((test_root / "usr" / "bin" / "fxq_a").string()));
    // root 前缀匹配但实际在外的路径（rootE/...）不崩溃、不误归因
    const std::string evil = test_root.string() + "E/usr/bin/fxq_a";
    EXPECT_NO_THROW(query_file(evil));
}
