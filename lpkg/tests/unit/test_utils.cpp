#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "base/exception.hpp"
#include "base/utils.hpp"

namespace fs = std::filesystem;

class UtilsTest : public ::testing::Test
{
protected:
    fs::path test_dir;

    void SetUp() override
    {
        test_dir = fs::current_path() / "tmp_utils_test";
        fs::remove_all(test_dir);
        fs::create_directories(test_dir);
    }

    void TearDown() override
    {
        fs::remove_all(test_dir);
    }
};

TEST_F(UtilsTest, StringReplaceAll_Basic)
{
    std::string s = "hello world hello";
    string_replace_all(s, "hello", "hi");
    EXPECT_EQ(s, "hi world hi");
}

TEST_F(UtilsTest, StringReplaceAll_NoMatch)
{
    std::string s = "hello world";
    string_replace_all(s, "xyz", "abc");
    EXPECT_EQ(s, "hello world");
}

TEST_F(UtilsTest, StringReplaceAll_EmptyFrom)
{
    std::string s = "hello";
    string_replace_all(s, "", "x");
    EXPECT_EQ(s, "hello");
}

TEST_F(UtilsTest, StringReplaceAll_EmptyTo)
{
    std::string s = "hello world hello";
    string_replace_all(s, "hello", "");
    EXPECT_EQ(s, " world ");
}

TEST_F(UtilsTest, StringReplaceAll_OverlappingPattern)
{
    std::string s = "aaa";
    string_replace_all(s, "aa", "b");
    EXPECT_EQ(s, "ba");
}

TEST_F(UtilsTest, EnsureDirExists_NewDir)
{
    fs::path new_dir = test_dir / "a" / "b" / "c";
    EXPECT_NO_THROW(ensure_dir_exists(new_dir));
    EXPECT_TRUE(fs::exists(new_dir));
}

TEST_F(UtilsTest, EnsureDirExists_AlreadyExists)
{
    EXPECT_NO_THROW(ensure_dir_exists(test_dir));
    EXPECT_TRUE(fs::exists(test_dir));
}

TEST_F(UtilsTest, EnsureFileExists_NewFile)
{
    fs::path new_file = test_dir / "test_file.txt";
    EXPECT_NO_THROW(ensure_file_exists(new_file));
    EXPECT_TRUE(fs::exists(new_file));
}

TEST_F(UtilsTest, EnsureFileExists_AlreadyExists)
{
    fs::path f = test_dir / "existing.txt";
    {
        std::ofstream of(f);
        of << "data";
    }
    EXPECT_NO_THROW(ensure_file_exists(f));
    EXPECT_TRUE(fs::exists(f));
}

TEST_F(UtilsTest, ReadSetFromFile)
{
    // 直接用 ofstream 造文件 —— 不经过任何"写集合"的助手：生产侧写集合文件的是
    // `Cache` 自己的 `ofstream + fsync_and_rename`，曾经那个 `write_set_to_file()`
    // 生产零调用（已删），留着它只会让这条用例变成"自己写、自己读"的往返，
    // 证明不了读侧对**真实文件格式**的处理。
    const fs::path f = test_dir / "set.txt";
    {
        std::ofstream of(f);
        of << "foo\nbar\nbaz\n";  // 每行一个元素；末尾换行是生产写侧的形态
    }
    const std::unordered_set<std::string> expected = {"foo", "bar", "baz"};
    EXPECT_EQ(read_set_from_file(f), expected);
}

TEST_F(UtilsTest, ReadSetFromFile_Empty)
{
    const fs::path f = test_dir / "empty_set.txt";
    {
        std::ofstream of(f);  // 存在但为空
    }
    // "存在但空" ≠ "缺失"：后者按策略抛（见 ReadSetFromFile_NotFound），前者是合法空集
    EXPECT_TRUE(read_set_from_file(f).empty());
    EXPECT_TRUE(read_set_from_file(f, MissingSetFilePolicy::Empty).empty());
}

TEST_F(UtilsTest, ReadSetFromFile_NotFound)
{
    fs::path missing = test_dir / "nonexistent.txt";
    EXPECT_THROW(read_set_from_file(missing), LpkgException);
}

TEST_F(UtilsTest, SplitStringView)
{
    auto parts = split_string_view("a,b,c", ',');
    ASSERT_EQ(parts.size(), 3);
    EXPECT_EQ(parts[0], "a");
    EXPECT_EQ(parts[1], "b");
    EXPECT_EQ(parts[2], "c");
}

TEST_F(UtilsTest, SplitStringView_Empty)
{
    auto parts = split_string_view("", ',');
    ASSERT_EQ(parts.size(), 1);
    EXPECT_EQ(parts[0], "");
}

TEST_F(UtilsTest, SplitStringView_NoDelimiter)
{
    auto parts = split_string_view("hello", ',');
    ASSERT_EQ(parts.size(), 1);
    EXPECT_EQ(parts[0], "hello");
}

// ── strip_trailing_slash：**唯一**一份"剥尾斜杠"实现（合并了 strip_trailing_sep）──
//
// 不变量：只剥**末尾**的 '/'，根 "/" 原样保留，**不做**任何 lexically_normal 之外的规范化
// （合并后 `strip_trailing_sep` 仍先 lexically_normal 再走这里，行为与合并前逐字节一致）。

TEST_F(UtilsTest, StripTrailingSlashRemovesOnlyTrailingSeparators)
{
    EXPECT_EQ(strip_trailing_slash("/usr/share/x/"), fs::path("/usr/share/x"));
    EXPECT_EQ(strip_trailing_slash("/usr/share/x///"), fs::path("/usr/share/x"));
    EXPECT_EQ(strip_trailing_slash("/usr/share/x"), fs::path("/usr/share/x"));
    // 根不能剥成空串
    EXPECT_EQ(strip_trailing_slash("/"), fs::path("/"));
    // 相对路径同样只剥尾部
    EXPECT_EQ(strip_trailing_slash("a/b/"), fs::path("a/b"));
    EXPECT_EQ(strip_trailing_slash(""), fs::path(""));
}
