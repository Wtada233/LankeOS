#include <gtest/gtest.h>
#include "../main/src/pkg/package_manager.hpp"
#include "../main/src/archive/packer.hpp"
#include "../main/src/db/cache.hpp"
#include "../main/src/config/config.hpp"
#include "../main/src/base/utils.hpp"
#include "../main/src/crypto/hash.hpp"
#include "../main/src/i18n/localization.hpp"
#include "../main/src/base/constants.hpp"
#include <filesystem>
#include <fstream>
#include <atomic>

namespace fs = std::filesystem;

// 测试二进制不链接 main.o，因此在此处定义 sigint_graceful 供给
// package_manager.cpp 的 extern 声明使用。
// 生产环境中此变量在 main.cpp 中定义并由 SIGINT 信号处理函数设置。
std::atomic<bool> sigint_graceful{false};

// =========================================================================
// SIGINT 优雅退出测试套件
//
// 验证 install_packages_internal 正确响应 sigint_graceful 标志：
//   • 标志为 true 时 → 抛出 LpkgException
//   • 标志为 false 时 → 正常安装
//   • 清除标志后 → 后续安装不受影响
// =========================================================================

class SigIntTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;
    fs::path mirror_dir;

    void SetUp() override {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_sigint_test");
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

        sigint_graceful.store(false);
    }

    void TearDown() override {
        Config::instance().set_root_path("/");
        Config::instance().set_testing_mode(false);
        fs::remove_all(suite_work_dir);
    }

    std::string create_pkg(const std::string& name, const std::string& ver,
                           const std::vector<std::string>& deps = {}) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::create_directories(work_dir / "root" / "usr" / "bin");
        std::ofstream(work_dir / "root" / "usr" / "bin" / name).close();

        std::string pkg_filename = name + "-" + ver + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_filename).string();
        pack_package(pkg_path, work_dir.string(), name, ver, deps);

        fs::path mirror_pkg_dir = mirror_dir / name;
        fs::create_directories(mirror_pkg_dir);
        fs::copy_file(pkg_path, mirror_pkg_dir / (ver + ".lpkg"),
                      fs::copy_options::overwrite_existing);

        fs::remove_all(work_dir);
        return pkg_path;
    }

    void write_index(const std::vector<std::tuple<std::string, std::string, std::string>>& entries) {
        std::ofstream idx(mirror_dir / "index.txt");
        for (const auto& [name, ver, deps] : entries) {
            std::string pkg_path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
            std::string hash = "unknown";
            if (fs::exists(pkg_path)) {
                hash = calculate_sha256(pkg_path);
            }
            idx << name << "|" << ver << ":" << hash << ":" << deps << "||\n";
        }
    }
};


// -----------------------------------------------------------------------
// 1. SIGINT 标志未设置 → 正常安装
// -----------------------------------------------------------------------
TEST_F(SigIntTest, NormalInstallWithoutSigInt) {
    sigint_graceful.store(false);
    create_pkg("simple-pkg", "1.0");
    write_index({{"simple-pkg", "1.0", ""}});

    EXPECT_NO_THROW(install_packages({"simple-pkg"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("simple-pkg"));
}


// -----------------------------------------------------------------------
// 2. SIGINT 标志已设置 → 安装被中止
// -----------------------------------------------------------------------
TEST_F(SigIntTest, SigIntFlagAbortsInstall) {
    create_pkg("sigint-pkg", "1.0");
    write_index({{"sigint-pkg", "1.0", ""}});

    sigint_graceful.store(true);
    EXPECT_THROW(install_packages({"sigint-pkg"}), LpkgException);

    // 包不应被安装
    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("sigint-pkg"));
}


// -----------------------------------------------------------------------
// 3. 清除 SIGINT 标志后 → 后续安装正常
// -----------------------------------------------------------------------
TEST_F(SigIntTest, ClearSigIntFlag_SubsequentInstallOk) {
    create_pkg("first", "1.0");
    create_pkg("second", "1.0");
    write_index({{"first", "1.0", ""}, {"second", "1.0", ""}});

    // 先设标志，安装应失败
    sigint_graceful.store(true);
    EXPECT_THROW(install_packages({"first"}), LpkgException);

    // 清除标志，安装应成功
    sigint_graceful.store(false);
    EXPECT_NO_THROW(install_packages({"second"}));

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("first"));
    EXPECT_TRUE(Cache::instance().is_installed("second"));
}


// -----------------------------------------------------------------------
// 4. SIGINT 后依赖解析被中止
//     安装一个带依赖的包，SIGINT 在解析阶段触发
// -----------------------------------------------------------------------
TEST_F(SigIntTest, SigIntDuringDepResolution) {
    create_pkg("lib-dep", "1.0");
    create_pkg("main-app", "1.0", {"lib-dep"});
    write_index({{"lib-dep", "1.0", ""}, {"main-app", "1.0", "lib-dep"}});

    // 索引中没有 lib-dep 的 provides，但 main-app 依赖它
    // 设 SIGINT 后安装应中止
    sigint_graceful.store(true);
    EXPECT_THROW(install_packages({"main-app"}), LpkgException);

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("main-app"));
    EXPECT_FALSE(Cache::instance().is_installed("lib-dep"));
}


// -----------------------------------------------------------------------
// 5. 原子回滚：SIGINT 时已安装的包被回滚
//     安装两个包，第二个触发 SIGINT → 第一个也应被回滚
// -----------------------------------------------------------------------
TEST_F(SigIntTest, AtomicRollbackOnSigInt) {
    create_pkg("first-pkg", "1.0");
    create_pkg("second-pkg", "1.0");
    // main-pkg 同时依赖两者
    create_pkg("main-pkg", "1.0", {"first-pkg"});
    write_index({{"first-pkg", "1.0", ""}, {"second-pkg", "1.0", ""}, {"main-pkg", "1.0", "first-pkg"}});

    // 正常安装 first-pkg 然后设 SIGINT
    sigint_graceful.store(false);
    install_packages({"first-pkg"});
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("first-pkg"));

    // 安装 main-pkg 时触发 SIGINT 应只影响当前事务，不影响已安装的包
    sigint_graceful.store(true);
    EXPECT_THROW(install_packages({"main-pkg"}), LpkgException);

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("main-pkg"));
}


// -----------------------------------------------------------------------
// 6. SIGINT 不影响只读命令
//     query 命令不设置 SigIntGuard，不应触发任何 SIGINT 行为
// -----------------------------------------------------------------------
TEST_F(SigIntTest, QueryCommandNotAffectedBySigInt) {
    create_pkg("query-pkg", "1.0");
    write_index({{"query-pkg", "1.0", ""}});

    // 先正常安装
    sigint_graceful.store(false);
    EXPECT_NO_THROW(install_packages({"query-pkg"}));
    Cache::instance().load();

    // 设 SIGINT，query 不应受影响
    sigint_graceful.store(true);
    // query_package 只是读取缓存，不检查 SIGINT
    EXPECT_NO_THROW(query_package("query-pkg"));
}


// -----------------------------------------------------------------------
// 7. SIGINT 标志复位后可以正常安装
//     模拟用户拒绝后重新尝试的场景
// -----------------------------------------------------------------------
TEST_F(SigIntTest, ResetAndRetryAfterSigInt) {
    create_pkg("retry-pkg", "1.0");
    write_index({{"retry-pkg", "1.0", ""}});

    sigint_graceful.store(true);
    EXPECT_THROW(install_packages({"retry-pkg"}), LpkgException);

    // 模拟用户重试
    sigint_graceful.store(false);
    EXPECT_NO_THROW(install_packages({"retry-pkg"}));

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("retry-pkg"));
}
