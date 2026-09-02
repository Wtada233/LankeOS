/**
 * test_upgrade_obsolete_dir_cleanup.cpp — 目录整树删除的回归测试
 *
 * 真实事故（python-setuptools 83 → 84）：升级后 site-packages 残留
 *   setuptools-83.0.0.dist-info/licenses/   （空嵌套目录）
 * 旧版的整个 `*-<ver>.dist-info/`（含其内 `licenses/`）是"旧文件清单里不再属于新版
 * 的条目"。升级的旧文件移除逻辑把目录删除混在文件循环里、只做单层 is_empty 判空：
 * 旧文件先 rename 成 .lpkg_bak（deferred 到提交后清理）占着目录 → 目录判"非空"被
 * 跳过；CLEANUP 清完 bak 后没人回头删 → 嵌套第 2 层（licenses/）空壳永久残留，
 * 下游构建自动探测 site-packages 的 dist-info 崩溃。
 *
 * 正确语义（remove 与 upgrade 共用 detail::backup_dir_tree_whole，ARCH.md §3.6）：
 *   目录走独立阶段、**最深优先**，逐目录整树 rename 成单个 .lpkg_bak（CLEANUP 一次
 *   清光）。先决条件是目录此刻已是"纯本包残留"——每个直接子项都是本包 .lpkg_bak
 *   （更深的本包目录先被整树 rename 成 bak）。**任何非本包残留**（无主文件/目录、
 *   lpkg 自身状态目录、保留的 conffile、其他包文件）都会让整树保留，绝不误删不属于
 *   本包的东西（曾用"子树无其他包 owner 即可删"的递归规则把共享祖先下
 *   usr/share/lpkg/docs 等删掉，3 套测试回归，已否决）。
 *
 * 本文件 pin：
 *   upgrade/remove 各自：整棵 owned dist-info（含嵌套第 2 层）被清掉；
 *   深嵌套（>2 层）owned 目录被清掉；空 owned 目录被清掉；
 *   目录里若有**无主**文件 → 整树保留、无主文件原样不删（安全边界）；
 *   目录被**其他包**持有 → 整树保留；
 *   upgrade 中途整批回滚 → 被整树 rename 的目录能从 .lpkg_bak 恢复。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class UpgradeObsoleteDirCleanupTest : public IntegrationTestBase
{
protected:
    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        setup_local_mirror();  // 空镜像：让 install 的 repo.load_index 快速且不联网
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        IntegrationTestBase::TearDown();
    }

    static void write_file(const fs::path& p, const std::string& content = "x")
    {
        fs::create_directories(p.parent_path());
        std::ofstream f(p);
        f << content;
    }

    fs::path site() const
    {
        return test_root / "usr" / "lib" / "python3.14" / "site-packages";
    }

    /**
     * 构造一个"setuptools 风格"的 dist-info 虚拟包：
     *   content/usr/lib/python3.14/site-packages/<rel...>
     * @param files      相对 site() 的**文件**路径列表（自动建父目录）
     * @param empty_dirs 相对 site() 的**空目录**列表（打包时若原样保留，会被登记为 owned）
     */
    std::string build_site_pkg(const std::string& name, const std::string& ver,
                               const std::vector<std::string>& files,
                               const std::vector<std::string>& empty_dirs = {})
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "-" + ver);
        const fs::path base = work / "content" / "usr" / "lib" / "python3.14" / "site-packages";
        fs::create_directories(base);  // 空包也保留 content/（pack 要求目录存在）
        for (const auto& rel : files) write_file(base / rel);
        for (const auto& rel : empty_dirs) fs::create_directories(base / rel);

        const fs::path pkg_file = pkg_dir / (name + "-" + ver + ".lpkg");
        pack_package(pkg_file.string(), work.string(), name, ver, /*deps=*/{}, /*provides=*/{},
                     /*man=*/"Man page for " + name, /*needed_so=*/{});
        return pkg_file.string();
    }

    /// 往已装好的 dist-info 里塞一个**不属于任何包**的占位文件（模拟下游工具事后写入）
    void drop_unowned_placeholder(const fs::path& rel_file)
    {
        write_file(site() / rel_file, "leftover placeholder from a build tool");
    }

    /**
     * 模拟真实系统里的"基础包"：持有 site-packages/ 及其祖先目录。
     * 否则沙盒里只有一个包时，它是 usr/…/site-packages 的**唯一**目录持有者，
     * remove 按引用计数把它整树删掉是符合模型的行为，会掩盖"共享祖先目录是否被误删"
     * 的断言（真实系统里这些目录由 filesystem/base 包持有，永不唯一）。
     */
    void install_sysroot()
    {
        install_packages({build_site_pkg("sysroot-base", "1.0", {".sysroot"})});
        ASSERT_FALSE(Cache::instance().get_installed_version("sysroot-base").empty());
    }

    /// 构造各版本默认的 setuptools-like 内容（v.dist-info 含嵌套 licenses/）
    static std::vector<std::string> dist_files(const std::string& ver)
    {
        return {std::string("setuptools-") + ver + ".dist-info/METADATA",
                std::string("setuptools-") + ver + ".dist-info/WHEEL",
                std::string("setuptools-") + ver + ".dist-info/licenses/LICENSE"};
    }
};

// ============================================================================
// 1) upgrade：旧版本整棵 owned dist-info（含嵌套 licenses 第 2 层）必须被移除
// ============================================================================

TEST_F(UpgradeObsoleteDirCleanupTest, UpgradeRemovesWholeObsoleteDistInfoTreeWithNestedDir)
{
    const std::string pkg = "python-setuptools";
    const fs::path old_dist = site() / "setuptools-83.0.0.dist-info";
    const fs::path new_dist = site() / "setuptools-84.0.0.dist-info";

    install_packages({build_site_pkg(pkg, "83.0.0", dist_files("83.0.0"))});
    ASSERT_TRUE(fs::exists(old_dist / "licenses" / "LICENSE")) << "83 安装应就位";

    install_packages({build_site_pkg(pkg, "84.0.0", dist_files("84.0.0"))});
    ASSERT_TRUE(fs::exists(new_dist / "licenses" / "LICENSE")) << "84 安装应就位";

    EXPECT_FALSE(fs::exists(old_dist))
        << "升级后旧版本整棵 dist-info（含嵌套 licenses/）必须被删除";
    EXPECT_FALSE(fs::exists(old_dist / "licenses")) << "不得残留第二层空壳 licenses/";
    EXPECT_TRUE(fs::is_directory(site())) << "共享祖先目录 site-packages 必须保留";
}

// ============================================================================
// 2) remove：安装中的版本，含嵌套 licenses 第 2 层的 dist-info 整棵删除
// ============================================================================

TEST_F(UpgradeObsoleteDirCleanupTest, RemoveCleansWholeDistInfoTreeWithNestedDir)
{
    const std::string pkg = "python-setuptools";
    const fs::path dist = site() / "setuptools-84.0.0.dist-info";

    install_sysroot();  // 祖先目录的"基础包"持有者（见 helper 注释）
    install_packages({build_site_pkg(pkg, "84.0.0", dist_files("84.0.0"))});
    ASSERT_TRUE(fs::exists(dist / "licenses" / "LICENSE"));

    remove_package(pkg, /*force=*/false);

    EXPECT_FALSE(fs::exists(dist)) << "remove 后 dist-info 整棵（含 licenses/ 第 2 层）应消失";
    EXPECT_FALSE(fs::exists(dist / "licenses"));
    EXPECT_TRUE(Cache::instance().get_installed_version(pkg).empty());
    EXPECT_TRUE(fs::is_directory(site())) << "sysroot-base 持有的祖先目录必须保留";
}

// ============================================================================
// 3) 安全边界（upgrade）：目录里的**无主**文件不属于本包 → 整树保留、文件原样不删
// ============================================================================

TEST_F(UpgradeObsoleteDirCleanupTest, UpgradePreservesObsoleteDirHoldingUnownedFile)
{
    const std::string pkg = "python-setuptools";
    const fs::path old_dist = site() / "setuptools-83.0.0.dist-info";
    const fs::path stray = old_dist / "licenses" / ".unowned-stray";

    install_packages({build_site_pkg(pkg, "83.0.0", dist_files("83.0.0"))});
    drop_unowned_placeholder("setuptools-83.0.0.dist-info/licenses/.unowned-stray");
    ASSERT_TRUE(fs::exists(stray));

    install_packages({build_site_pkg(pkg, "84.0.0", dist_files("84.0.0"))});

    // 绝不删除不属于本包的无主文件：整树保留（本包 owned 文件仍被移除）
    EXPECT_TRUE(fs::exists(stray)) << "无主文件必须原样保留，不得被连带删除";
    EXPECT_TRUE(fs::exists(old_dist)) << "含无主文件 → 目录整树保留（安全边界）";
    EXPECT_FALSE(fs::exists(old_dist / "licenses" / "LICENSE"))
        << "本包 owned 的废弃文件仍应被移除";
}

// ============================================================================
// 4) 深嵌套（>2 层）owned 目录一样整树清掉（upgrade）
// ============================================================================

TEST_F(UpgradeObsoleteDirCleanupTest, UpgradeRemovesDeeplyNestedObsoleteDirs)
{
    const std::string pkg = "python-setuptools";
    const fs::path old_dist = site() / "setuptools-83.0.0.dist-info";

    install_packages({build_site_pkg(pkg, "83.0.0",
                                     {"setuptools-83.0.0.dist-info/licenses/deep/a/b/c/deep.txt",
                                      "setuptools-83.0.0.dist-info/METADATA"})});
    ASSERT_TRUE(fs::exists(old_dist / "licenses" / "deep" / "a" / "b" / "c" / "deep.txt"));

    install_packages({build_site_pkg(pkg, "84.0.0", dist_files("84.0.0"))});

    EXPECT_FALSE(fs::exists(old_dist)) << ">2 层深嵌套的旧版 dist-info 也必须整树删除";
}

// ============================================================================
// 5) upgrade：目录仍被**其他包**持有 → 整树保留（引用计数）
// ============================================================================

TEST_F(UpgradeObsoleteDirCleanupTest, UpgradeKeepsObsoleteDirStillUsedByAnotherPackage)
{
    const std::string a = "pyhost";
    const std::string b = "pyplugin";
    const fs::path shared_dir = site() / "setuptools-83.0.0.dist-info";
    const fs::path keeper = shared_dir / "keeper.txt";

    install_packages({build_site_pkg(a, "1.0", dist_files("83.0.0"))});
    install_packages(
        {build_site_pkg(b, "1.0", {"setuptools-83.0.0.dist-info/keeper.txt"})});
    ASSERT_TRUE(fs::exists(keeper));

    install_packages({build_site_pkg(a, "2.0", dist_files("84.0.0"))});

    EXPECT_EQ(Cache::instance().get_installed_version(a), "2.0");
    EXPECT_FALSE(Cache::instance().get_installed_version(b).empty());
    EXPECT_TRUE(fs::exists(keeper)) << "其他包 b 的文件必须原样保留";
    EXPECT_TRUE(fs::exists(shared_dir)) << "b 仍持有的目录必须保留";
}

// ============================================================================
// 6) upgrade 中途整批回滚 → 被整树 rename 掉的旧目录能从 .lpkg_bak 恢复
// ============================================================================

TEST_F(UpgradeObsoleteDirCleanupTest, UpgradeRollbackRestoresWholeRemovedObsoleteDirTree)
{
    const std::string a = "pyhost";
    const std::string c = "pytrigger";
    const fs::path old_file = site() / "setuptools-1.0.0.dist-info" / "licenses" / "LICENSE";

    install_packages({build_site_pkg(a, "1.0", dist_files("1.0.0"))});
    ASSERT_TRUE(fs::exists(old_file));

    // 升级 a→2.0 的同时装 c；c 复制中途失败 → 整批回滚（a 的旧目录此时已被整树 rename）
    auto a2 = build_site_pkg(a, "2.0", dist_files("2.0.0"));
    auto c1 = build_site_pkg(c, "1.0", {c});  // c 至少一个文件，copy 循环才会触发断点
    BreakpointManager::instance().set("copy_after_wal_" + c, [] {
        throw LpkgException("injected: force batch rollback after a upgraded");
    });

    std::vector<std::string> upgrade = {a2, c1};
    EXPECT_THROW(install_packages(upgrade), LpkgException);

    BreakpointManager::instance().clear_all();
    trim_completed();
    Cache::instance().load();

    EXPECT_EQ(Cache::instance().get_installed_version(a), "1.0") << "a 应回滚回旧版本";
    EXPECT_TRUE(fs::exists(old_file))
        << "整树 rename 掉的旧 dist-info（含嵌套 licenses/）必须从 .lpkg_bak 恢复";
    EXPECT_TRUE(Cache::instance().get_installed_version(c).empty());
}

// ============================================================================
// 7) upgrade：旧版废弃的空 owned 目录条目也要被清掉
// ============================================================================

TEST_F(UpgradeObsoleteDirCleanupTest, UpgradeRemovesObsoleteEmptyDirEntry)
{
    const std::string pkg = "pyhost";
    const fs::path empty_dir = site() / "only-empty-dir";

    install_packages({build_site_pkg(pkg, "1.0", {"a.txt"}, {"only-empty-dir"})});
    ASSERT_TRUE(fs::is_directory(empty_dir));

    install_packages({build_site_pkg(pkg, "2.0", {"b.txt"})});

    EXPECT_FALSE(fs::exists(empty_dir)) << "旧版废弃的空目录条目也应被清掉";
}

// ============================================================================
// 8) remove：目录仍被**其他包**持有 → 整树保留（引用计数）
// ============================================================================

TEST_F(UpgradeObsoleteDirCleanupTest, RemoveKeepsDirStillUsedByAnotherPackage)
{
    const std::string a = "pyhost";
    const std::string b = "pyplugin";
    const fs::path shared_dir = site() / "setuptools-83.0.0.dist-info";
    const fs::path keeper = shared_dir / "keeper.txt";

    install_packages({build_site_pkg(a, "1.0", dist_files("83.0.0"))});
    install_packages(
        {build_site_pkg(b, "1.0", {"setuptools-83.0.0.dist-info/keeper.txt"})});
    ASSERT_TRUE(fs::exists(keeper));

    remove_package(a, /*force=*/false);

    EXPECT_TRUE(Cache::instance().get_installed_version(a).empty());
    EXPECT_TRUE(fs::exists(keeper)) << "其他包 b 的文件必须原样保留";
    EXPECT_TRUE(fs::exists(shared_dir)) << "b 仍持有的目录必须保留";
}

// ============================================================================
// 9) 安全边界（remove）：目录里的**无主**文件不属于本包 → 整树保留、文件原样不删
// ============================================================================

TEST_F(UpgradeObsoleteDirCleanupTest, RemovePreservesDirHoldingUnownedFile)
{
    const std::string pkg = "python-setuptools";
    const fs::path dist = site() / "setuptools-84.0.0.dist-info";
    const fs::path stray = dist / "licenses" / ".unowned-stray";

    install_sysroot();
    install_packages({build_site_pkg(pkg, "84.0.0", dist_files("84.0.0"))});
    drop_unowned_placeholder("setuptools-84.0.0.dist-info/licenses/.unowned-stray");
    ASSERT_TRUE(fs::exists(stray));

    remove_package(pkg, /*force=*/false);

    EXPECT_TRUE(fs::exists(stray)) << "无主文件必须原样保留，不得被连带删除";
    EXPECT_TRUE(fs::exists(dist)) << "含无主文件 → 目录整树保留（安全边界）";
    EXPECT_FALSE(fs::exists(dist / "licenses" / "LICENSE")) << "本包 owned 文件仍应被移除";
}

// ============================================================================
// 10) remove：深嵌套（>2 层）owned 目录整树清掉
// ============================================================================

TEST_F(UpgradeObsoleteDirCleanupTest, RemoveCleansDeepNestedOwnedDirs)
{
    const std::string pkg = "python-setuptools";
    const fs::path dist = site() / "setuptools-84.0.0.dist-info";
    const fs::path deep = dist / "licenses" / "deep" / "a" / "b" / "c" / "deep.txt";

    install_sysroot();
    install_packages({build_site_pkg(pkg, "84.0.0",
                                     {"setuptools-84.0.0.dist-info/licenses/deep/a/b/c/deep.txt",
                                      "setuptools-84.0.0.dist-info/METADATA"})});
    ASSERT_TRUE(fs::exists(deep));

    remove_package(pkg, /*force=*/false);

    EXPECT_FALSE(fs::exists(dist)) << "remove 应整树清掉 >2 层深嵌套的 owned dist-info";
    EXPECT_TRUE(fs::is_directory(site())) << "sysroot-base 持有的祖先目录必须保留";
}
