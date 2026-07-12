#include <gtest/gtest.h>
#include "base/utils.hpp"
#include "base/exception.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class UtilsTest : public ::testing::Test {
protected:
    fs::path test_dir;

    void SetUp() override {
        test_dir = fs::current_path() / "tmp_utils_test";
        fs::remove_all(test_dir);
        fs::create_directories(test_dir);
    }

    void TearDown() override {
        fs::remove_all(test_dir);
    }
};

TEST_F(UtilsTest, StringReplaceAll_Basic) {
    std::string s = "hello world hello";
    string_replace_all(s, "hello", "hi");
    EXPECT_EQ(s, "hi world hi");
}

TEST_F(UtilsTest, StringReplaceAll_NoMatch) {
    std::string s = "hello world";
    string_replace_all(s, "xyz", "abc");
    EXPECT_EQ(s, "hello world");
}

TEST_F(UtilsTest, StringReplaceAll_EmptyFrom) {
    std::string s = "hello";
    string_replace_all(s, "", "x");
    EXPECT_EQ(s, "hello");
}

TEST_F(UtilsTest, StringReplaceAll_EmptyTo) {
    std::string s = "hello world hello";
    string_replace_all(s, "hello", "");
    EXPECT_EQ(s, " world ");
}

TEST_F(UtilsTest, StringReplaceAll_OverlappingPattern) {
    std::string s = "aaa";
    string_replace_all(s, "aa", "b");
    EXPECT_EQ(s, "ba");
}

TEST_F(UtilsTest, EnsureDirExists_NewDir) {
    fs::path new_dir = test_dir / "a" / "b" / "c";
    EXPECT_NO_THROW(ensure_dir_exists(new_dir));
    EXPECT_TRUE(fs::exists(new_dir));
}

TEST_F(UtilsTest, EnsureDirExists_AlreadyExists) {
    EXPECT_NO_THROW(ensure_dir_exists(test_dir));
    EXPECT_TRUE(fs::exists(test_dir));
}

TEST_F(UtilsTest, EnsureFileExists_NewFile) {
    fs::path new_file = test_dir / "test_file.txt";
    EXPECT_NO_THROW(ensure_file_exists(new_file));
    EXPECT_TRUE(fs::exists(new_file));
}

TEST_F(UtilsTest, EnsureFileExists_AlreadyExists) {
    fs::path f = test_dir / "existing.txt";
    { std::ofstream of(f); of << "data"; }
    EXPECT_NO_THROW(ensure_file_exists(f));
    EXPECT_TRUE(fs::exists(f));
}

TEST_F(UtilsTest, ReadWriteSetToFile) {
    std::unordered_set<std::string> data = {"foo", "bar", "baz"};
    fs::path f = test_dir / "set.txt";

    write_set_to_file(f, data);
    EXPECT_TRUE(fs::exists(f));

    auto loaded = read_set_from_file(f);
    EXPECT_EQ(loaded, data);
}

TEST_F(UtilsTest, ReadWriteSetToFile_Empty) {
    std::unordered_set<std::string> empty;
    fs::path f = test_dir / "empty_set.txt";

    write_set_to_file(f, empty);
    EXPECT_TRUE(fs::exists(f));

    auto loaded = read_set_from_file(f);
    EXPECT_TRUE(loaded.empty());
}

TEST_F(UtilsTest, ReadSetFromFile_NotFound) {
    fs::path missing = test_dir / "nonexistent.txt";
    EXPECT_THROW(read_set_from_file(missing), LpkgException);
}

TEST_F(UtilsTest, SplitStringView) {
    auto parts = split_string_view("a,b,c", ',');
    ASSERT_EQ(parts.size(), 3);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "b");
    EXPECT_EQ(parts[2], "c");
}

TEST_F(UtilsTest, SplitStringView_Empty) {
    auto parts = split_string_view("", ',');
    ASSERT_EQ(parts.size(), 1);
    EXPECT_EQ(parts[0], "");
}

TEST_F(UtilsTest, SplitStringView_NoDelimiter) {
    auto parts = split_string_view("hello", ',');
    ASSERT_EQ(parts.size(), 1);
    EXPECT_EQ(parts[0], "hello");
}
