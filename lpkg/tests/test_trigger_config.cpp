#include <gtest/gtest.h>
#include "../main/src/utils.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <iostream>

namespace fs = std::filesystem;

class TriggerAndConfigTest : public ::testing::Test {
protected:
    fs::path test_root;

    void SetUp() override {
        test_root = fs::absolute("tmp_trigger_test");
        if (fs::exists(test_root)) fs::remove_all(test_root);
        fs::create_directories(test_root);
    }

    void TearDown() override {
        if (fs::exists(test_root)) fs::remove_all(test_root);
    }
};

TEST_F(TriggerAndConfigTest, ValidatePath) {
    fs::path root = test_root / "root";
    fs::create_directories(root);
    
    EXPECT_NO_THROW(validate_path("usr/bin/ls", root));
    EXPECT_NO_THROW(validate_path("etc/config", root));
    
    EXPECT_THROW(validate_path("/etc/passwd", root), LpkgException);
    EXPECT_THROW(validate_path("../outside", root), LpkgException);
    EXPECT_THROW(validate_path("a/../../outside", root), LpkgException);
}
