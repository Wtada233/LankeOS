#include <gtest/gtest.h>
#include "../main/src/config.hpp"
#include <filesystem>

namespace fs = std::filesystem;

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset to default
        set_root_path("/");
    }
};

TEST_F(ConfigTest, DefaultRoot) {
    EXPECT_EQ(ROOT_DIR, "/");
    EXPECT_EQ(CONFIG_DIR, "/etc/lpkg");
    EXPECT_EQ(STATE_DIR, "/var/lib/lpkg");
}

TEST_F(ConfigTest, CustomRoot) {
    std::string root = "/mnt/new_root";
    set_root_path(root);
    
    EXPECT_EQ(ROOT_DIR, fs::path(root));
    EXPECT_EQ(CONFIG_DIR, fs::path(root) / "etc/lpkg");
    EXPECT_EQ(STATE_DIR, fs::path(root) / "var/lib/lpkg");
    EXPECT_EQ(FILES_DB, fs::path(root) / "var/lib/lpkg/files.db");
}

TEST_F(ConfigTest, CustomRootWithTrailingSlash) {
    std::string root = "/mnt/new_root/";
    set_root_path(root);
    
    // Check if double slashes are handled or doesn't matter for path equality
    EXPECT_EQ(CONFIG_DIR, fs::path(root) / "etc/lpkg");
}
