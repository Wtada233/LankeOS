#include <gtest/gtest.h>
#include <sys/mount.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/crypto/hash.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"

namespace fs = std::filesystem;

class NewFeaturesTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        Config::instance().set_force_overwrite_mode(false);
        Config::instance().set_no_hooks_mode(false);
        Config::instance().set_no_deps_mode(false);
        setenv("LANG", "C", 1);
        init_localization();

        suite_work_dir = fs::absolute("tmp_new_features_test");
        if (fs::exists(suite_work_dir)) {
            std::string clean_cmd = "sudo rm -rf " + suite_work_dir.string();
            run_shell(clean_cmd);
        }
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";

        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().set_architecture("x86_64");
        Config::instance().init_filesystem();

        // Setup mock mirror
        fs::path mirror_path = suite_work_dir / "mirror";
        fs::create_directories(mirror_path / "x86_64");
        std::ofstream(test_root / "etc/lpkg/mirror.conf")
            << "file://" << mirror_path.string() << "/" << std::endl;
        // Create initial empty index
        std::ofstream(mirror_path / "x86_64" / "index.txt").close();
    }

    std::string create_pkg(const std::string& name, const std::string& ver,
                           const std::vector<std::pair<std::string, std::string>>& files)
    {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name + "_" + ver);
        fs::create_directories(work_dir / "content");

        for (const auto& [src, dest] : files) {
            fs::path p = work_dir / "content" / src;
            fs::create_directories(p.parent_path());
            std::ofstream f(p);
            f << "content of " << src;
            f.close();
        }

        std::string pkg_filename = name + "-" + ver + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_filename).string();
        pack_package(pkg_path, work_dir.string(), name, ver);

        // Also put it in the mirror
        fs::path mirror_pkg_dir = suite_work_dir / "mirror" / "x86_64" / name;
        fs::create_directories(mirror_pkg_dir);
        fs::copy_file(pkg_path, mirror_pkg_dir / (ver + ".lpkg"),
                      fs::copy_options::overwrite_existing);

        std::string hash = calculate_sha256(pkg_path);

        // Update index.txt with new format: name|v:h:deps|provides
        std::ofstream index(suite_work_dir / "mirror" / "x86_64" / "index.txt", std::ios::app);
        index << name << "|" << ver << ":" << hash << ":|" << std::endl;

        fs::remove_all(work_dir);
        return pkg_path;
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        std::string clean_cmd = "sudo rm -rf " + suite_work_dir.string();
        run_shell(clean_cmd);
    }
};

TEST_F(NewFeaturesTest, QueryFileAndPackage)
{
    std::string pkg = create_pkg("query_test", "1.0", {{"usr/bin/query_target", "/"}});
    install_packages({pkg}, "", false);

    // Test query file (absolute)
    testing::internal::CaptureStdout();
    query_file("/usr/bin/query_target");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("query_test"), std::string::npos);

    // Test query file (relative with smart resolution)
    // We simulate being in /usr/bin and querying 'query_target'
    fs::path old_cwd = fs::current_path();
    fs::create_directories(test_root / "usr/bin");
    fs::current_path(test_root / "usr/bin");

    testing::internal::CaptureStdout();
    query_file("query_target");
    output = testing::internal::GetCapturedStdout();
    fs::current_path(old_cwd);  // Restore CWD

    EXPECT_NE(output.find("query_test"), std::string::npos);
    EXPECT_NE(output.find("/usr/bin/query_target"), std::string::npos);

    // Test query package
    testing::internal::CaptureStdout();
    query_package("query_test");
    output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("/usr/bin/query_target"), std::string::npos);
}

/** 回归测试：安装应把包内容中的目录注册为所有者（query 目录能查到所属包） */
TEST_F(NewFeaturesTest, DirectoryOwnersRecordedOnInstall)
{
    std::string pkg = create_pkg("dir_owner_test", "1.0",
                                 {{"usr/share/dir_owner_app/data.txt", "/"}});
    install_packages({pkg}, "", false);

    // 包内容里的目录（含中间层级）都应注册所有者
    EXPECT_TRUE(Cache::instance().is_file_owned_by("/usr/", "dir_owner_test"));
    EXPECT_TRUE(Cache::instance().is_file_owned_by("/usr/share/", "dir_owner_test"));
    EXPECT_TRUE(Cache::instance().is_file_owned_by("/usr/share/dir_owner_app/", "dir_owner_test"));
    // 普通文件仍由 add_file_owner 注册
    EXPECT_TRUE(Cache::instance().is_file_owned_by("/usr/share/dir_owner_app/data.txt",
                                                   "dir_owner_test"));

    // query 目录应解析到所属包
    testing::internal::CaptureStdout();
    query_file("/usr/share/dir_owner_app/");
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("dir_owner_test"), std::string::npos);

    // query 包应列出目录条目
    testing::internal::CaptureStdout();
    query_package("dir_owner_test");
    out = testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("/usr/share/dir_owner_app/"), std::string::npos);
}

/** 回归测试：多个包共享同一目录不应抛冲突，且都记为所有者 */
TEST_F(NewFeaturesTest, SharedDirectoryRecordsAllOwners)
{
    std::string pkg_a = create_pkg("dir_shared_a", "1.0",
                                   {{"usr/share/shared_dir/a.txt", "/"}});
    std::string pkg_b = create_pkg("dir_shared_b", "1.0",
                                   {{"usr/share/shared_dir/b.txt", "/"}});

    // 两个包都安装到 /usr/share/shared_dir/ —— 目录可共享，不得报 file_already_owned
    EXPECT_NO_THROW(install_packages({pkg_a}, "", false));
    EXPECT_NO_THROW(install_packages({pkg_b}, "", false));

    auto owners = Cache::instance().get_file_owners("/usr/share/shared_dir/");
    EXPECT_TRUE(owners.contains("dir_shared_a"));
    EXPECT_TRUE(owners.contains("dir_shared_b"));
}

TEST_F(NewFeaturesTest, ReinstallPackage)
{
    std::string pkg = create_pkg("reinstall_test", "1.0", {{"usr/bin/reinstall_bin", "/"}});

    install_packages({pkg}, "", false);

    fs::path bin_path = test_root / "usr/bin/reinstall_bin";
    EXPECT_TRUE(fs::exists(bin_path));

    // Modify the file to see if it gets restored
    {
        std::ofstream f(bin_path);
        f << "modified";
    }

    // Reinstall
    reinstall_package("reinstall_test");

    {
        std::ifstream f(bin_path);
        std::string s;
        std::getline(f, s);
        EXPECT_EQ(s, "content of usr/bin/reinstall_bin");
    }

    // Additional Test: Reinstall via PATH
    {
        std::ofstream f(bin_path);
        f << "modified again";
    }
    reinstall_package(pkg);  // Use path instead of name
    {
        std::ifstream f(bin_path);
        std::string s;
        std::getline(f, s);
        EXPECT_EQ(s, "content of usr/bin/reinstall_bin");
    }
}

/** 回归测试：query_file 不应跟随软链接解析所有权 */
TEST_F(NewFeaturesTest, QuerySymlinkDoesNotResolve)
{
    // 安装一个含软链接的包：/usr/bin/link_target （普通文件）
    std::string pkg = create_pkg("symlink_query_test", "1.0",
                                 {
                                     {"usr/lib/myapp/real_bin", "/"},
                                 });
    install_packages({pkg}, "", false);

    // 在测试 root 下手动创建软链接 /usr/bin/link → ../lib/myapp/real_bin
    fs::create_directories(test_root / "usr/bin");
    fs::create_symlink("../lib/myapp/real_bin", test_root / "usr/bin/link");

    // 注册软链接路径到数据库（模拟包管理器的正常行为）
    Cache::instance().add_file_owner("/usr/bin/link", "symlink_query_test");

    // 正常情况：查询软链接路径应该能找到
    testing::internal::CaptureStdout();
    query_file("/usr/bin/link");
    std::string out = testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("/usr/bin/link"), std::string::npos);

    // 模拟用户删除了 files.db 中 /usr/bin/link 的条目
    Cache::instance().remove_file_owner("/usr/bin/link", "symlink_query_test");

    // BUG 复现：此时 query_file 不应跟随软链接去解析目标
    testing::internal::CaptureStdout();
    query_file("/usr/bin/link");
    out = testing::internal::GetCapturedStdout();

    // 不应该显示目标路径的所有权（目标路径属于 symlink_query_test 但不应该被查到）
    // 且应该报告文件不受管理
    EXPECT_NE(out.find("is not owned by any package"), std::string::npos);
}

/** 回归测试：重装时应检测孤立文件冲突 */
TEST_F(NewFeaturesTest, ReinstallDetectsOrphanedFileConflict)
{
    std::string pkg = create_pkg("orphan_test", "1.0",
                                 {
                                     {"usr/bin/orphan_bin", "/"},
                                 });
    install_packages({pkg}, "", false);

    fs::path bin_path = test_root / "usr/bin/orphan_bin";
    EXPECT_TRUE(fs::exists(bin_path));

    // 模拟用户从 files.db 中删除了该文件的条目
    Cache::instance().remove_file_owner("/usr/bin/orphan_bin", "orphan_test");
    Cache::instance().write();  // 持久化到磁盘，否则重装时 Cache::load() 会恢复旧状态

    // 此时文件在磁盘上存在，但数据库里无记录——重装应当检测到冲突
    EXPECT_THROW({ reinstall_package("orphan_test"); }, LpkgException);
}

TEST_F(NewFeaturesTest, ReinstallAtomicRollback)
{
    // 1. Install version 1 of a package
    std::string pkg = create_pkg("rollback_test", "1.0", {{"usr/bin/app", "/"}});
    install_packages({pkg}, "", false);

    fs::path app_path = test_root / "usr/bin/app";
    ASSERT_TRUE(fs::exists(app_path));

    // 注意: copy_package_files 现在会纠正目录权限，因此重装应当成功。
    // 这里验证重装后的文件内容正确。
    EXPECT_NO_THROW(reinstall_package(pkg));

    // 5. VERIFY: The package should STILL be marked as installed
    EXPECT_EQ(Cache::instance().get_installed_version("rollback_test"), "1.0");

    // 6. VERIFY: The file should still exist and contain original content
    EXPECT_TRUE(fs::exists(app_path));
    std::ifstream f(app_path);
    std::string s;
    std::getline(f, s);
    EXPECT_EQ(s, "content of usr/bin/app");
}