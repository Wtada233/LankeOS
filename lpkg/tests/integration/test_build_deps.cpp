#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "vercmp/dep_parser.hpp"

// =========================================================================
// build_dep 版本约束解析测试
// =========================================================================

TEST(BuildDepsTest, PlainName)
{
    auto deps = detail::parse_dep_strings({"cmake"});
    ASSERT_EQ(deps.size(), 1);
    EXPECT_EQ(deps[0].name, "cmake");
    EXPECT_TRUE(deps[0].constraints.empty());
}

TEST(BuildDepsTest, NameWithGreaterEqual)
{
    auto deps = detail::parse_dep_strings({"cmake >= 3.20"});
    ASSERT_EQ(deps.size(), 1);
    EXPECT_EQ(deps[0].name, "cmake");
    ASSERT_FALSE(deps[0].constraints.empty());
    EXPECT_EQ(deps[0].constraints[0].op, ">=");
    EXPECT_EQ(deps[0].constraints[0].version, "3.20");
}

TEST(BuildDepsTest, NameWithEqual)
{
    auto deps = detail::parse_dep_strings({"ninja = 1.12"});
    ASSERT_EQ(deps.size(), 1);
    EXPECT_EQ(deps[0].name, "ninja");
    ASSERT_FALSE(deps[0].constraints.empty());
    EXPECT_EQ(deps[0].constraints[0].op, "=");
    EXPECT_EQ(deps[0].constraints[0].version, "1.12");
}

TEST(BuildDepsTest, NameWithLessThan)
{
    auto deps = detail::parse_dep_strings({"rustc < 2.0"});
    ASSERT_EQ(deps.size(), 1);
    EXPECT_EQ(deps[0].name, "rustc");
    ASSERT_FALSE(deps[0].constraints.empty());
    EXPECT_EQ(deps[0].constraints[0].op, "<");
    EXPECT_EQ(deps[0].constraints[0].version, "2.0");
}

TEST(BuildDepsTest, CompoundConstraint)
{
    auto deps = detail::parse_dep_strings({"cmake >= 3.20, < 4.0"});
    ASSERT_EQ(deps.size(), 1);
    EXPECT_EQ(deps[0].name, "cmake");
}

TEST(BuildDepsTest, MultipleDeps)
{
    auto deps = detail::parse_dep_strings({"cmake >= 3.20", "ninja"});
    ASSERT_EQ(deps.size(), 2);
    EXPECT_EQ(deps[0].name, "cmake");
    EXPECT_FALSE(deps[0].constraints.empty());
    EXPECT_EQ(deps[1].name, "ninja");
    EXPECT_TRUE(deps[1].constraints.empty());
}

TEST(BuildDepsTest, EmptyList)
{
    auto deps = detail::parse_dep_strings({});
    EXPECT_TRUE(deps.empty());
}

TEST(BuildDepsTest, MultipleDepsWithConstraints)
{
    auto deps = detail::parse_dep_strings({
        "cmake >= 3.20",
        "gcc >= 12",
        "rustc >= 1.70",
        "python >= 3.11",
        "nodejs >= 20",
    });
    ASSERT_EQ(deps.size(), 5);
    for (const auto& d : deps) {
        EXPECT_FALSE(d.constraints.empty()) << d.name << " 应有版本约束";
        EXPECT_EQ(d.constraints[0].op, ">=");
    }
    EXPECT_EQ(deps[0].name, "cmake");
    EXPECT_EQ(deps[1].name, "gcc");
    EXPECT_EQ(deps[2].name, "rustc");
    EXPECT_EQ(deps[3].name, "python");
    EXPECT_EQ(deps[4].name, "nodejs");
}

TEST(BuildDepsTest, GreaterThanConstraint)
{
    auto deps = detail::parse_dep_strings({"meson > 1.0"});
    ASSERT_EQ(deps.size(), 1);
    EXPECT_EQ(deps[0].name, "meson");
    ASSERT_FALSE(deps[0].constraints.empty());
    EXPECT_EQ(deps[0].constraints[0].op, ">");
    EXPECT_EQ(deps[0].constraints[0].version, "1.0");
}

TEST(BuildDepsTest, NotEqualConstraint)
{
    auto deps = detail::parse_dep_strings({"python != 2.7"});
    ASSERT_EQ(deps.size(), 1);
    EXPECT_EQ(deps[0].name, "python");
    ASSERT_FALSE(deps[0].constraints.empty());
    EXPECT_EQ(deps[0].constraints[0].op, "!=");
    EXPECT_EQ(deps[0].constraints[0].version, "2.7");
}
