// 回归测试：--missing-so-no-error / --use-system-soname 两个 flag 的 Config 存取，
// 以及 has_system_soname 的系统 .so 检测（ABI 过渡备份场景）。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "../../main/src/config/config.hpp"
#include "../../main/src/base/constants.hpp"

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
