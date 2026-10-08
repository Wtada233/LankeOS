/**
 * test_version_bridge_solver.cpp — **求解器选版本**在端到端安装里的后果
 *
 * 钉的是：求解器选出来的版本，必须正是 lpkg 语义下该选的那个，装完不再被安装期判据推翻。
 *
 * ⚠️ 文件名里的 "bridge" 是历史：版本语义直接采用 rpm EVR，那套"编码桥"
 * （`to_libsolv_evr` / `^^` 分隔符）已整套删除（见 `vercmp/version.hpp`），求解器与安装期
 * 从此共用同一份判据。用例里的 `1.0+1` 只是普通版本号（`+` 无特殊含义），下面各格标注的
 * "旧"指那套已废弃的编码桥语义：
 *   · `= 1.0`     —— 旧：libsolv 认为 `1.0+1` 也满足 ⇒ 选中它 ⇒ 安装期判据拒绝 ⇒ 整批失败；
 *   · `> 1.0`     —— 旧：libsolv 认为**没有任何**版本满足（`1.0+1` 被当成"只差 release"）⇒
 * 事务无解； · `>= 1.0+1`  —— 旧：libsolv 认为 `1.0` 也满足（release 缺失即通配）⇒ 可能选中更旧的；
 *   · `= 1.0`（仓库里只有 `1.0+1`）—— 旧：求解器照样放行，装到一半才被拒。
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/crypto/hash.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"

namespace fs = std::filesystem;

class VersionBridgeSolverTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;
    fs::path mirror_dir;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_version_bridge_test_" + std::to_string(::getpid()));
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

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    void create_pkg(const std::string& name, const std::string& ver,
                    const std::vector<std::string>& deps = {})
    {
        fs::path work_dir = suite_work_dir / ("work_" + name + "_" + ver);
        fs::create_directories(work_dir / "content" / "usr" / "bin");
        std::ofstream(work_dir / "content" / "usr" / "bin" / name).close();

        const std::string pkg_path = (pkg_dir / std::format("{}-{}.lpkg", name, ver)).string();
        pack_package(pkg_path, work_dir.string(), name, ver, deps);

        fs::path mirror_pkg_dir = mirror_dir / name;
        fs::create_directories(mirror_pkg_dir);
        fs::copy_file(pkg_path, mirror_pkg_dir / (ver + ".lpkg"),
                      fs::copy_options::overwrite_existing);
        fs::remove_all(work_dir);
    }

    /// 索引行：`name|ver:hash:deps:provides:|`（deps 里可以带版本约束，仓库解析器认）
    void update_index(const std::vector<std::tuple<std::string, std::string, std::string>>& entries)
    {
        std::ofstream index(mirror_dir / "index.txt");
        for (const auto& [name, ver, deps] : entries) {
            const std::string pkg_path = (pkg_dir / std::format("{}-{}.lpkg", name, ver)).string();
            index << name << "|" << ver << ":" << calculate_sha256(pkg_path) << ":" << deps
                  << ":::|\n";
        }
    }

    std::string installed_version(const std::string& pkg)
    {
        Cache::instance().load();
        return Cache::instance().get_installed_version(pkg);
    }
};

TEST_F(VersionBridgeSolverTest, ExactConstraintPicksTheExactVersionNotARelease)
{
    // `lib = 1.0` 只该选中 `1.0`。旧编码下 libsolv 认为 `1.0+1` 也满足（release 通配），
    // 而它偏好更新的候选 ⇒ 选中 `1.0+1` ⇒ 安装期判据拒绝整批（`error.dep_version_mismatch`）。
    create_pkg("lib", "1.0");
    create_pkg("lib", "1.0+1");
    create_pkg("app", "1.0", {"lib = 1.0"});
    update_index({{"lib", "1.0", ""}, {"lib", "1.0+1", ""}, {"app", "1.0", "lib = 1.0"}});

    ASSERT_NO_THROW(install_packages({"app"})) << "`= 1.0` 必须能装（旧编码在这里整批失败）";
    EXPECT_EQ(installed_version("lib"), "1.0")
        << "`= 1.0` 选中了带 release 的版本 —— 求解器与安装期判据分叉了";
}

TEST_F(VersionBridgeSolverTest, GreaterThanWithoutReleaseIsSatisfiedByARelease)
{
    // `lib > 1.0`：lpkg 语义下 `1.0+1 > 1.0` ⇒ 必须能解、且选 `1.0+1`。
    // 旧编码下 libsolv 认为 `1.0+1` 只是"差在 release"⇒ 不满足 `>` ⇒ **事务无解**。
    create_pkg("lib", "1.0");
    create_pkg("lib", "1.0+1");
    create_pkg("app", "1.0", {"lib > 1.0"});
    update_index({{"lib", "1.0", ""}, {"lib", "1.0+1", ""}, {"app", "1.0", "lib > 1.0"}});

    ASSERT_NO_THROW(install_packages({"app"})) << "`> 1.0` 必须能被 `1.0+1` 满足";
    EXPECT_EQ(installed_version("lib"), "1.0+1");
}

TEST_F(VersionBridgeSolverTest, GreaterOrEqualWithReleaseForcesUpgradeOfTheInstalledOlderOne)
{
    // 已装 `lib 1.0`，新来的 app 要求 `lib >= 1.0+1` ⇒ 必须把 lib **升级**到 `1.0+1`。
    //
    // 这一格钉的是**假满足**那一侧（比"装不上"更危险）：旧编码下 libsolv 认为已装的 `1.0`
    // 就满足 `>= 1.0+1`（release 缺失即通配）⇒ 不升级 ⇒ app 带着一个 lpkg 判据下**不满足**
    // 的依赖被装上去，端到端**不会报任何错**。
    create_pkg("lib", "1.0");
    create_pkg("lib", "1.0+1");
    create_pkg("app", "1.0", {"lib >= 1.0+1"});
    update_index({{"lib", "1.0", ""}, {"lib", "1.0+1", ""}, {"app", "1.0", "lib >= 1.0+1"}});

    ASSERT_NO_THROW(install_packages({"lib:1.0"})) << "fixture：先把旧版装上";
    ASSERT_EQ(installed_version("lib"), "1.0");

    ASSERT_NO_THROW(install_packages({"app"}));
    EXPECT_EQ(installed_version("lib"), "1.0+1")
        << "已装的 `1.0` 不满足 `>= 1.0+1`，求解器必须把 lib 升上去";
}

TEST_F(VersionBridgeSolverTest, ConflictMessageNamesTheRealVersions)
{
    // 用户可见的冲突消息必须让用户直接对上仓库里的版本号。libsolv 自己拼的规则描述
    // （`solver_ruleinfo2str`）会带池里的内部名 —— 今天是 `so:` SONAME 前缀，由
    // `solver.cpp` 的 `decode_libsolv_message` 剥掉；版本那一侧则由这里断言：消息里必须
    // 出现**真实的版本串**，用户据此能 grep 到仓库。
    //
    // （版本串原样进池、不经过编码，所以消息里不会出现 `^^` 这类中间痕迹 —— 无需为它加断言。）
    create_pkg("lib", "1.0+1");
    create_pkg("lib", "2.0+1");
    create_pkg("appA", "1.0", {"lib = 1.0+1"});
    create_pkg("appB", "1.0", {"lib = 2.0+1"});
    update_index({{"lib", "1.0+1", ""},
                  {"lib", "2.0+1", ""},
                  {"appA", "1.0", "lib = 1.0+1"},
                  {"appB", "1.0", "lib = 2.0+1"}});

    try {
        install_packages({"appA", "appB"});
        FAIL() << "两个 app 要求同一个包的两个互斥版本，必须拒绝";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("1.0+1"), std::string::npos)
            << "冲突消息应点名真实版本串（`1.0+1`），用户才能对回仓库: " << msg;
    }
}

TEST_F(VersionBridgeSolverTest, ExactConstraintIsNotSatisfiedByAnInstalledReleaseOnlyVersion)
{
    // 已装 `lib 1.0+1`、仓库里也没有别的 lib，而 app 要求 `lib = 1.0` ⇒ **必须被拒**。
    //
    // ⚠️ 这一格**不区分两种编码**（旧编码下它也红不了）—— 因为旧编码下 libsolv 虽然
    // 会被"已装的 `1.0+1` 满足 `= 1.0`"骗过，lpkg 自己在装之前还有一道
    // `version_satisfies_all`（`installation_task.cpp`）会把它拒掉，端到端结局相同、
    // 只是报错文案不同。留着它的价值是钉"最终必须被拒"这条底线（万一那道判据被删，这里会红）；
    // "桥接本身选对了没有"由上面三条（含已装侧升级那一格）覆盖。
    create_pkg("lib", "1.0+1");
    create_pkg("app", "1.0", {"lib = 1.0"});
    update_index({{"lib", "1.0+1", ""}, {"app", "1.0", "lib = 1.0"}});

    ASSERT_NO_THROW(install_packages({"lib:1.0+1"})) << "fixture：先装上 1.0+1";
    Cache::instance().load();
    ASSERT_EQ(Cache::instance().get_installed_version("lib"), "1.0+1");

    EXPECT_THROW(install_packages({"app"}), LpkgException)
        << "`= 1.0` 不该被 `1.0+1` 满足（旧编码会静默把 app 装上）";
    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("app"));
}
