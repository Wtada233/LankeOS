// 回归测试：--missing-so-no-error / --use-system-soname 两个 flag 的 Config 存取，
// 以及 has_system_soname 的系统 .so 检测（ABI 过渡备份场景）。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>

#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/install_common.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/pkg/solver.hpp"
#include "../../main/src/repo/repository.hpp"

namespace fs = std::filesystem;

class SonameFlagTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 每个用例前重置 flag + root
        Config::instance().set_missing_so_no_error_mode(false);
        Config::instance().set_use_system_soname_mode(false);
        Config::instance().set_root_path("/");
    }
};

TEST_F(SonameFlagTest, MissingSoNoErrorFlagGetSet)
{
    EXPECT_FALSE(Config::instance().missing_so_no_error_mode());
    Config::instance().set_missing_so_no_error_mode(true);
    EXPECT_TRUE(Config::instance().missing_so_no_error_mode());
    Config::instance().set_missing_so_no_error_mode(false);
    EXPECT_FALSE(Config::instance().missing_so_no_error_mode());
}

TEST_F(SonameFlagTest, UseSystemSonameFlagGetSet)
{
    EXPECT_FALSE(Config::instance().use_system_soname_mode());
    Config::instance().set_use_system_soname_mode(true);
    EXPECT_TRUE(Config::instance().use_system_soname_mode());
    Config::instance().set_use_system_soname_mode(false);
    EXPECT_FALSE(Config::instance().use_system_soname_mode());
}

TEST_F(SonameFlagTest, HasSystemSonameInUsrLib)
{
    // ABI 过渡场景：backup 的旧 SONAME .so 放在 root/usr/lib
    std::string root = "/tmp/lpkg_test_soname";
    fs::remove_all(root);
    fs::create_directories(fs::path(root) / constants::USR_LIB);
    std::ofstream(fs::path(root) / constants::USR_LIB / "libold.so.1");
    Config::instance().set_root_path(root);

    EXPECT_TRUE(Config::instance().has_system_soname("libold.so.1"));
    EXPECT_FALSE(Config::instance().has_system_soname("libmissing.so.9"));

    fs::remove_all(root);
    Config::instance().set_root_path("/");
}

TEST_F(SonameFlagTest, HasSystemSonameInUsrLib64)
{
    std::string root = "/tmp/lpkg_test_soname64";
    fs::remove_all(root);
    fs::create_directories(fs::path(root) / constants::USR_LIB64);
    std::ofstream(fs::path(root) / constants::USR_LIB64 / "libold.so.2");
    Config::instance().set_root_path(root);

    EXPECT_TRUE(Config::instance().has_system_soname("libold.so.2"));
    EXPECT_FALSE(Config::instance().has_system_soname("libabsent.so.0"));

    fs::remove_all(root);
    Config::instance().set_root_path("/");
}

TEST_F(SonameFlagTest, ForwardCheckToleratedUnderMissingSoNoError)
{
    // 回归：libsolv 求解时，缺失 SONAME 的容忍必须纳入 flag——过渡期缺失不再硬抛
    // （否则 bootstrap 死锁回归）。无 flag 时孤儿 SONAME 仍是真实错误。
    init_localization();
    std::string work = "/tmp/lpkg_test_forward";
    fs::remove_all(work);
    fs::create_directories(fs::path(work) / "mirror/x86_64");
    fs::create_directories(fs::path(work) / "root/etc/lpkg");
    // repo 提供 glibc(libc.so.6) + orphan（需要 libfoo.so.1，无 provider）
    std::ofstream(fs::path(work) / "mirror/x86_64/index.txt")
        << "glibc|2.39:abc::libc.so.6:|\n"
        << "orphan|1.0:abc:::libfoo.so.1:\n";
    std::ofstream(fs::path(work) / "root/etc/lpkg/mirror.conf")
        << "file://" << work << "/mirror/" << std::endl;
    Config::instance().set_root_path(work + "/root");
    Config::instance().init_filesystem();

    Repository repo;
    repo.load_index();

    solv::SolveOptions opts;

    // 无 flag：孤儿 SONAME 是真实错误
    auto r1 = solv::solve_install(repo, {}, {}, {{"orphan", "latest"}}, opts);
    EXPECT_FALSE(r1.ok());

    // --missing-so-no-error：过渡期容忍
    opts.missing_so_no_error = true;
    auto r2 = solv::solve_install(repo, {}, {}, {{"orphan", "latest"}}, opts);
    EXPECT_TRUE(r2.ok());

    // --use-system-soname：系统 /usr/lib 已有该 .so（ABI 过渡 backup）→ 视为满足
    opts.missing_so_no_error = false;
    opts.use_system_soname = true;
    opts.system_sonames = {"libfoo.so.1"};
    fs::create_directories(fs::path(work) / "root" / constants::USR_LIB);
    std::ofstream(fs::path(work) / "root" / constants::USR_LIB / "libfoo.so.1");
    auto r3 = solv::solve_install(repo, {}, {}, {{"orphan", "latest"}}, opts);
    EXPECT_TRUE(r3.ok());

    fs::remove_all(work);
    Config::instance().set_root_path("/");
    Config::instance().set_missing_so_no_error_mode(false);
    Config::instance().set_use_system_soname_mode(false);
}
