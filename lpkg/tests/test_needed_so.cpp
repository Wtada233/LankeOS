#include <gtest/gtest.h>
#include "../main/src/pkg/package_manager.hpp"
#include "../main/src/archive/packer.hpp"
#include "../main/src/crypto/hash.hpp"
#include "../main/src/db/cache.hpp"
#include "../main/src/config/config.hpp"
#include "../main/src/base/utils.hpp"
#include "../main/src/i18n/localization.hpp"
#include "../main/src/base/constants.hpp"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;
using json = nlohmann::json;

// =========================================================================
// needed_so 完整性校验测试套件
//
// 验证 ensure_dependencies_satisfied 和 install_packages 的预检查阶段
// 能正确拒绝 needed_so 无提供者的包，并通过有提供者的包。
//
// 测试矩阵：
//   needed_so 来源 → repo.index.provides / plan.provides / cache.provides
//   校验时机     → 预检查（展示计划前） + 安装事务中
// =========================================================================

class NeededSoTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;
    fs::path mirror_dir;

    void SetUp() override {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_needed_so_test");
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

        std::ofstream(test_root / "etc/lpkg/mirror.conf")
            << "file://" << suite_work_dir.string() << "/mirror/" << std::endl;
    }

    void TearDown() override {
        Config::instance().set_root_path("/");
        Config::instance().set_testing_mode(false);
        fs::remove_all(suite_work_dir);
    }

    std::string create_pkg(const std::string& name, const std::string& ver,
                           const std::vector<std::string>& deps = {},
                           const std::vector<std::string>& provides = {},
                           const std::vector<std::string>& needed_so = {}) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::create_directories(work_dir / "root" / "usr" / "bin");
        std::ofstream(work_dir / "root" / "usr" / "bin" / name).close();

        std::string pkg_filename = name + "-" + ver + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_filename).string();
        pack_package(pkg_path, work_dir.string(), name, ver, deps, provides, "", needed_so);

        // Put in mirror
        fs::path mirror_pkg_dir = mirror_dir / name;
        fs::create_directories(mirror_pkg_dir);
        fs::copy_file(pkg_path, mirror_pkg_dir / (ver + ".lpkg"),
                      fs::copy_options::overwrite_existing);

        fs::remove_all(work_dir);
        return pkg_path;
    }

    void update_index(const std::vector<
        std::tuple<std::string, std::string, std::string, std::string, std::string>>& entries) {
        std::ofstream index(mirror_dir / "index.txt");
        for (const auto& [name, ver, deps, provides, needed_so] : entries) {
            std::string pkg_filename = name + "-" + ver + ".lpkg";
            std::string pkg_path = (pkg_dir / pkg_filename).string();
            std::string hash = "unknown";
            if (fs::exists(pkg_path)) {
                hash = calculate_sha256(pkg_path);
            }
            index << name << "|" << ver << ":" << hash << ":" << deps
                  << "|" << provides << "|" << needed_so << "\n";
        }
    }
};


// -----------------------------------------------------------------------
// 1. needed_so 由 repo index 的 provides 提供
//    libA 的 index 声明 provides=libA.so.1, app 需要 libA.so.1 → 通过
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, ResolvedFromRepoIndex) {
    create_pkg("libA", "1.0", {}, {"libA.so.1"});
    create_pkg("app", "1.0", {"libA"}, {}, {"libA.so.1"});
    update_index({
        {"app", "1.0", "libA", "", "libA.so.1"},
        {"libA", "1.0", "", "libA.so.1", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().is_installed("libA"));
}


// -----------------------------------------------------------------------
// 2. needed_so 由同一事务中另一个包的 provides 解决
//    libB 被一起安装，libB 提供 libB.so.1，app 需要 libB.so.1 → 通过
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, ResolvedFromPlan) {
    create_pkg("libB", "1.0", {}, {"libB.so.1"});
    create_pkg("app", "1.0", {"libB"}, {}, {"libB.so.1"});
    update_index({
        {"app", "1.0", "libB", "", "libB.so.1"},
        {"libB", "1.0", "", "libB.so.1", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().is_installed("libB"));
}


// -----------------------------------------------------------------------
// 3. needed_so 由已安装包的缓存 provides 解决（不在当前 plan 中）
//    libC 已安装且缓存了 provides=libC.so.1，app 需要 libC.so.1 → 通过
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, ResolvedFromInstalledCache) {
    create_pkg("libC", "1.0", {}, {"libC.so.1"});
    update_index({
        {"libC", "1.0", "", "libC.so.1", ""},
    });

    // 先安装 libC 使其进入缓存
    ASSERT_NO_THROW(install_packages({"libC"}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("libC"));

    // app 需要 libC.so.1，libC 已安装 → 应通过
    create_pkg("app", "1.0", {}, {}, {"libC.so.1"});
    update_index({
        {"app", "1.0", "", "", "libC.so.1"},
        {"libC", "1.0", "", "libC.so.1", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
}


// -----------------------------------------------------------------------
// 4. needed_so 完全无提供者 → 拒绝
//    app 需要 ghost.so.1，没有任何包提供 → 抛出异常
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, NoProviderThrows) {
    create_pkg("app", "1.0", {}, {}, {"ghost.so.1"});
    update_index({
        {"app", "1.0", "", "", "ghost.so.1"},
    });

    EXPECT_THROW(install_packages({"app"}), LpkgException);
}


// -----------------------------------------------------------------------
// 5. needed_so 自引用 — 包提供自身的 SONAME
//    插件型包，自身提供 foo.so.1 且需要 foo.so.1 → 通过
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, SelfProvidedOk) {
    create_pkg("foo", "1.0", {}, {"foo.so.1"}, {"foo.so.1"});
    update_index({
        {"foo", "1.0", "", "foo.so.1", "foo.so.1"},
    });

    EXPECT_NO_THROW(install_packages({"foo"}));
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("foo"));
}


// -----------------------------------------------------------------------
// 6. 无 needed_so 的包 → 跳过校验，直接通过
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, EmptyNeededSoSkipsCheck) {
    create_pkg("data", "1.0");
    update_index({
        {"data", "1.0", "", "", ""},
    });

    EXPECT_NO_THROW(install_packages({"data"}));
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("data"));
}


// -----------------------------------------------------------------------
// 7. 多个 needed_so 全部可解析 → 通过
//    app 需要 libA.so.1 + libB.so.1 + libC.so.1 → 三个都在 index 有提供者
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, MultipleAllResolved) {
    create_pkg("libA", "1.0", {}, {"libA.so.1"});
    create_pkg("libB", "1.0", {}, {"libB.so.1"});
    create_pkg("libC", "1.0", {}, {"libC.so.1"});
    create_pkg("app", "1.0", {"libA","libB","libC"}, {},
               {"libA.so.1", "libB.so.1", "libC.so.1"});
    update_index({
        {"app", "1.0", "libA,libB,libC", "", "libA.so.1,libB.so.1,libC.so.1"},
        {"libA", "1.0", "", "libA.so.1", ""},
        {"libB", "1.0", "", "libB.so.1", ""},
        {"libC", "1.0", "", "libC.so.1", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().is_installed("libA"));
    EXPECT_TRUE(Cache::instance().is_installed("libB"));
    EXPECT_TRUE(Cache::instance().is_installed("libC"));
}


// -----------------------------------------------------------------------
// 8. 多个 needed_so，其中一个无提供者 → 拒绝
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, OneOfManyMissingThrows) {
    create_pkg("libD", "1.0", {}, {"libD.so.1"});
    create_pkg("app", "1.0", {"libD"}, {},
               {"libD.so.1", "ghost.so.1"});  // ghost.so.1 无提供者
    update_index({
        {"app", "1.0", "libD", "", "libD.so.1,ghost.so.1"},
        {"libD", "1.0", "", "libD.so.1", ""},
    });

    EXPECT_THROW(install_packages({"app"}), LpkgException);
}


// -----------------------------------------------------------------------
// 9. 动态重解析后 needed_so 可满足
//    index 说 app 依赖 libA（无 needed_so），实际 metadata 说需要 libE.so.1
//    libE 提供 libE.so.1 → 重解析后安装 libE，校验通过
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, DynamicReResolutionSatisfiesNeededSo) {
    create_pkg("libA", "1.0", {}, {"libA.so.1"});
    create_pkg("libE", "1.0", {}, {"libE.so.1"});

    // app 实际 metadata 依赖 libE，needed_so=libE.so.1
    create_pkg("app", "1.0", {"libE"}, {}, {"libE.so.1"});

    // index 却说 app 依赖 libA（误导），且 libA 的 index provides 有 libA.so.1
    update_index({
        {"app", "1.0", "libA", "", "libA.so.1"},
        {"libA", "1.0", "", "libA.so.1", ""},
        {"libE", "1.0", "", "libE.so.1", ""},
    });

    // 重解析后 app 实际依赖 libE，libE 提供 libE.so.1 → 通过
    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().is_installed("libE"));
    EXPECT_FALSE(Cache::instance().is_installed("libA"));
}


// -----------------------------------------------------------------------
// 10. 动态重解析后 needed_so 仍不可满足 → 拒绝
//     index 说 app 依赖 libA，实际 metadata 需要 ghost.so.1，
//     ghost.so.1 无任何提供者 → 重解析后抛出
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, DynamicReResolutionFailsOnMissingNeededSo) {
    create_pkg("libA", "1.0", {}, {"libA.so.1"});
    // app 实际 metadata 依赖并需要 ghost.so.1（不存在）
    create_pkg("app", "1.0", {"lib-new"}, {}, {"ghost.so.1"});

    update_index({
        {"app", "1.0", "libA", "", "libA.so.1"},
        {"libA", "1.0", "", "libA.so.1", ""},
    });

    EXPECT_THROW(install_packages({"app"}), LpkgException);

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("app"));
    EXPECT_FALSE(Cache::instance().is_installed("libA"));
}


// -----------------------------------------------------------------------
// 11. 本地 .lpkg 文件安装，needed_so 无提供者 → 拒绝
//     与通过网络 index 安装走同一套校验逻辑
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, LocalFileInstallRejectsMissingProvider) {
    // 创建本地包，needed_so 指向 ghost.so.1
    create_pkg("local-app", "1.0", {}, {}, {"ghost.so.1"});
    // 不写入 index（本地包不需要 index）

    std::string local_pkg = (pkg_dir / "local-app-1.0.lpkg").string();

    // 本地包安装应当触发 needed_so 校验
    EXPECT_THROW(install_packages({local_pkg}), LpkgException);
}


// -----------------------------------------------------------------------
// 12. index 的 provides 被清空（用户场景），
//     但包本身已安装且缓存有 provides → plan 不包含它时通过
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, InstalledPackageBypassesEmptyIndexProvides) {
    // 先安装 libF（将 libF.so.1 写入缓存）
    create_pkg("libF", "1.0", {}, {"libF.so.1"});
    update_index({{"libF", "1.0", "", "libF.so.1", ""}});
    ASSERT_NO_THROW(install_packages({"libF"}));
    Cache::instance().load();

    // 清空 index 中 libF 的 provides
    update_index({{"libF", "1.0", "", "", ""}});

    // app 需要 libF.so.1，libF 已安装（不在 plan 中）→ 通过
    create_pkg("app", "1.0", {}, {}, {"libF.so.1"});
    update_index({
        {"app", "1.0", "", "", "libF.so.1"},
        {"libF", "1.0", "", "", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
}
