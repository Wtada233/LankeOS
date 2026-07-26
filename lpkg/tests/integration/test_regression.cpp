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

class RegressionTest : public ::testing::Test
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
        init_localization();

        suite_work_dir = fs::absolute("tmp_regression_test");
        if (fs::exists(suite_work_dir)) {
            run_shell("sudo rm -rf " + suite_work_dir.string());
        }
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";

        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().set_architecture("x86_64");
        Config::instance().init_filesystem();

        fs::path mirror_path = suite_work_dir / "mirror";
        fs::create_directories(mirror_path / "x86_64");
        std::ofstream(test_root / "etc/lpkg/mirror.conf")
            << "file://" << mirror_path.string() << "/" << std::endl;
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
        fs::path mirror_pkg_dir = suite_work_dir / "mirror" / "x86_64" / name;
        fs::create_directories(mirror_pkg_dir);
        fs::copy_file(pkg_path, mirror_pkg_dir / (ver + ".lpkg"),
                      fs::copy_options::overwrite_existing);
        std::string hash = calculate_sha256(pkg_path);
        std::ofstream index(suite_work_dir / "mirror" / "x86_64" / "index.txt", std::ios::app);
        index << name << "|" << ver << ":" << hash << ":|" << std::endl;
        fs::remove_all(work_dir);
        return pkg_path;
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        run_shell("sudo rm -rf " + suite_work_dir.string());
    }
};

/** 配置文件在非 force 卸载时应保留在磁盘上 */
TEST_F(RegressionTest, ConfigFilePreservedOnNormalRemove)
{
    std::string pkg = create_pkg("conf-keep", "1.0",
                                 {
                                     {"etc/myapp.conf", "/"},
                                     {"usr/bin/myapp", "/"},
                                 });
    install_packages({pkg}, "", false);

    fs::path conf = test_root / "etc/myapp.conf";
    fs::path bin = test_root / "usr/bin/myapp";
    EXPECT_TRUE(fs::exists(conf));
    EXPECT_TRUE(fs::exists(bin));

    // 正常卸载——配置文件应保留，普通文件应删除
    remove_package("conf-keep", false);

    EXPECT_TRUE(fs::exists(conf)) << "配置文件应在普通卸载时保留";
    EXPECT_FALSE(fs::exists(bin)) << "普通文件应在卸载时删除";

    // 所有权应已被移除
    auto owners = Cache::instance().get_file_owners("/etc/myapp.conf");
    EXPECT_TRUE(owners.empty()) << "配置文件的所有权应从 DB 移除";
}

/** force 卸载时应删除配置文件 */
TEST_F(RegressionTest, ConfigFileDeletedOnForceRemove)
{
    std::string pkg = create_pkg("conf-force", "1.0",
                                 {
                                     {"etc/myapp.conf", "/"},
                                 });
    install_packages({pkg}, "", false);

    fs::path conf = test_root / "etc/myapp.conf";
    EXPECT_TRUE(fs::exists(conf));

    remove_package("conf-force", true);

    EXPECT_FALSE(fs::exists(conf)) << "配置文件应在 force 卸载时删除";
}

/** 两个包不能同时拥有同一个配置文件 */
TEST_F(RegressionTest, CrossPackageConfigConflict)
{
    std::string pkgA = create_pkg("pkgA", "1.0", {{"etc/shared.conf", "/"}});
    install_packages({pkgA}, "", false);

    std::string pkgB = create_pkg("pkgB", "1.0", {{"etc/shared.conf", "/"}});
    EXPECT_THROW(install_packages({pkgB}, "", false), LpkgException);
}

/** 同包升级应产生 .lpkgnew 且原配置保留 */
TEST_F(RegressionTest, SamePackageConfigNewOnUpgrade)
{
    std::string pkg = create_pkg("cfg-upgrade", "1.0", {{"etc/app.conf", "/"}});
    install_packages({pkg}, "", false);

    fs::path conf = test_root / "etc/app.conf";
    fs::path conf_new = test_root / "etc/app.conf.lpkgnew";
    ASSERT_TRUE(fs::exists(conf));

    // 模拟用户修改配置
    {
        std::ofstream f(conf);
        f << "user modified";
        f.close();
    }

    // 安装新版本（同包名）
    std::string pkg2 = create_pkg("cfg-upgrade", "2.0", {{"etc/app.conf", "/"}});
    EXPECT_NO_THROW(install_packages({pkg2}, "", false));

    // 原配置应保留，新版本应产生 .lpkgnew
    {
        std::ifstream f(conf);
        std::string s;
        std::getline(f, s);
        EXPECT_EQ(s, "user modified") << "原配置应保留";
    }
    EXPECT_TRUE(fs::exists(conf_new)) << "应产生 .lpkgnew";
}
