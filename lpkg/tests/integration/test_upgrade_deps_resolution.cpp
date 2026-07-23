#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/archive/packer.hpp"
#include "../../main/src/crypto/hash.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/base/constants.hpp"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================================
// 升级依赖解析回归测试
//
// 场景 A（纯依赖发现）：
//   旧版本无外部依赖，新版本引入了新的依赖包。升级时应自动解析并安装。
//
// 场景 B（自有库 → 外部依赖）：
//   旧版本自带了 libX.so，新版本不再自带但依赖外部包 P。
//   升级时必须先安装 P 让 P 接管 libX.so，否则旧版本文件清理会删掉 libX.so。
// ============================================================================

class UpgradeDepsResolutionTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;
    fs::path mirror_dir;

    void SetUp() override {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_upgrade_deps_test");
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        mirror_dir = suite_work_dir / "mirror" / "x86_64";

        fs::remove_all(suite_work_dir);
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);
        fs::create_directories(mirror_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().set_architecture("x86_64");
        Config::instance().init_filesystem();
        Cache::instance().load();

        // 设置本地镜像
        std::ofstream(test_root / "etc/lpkg/mirror.conf")
            << "file://" << suite_work_dir.string() << "/mirror/" << std::endl;
    }

    void TearDown() override {
        Config::instance().set_root_path("/");
        Config::instance().set_testing_mode(false);
        fs::remove_all(suite_work_dir);
    }

    /** 创建虚拟包，支持自定义文件列表 */
    std::string create_pkg(
        const std::string& name, const std::string& ver,
        const std::vector<std::pair<std::string, std::string>>& files,
        const std::vector<std::string>& deps = {},
        const std::vector<std::string>& provides = {},
        const std::vector<std::string>& needed_so = {}) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name + "_" + ver);
        fs::remove_all(work_dir);
        fs::create_directories(work_dir / "content");

        for (const auto& [path, content] : files) {
            fs::path p = work_dir / "content" / path;
            ensure_dir_exists(p.parent_path());
            std::ofstream f(p);
            f << content;
            f.close();
        }

        std::string pkg_file = name + "-" + ver + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_file).string();
        pack_package(pkg_path, work_dir.string(), name, ver, deps, provides,
                     "man " + name, needed_so);

        fs::remove_all(work_dir);
        return pkg_path;
    }

    /** 将包加入本地镜像仓库 */
    void add_to_mirror(const std::string& name, const std::string& ver) {
        fs::path pkg_subdir = mirror_dir / name;
        fs::create_directories(pkg_subdir);
        fs::copy(pkg_dir / (name + "-" + ver + ".lpkg"),
                 pkg_subdir / (ver + ".lpkg"),
                 fs::copy_options::overwrite_existing);
    }

    /** 更新仓库索引 */
    void update_index(const std::vector<
        std::tuple<std::string, std::string, std::string,
                   std::string, std::string>>& entries) {
        std::ofstream index(mirror_dir / "index.txt");
        for (const auto& [name, ver, deps, provides, needed_so] : entries) {
            std::string pkg_path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
            std::string hash = "unknown";
            if (fs::exists(pkg_path))
                hash = calculate_sha256(pkg_path);
            index << name << "|" << ver << ":" << hash << ":"
                  << deps << ":" << provides << ":" << needed_so << "\n";
        }
    }
};

// ===================================================================
// 场景 A：纯依赖发现 — app v1 无外部依赖，app v2 新增依赖 libprovider
// ===================================================================

TEST_F(UpgradeDepsResolutionTest, UpgradeDiscoversNewDependency) {
    // ── 第1步：安装 app v1（不捆绑任何库） ─────────────────────
    std::string p1 = create_pkg("app", "1.0",
        {{"usr/bin/app", "app v1"}});  // 只有 app 自身，不捆绑任何共享库
    ASSERT_NO_THROW(install_packages({p1}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("app"));
    ASSERT_EQ(Cache::instance().get_installed_version("app"), "1.0");

    // ── 第2步：设置仓库 ─────────────────────────────────────────
    // app v2：新增依赖 libprovider
    create_pkg("app", "2.0",
        {{"usr/bin/app", "app v2"}},
        {"libprovider"});

    // libprovider v1：独立的依赖包
    create_pkg("libprovider", "1.0",
        {{"usr/lib/libhelper.so.1", "helper from provider\n"}},
        {},
        {"libhelper.so.1"});

    add_to_mirror("app", "2.0");
    add_to_mirror("libprovider", "1.0");
    update_index({
        {"app", "2.0", "libprovider", "", ""},
        {"libprovider", "1.0", "", "libhelper.so.1", ""},
    });

    // ── 第3步：执行升级 ─────────────────────────────────────────
    EXPECT_NO_THROW(upgrade_packages());

    // ── 第4步：验证 ─────────────────────────────────────────────
    Cache::instance().load();
    // app 应升级到 v2
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_EQ(Cache::instance().get_installed_version("app"), "2.0");
    // libprovider 应被自动安装
    EXPECT_TRUE(Cache::instance().is_installed("libprovider"))
        << "upgrade_packages() 应自动安装新版本引入的依赖";
}

// ===================================================================
// 场景 B：自有库 → 外部依赖（真实 bug 场景）
// app v1 自带 libhelper.so.1，v2 不再自带但依赖 libprovider
// ===================================================================

TEST_F(UpgradeDepsResolutionTest, BundledLibTransitionsToExternalDep) {
    // ── 第1步：安装 app v1（自带 libhelper.so.1） ──────────────
    std::string p1 = create_pkg("app", "1.0",
        {{"usr/bin/app", "#!/bin/sh\necho app\n"},
         {"usr/lib/libhelper.so.1", "helper lib v1"}},
        {},               // 无依赖
        {"libhelper.so.1"});  // 提供 libhelper.so.1
    ASSERT_NO_THROW(install_packages({p1}));

    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("app"));
    ASSERT_EQ(Cache::instance().get_installed_version("app"), "1.0");
    ASSERT_TRUE(fs::exists(test_root / "usr/lib/libhelper.so.1"));

    // ── 第2步：设置仓库 ─────────────────────────────────────────
    // app v2：不再自带 libhelper，依赖 libprovider
    create_pkg("app", "2.0",
        {{"usr/bin/app", "#!/bin/sh\necho app v2\n"}},
        {"libprovider"});

    // libprovider v1：提供 libhelper.so.1
    create_pkg("libprovider", "1.0",
        {{"usr/lib/libhelper.so.1", "helper from provider\n"}},
        {},
        {"libhelper.so.1"});

    add_to_mirror("app", "2.0");
    add_to_mirror("libprovider", "1.0");
    update_index({
        {"app", "2.0", "libprovider", "", ""},
        {"libprovider", "1.0", "", "libhelper.so.1", ""},
    });

    // ── 第3步：执行升级 ─────────────────────────────────────────
    EXPECT_NO_THROW(upgrade_packages());

    // ── 第4步：验证 ─────────────────────────────────────────────
    Cache::instance().load();

    // app 应升级到 v2
    EXPECT_EQ(Cache::instance().get_installed_version("app"), "2.0");

    // libprovider 应被自动安装
    EXPECT_TRUE(Cache::instance().is_installed("libprovider"))
        << "升级应自动安装新依赖 libprovider";

    // libhelper.so.1 应仍然存在（现在由 libprovider 提供）
    EXPECT_TRUE(fs::exists(test_root / "usr/lib/libhelper.so.1"))
        << "libhelper.so.1 不应被删除（libprovider 现在提供该文件）";

    // libhelper.so.1 的所有权应已转移给 libprovider
    auto owners = Cache::instance().get_file_owners("/usr/lib/libhelper.so.1");
    EXPECT_TRUE(owners.contains("libprovider"))
        << "libhelper.so.1 应归属于 libprovider";
    // app 可能仍共享持有该文件（取决于 old_file 清理时点的注册顺序）
    // 关键是 libprovider 持有它，且文件未被物理删除
}

// ===================================================================
// 边界：新版本依赖的包已经安装 → 升级正常，不重复安装
// ===================================================================

TEST_F(UpgradeDepsResolutionTest, NoopWhenDepAlreadyInstalled) {
    // ── 第1步：安装 libprovider 和 app v1（不捆绑 lib） ──────
    std::string libpkg = create_pkg("libprovider", "1.0",
        {{"usr/lib/libhelper.so.1", "helper"}},
        {},
        {"libhelper.so.1"});
    ASSERT_NO_THROW(install_packages({libpkg}));

    std::string p1 = create_pkg("app", "1.0",
        {{"usr/bin/app", "app v1"}});  // app 不捆绑 lib
    ASSERT_NO_THROW(install_packages({p1}));

    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("app"));
    ASSERT_TRUE(Cache::instance().is_installed("libprovider"));
    ASSERT_EQ(Cache::instance().get_installed_version("app"), "1.0");

    // ── 第2步：设置仓库 ─────────────────────────────────────────
    create_pkg("app", "2.0",
        {{"usr/bin/app", "app v2"}},
        {"libprovider"});
    add_to_mirror("app", "2.0");
    update_index({
        {"app", "2.0", "libprovider", "", ""},
        {"libprovider", "1.0", "", "libhelper.so.1", ""},
    });

    // ── 第3步：升级 ─────────────────────────────────────────────
    EXPECT_NO_THROW(upgrade_packages());

    // ── 第4步：验证 ─────────────────────────────────────────────
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("app"), "2.0");
    EXPECT_TRUE(Cache::instance().is_installed("libprovider"));
    EXPECT_TRUE(fs::exists(test_root / "usr/lib/libhelper.so.1"));
}

// ===================================================================
// 边界：版本号相同不触发升级
// ===================================================================

TEST_F(UpgradeDepsResolutionTest, SameVersionSkipsUpgrade) {
    std::string p1 = create_pkg("app", "1.0",
        {{"usr/bin/app", "app v1"}});
    ASSERT_NO_THROW(install_packages({p1}));
    Cache::instance().load();
    ASSERT_EQ(Cache::instance().get_installed_version("app"), "1.0");

    // 仓库中 app v1 版本号相同
    create_pkg("app", "1.0",
        {{"usr/bin/app", "app v1 rebuilt"}},
        {"libprovider"});  // 索引说依赖 libprovider
    add_to_mirror("app", "1.0");
    update_index({{"app", "1.0", "libprovider", "", ""}});

    EXPECT_NO_THROW(upgrade_packages());
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("app"), "1.0");
    EXPECT_FALSE(Cache::instance().is_installed("libprovider"))
        << "版本号相同时不应触发升级或安装新依赖";
}

// ===================================================================
// 多包同时升级，各自引入新依赖
// ===================================================================

TEST_F(UpgradeDepsResolutionTest, MultipleUpgradesWithNewDeps) {
    std::string pa1 = create_pkg("appA", "1.0",
        {{"usr/bin/appA", "A v1"}});
    std::string pb1 = create_pkg("appB", "1.0",
        {{"usr/bin/appB", "B v1"}});
    ASSERT_NO_THROW(install_packages({pa1}));
    ASSERT_NO_THROW(install_packages({pb1}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("appA"));
    ASSERT_TRUE(Cache::instance().is_installed("appB"));

    create_pkg("appA", "2.0", {{"usr/bin/appA", "A v2"}}, {"libX"});
    create_pkg("appB", "2.0", {{"usr/bin/appB", "B v2"}}, {"libY"});
    create_pkg("libX", "1.0", {{"usr/lib/libX.so.1", "X"}}, {}, {"libX.so.1"});
    create_pkg("libY", "1.0", {{"usr/lib/libY.so.1", "Y"}}, {}, {"libY.so.1"});

    add_to_mirror("appA", "2.0");
    add_to_mirror("appB", "2.0");
    add_to_mirror("libX", "1.0");
    add_to_mirror("libY", "1.0");
    update_index({
        {"appA", "2.0", "libX", "", ""},
        {"appB", "2.0", "libY", "", ""},
        {"libX", "1.0", "", "libX.so.1", ""},
        {"libY", "1.0", "", "libY.so.1", ""},
    });

    EXPECT_NO_THROW(upgrade_packages());

    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("appA"), "2.0");
    EXPECT_EQ(Cache::instance().get_installed_version("appB"), "2.0");
    EXPECT_TRUE(Cache::instance().is_installed("libX"));
    EXPECT_TRUE(Cache::instance().is_installed("libY"));
}

// ===================================================================
// 边界：新依赖在仓库中不存在 → 抛出异常
// ===================================================================

TEST_F(UpgradeDepsResolutionTest, MissingDepThrows) {
    std::string p1 = create_pkg("app", "1.0",
        {{"usr/bin/app", "app v1"}});
    ASSERT_NO_THROW(install_packages({p1}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("app"));

    create_pkg("app", "2.0",
        {{"usr/bin/app", "app v2"}},
        {"missing-dep"});
    add_to_mirror("app", "2.0");
    update_index({{"app", "2.0", "missing-dep", "", ""}});

    EXPECT_THROW(upgrade_packages(), LpkgException);
}

// ===================================================================
// 边界：升级后 held 状态保持不变
// ===================================================================

TEST_F(UpgradeDepsResolutionTest, HoldPreservedAfterUpgrade) {
    std::string p1 = create_pkg("app", "1.0",
        {{"usr/bin/app", "app v1"}});
    ASSERT_NO_THROW(install_packages({p1}));
    // 标记为 held
    Cache::instance().load();
    Cache::instance().add_installed("app", "1.0", true);
    ASSERT_TRUE(Cache::instance().is_held("app"));

    create_pkg("app", "2.0", {{"usr/bin/app", "app v2"}});
    add_to_mirror("app", "2.0");
    update_index({{"app", "2.0", "", "", ""}});

    EXPECT_NO_THROW(upgrade_packages());

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_held("app"))
        << "升级后包应保持 held 状态";
}

// ===================================================================
// 边界：新依赖在升级前已手动安装
// ===================================================================

TEST_F(UpgradeDepsResolutionTest, NewDepAlreadyInstalledManually) {
    std::string p1 = create_pkg("app", "1.0",
        {{"usr/bin/app", "app v1"}});
    ASSERT_NO_THROW(install_packages({p1}));

    // 提前手动安装 libprovider
    std::string libpkg = create_pkg("libprovider", "1.0",
        {{"usr/lib/libhelper.so.1", "helper"}},
        {},
        {"libhelper.so.1"});
    ASSERT_NO_THROW(install_packages({libpkg}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("app"));
    ASSERT_TRUE(Cache::instance().is_installed("libprovider"));

    create_pkg("app", "2.0",
        {{"usr/bin/app", "app v2"}},
        {"libprovider"});
    add_to_mirror("app", "2.0");
    add_to_mirror("libprovider", "1.0");
    update_index({
        {"app", "2.0", "libprovider", "", ""},
        {"libprovider", "1.0", "", "libhelper.so.1", ""},
    });

    EXPECT_NO_THROW(upgrade_packages());
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("app"), "2.0");
    EXPECT_TRUE(Cache::instance().is_installed("libprovider"));
}
