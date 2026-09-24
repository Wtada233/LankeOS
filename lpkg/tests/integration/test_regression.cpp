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
        setenv("LANG", "C", 1);
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

/** 普通卸载：配置文件改名为 .lpkgsave 保留（不原地留、也不删），普通文件真删 */
TEST_F(RegressionTest, ConfigFileSavedAsLpkgsaveOnNormalRemove)
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

    // 正常卸载——配置文件应改名为 <路径>.lpkgsave，普通文件应删除
    remove_package("conf-keep", false);

    fs::path saved = test_root / "etc/myapp.conf.lpkgsave";
    EXPECT_FALSE(fs::exists(conf)) << "配置不得原地保留（原地留 = 移除动作没做完）";
    EXPECT_TRUE(fs::exists(saved)) << "配置文件应改名为 .lpkgsave 保留";
    EXPECT_FALSE(fs::exists(bin)) << "普通文件应在卸载时删除";

    // 所有权应已被移除
    auto owners = Cache::instance().get_file_owners("/etc/myapp.conf");
    EXPECT_TRUE(owners.empty()) << "配置文件的所有权应从 DB 移除";
}

/** force 卸载：同样改名保留（force 的语义是跳过安全检查，不是丢配置） */
TEST_F(RegressionTest, ConfigFileSavedAsLpkgsaveOnForceRemove)
{
    std::string pkg = create_pkg("conf-force", "1.0",
                                 {
                                     {"etc/myapp.conf", "/"},
                                 });
    install_packages({pkg}, "", false);

    fs::path conf = test_root / "etc/myapp.conf";
    EXPECT_TRUE(fs::exists(conf));

    remove_package("conf-force", true);

    EXPECT_FALSE(fs::exists(conf)) << "配置不得原地保留";
    EXPECT_TRUE(fs::exists(test_root / "etc/myapp.conf.lpkgsave")) << "--force 不得把配置真删";
}

/** 只有显式 --purge-config 才真删配置文件 */
TEST_F(RegressionTest, ConfigFilePurgedOnlyWithPurgeConfig)
{
    std::string pkg = create_pkg("conf-purge", "1.0",
                                 {
                                     {"etc/myapp.conf", "/"},
                                 });
    install_packages({pkg}, "", false);

    fs::path conf = test_root / "etc/myapp.conf";
    EXPECT_TRUE(fs::exists(conf));

    remove_package("conf-purge", /*force=*/false, /*wrap_in_txn=*/true, /*purge_config=*/true);

    EXPECT_FALSE(fs::exists(conf));
    EXPECT_FALSE(fs::exists(test_root / "etc/myapp.conf.lpkgsave"));
}

/** 两个包不能同时拥有同一个配置文件 */
TEST_F(RegressionTest, CrossPackageConfigConflict)
{
    std::string pkgA = create_pkg("pkgA", "1.0", {{"etc/shared.conf", "/"}});
    install_packages({pkgA}, "", false);

    std::string pkgB = create_pkg("pkgB", "1.0", {{"etc/shared.conf", "/"}});
    EXPECT_THROW(install_packages({pkgB}, "", false), LpkgException);
}

/**
 * 同包升级、且**包内配置逐字节未变**：保留用户改过的配置，且**不产生** .lpkgnew。
 *
 * 这是三哈希分流（pacman `add.c`）的第 ② 条：`hash_orig == hash_pkg`（旧包记录 == 新包）
 * = 包本身没改这个配置文件 → 没有"新东西"要给用户审阅，保留用户文件、连 `.lpkgnew` 都不产生。
 * （改前：无条件产生 `.lpkgnew`，每次升级都往 /etc 堆一个"其实和用户文件无关"的新版。
 *  v1/v2 配置**不同**时必须产生 `.lpkgnew` 的那条由
 *  tests/integration/test_config_three_way_hash.cpp 覆盖。）
 *
 * 同文件另一条不变量（用户改过的配置**永远**不被静默覆盖）在本用例里同样成立：
 * 盘上那份仍是 user modified。
 */
TEST_F(RegressionTest, SamePackageUnchangedConfigKeepsUserFile)
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

    // 安装新版本（同包名，配置内容与 v1 相同）
    std::string pkg2 = create_pkg("cfg-upgrade", "2.0", {{"etc/app.conf", "/"}});
    EXPECT_NO_THROW(install_packages({pkg2}, "", false));

    {
        std::ifstream f(conf);
        std::string s;
        std::getline(f, s);
        EXPECT_EQ(s, "user modified") << "原配置应保留";
    }
    EXPECT_FALSE(fs::exists(conf_new))
        << "包没改这个配置（旧记录 == 新包）→ 没有要审阅的新内容，不该产生 .lpkgnew";
}
