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
    // 回归：删除容器 rm -rf hack 后，前向 needed_so 检查（check_forward_soname_integrity）
    // 必须纳入 flag 容忍——过渡期缺失 SONAME 不再硬抛（否则 bootstrap 死锁回归）。
    // 无 flag 时孤儿 SONAME 仍是真实错误 → 硬抛（不变量保留在真实系统侧）。
    init_localization();
    std::string work = "/tmp/lpkg_test_forward";
    fs::remove_all(work);
    fs::create_directories(fs::path(work) / "mirror/x86_64");
    fs::create_directories(fs::path(work) / "root/etc/lpkg");
    // repo 只提供 libc.so.6，不提供 libfoo.so.1（孤儿 SONAME）
    std::ofstream(fs::path(work) / "mirror/x86_64/index.txt")
        << "glibc|2.39:abc::libc.so.6:|\n";
    std::ofstream(fs::path(work) / "root/etc/lpkg/mirror.conf")
        << "file://" << work << "/mirror/" << std::endl;
    Config::instance().set_root_path(work + "/root");
    Config::instance().init_filesystem();

    Repository repo;
    repo.load_index();

    std::map<std::string, InstallPlan> plan;
    InstallPlan p;
    p.name = "orphan";
    p.needed_so = {"libfoo.so.1"}; // plan/repo/缓存全无 provider
    plan["orphan"] = p;

    // 无 flag：孤儿 SONAME 是真实错误，必须硬抛
    Config::instance().set_missing_so_no_error_mode(false);
    Config::instance().set_use_system_soname_mode(false);
    EXPECT_THROW(detail::check_forward_soname_integrity(plan, repo), LpkgException);

    // --missing-so-no-error：过渡期容忍，警告继续
    Config::instance().set_missing_so_no_error_mode(true);
    Config::instance().set_use_system_soname_mode(false);
    EXPECT_NO_THROW(detail::check_forward_soname_integrity(plan, repo));

    // --use-system-soname：系统 /usr/lib 已有该 .so（ABI 过渡 backup）→ 视为满足
    Config::instance().set_missing_so_no_error_mode(false);
    Config::instance().set_use_system_soname_mode(true);
    fs::create_directories(fs::path(work) / "root" / constants::USR_LIB);
    std::ofstream(fs::path(work) / "root" / constants::USR_LIB / "libfoo.so.1");
    EXPECT_NO_THROW(detail::check_forward_soname_integrity(plan, repo));

    fs::remove_all(work);
    Config::instance().set_root_path("/");
    Config::instance().set_missing_so_no_error_mode(false);
    Config::instance().set_use_system_soname_mode(false);
}
