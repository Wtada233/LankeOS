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
        fs::create_directories(work_dir / "content" / "usr" / "bin");
        std::ofstream(work_dir / "content" / "usr" / "bin" / name).close();

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
                  << ":" << provides << ":" << needed_so << "\n";
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
    // libA 可能仍存在（无原子回滚），但 app 的正确依赖 libE 已安装
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


// -----------------------------------------------------------------------
// 13. needed_so 本地文件持久化
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, NeededSoFilePersisted) {
    create_pkg("libP", "1.0", {}, {"libP.so.1", "libQ.so.1"});
    create_pkg("app", "1.0", {"libP"}, {},
               {"libP.so.1", "libQ.so.1"});
    update_index({
        {"app", "1.0", "libP", "", "libP.so.1,libQ.so.1"},
        {"libP", "1.0", "", "libP.so.1,libQ.so.1", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));
    Cache::instance().load();

    fs::path nso_file = Config::instance().needed_so_dir() / "app";
    EXPECT_TRUE(fs::exists(nso_file));

    std::ifstream f(nso_file);
    std::set<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) lines.insert(line);
    }
    EXPECT_TRUE(lines.contains("libP.so.1"));
    EXPECT_TRUE(lines.contains("libQ.so.1"));
    EXPECT_EQ(lines.size(), 2);
}


// -----------------------------------------------------------------------
// 14. needed_so 解析结果写入 deps 文件
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, ProviderNameInDepsFile) {
    create_pkg("libR", "1.0", {}, {"libR.so.1"});
    create_pkg("app", "1.0", {"libR"}, {}, {"libR.so.1"});
    update_index({
        {"app", "1.0", "libR", "", "libR.so.1"},
        {"libR", "1.0", "", "libR.so.1", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));
    Cache::instance().load();

    fs::path dep_file = Config::instance().dep_dir() / "app";
    EXPECT_TRUE(fs::exists(dep_file));
    std::ifstream f(dep_file);
    bool found_libR = false;
    std::string line;
    while (std::getline(f, line)) {
        if (line == "libR") found_libR = true;
    }
    EXPECT_TRUE(found_libR);
    EXPECT_TRUE(Cache::instance().get_reverse_deps("libR").contains("app"));
}


// -----------------------------------------------------------------------
// 15. needed_so 产生的逆向依赖阻止非 force 移除
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, ReverseDepBlocksRemoval) {
    create_pkg("libS", "1.0", {}, {"libS.so.1"});
    create_pkg("app", "1.0", {}, {}, {"libS.so.1"});
    update_index({
        {"app", "1.0", "", "", "libS.so.1"},
        {"libS", "1.0", "", "libS.so.1", ""},
    });

    // 分步安装：先装提供者使缓存有 provides，再装依赖者触发 needed_so 解析
    EXPECT_NO_THROW(install_packages({"libS"}));
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("libS"));

    EXPECT_NO_THROW(install_packages({"app"}));
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().get_reverse_deps("libS").contains("app"));

    // 非 force 移除被 needed_so 逆向依赖阻止（检查内存缓存，remove_package 不写磁盘）
    remove_package("libS", /*force=*/false);
    EXPECT_TRUE(Cache::instance().is_installed("libS"));
}


// -----------------------------------------------------------------------
// 16. force 模式下绕过 needed_so 逆向依赖保护
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, ForceRemoveBypassesReverseDep) {
    create_pkg("libT", "1.0", {}, {"libT.so.1"});
    create_pkg("app", "1.0", {}, {}, {"libT.so.1"});
    update_index({
        {"app", "1.0", "", "", "libT.so.1"},
        {"libT", "1.0", "", "libT.so.1", ""},
    });

    EXPECT_NO_THROW(install_packages({"libT"}));
    Cache::instance().load();

    EXPECT_NO_THROW(install_packages({"app"}));
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().get_reverse_deps("libT").contains("app"));

    // force 移除成功（检查内存缓存，remove_package 不写磁盘）
    remove_package("libT", /*force=*/true);
    EXPECT_FALSE(Cache::instance().is_installed("libT"));
}


// -----------------------------------------------------------------------
// 17. 多个 needed_so 各自产生逆向依赖
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, MultipleNeededSoMultipleReverseDeps) {
    create_pkg("libU", "1.0", {}, {"libU.so.1"});
    create_pkg("libV", "1.0", {}, {"libV.so.2"});
    create_pkg("app", "1.0", {}, {},
               {"libU.so.1", "libV.so.2"});
    update_index({
        {"app", "1.0", "", "", "libU.so.1,libV.so.2"},
        {"libU", "1.0", "", "libU.so.1", ""},
        {"libV", "1.0", "", "libV.so.2", ""},
    });

    EXPECT_NO_THROW(install_packages({"libU", "libV"}));
    Cache::instance().load();

    EXPECT_NO_THROW(install_packages({"app"}));
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().get_reverse_deps("libU").contains("app"));
    EXPECT_TRUE(Cache::instance().get_reverse_deps("libV").contains("app"));
}


// -----------------------------------------------------------------------
// 18. needed_so 不产生自引用
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, SelfNeededSoNoSelfReverseDep) {
    create_pkg("libW", "1.0", {}, {"libW.so.1"}, {"libW.so.1"});
    create_pkg("app", "1.0", {"libW"}, {}, {"libW.so.1"});
    update_index({
        {"app", "1.0", "libW", "", "libW.so.1"},
        {"libW", "1.0", "", "libW.so.1", "libW.so.1"},
    });

    EXPECT_NO_THROW(install_packages({"app", "libW"}));
    Cache::instance().load();

    auto rdeps = Cache::instance().get_reverse_deps("libW");
    EXPECT_FALSE(rdeps.contains("libW"));   // 不自引用
    EXPECT_TRUE(rdeps.contains("app"));     // app 依赖 libW
}


// -----------------------------------------------------------------------
// 19. autoremove 不移除被 needed_so 依赖的包
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, AutoremoveKeepsNeededSoDep) {
    create_pkg("libX", "1.0", {}, {"libX.so.1"});
    create_pkg("app", "1.0", {"libX"}, {}, {"libX.so.1"});
    update_index({
        {"app", "1.0", "libX", "", "libX.so.1"},
        {"libX", "1.0", "", "libX.so.1", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("libX"));

    EXPECT_NO_THROW(autoremove());
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("libX"));
    EXPECT_TRUE(Cache::instance().is_installed("app"));
}


// =========================================================================
// 多版本决策测试套件
//
// 测试仓库中存在某个包的多个版本且各版本提供不同的 SONAME 集时，
// 系统的版本选择与 needed_so 校验如何协同工作。
//
// 覆盖场景：
//   - 多版本中最新版提供所需 SONAME（happy path）
//   - 版本约束锁定到提供所需 SONAME 的中间版本
//   - 版本约束强制升级到提供所需 SONAME 的版本
//   - 版本区间 + SONAME 共同约束版本选择
//   - 多个依赖分别按 SONAME 选择正确版本
//   - 传递依赖的 SONAME 传播到版本选择
//   - 版本约束排除提供 SONAME 的版本 → 拒绝（版本级校验修复）
//   - 同一 SONAME 多包提供时的正确选择
//   - 升级 SONAME 变更 → 依赖者被自动移除（升级一致性检查）
//   - 已安装版本不提供 SONAME → 拒绝
//   - autoremove 保护 needed_so 提供者
//   - 本地包 + 多版本仓库的 needed_so 校验
// =========================================================================

// -----------------------------------------------------------------------
// 1. 多版本仓库，最新版提供所需 SONAME
//    libA-1.0 → libA.so.1，libA-2.0 → libA.so.2
//    app 需要 libA.so.2 → 系统选择 libA-2.0（最新版），校验通过
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, MultiVersionLatestProvidesSoname) {
    create_pkg("libA", "1.0", {}, {"libA.so.1"});
    create_pkg("libA", "2.0", {}, {"libA.so.2"});
    create_pkg("app", "1.0", {"libA"}, {}, {"libA.so.2"});
    update_index({
        {"app", "1.0", "libA", "", "libA.so.2"},
        {"libA", "1.0", "", "libA.so.1", ""},
        {"libA", "2.0", "", "libA.so.2", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().is_installed("libA"));
    EXPECT_EQ(Cache::instance().get_installed_version("libA"), "2.0");
}


// -----------------------------------------------------------------------
// 2. 版本约束锁定到提供所需 SONAME 的中间版本
//    libB-1.0 → libB.so.1
//    libB-2.0 → libB.so.1, libB.so.2
//    libB-3.0 → libB.so.3
//    app 依赖 libB >= 2.0 < 3.0，需要 libB.so.1
//    → 应选择 libB-2.0（满足约束且提供 libB.so.1）
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, MultiVersionConstraintPicksVersionProvidingSoname) {
    create_pkg("libB", "1.0", {}, {"libB.so.1"});
    create_pkg("libB", "2.0", {}, {"libB.so.1", "libB.so.2"});
    create_pkg("libB", "3.0", {}, {"libB.so.3"});
    create_pkg("app", "1.0", {"libB >= 2.0 < 3.0"}, {}, {"libB.so.1"});
    update_index({
        {"app", "1.0", "libB >= 2.0 < 3.0", "", "libB.so.1"},
        {"libB", "1.0", "", "libB.so.1", ""},
        {"libB", "2.0", "", "libB.so.1,libB.so.2", ""},
        {"libB", "3.0", "", "libB.so.3", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().is_installed("libB"));
    EXPECT_EQ(Cache::instance().get_installed_version("libB"), "2.0");
}


// -----------------------------------------------------------------------
// 3. 版本约束强制升级到提供所需 SONAME 的版本
//    libC-1.0 已安装（提供 libC.so.1），libC-2.0 在仓库（提供 libC.so.2）
//    app 依赖 libC >= 2.0（排除已安装的 1.0），需要 libC.so.2
//    → 系统将 libC 从 1.0 升级到 2.0，以满足版本约束和 SONAME
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, VersionConstraintForcesUpgradeForSoname) {
    create_pkg("libC", "1.0", {}, {"libC.so.1"});
    update_index({{"libC", "1.0", "", "libC.so.1", ""}});
    ASSERT_NO_THROW(install_packages({"libC"}));
    Cache::instance().load();
    ASSERT_EQ(Cache::instance().get_installed_version("libC"), "1.0");

    create_pkg("libC", "2.0", {}, {"libC.so.2"});
    create_pkg("app", "1.0", {"libC >= 2.0"}, {}, {"libC.so.2"});
    update_index({
        {"app", "1.0", "libC >= 2.0", "", "libC.so.2"},
        {"libC", "1.0", "", "libC.so.1", ""},
        {"libC", "2.0", "", "libC.so.2", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().is_installed("libC"));
    EXPECT_EQ(Cache::instance().get_installed_version("libC"), "2.0");
}


// -----------------------------------------------------------------------
// 4. 版本区间约束 + SONAME 共同约束版本选择
//    libD-2.5 → libD.so.2
//    libD-2.6 → libD.so.3
//    libD-3.0 → libD.so.4
//    app 依赖 libD >= 2.0 < 3.0，需要 libD.so.3
//    → 应选择 libD-2.6（区间内且提供 libD.so.3）
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, VersionRangeAndSonameCombineCorrectly) {
    create_pkg("libD", "2.5", {}, {"libD.so.2"});
    create_pkg("libD", "2.6", {}, {"libD.so.3"});
    create_pkg("libD", "3.0", {}, {"libD.so.4"});
    create_pkg("app", "1.0", {"libD >= 2.0 < 3.0"}, {}, {"libD.so.3"});
    update_index({
        {"app", "1.0", "libD >= 2.0 < 3.0", "", "libD.so.3"},
        {"libD", "2.5", "", "libD.so.2", ""},
        {"libD", "2.6", "", "libD.so.3", ""},
        {"libD", "3.0", "", "libD.so.4", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_EQ(Cache::instance().get_installed_version("libD"), "2.6");
}


// -----------------------------------------------------------------------
// 5. 多个依赖分别根据 SONAME + 约束选择正确版本
//    libE-1.0 → libE.so.1，libE-2.0 → libE.so.2
//    libF-1.0 → libF.so.1，libF-2.0 → libF.so.2
//    app 需要 libE.so.2 和 libF.so.1
//    → libE 选 2.0（最新），libF 通过约束 >=1.0 <2.0 锁定到 1.0
//    → 二者均提供所需 SONAME
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, MultipleDepsEachSelectCorrectVersionForSoname) {
    create_pkg("libE", "1.0", {}, {"libE.so.1"});
    create_pkg("libE", "2.0", {}, {"libE.so.2"});
    create_pkg("libF", "1.0", {}, {"libF.so.1"});
    create_pkg("libF", "2.0", {}, {"libF.so.2"});
    create_pkg("app", "1.0", {"libE", "libF >= 1.0 < 2.0"}, {},
               {"libE.so.2", "libF.so.1"});
    update_index({
        {"app", "1.0", "libE,libF >= 1.0 < 2.0", "", "libE.so.2,libF.so.1"},
        {"libE", "1.0", "", "libE.so.1", ""},
        {"libE", "2.0", "", "libE.so.2", ""},
        {"libF", "1.0", "", "libF.so.1", ""},
        {"libF", "2.0", "", "libF.so.2", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_EQ(Cache::instance().get_installed_version("libE"), "2.0");
    EXPECT_EQ(Cache::instance().get_installed_version("libF"), "1.0");
}


// -----------------------------------------------------------------------
// 6. 传递依赖的 SONAME 影响传递依赖的版本选择
//    app → libG → libH
//    libG-2.0 需要 libH.so.2
//    libH-1.0 → libH.so.1，libH-2.0 → libH.so.2
//    → libH 被选择为 2.0（最新版提供 libH.so.2）
//      以满足 libG 的 needed_so 校验
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, TransitiveDepSonameDrivesVersionSelection) {
    create_pkg("libH", "1.0", {}, {"libH.so.1"});
    create_pkg("libH", "2.0", {}, {"libH.so.2"});
    create_pkg("libG", "2.0", {"libH"}, {}, {"libH.so.2"});
    create_pkg("app", "1.0", {"libG"});
    update_index({
        {"app", "1.0", "libG", "", ""},
        {"libG", "2.0", "libH", "", "libH.so.2"},
        {"libH", "1.0", "", "libH.so.1", ""},
        {"libH", "2.0", "", "libH.so.2", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().is_installed("libG"));
    EXPECT_TRUE(Cache::instance().is_installed("libH"));
    EXPECT_EQ(Cache::instance().get_installed_version("libH"), "2.0");
}


// -----------------------------------------------------------------------
// 7. 版本约束排除提供所需 SONAME 的版本 → 拒绝（版本级校验修复）
//    libR-1.0 → libR.so.1，libR-2.0 → libR.so.2（不再提供 libR.so.1）
//    app 依赖 libR >= 2.0（排除 1.0），但需要 libR.so.1
//    → 版本选择器选 libR-2.0（唯一满足约束的版本）
//    → 版本级 needed_so 检查发现 libR-2.0.provides 不包含 libR.so.1
//    → 拒绝安装（不再走包级回退）
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, VersionConstraintExcludesSonameProvider) {
    create_pkg("libR", "1.0", {}, {"libR.so.1"});
    create_pkg("libR", "2.0", {}, {"libR.so.2"});
    create_pkg("app", "1.0", {"libR >= 2.0"}, {}, {"libR.so.1"});
    update_index({
        {"app", "1.0", "libR >= 2.0", "", "libR.so.1"},
        {"libR", "1.0", "", "libR.so.1", ""},
        {"libR", "2.0", "", "libR.so.2", ""},
    });

    EXPECT_THROW(install_packages({"app"}), LpkgException);
}


// -----------------------------------------------------------------------
// 8. 同一 SONAME 由多个包的不同版本提供 → 选择正确的提供者
//    libJ 和 libK 都提供 "crypto-core"
//    app 显式依赖 libJ，需要 "crypto-core"
//    → 应选择 libJ（app 声明的依赖），而非 libK
//    （验证 needed_so 的提供者查找不会"跨包抢用"）
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, SonameFromMultipleProvidersChoosesCorrectPackage) {
    create_pkg("libJ", "1.0", {}, {"crypto-core", "libJ.so.1"});
    create_pkg("libK", "1.0", {}, {"crypto-core", "libK.so.1"});
    create_pkg("app", "1.0", {"libJ"}, {}, {"crypto-core"});
    update_index({
        {"app", "1.0", "libJ", "", "crypto-core"},
        {"libJ", "1.0", "", "crypto-core,libJ.so.1", ""},
        {"libK", "1.0", "", "crypto-core,libK.so.1", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().is_installed("libJ"));
    EXPECT_FALSE(Cache::instance().is_installed("libK"));
}


// -----------------------------------------------------------------------
// 9. 升级 SONAME 变更 → 依赖者被自动移除（升级一致性检查）
//    libM-1.0 已安装（提供 libM.so.1）
//    app 已安装（需要 libM.so.1）
//    libM-2.0 在仓库（提供 libM.so.2，不再提供 libM.so.1）
//    升级 libM 时，一致性检查发现 app 会被破坏
//    在非交互模式下自动移除 app
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, SonameChangeInUpgradeBreaksDependents) {
    create_pkg("libM", "1.0", {}, {"libM.so.1"});
    update_index({{"libM", "1.0", "", "libM.so.1", ""}});
    ASSERT_NO_THROW(install_packages({"libM"}));
    Cache::instance().load();
    ASSERT_EQ(Cache::instance().get_installed_version("libM"), "1.0");

    create_pkg("app", "1.0", {"libM"}, {}, {"libM.so.1"});
    update_index({
        {"app", "1.0", "libM", "", "libM.so.1"},
        {"libM", "1.0", "", "libM.so.1", ""},
    });
    ASSERT_NO_THROW(install_packages({"app"}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("app"));

    // 确认 app 的 needed_so 文件记录了 libM.so.1
    {
        fs::path nso_file = Config::instance().needed_so_dir() / "app";
        std::ifstream f(nso_file);
        bool found = false;
        for (std::string l; std::getline(f, l); )
            if (l == "libM.so.1") found = true;
        ASSERT_TRUE(found) << "app's needed_so should contain libM.so.1";
    }

    // 加入 libM-2.0 并升级 libM
    create_pkg("libM", "2.0", {}, {"libM.so.2"});
    update_index({
        {"libM", "1.0", "", "libM.so.1", ""},
        {"libM", "2.0", "", "libM.so.2", ""},
        {"app", "1.0", "libM", "", "libM.so.1"},
    });

    // 一致性检查检测到升级会破坏 app → NonInteractive YES 自动移除 app
    EXPECT_NO_THROW(install_packages({"libM"}));

    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("libM"), "2.0");
    // app 被一致性检查自动移除（libM.so.1 不再提供）
    EXPECT_FALSE(Cache::instance().is_installed("app"));
}


// -----------------------------------------------------------------------
// 10. 已安装的库版本不提供所需 SONAME → 拒绝（版本级校验修复）
//     libN-2.0 已安装（提供 libN.so.2，仓库中最新版）
//     libN-1.0 在仓库中（提供 libN.so.1）
//     app 无显式 deps，仅 needed_so = libN.so.1
//     → 版本级检查发现 libN-2.0.provides 不含 libN.so.1
//     → 拒绝安装（不再靠旧版在仓库中的存在而放行）
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, InstalledVersionLacksSonameButPackageLevelCheckPasses) {
    create_pkg("libN", "1.0", {}, {"libN.so.1"});
    create_pkg("libN", "2.0", {}, {"libN.so.2"});
    update_index({
        {"libN", "1.0", "", "libN.so.1", ""},
        {"libN", "2.0", "", "libN.so.2", ""},
    });
    ASSERT_NO_THROW(install_packages({"libN"}));
    Cache::instance().load();
    ASSERT_EQ(Cache::instance().get_installed_version("libN"), "2.0");

    // app 无 deps，仅 needed_so 引用 libN.so.1
    // 版本级检查会拒绝，因为 libN-2.0 不提供 libN.so.1
    create_pkg("app", "1.0", {}, {}, {"libN.so.1"});
    update_index({
        {"app", "1.0", "", "", "libN.so.1"},
        {"libN", "1.0", "", "libN.so.1", ""},
        {"libN", "2.0", "", "libN.so.2", ""},
    });

    EXPECT_THROW(install_packages({"app"}), LpkgException);
}


// -----------------------------------------------------------------------
// 11. autoremove 保护 needed_so 提供者
//     libO-1.0 已安装（提供 libO.so.1）
//     app 已安装（依赖 libO，需要 libO.so.1）
//     autoremove 不应移除 libO（app 通过 deps 依赖它）
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, MultiVersionAutoremovePreservesSonameProvider) {
    create_pkg("libO", "1.0", {}, {"libO.so.1"});
    update_index({{"libO", "1.0", "", "libO.so.1", ""}});
    ASSERT_NO_THROW(install_packages({"libO"}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("libO"));

    create_pkg("app", "1.0", {"libO"}, {}, {"libO.so.1"});
    update_index({
        {"app", "1.0", "libO", "", "libO.so.1"},
        {"libO", "1.0", "", "libO.so.1", ""},
    });

    EXPECT_NO_THROW(install_packages({"app"}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("app"));
    ASSERT_TRUE(Cache::instance().is_installed("libO"));

    // autoremove 不应移除 libO（app 通过 deps 依赖它）
    EXPECT_NO_THROW(autoremove());
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("libO"));
    EXPECT_TRUE(Cache::instance().is_installed("app"));
}


// -----------------------------------------------------------------------
// 12. 本地包 + 多版本仓库的 needed_so 校验
//     libP-1.0 → libP.so.1，libP-2.0 → libP.so.2
//     本地安装 app.lpkg（需要 libP.so.2，依赖 libP）
//     → 系统应从仓库安装 libP-2.0（最新版提供 libP.so.2）
// -----------------------------------------------------------------------
TEST_F(NeededSoTest, LocalPackageMultiVersionSonameResolution) {
    create_pkg("libP", "1.0", {}, {"libP.so.1"});
    create_pkg("libP", "2.0", {}, {"libP.so.2"});
    update_index({
        {"libP", "1.0", "", "libP.so.1", ""},
        {"libP", "2.0", "", "libP.so.2", ""},
    });

    // 创建本地 app.lpkg（元数据中依赖 libP，需要 libP.so.2）
    fs::path work_dir = suite_work_dir / "local_app_work";
    fs::create_directories(work_dir / "content" / "usr" / "bin");
    std::ofstream(work_dir / "content" / "usr" / "bin" / "local-app").close();
    std::string local_pkg = (pkg_dir / "local-app-1.0.lpkg").string();
    pack_package(local_pkg, work_dir.string(), "local-app", "1.0",
                 {"libP"}, {}, "", {"libP.so.2"});
    fs::remove_all(work_dir);

    ASSERT_TRUE(fs::exists(local_pkg));

    EXPECT_NO_THROW(install_packages({local_pkg}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("local-app"));
    EXPECT_TRUE(Cache::instance().is_installed("libP"));
    EXPECT_EQ(Cache::instance().get_installed_version("libP"), "2.0");
}
