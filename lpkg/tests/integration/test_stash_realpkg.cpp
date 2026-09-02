/**
 * test_stash_realpkg.cpp — 真实包 fixture 下的 stash / DIR_RM 终极回归
 *
 * fixture：tests/testdata/ 里放的是从 farm 输出复制的真实 acl/attr 包，但**精简过**
 * metadata（needed_so 里去掉 libc.so.6、deps/provides 置空、去掉 hooks）——沙盒里没有
 * glibc，带 needed_so 会触发 unresolved-drift。content 原样保留。
 *   acl-2.4.0+2.lpkg  真实 acl（libacl 库 + setfacl 等 + doc + man + locale + include）
 *   attr-2.6.0+2.lpkg 真实 attr
 *   acl-2.5.0.lpkg    acl 的"全文件删除"升级样本（content 空）→ 验证 acl 专属目录被删
 *
 * 重点（终极用例）：真实包 install / upgrade / 中断→整批回滚，acl 专属目录
 * （usr/share/doc/acl、usr/include/acl、usr/include/sys、usr/share/man/man3 等）在
 * 升级到空版本后能否被 DIR_RM 清掉，而共享祖先（sysroot 持有）与 attr 保持不变。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

namespace
{
/** testdata 目录（tests/testdata），相对运行 cwd（/app） */
fs::path testdata()
{
    return fs::absolute("tests/testdata");
}
}  // namespace

class StashRealPkgTest : public IntegrationTestBase
{
protected:
    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        Config::instance().set_no_hooks_mode(true);  // fixture 已去 hooks，双保险
        BreakpointManager::instance().clear_all();
        setup_local_mirror();  // 空镜像：install 的 repo 加载快速不联网
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        IntegrationTestBase::TearDown();
    }

    fs::path pkg(const std::string& file) const
    {
        return testdata() / file;
    }

    std::string read_wal() const
    {
        std::ifstream f(wal::wal_log_path());
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    /**
     * sysroot 基础包：持有 usr/… 的共享祖先目录（usr、usr/bin、usr/lib、usr/include、
     * usr/share、usr/share/man、usr/share/doc、usr/share/locale），保证移除/空升级 acl 时
     * 祖先不被当作 acl 唯一持有者误删；acl 专属子目录（doc/acl、include/acl、include/sys、
     * man/man3 等）仍只归 acl。
     */
    void install_sysroot()
    {
        const fs::path work = suite_work_dir / "_pkg_sysroot";
        fs::remove_all(work);
        for (const char* d : {"usr", "usr/bin", "usr/lib", "usr/include", "usr/share",
                              "usr/share/man", "usr/share/doc", "usr/share/locale"}) {
            fs::path p = work / "content" / d / ".sysroot";
            fs::create_directories(p.parent_path());
            std::ofstream(p) << "sysroot\n";
        }
        const fs::path pkg_file = pkg_dir / "sysroot-base-1.0.lpkg";
        pack_package(pkg_file.string(), work.string(), "sysroot-base", "1.0");
        install_packages({pkg_file.string()});
        ASSERT_FALSE(Cache::instance().get_installed_version("sysroot-base").empty());
    }

    /** 整树扫出任何 .lpkg_bak 残留（含根下 stash 目录）= true */
    bool has_backup_leftover() const
    {
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(test_root, ec)) {
            if (ec) break;
            if (e.path().filename().string().find(".lpkg_bak") != std::string::npos) return true;
        }
        return false;
    }

    void expect_no_backup_leftover() const
    {
        EXPECT_FALSE(has_backup_leftover()) << "不得残留任何 .lpkg_bak / stash 目录";
    }
};

// ============================================================================
// 1) 真实 acl 安装：文件 + symlink + 目录齐全，无残留
// ============================================================================

TEST_F(StashRealPkgTest, InstallRealAclFilesAndSymlinksPresent)
{
    install_sysroot();
    install_packages({pkg("acl-2.4.0+2.lpkg").string()});

    EXPECT_EQ(Cache::instance().get_installed_version("acl"), "2.4.0+2");
    EXPECT_TRUE(fs::exists(test_root / "usr" / "lib" / "libacl.so.1.2.2400"));
    EXPECT_TRUE(fs::exists(test_root / "usr" / "bin" / "setfacl"));
    EXPECT_TRUE(fs::exists(test_root / "usr" / "include" / "acl" / "libacl.h"));
    EXPECT_TRUE(fs::exists(test_root / "usr" / "share" / "doc" / "acl" / "COPYING"));
    EXPECT_TRUE(fs::is_symlink(test_root / "usr" / "lib" / "libacl.so") ||
                fs::exists(test_root / "usr" / "lib" / "libacl.so"));
    EXPECT_TRUE(Cache::instance().get_file_owners("/usr/share/doc/acl/").contains("acl"))
        << "acl 专属目录必须登记为 acl 所有";
    expect_no_backup_leftover();
}

// ============================================================================
// 2) attr 共装：共享 /usr/lib、/usr/bin 目录两包文件并存
// ============================================================================

TEST_F(StashRealPkgTest, CoinstallAttrSharedDirsIntact)
{
    install_sysroot();
    install_packages({pkg("acl-2.4.0+2.lpkg").string()});
    install_packages({pkg("attr-2.6.0+2.lpkg").string()});

    EXPECT_FALSE(Cache::instance().get_installed_version("attr").empty());
    EXPECT_TRUE(fs::exists(test_root / "usr" / "lib" / "libattr.so.1.1.2600"));
    EXPECT_TRUE(fs::exists(test_root / "usr" / "lib" / "libacl.so.1.2.2400"));
    // 共享目录两包都持有
    EXPECT_TRUE(Cache::instance().get_file_owners("/usr/lib/").contains("acl"));
    EXPECT_TRUE(Cache::instance().get_file_owners("/usr/lib/").contains("attr"));
    expect_no_backup_leftover();
}

// ============================================================================
// 3) 终极·升级到空版本：acl 专属目录被 DIR_RM 清掉，共享祖先与 attr 保留
// ============================================================================

TEST_F(StashRealPkgTest, UpgradeAclToEmptyRemovesAclPrivateDirsKeepsShared)
{
    install_sysroot();
    install_packages({pkg("acl-2.4.0+2.lpkg").string()});
    install_packages({pkg("attr-2.6.0+2.lpkg").string()});

    install_packages({pkg("acl-2.5.0.lpkg").string()});

    EXPECT_EQ(Cache::instance().get_installed_version("acl"), "2.5.0");
    // v1 的文件全没了
    EXPECT_FALSE(fs::exists(test_root / "usr" / "lib" / "libacl.so.1.2.2400"));
    EXPECT_FALSE(fs::exists(test_root / "usr" / "bin" / "setfacl"));
    // acl 专属目录被删除（升级旧目录 → 空 → DIR_RM）
    EXPECT_FALSE(fs::exists(test_root / "usr" / "share" / "doc" / "acl"))
        << "acl 专属 doc/acl 目录必须被删除";
    EXPECT_FALSE(fs::exists(test_root / "usr" / "include" / "acl"));
    EXPECT_FALSE(fs::exists(test_root / "usr" / "include" / "sys"));
    // 共享祖先仍在（sysroot 持有）
    EXPECT_TRUE(fs::is_directory(test_root / "usr" / "share" / "doc"));
    EXPECT_TRUE(fs::is_directory(test_root / "usr" / "lib"));
    EXPECT_TRUE(fs::is_directory(test_root / "usr" / "include"));
    // 目录 owner 已清
    EXPECT_FALSE(Cache::instance().get_file_owners("/usr/share/doc/acl/").contains("acl"));
    // attr 不受影响
    EXPECT_FALSE(Cache::instance().get_installed_version("attr").empty());
    EXPECT_TRUE(fs::exists(test_root / "usr" / "lib" / "libattr.so.1.1.2600"));
    expect_no_backup_leftover();
}

// ============================================================================
// 4) 终极·回滚：acl→空升级 + 第二包复制中断 → 整批回滚，acl 回到 v1、目录重建
// ============================================================================

TEST_F(StashRealPkgTest, UpgradeAclToEmptyBatchRollbackRestoresAll)
{
    install_sysroot();
    install_packages({pkg("acl-2.4.0+2.lpkg").string()});
    install_packages({pkg("attr-2.6.0+2.lpkg").string()});

    // 触发包：一个会在复制中途抛错的普通包
    const fs::path trigger = pkg_dir / "stash-trigger-1.0.lpkg";
    {
        const fs::path work = suite_work_dir / "_pkg_trigger";
        fs::create_directories(work / "content" / "usr" / "bin");
        std::ofstream(work / "content" / "usr" / "bin" / "stash-trigger") << "x\n";
        pack_package(trigger.string(), work.string(), "stash-trigger", "1.0");
    }
    BreakpointManager::instance().set("copy_after_wal_stash-trigger", [] {
        throw LpkgException("injected: force batch rollback after acl upgraded to empty");
    });

    std::vector<std::string> batch = {pkg("acl-2.5.0.lpkg").string(), trigger.string()};
    EXPECT_THROW(install_packages(batch), LpkgException);

    BreakpointManager::instance().clear_all();
    trim_completed();
    Cache::instance().load();

    // acl 回滚回 v1，文件与专属目录都恢复（DIR_RM reverse 重建 + 文件从 stash 还原）
    EXPECT_EQ(Cache::instance().get_installed_version("acl"), "2.4.0+2");
    EXPECT_TRUE(fs::exists(test_root / "usr" / "lib" / "libacl.so.1.2.2400"));
    EXPECT_TRUE(fs::exists(test_root / "usr" / "bin" / "setfacl"));
    EXPECT_TRUE(fs::exists(test_root / "usr" / "share" / "doc" / "acl" / "COPYING"));
    // attr 不受影响
    EXPECT_FALSE(Cache::instance().get_installed_version("attr").empty());
    EXPECT_TRUE(fs::exists(test_root / "usr" / "lib" / "libattr.so.1.1.2600"));
    // 触发包未装
    EXPECT_TRUE(Cache::instance().get_installed_version("stash-trigger").empty());
    expect_no_backup_leftover();
}

// ============================================================================
// 5) 中断·真实 acl 全新安装复制中途失败 → 无残留、attr 不动
// ============================================================================

TEST_F(StashRealPkgTest, InstallRealAclInterruptedLeavesCleanState)
{
    install_sysroot();
    install_packages({pkg("attr-2.6.0+2.lpkg").string()});

    BreakpointManager::instance().set(
        "copy_after_wal_acl", [] { throw LpkgException("injected: interrupt acl install"); });
    EXPECT_THROW(install_packages({pkg("acl-2.4.0+2.lpkg").string()}), LpkgException);

    BreakpointManager::instance().clear_all();
    trim_completed();
    Cache::instance().load();

    EXPECT_TRUE(Cache::instance().get_installed_version("acl").empty());
    EXPECT_FALSE(fs::exists(test_root / "usr" / "bin" / "setfacl"));
    EXPECT_FALSE(fs::exists(test_root / "usr" / "share" / "doc" / "acl"));
    EXPECT_FALSE(Cache::instance().get_installed_version("attr").empty());
    EXPECT_TRUE(fs::exists(test_root / "usr" / "lib" / "libattr.so.1.1.2600"));
    expect_no_backup_leftover();
}

// ============================================================================
// 6) 移除真实 acl：专属目录清掉，attr/sysroot 及共享祖先保留
// ============================================================================

TEST_F(StashRealPkgTest, RemoveRealAclCleansPrivateDirsKeepsOthers)
{
    install_sysroot();
    install_packages({pkg("acl-2.4.0+2.lpkg").string()});
    install_packages({pkg("attr-2.6.0+2.lpkg").string()});

    remove_package("acl", /*force=*/false);
    write_cache();

    EXPECT_TRUE(Cache::instance().get_installed_version("acl").empty());
    EXPECT_FALSE(fs::exists(test_root / "usr" / "bin" / "setfacl"));
    EXPECT_FALSE(fs::exists(test_root / "usr" / "share" / "doc" / "acl"))
        << "移除后 acl 专属目录必须被删除";
    EXPECT_FALSE(fs::exists(test_root / "usr" / "include" / "acl"));
    // 共享目录仍存活（sysroot + attr）
    EXPECT_TRUE(fs::is_directory(test_root / "usr" / "lib"));
    EXPECT_TRUE(fs::is_directory(test_root / "usr" / "share" / "doc"));
    EXPECT_TRUE(fs::exists(test_root / "usr" / "lib" / "libattr.so.1.1.2600"));
    expect_no_backup_leftover();
}

// ============================================================================
// 7) 收尾：整棵 usr 下没有任何 .lpkg_bak 残留（stash 全局已清）
// ============================================================================

TEST_F(StashRealPkgTest, NoBackupLeftoverAcrossFullUsrTree)
{
    install_sysroot();
    install_packages({pkg("acl-2.4.0+2.lpkg").string()});
    install_packages({pkg("attr-2.6.0+2.lpkg").string()});
    remove_package("attr", /*force=*/false);
    install_packages({pkg("acl-2.5.0.lpkg").string()});
    remove_package("acl", /*force=*/false);

    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(test_root / "usr", ec)) {
        if (ec) break;
        EXPECT_EQ(e.path().filename().string().find(".lpkg_bak"), std::string::npos)
            << "usr 树下不得有任何 .lpkg_bak 残留: " << e.path();
    }
    expect_no_backup_leftover();
}
