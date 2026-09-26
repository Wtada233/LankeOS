/**
 * test_sigint_exit_code.cpp — upgrade 遇 SIGINT 必须抛异常（= 退出码 1），不能"正常返回"
 *
 * 缺陷：`upgrade_packages()` 在"收集可升级目标"的循环里对 `sigint_graceful` 只做了
 * `log_info(...); return;` —— 于是 CLI 走到 `return 0`：脚本/farm 看到的是
 * "升级已全部完成"，实际**一个包都没升**。全仓另外 12 处 SIGINT 检查（install/remove/
 * install_packages_internal/…）都是 `throw LpkgException(get_string("info.sigint_aborted"))`，
 * CLI 的 `run_cli()`（main/src/main_cli.cpp）里 `catch (const LpkgException&)` 打印后
 * `return 1`。本用例钉的就是这一处对齐。
 *
 * 本文件钉住两条：
 *   ① 置位 sigint_graceful → `upgrade_packages()` **抛 LpkgException**（而不是返回），
 *      且异常文本就是 `info.sigint_aborted`（证明它来自这个分支，不是别的失败路径），
 *      并断言**没有任何包被改动**（旧版本仍在：中止要发生在动盘之前）；
 *   ② 正向对照：同一套仓库/索引，不置位 → 不抛、且真的升到新版本 ——
 *      否则"夹具本身就会抛/就没得升"也能让 ① 变绿（那 ① 就是假绿）。
 *
 * 注意：`sigint_graceful` 的**定义**在 main/src/main_cli.cpp（2026-09-26 从 main.cpp 下沉，
 * 随 LPKG_OBJS 进测试二进制），这里只 `extern` 引用、**不要**再定义一份
 * （重复定义 → 链接错误）。tests/integration/test_solver_regressions.cpp 用的是同一手法。
 */

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/crypto/hash.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"

namespace fs = std::filesystem;

// 定义在 main/src/main_cli.cpp（随 LPKG_OBJS 进测试二进制）
extern std::atomic<bool> sigint_graceful;

class UpgradeSigintExitCodeTest : public ::testing::Test
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

        suite_work_dir = fs::absolute("tmp_upgrade_sigint_test");
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

        // 本地镜像（upgrade_packages 走 Repository 索引，不是包文件路径）
        std::ofstream(test_root / "etc/lpkg/mirror.conf")
            << "file://" << (suite_work_dir / "mirror").string() << "/\n";

        sigint_graceful.store(false);  // 别把"中断"状态泄漏给后续用例
    }

    void TearDown() override
    {
        sigint_graceful.store(false);
        Config::instance().set_root_path("/");
        Config::instance().set_testing_mode(false);
        fs::remove_all(suite_work_dir);
    }

    /** 造一个只含 usr/bin/<name> 的虚拟包，返回 .lpkg 路径 */
    std::string create_pkg(const std::string& name, const std::string& ver,
                           const std::string& content)
    {
        const fs::path work_dir = suite_work_dir / ("pkg_work_" + name + "_" + ver);
        fs::remove_all(work_dir);
        fs::create_directories(work_dir / "content/usr/bin");
        std::ofstream(work_dir / "content/usr/bin" / name) << content << "\n";

        const std::string pkg_path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(pkg_path, work_dir.string(), name, ver, {}, {}, "man " + name, {});
        fs::remove_all(work_dir);
        return pkg_path;
    }

    void add_to_mirror(const std::string& name, const std::string& ver)
    {
        const fs::path sub = mirror_dir / name;
        fs::create_directories(sub);
        fs::copy_file(pkg_dir / (name + "-" + ver + ".lpkg"), sub / (ver + ".lpkg"),
                      fs::copy_options::overwrite_existing);
    }

    /** 写 index.txt（5 字段 name|ver:hash:deps:provides:needed_so，与
     *  tests/integration/test_upgrade_deps_resolution.cpp 的 update_index 同格式） */
    void update_index(const std::vector<std::tuple<std::string, std::string, std::string>>& entries)
    {
        std::ofstream index(mirror_dir / "index.txt");
        for (const auto& [name, ver, deps] : entries) {
            const fs::path pkg = pkg_dir / (name + "-" + ver + ".lpkg");
            const std::string hash = fs::exists(pkg) ? calculate_sha256(pkg) : "unknown";
            index << name << "|" << ver << ":" << hash << ":" << deps << "::\n";
        }
    }

    /** 装 v1.0 并把 v2.0 放进仓库：返回 v2 的包路径（此时确实"有得升"） */
    std::string setup_installed_v1_and_repo_v2()
    {
        const std::string v1 = create_pkg("app", "1.0", "app v1");
        EXPECT_NO_THROW(install_packages({v1}));
        Cache::instance().load();
        EXPECT_TRUE(Cache::instance().is_installed("app"));
        EXPECT_EQ(Cache::instance().get_installed_version("app"), "1.0");

        const std::string v2 = create_pkg("app", "2.0", "app v2");
        add_to_mirror("app", "2.0");
        update_index({{"app", "2.0", ""}});
        return v2;
    }
};

// ── ① 置位 SIGINT → 抛 LpkgException（而不是 return 0） ────────────────────
TEST_F(UpgradeSigintExitCodeTest, SigintAbortsUpgradeByThrowingInsteadOfReturning)
{
    setup_installed_v1_and_repo_v2();

    sigint_graceful.store(true);
    std::string message;
    try {
        upgrade_packages();
        FAIL() << "SIGINT 置位时 upgrade_packages() 正常返回了：CLI 会 return 0，"
                  "脚本/farm 会以为升级全部完成（实际一个包都没升）";
    } catch (const LpkgException& e) {
        message = e.what();
    } catch (...) {
        FAIL() << "抛的不是 LpkgException：main.cpp 只把它映射成退出码 1";
    }
    sigint_graceful.store(false);

    EXPECT_EQ(message, get_string("info.sigint_aborted"))
        << "异常不是来自 SIGINT 分支（文本对不上），这条用例没能钉住那一行";

    // 中止必须发生在动盘之前：DB 与盘面都还停在 1.0
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("app"), "1.0")
        << "SIGINT 中止却已经把包升掉了";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/app"));
    std::ifstream f(test_root / "usr/bin/app");
    std::string line;
    std::getline(f, line);
    EXPECT_EQ(line, "app v1") << "盘上内容已是新版本，说明中止发生在复制之后";
}

// ── ② 正向对照：同一夹具不置位 → 不抛且真的升到 2.0 ────────────────────────
TEST_F(UpgradeSigintExitCodeTest, SameSetupWithoutSigintUpgradesNormally)
{
    setup_installed_v1_and_repo_v2();

    sigint_graceful.store(false);
    EXPECT_NO_THROW(upgrade_packages());

    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("app"), "2.0")
        << "夹具里根本没有可升级目标 —— 那么 ① 的\"抛异常\"就不是 SIGINT 造成的";
}
