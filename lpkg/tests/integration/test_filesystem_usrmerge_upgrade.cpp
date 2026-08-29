/**
 * test_filesystem_usrmerge_upgrade.cpp
 *
 * 回归测试：升级 filesystem 包（usr-merge 布局，keep_fs_layout=true）时，
 * 不得删除其他包安装到 /usr/bin、/usr/lib 的内容。
 *
 * 背景 bug：filesystem 包持有根级 usr-merge 符号链接（bin→usr/bin、lib→usr/lib、
 * lib64→usr/lib、usr/lib64→lib 等）以及 usr/bin/、usr/lib/、usr/include/ 等空目录
 * 条目。升级它（旧版→新版，新版新增 /usr/share/pixmaps/lankeos-logo.svg）时，
 * 其他包安装到 /usr/bin 与 /usr/lib 的文件全部被删除（overlayFS 下表现为 whiteout）。
 *
 * 本测试构造内容"差不多"的两个 filesystem 包 + 一个持有 /usr/bin、/usr/lib
 * 文件的 binary 包，执行升级后断言 binary 包的文件依然存在。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"

namespace fs = std::filesystem;

class FilesystemUsrMergeUpgradeTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        Config::instance().set_no_hooks_mode(true);
        Config::instance().set_no_deps_mode(false);
        init_localization();

        suite_work_dir = fs::absolute("tmp_fs_usrmerge_test");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";

        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    /** 生成一个 .lpkg 包文件，返回路径。content_map: 相对 content/ 的路径 → 内容 */
    std::string pack_pkg(const std::string& name, const std::string& version,
                         const std::map<std::string, std::string>& content_map,
                         const std::map<std::string, std::string>& dir_symlinks = {},
                         const std::map<std::string, std::string>& file_symlinks = {})
    {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::remove_all(work_dir);
        fs::path content = work_dir / "content";
        fs::create_directories(content);

        // 文件（含空目录占位：以 / 结尾的路径创建目录）
        for (const auto& [path, data] : content_map) {
            fs::path full = content / path;
            if (path.ends_with('/')) {
                fs::create_directories(full);
            } else {
                ensure_dir_exists(full.parent_path());
                std::ofstream f(full);
                f << data;
            }
        }
        // 目录符号链接
        for (const auto& [src, target] : dir_symlinks) {
            fs::path link_path = content / src;
            ensure_dir_exists(link_path.parent_path());
            fs::create_directory_symlink(target, link_path);
        }
        // 文件符号链接（如 etc/os-release → ../usr/lib/os-release）
        for (const auto& [src, target] : file_symlinks) {
            fs::path link_path = content / src;
            ensure_dir_exists(link_path.parent_path());
            fs::create_symlink(target, link_path);
        }

        std::string pkg_path = (pkg_dir / (name + "-" + version + ".lpkg")).string();
        pack_package(pkg_path, work_dir.string(), name, version, {}, {}, "man " + name, {});
        fs::remove_all(work_dir);
        return pkg_path;
    }

    /**
     * 构造 filesystem 风格包：usr-merge 布局（根级 symlink + 空目录）+ 基础文件。
     * 与真实 pkgs/filesystem（keep_fs_layout=true）的打包形态一致。
     */
    std::string make_filesystem_pkg(const std::string& version, bool with_logo)
    {
        std::map<std::string, std::string> content = {
            // 目录（builder setup_build_directories 预建的空目录，keep_fs_layout 保留）
            {"usr/bin/", ""},
            {"usr/include/", ""},
            {"usr/lib/", ""},
            {"usr/share/", ""},
            {"usr/share/man/", ""},
            {"usr/share/lankeos/", ""},
            {"usr/share/lankeos/fonts/", ""},
            {"usr/share/pixmaps/", ""},
            {"etc/", ""},
            // 文件
            {"usr/lib/os-release", "NAME=\"LankeOS\"\n"},
            {"usr/share/lankeos/logo.txt", "LankeOS\n"},
            {"usr/share/lankeos/fonts/unicode.pf2", "PF2\n"},
            {"etc/lanke-release", "LankeOS\n"},
            {"etc/os-release-target", "dummy\n"},
        };
        if (with_logo) {
            content.emplace("usr/share/pixmaps/lankeos-logo.svg", "<svg/>\n");
        }
        return pack_pkg("filesystem", version, content,
                        /*dir_symlinks=*/
                        {{"bin", "usr/bin"},
                         {"sbin", "usr/bin"},
                         {"lib", "usr/lib"},
                         {"lib64", "usr/lib"},
                         {"usr/sbin", "bin"},
                         {"usr/lib64", "lib"}},
                        /*file_symlinks=*/
                        {{"etc/os-release", "../usr/lib/os-release"}});
    }

    bool exists_in_root(const std::string& rel_path)
    {
        return fs::exists(test_root / rel_path);
    }

    bool is_registered(const std::string& name)
    {
        return Cache::instance().is_installed(name);
    }
};

// =========================================================================
// 核心回归：升级 filesystem 不得删除其他包的 /usr/bin、/usr/lib 内容
// =========================================================================

TEST_F(FilesystemUsrMergeUpgradeTest, UpgradeFilesystemPreservesOtherPkgsBinaries)
{
    // 1. 先装一个持有 /usr/bin 与 /usr/lib 文件的包（模拟 bash/coreutils）
    std::string bin_pkg = pack_pkg("coreutils-like", "1.0",
                                   {{"usr/bin/ls", "#!/bin/sh\nls\n"},
                                    {"usr/lib/libfoo.so.1", "ELF\n"}});
    ASSERT_NO_THROW(install_packages({bin_pkg}));
    write_cache();
    ASSERT_TRUE(exists_in_root("usr/bin/ls"));
    ASSERT_TRUE(exists_in_root("usr/lib/libfoo.so.1"));

    // 2. 安装 filesystem 旧版（usr-merge 布局，无 logo）
    std::string fs_v1 = make_filesystem_pkg("1.5.1+2", /*with_logo=*/false);
    ASSERT_NO_THROW(install_packages({fs_v1}));
    write_cache();
    ASSERT_TRUE(fs::is_symlink(test_root / "bin")) << "/bin 应为 usr-merge 符号链接";
    ASSERT_TRUE(fs::is_symlink(test_root / "lib")) << "/lib 应为 usr-merge 符号链接";

    // 3. 升级到新版（多一个 usr/share/pixmaps/lankeos-logo.svg）
    std::string fs_v2 = make_filesystem_pkg("1.5.1+3", /*with_logo=*/true);
    ASSERT_NO_THROW(install_packages({fs_v2}));
    write_cache();

    // 4. 其他包的文件必须完好
    EXPECT_TRUE(exists_in_root("usr/bin/ls"))
        << "升级 filesystem 后 /usr/bin/ls 被删除（回归 bug）";
    EXPECT_TRUE(exists_in_root("usr/lib/libfoo.so.1"))
        << "升级 filesystem 后 /usr/lib/libfoo.so.1 被删除（回归 bug）";

    // 5. usr-merge 符号链接必须仍是符号链接（不得被解析成目录覆盖）
    EXPECT_TRUE(fs::is_symlink(test_root / "bin"));
    EXPECT_TRUE(fs::is_symlink(test_root / "lib"));
    EXPECT_TRUE(fs::is_symlink(test_root / "usr/lib64"));

    // 6. filesystem 新版内容就位
    EXPECT_TRUE(exists_in_root("usr/share/pixmaps/lankeos-logo.svg"));
    EXPECT_TRUE(fs::is_symlink(test_root / "etc/os-release"));

    // 7. 升级后 /usr/bin 目录必须仍包含其他包的文件（整体检查）
    std::vector<std::string> bin_entries;
    for (const auto& e : fs::directory_iterator(test_root / "usr/bin")) {
        bin_entries.push_back(e.path().filename().string());
    }
    EXPECT_NE(std::find(bin_entries.begin(), bin_entries.end(), "ls"), bin_entries.end())
        << "/usr/bin 目录应包含其他包安装的 ls";
}

// 升级后其余二进制（/usr/lib64 symlink 解析）不受影响
TEST_F(FilesystemUsrMergeUpgradeTest, UpgradeKeepsLib64SymlinkResolving)
{
    std::string fs_v1 = make_filesystem_pkg("1.0", /*with_logo=*/false);
    ASSERT_NO_THROW(install_packages({fs_v1}));
    write_cache();

    std::string fs_v2 = make_filesystem_pkg("1.1", /*with_logo=*/true);
    ASSERT_NO_THROW(install_packages({fs_v2}));
    write_cache();

    // /usr/lib64 → lib → usr/lib 链完整
    EXPECT_EQ(fs::read_symlink(test_root / "usr/lib64"), fs::path("lib"));
    EXPECT_EQ(fs::read_symlink(test_root / "lib"), fs::path("usr/lib"));
    EXPECT_TRUE(exists_in_root("usr/lib"));
}
