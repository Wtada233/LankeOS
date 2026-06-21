#include <gtest/gtest.h>
#include "../main/src/config.hpp"
#include <filesystem>

namespace fs = std::filesystem;

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset to default
        Config::instance().set_root_path("/");
    }
};

TEST_F(ConfigTest, DefaultRoot) {
    EXPECT_EQ(Config::instance().root_dir(), "/");
    EXPECT_EQ(Config::instance().config_dir(), "/etc/lpkg");
    EXPECT_EQ(Config::instance().state_dir(), "/var/lib/lpkg");
}

TEST_F(ConfigTest, CustomRoot) {
    std::string root = "/mnt/new_root";
    Config::instance().set_root_path(root);
    
    EXPECT_EQ(Config::instance().root_dir(), fs::path(root));
    EXPECT_EQ(Config::instance().config_dir(), fs::path(root) / "etc/lpkg");
    EXPECT_EQ(Config::instance().state_dir(), fs::path(root) / "var/lib/lpkg");
    EXPECT_EQ(Config::instance().files_db(), fs::path(root) / "var/lib/lpkg/files.db");
}

TEST_F(ConfigTest, CustomRootWithTrailingSlash) {
    std::string root = "/mnt/new_root/";
    Config::instance().set_root_path(root);
    
    // Check if double slashes are handled or doesn't matter for path equality
    EXPECT_EQ(Config::instance().config_dir(), fs::path(root) / "etc/lpkg");
}
