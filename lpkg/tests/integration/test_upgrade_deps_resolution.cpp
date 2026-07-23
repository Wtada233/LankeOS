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
// 场景：旧版本包 A 自带了 libX.so，新版本包 A 不再自带但依赖外部包 P
// 来提供 libX.so。升级时必须解析出这个新依赖并先安装 P，
// 否则 libX.so 会因旧版本的文件清理而丢失。
//
// 该测试验证 upgrade_packages() 使用了和 install_packages() 相同的
// 依赖解析机制（resolve_package_dependencies），
// 能正确发现并安装新版本引入的依赖。
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

    /** 创建虚拟包，支持自定义文件和 SONAME */
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

    /** 更新仓库索引（格式：name|ver:hash:deps:provides:needed_so） */
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

// -----------------------------------------------------------------------
// 核心场景：新版本包不再自带 lib，依赖外部包来提供
//
// 步骤：
// 1. 安装 app v1（自带 /usr/lib/libhelper.so.1，提供 libhelper.so.1）
// 2. 设置仓库：app v2（依赖 libprovider，不再提供 libhelper.so.1）
//               libprovider v1（提供 libhelper.so.1）
// 3. 执行 upgrade_packages()
// 4. 验证：app 升级到 v2，libprovider 被自动安装，
//          libhelper.so.1 文件仍然存在
// -----------------------------------------------------------------------
TEST_F(UpgradeDepsResolutionTest, UpgradeDiscoversNewDependency) {
    // ── 第1步：安装 app v1（自带 libhelper.so.1） ────────────────
    std::string p1 = create_pkg("app", "1.0",
        {{"usr/bin/app", "#!/bin/sh\necho app\n"},
         {"usr/lib/libhelper.so.1", "helper lib v1"}},
        {},          // 无依赖
        {"libhelper.so.1"});  // 提供 libhelper.so.1
    ASSERT_NO_THROW(install_packages({p1}));

    // 验证安装完成
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("app"));
    ASSERT_EQ(Cache::instance().get_installed_version("app"), "1.0");
    ASSERT_TRUE(fs::exists(test_root / "usr/lib/libhelper.so.1"));

    // ── 第2步：设置仓库 ─────────────────────────────────────────
    // app v2：不再自带 libhelper.so.1，依赖 libprovider
    create_pkg("app", "2.0",
        {{"usr/bin/app", "#!/bin/sh\necho app v2\n"}},
        {"libprovider"},  // 新依赖
        {});              // 不再提供 libhelper.so.1

    // libprovider v1：提供 libhelper.so.1
    create_pkg("libprovider", "1.0",
        {{"usr/lib/libhelper.so.1", "helper lib from provider\n"}},
        {},
        {"libhelper.so.1"});

    // 将包加入镜像并更新索引
    add_to_mirror("app", "2.0");
    add_to_mirror("libprovider", "1.0");
    update_index({
        {"app", "2.0", "libprovider", "", ""},
        {"libprovider", "1.0", "", "libhelper.so.1", ""},
    });

    // ── 第3步：执行升级 ─────────────────────────────────────────
    // 此时 app v1 已安装，仓库中有 app v2（依赖 libprovider）
    // 升级应自动解析出 libprovider 并安装
    EXPECT_NO_THROW(upgrade_packages());

    // ── 第4步：验证结果 ─────────────────────────────────────────
    Cache::instance().load();

    // app 应升级到 v2
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_EQ(Cache::instance().get_installed_version("app"), "2.0");

    // libprovider 应被自动安装
    EXPECT_TRUE(Cache::instance().is_installed("libprovider"))
        << "upgrade_packages() 应自动安装新版本引入的依赖";

    // libhelper.so.1 文件应仍然存在（现在由 libprovider 提供）
    EXPECT_TRUE(fs::exists(test_root / "usr/lib/libhelper.so.1"))
        << "升级后 libhelper.so.1 不应被删除（libprovider 提供）";

    // libhelper.so.1 应该被 libprovider 持有
    auto owners = Cache::instance().get_file_owners("/usr/lib/libhelper.so.1");
    EXPECT_TRUE(owners.contains("libprovider"))
        << "libhelper.so.1 应归属于 libprovider";
}

// -----------------------------------------------------------------------
// 边界情况：新版本依赖已在系统中的包
//
// 如果 app v1 自带 libX，app v2 依赖外部包 P，
// 但 P 已经被安装（恰好满足依赖），升级不应重新安装 P
// -----------------------------------------------------------------------
TEST_F(UpgradeDepsResolutionTest, NoopWhenDepAlreadyInstalled) {
    // ── 第1步：安装 libprovider 和 app v1 ───────────────────────
    std::string libpkg = create_pkg("libprovider", "1.0",
        {{"usr/lib/libhelper.so.1", "helper"}},
        {},
        {"libhelper.so.1"});
    ASSERT_NO_THROW(install_packages({libpkg}));

    std::string p1 = create_pkg("app", "1.0",
        {{"usr/bin/app", "app v1"},
         {"usr/lib/libhelper.so.1", "helper"}},
        {},
        {"libhelper.so.1"});
    ASSERT_NO_THROW(install_packages({p1}));

    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("app"));
    ASSERT_TRUE(Cache::instance().is_installed("libprovider"));
    ASSERT_EQ(Cache::instance().get_installed_version("app"), "1.0");

    // ── 第2步：设置仓库 ─────────────────────────────────────────
    create_pkg("app", "2.0",
        {{"usr/bin/app", "app v2"}},
        {"libprovider"},
        {});
    create_pkg("libprovider", "1.0",
        {{"usr/lib/libhelper.so.1", "helper"}},
        {},
        {"libhelper.so.1"});

    add_to_mirror("app", "2.0");
    add_to_mirror("libprovider", "1.0");
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

// -----------------------------------------------------------------------
// 边界情况：新旧版本 hashes 不同但版本号相同
// 不应触发升级，也不应改变依赖状态
// -----------------------------------------------------------------------
TEST_F(UpgradeDepsResolutionTest, SameVersionSkipsUpgrade) {
    // ── 第1步：安装 app v1（自带 lib） ─────────────────────────
    std::string p1 = create_pkg("app", "1.0",
        {{"usr/bin/app", "app v1"},
         {"usr/lib/libhelper.so.1", "helper v1"}},
        {},
        {"libhelper.so.1"});
    ASSERT_NO_THROW(install_packages({p1}));
    Cache::instance().load();
    ASSERT_EQ(Cache::instance().get_installed_version("app"), "1.0");

    // ── 第2步：仓库中 app v1 有不同 hash，版本号不变 ────────────
    create_pkg("app", "1.0",
        {{"usr/bin/app", "app v1 rebuilt"}},
        {"libprovider"},  // 索引说依赖 libprovider
        {});
    add_to_mirror("app", "1.0");
    update_index({{"app", "1.0", "libprovider", "", ""}});

    // ── 第3步：升级（版本号相同，不应触发） ────────────────────
    EXPECT_NO_THROW(upgrade_packages());

    // ── 第4步：验证 ─────────────────────────────────────────────
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("app"), "1.0");
    // libprovider 不应被安装（未触发升级）
    EXPECT_FALSE(Cache::instance().is_installed("libprovider"))
        << "版本号相同时不应触发升级或安装新依赖";
}

// -----------------------------------------------------------------------
// 升级多个包，它们各自引入新的依赖
// appA v2 依赖 libX，appB v2 依赖 libY
// → upgrade_packages 应同时解析出 libX 和 libY
// -----------------------------------------------------------------------
TEST_F(UpgradeDepsResolutionTest, MultipleUpgradesWithNewDeps) {
    // ── 第1步：安装 appA v1 和 appB v1 ────────────────────────
    std::string pa1 = create_pkg("appA", "1.0",
        {{"usr/bin/appA", "A v1"}}, {}, {"libX.so.1"});
    std::string pb1 = create_pkg("appB", "1.0",
        {{"usr/bin/appB", "B v1"}}, {}, {"libY.so.1"});
    ASSERT_NO_THROW(install_packages({pa1}));
    ASSERT_NO_THROW(install_packages({pb1}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("appA"));
    ASSERT_TRUE(Cache::instance().is_installed("appB"));

    // ── 第2步：设置仓库 ─────────────────────────────────────────
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

    // ── 第3步：升级 ─────────────────────────────────────────────
    EXPECT_NO_THROW(upgrade_packages());

    // ── 第4步：验证 ─────────────────────────────────────────────
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("appA"), "2.0");
    EXPECT_EQ(Cache::instance().get_installed_version("appB"), "2.0");
    EXPECT_TRUE(Cache::instance().is_installed("libX"));
    EXPECT_TRUE(Cache::instance().is_installed("libY"));
}

// -----------------------------------------------------------------------
// 无可用依赖：新版本依赖的包在仓库中不存在 → 应抛出异常
// app v2 依赖 missing-dep，但 missing-dep 不在仓库中
// -----------------------------------------------------------------------
TEST_F(UpgradeDepsResolutionTest, MissingDepThrows) {
    // ── 第1步：安装 app v1 ─────────────────────────────────────
    std::string p1 = create_pkg("app", "1.0",
        {{"usr/bin/app", "app v1"}});
    ASSERT_NO_THROW(install_packages({p1}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("app"));

    // ── 第2步：设置仓库 ─────────────────────────────────────────
    // app v2 依赖 missing-dep，但 missing-dep 不在仓库中
    create_pkg("app", "2.0",
        {{"usr/bin/app", "app v2"}},
        {"missing-dep"});
    add_to_mirror("app", "2.0");
    update_index({{"app", "2.0", "missing-dep", "", ""}});

    // ── 第3步：升级应抛出异常 ──────────────────────────────────
    EXPECT_THROW(upgrade_packages(), LpkgException);
}

// -----------------------------------------------------------------------
// 之前被标记为 held（不自动移除）的包，升级后仍保持 held 状态
// -----------------------------------------------------------------------
TEST_F(UpgradeDepsResolutionTest, HoldPreservedAfterUpgrade) {
    // ── 第1步：安装 app v1 ─────────────────────────────────────
    std::string p1 = create_pkg("app", "1.0",
        {{"usr/bin/app", "app v1"}});
    ASSERT_NO_THROW(install_packages({p1}));

    // 标记为 held
    Cache::instance().load();
    Cache::instance().add_installed("app", "1.0", true);  // hold = true
    ASSERT_TRUE(Cache::instance().is_held("app"));

    // ── 第2步：设置仓库 ─────────────────────────────────────────
    create_pkg("app", "2.0", {{"usr/bin/app", "app v2"}});
    add_to_mirror("app", "2.0");
    update_index({{"app", "2.0", "", "", ""}});

    // ── 第3步：升级 ─────────────────────────────────────────────
    EXPECT_NO_THROW(upgrade_packages());

    // ── 第4步：验证 hold 状态保留 ─────────────────────────────
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_held("app"))
        << "升级后包应保持 held 状态";
}

// -----------------------------------------------------------------------
// 新引入的依赖在 resolve 阶段已安装（恰好在升级前手动安装）
// → 升级应正常进行，不再重复安装该依赖
// -----------------------------------------------------------------------
TEST_F(UpgradeDepsResolutionTest, NewDepAlreadyInstalledManually) {
    // ── 第1步：安装 app v1 ─────────────────────────────────────
    std::string p1 = create_pkg("app", "1.0",
        {{"usr/bin/app", "app v1"}});
    ASSERT_NO_THROW(install_packages({p1}));

    // 手动安装 libprovider（app v2 的新依赖）
    std::string libpkg = create_pkg("libprovider", "1.0",
        {{"usr/lib/libhelper.so.1", "helper"}},
        {},
        {"libhelper.so.1"});
    ASSERT_NO_THROW(install_packages({libpkg}));

    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("app"));
    ASSERT_TRUE(Cache::instance().is_installed("libprovider"));

    // ── 第2步：设置仓库 ─────────────────────────────────────────
    create_pkg("app", "2.0",
        {{"usr/bin/app", "app v2"}},
        {"libprovider"});
    add_to_mirror("app", "2.0");
    add_to_mirror("libprovider", "1.0");
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
}
