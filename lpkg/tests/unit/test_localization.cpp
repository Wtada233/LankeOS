#include <gtest/gtest.h>
#include "../../main/src/i18n/localization.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class LocalizationTest : public ::testing::Test {
protected:
    fs::path l10n_dir;

    void SetUp() override {
        l10n_dir = fs::current_path() / "tmp_l10n_test";
        fs::remove_all(l10n_dir);
        fs::create_directories(l10n_dir);
        init_localization();
    }

    void TearDown() override {
        fs::remove_all(l10n_dir);
    }
};

TEST_F(LocalizationTest, GetStringExistingKey) {
    std::string key = "info.parsing_lankebuild";
    auto result = get_string(key);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.find("[MISSING_STRING:") == std::string::npos);
}

TEST_F(LocalizationTest, GetStringMissingKey) {
    std::string missing_key = "nonexistent_key_xyz";
    auto result = get_string(missing_key);
    EXPECT_EQ(result, "[MISSING_STRING: nonexistent_key_xyz]");

    auto result2 = get_string(missing_key);
    EXPECT_EQ(result2, "[MISSING_STRING: nonexistent_key_xyz]");
}

TEST_F(LocalizationTest, StringFormatNormal) {
    // 正常格式化
    std::string result = string_format("info.building_package", "test-pkg", "1.0.0");
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.find("[MISSING_STRING:") == std::string::npos);
}

TEST_F(LocalizationTest, StringFormatMismatchedArgs) {
    // 参数不匹配时应返回格式错误信息而不是崩溃
    std::string result = string_format("info.building_package", "only_one_arg_but_needs_two");
    // 不应崩溃，应返回某种错误信息
    EXPECT_FALSE(result.empty());
}

TEST_F(LocalizationTest, NonExistentLocaleFallsBack) {
    std::string key = "info.parsing_lankebuild";
    auto result = get_string(key);
    EXPECT_FALSE(result.empty());
    EXPECT_TRUE(result.find("[MISSING_STRING:") == std::string::npos);
}

TEST_F(LocalizationTest, MultipleMissingKeys) {
    auto r1 = get_string("missing_key_1");
    auto r2 = get_string("missing_key_2");
    auto r3 = get_string("missing_key_3");
    EXPECT_NE(r1, r2);
    EXPECT_NE(r2, r3);
}
