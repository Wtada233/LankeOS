#include <gtest/gtest.h>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/vercmp/version.hpp"

// ===== version_compare 测试 =====
// version_compare(v1, v2) 返回 true 当且仅当 v1 < v2

TEST(VersionCompare, SimpleNumeric)
{
    EXPECT_TRUE(version_compare("1.0", "2.0"));   // 1.0 < 2.0 → true
    EXPECT_FALSE(version_compare("2.0", "1.0"));  // 2.0 < 1.0 → false
    EXPECT_FALSE(version_compare("1.0", "1.0"));  // equal → false
}

TEST(VersionCompare, MultiSegment)
{
    EXPECT_TRUE(version_compare("1.0", "1.0.1"));  // 1.0 < 1.0.1 → true
    // rpm 不补段：1.0.1 < 1.0.1.0（段数不同即不等，曾自研补 0 视为相等，已删除）
    EXPECT_TRUE(version_compare("1.0.1", "1.0.1.0"));
    EXPECT_FALSE(version_compare("1.0.1.0", "1.0.1"));   // 1.0.1.0 < 1.0.1 → false
    EXPECT_FALSE(version_compare("1.0.1", "1.0"));       // 1.0.1 < 1.0 → false
    EXPECT_TRUE(version_compare("1.0.0.0", "1.0.0.1"));  // 1.0.0.0 < 1.0.0.1 → true
}

TEST(VersionCompare, NumericHandling)
{
    // 6.16.1 > 6.6.1 — 纯字符串比较会错误，数字比较正确
    EXPECT_FALSE(version_compare("6.16.1", "6.6.1"));  // 6.16.1 < 6.6.1 → false
    EXPECT_TRUE(version_compare("6.6.1", "6.16.1"));   // 6.6.1 < 6.16.1 → true
    EXPECT_FALSE(version_compare("10.0", "9.9.9"));    // 10.0 < 9.9.9 → false
    EXPECT_FALSE(version_compare("2.10", "2.9"));      // 2.10 < 2.9 → false
    EXPECT_FALSE(version_compare("1.20", "1.3"));      // 1.20 < 1.3 → false
}

TEST(VersionCompare, DifferentLength)
{
    EXPECT_TRUE(version_compare("1.0", "1.0.1"));   // 1.0 < 1.0.1 → true
    EXPECT_FALSE(version_compare("1.0.1", "1.0"));  // 1.0.1 < 1.0 → false
    // rpm 不补段：1.0.0 > 1.0
    EXPECT_FALSE(version_compare("1.0.0", "1.0"));          // 1.0.0 < 1.0 → false
    EXPECT_TRUE(version_compare("1.0", "1.0.0"));           // 1.0 < 1.0.0 → true
    EXPECT_FALSE(version_compare("2.0.0", "1.0.0.0.0.1"));  // 2.0.0 < 1.x → false
}

TEST(VersionCompare, PreRelease)
{
    // beta < release
    EXPECT_TRUE(version_compare("1.0-beta", "1.0"));   // beta < release → true
    EXPECT_FALSE(version_compare("1.0", "1.0-beta"));  // release < beta → false

    // alpha < beta
    EXPECT_TRUE(version_compare("1.0-alpha", "1.0-beta"));
    EXPECT_FALSE(version_compare("1.0-beta", "1.0-alpha"));

    // beta.1 < beta.2
    EXPECT_TRUE(version_compare("1.0-beta.1", "1.0-beta.2"));
    EXPECT_FALSE(version_compare("1.0-beta.2", "1.0-beta.1"));

    // rc > beta → beta < rc
    EXPECT_TRUE(version_compare("1.0-beta", "1.0-rc"));
    EXPECT_FALSE(version_compare("1.0-rc", "1.0-beta"));
}

TEST(VersionCompare, PreReleaseMultipleIdentifiers)
{
    EXPECT_TRUE(version_compare("1.0-alpha", "1.0-alpha.1"));
    EXPECT_TRUE(version_compare("1.0-beta.1", "1.0-beta.2"));
    EXPECT_TRUE(version_compare("1.0-rc.2", "1.0-rc.3"));
}

// 不再自研格式校验：比较完全委托 libsolv EVRCMP，异常/哨兵格式不抛异常（宽容比较）。
// 版本合法性由打包/仓库构建阶段保证，比较期不校验。
TEST(VersionCompare, LenientOnUnexpectedFormats)
{
    EXPECT_NO_THROW(version_compare("", "1.0"));
    EXPECT_NO_THROW(version_compare("1.0", ""));
    EXPECT_NO_THROW(version_compare("abc", "1.0"));
    EXPECT_NO_THROW(version_compare("1.0+2-rc1", "1.0"));
    EXPECT_NO_THROW(version_compare("1.0-", "1.0"));
    EXPECT_NO_THROW(version_compare("virtual", "1.0"));  // 哨兵值不再触发 invalid_version_format
}

// ===== version_satisfies 测试 =====

TEST(VersionSatisfies, Equal)
{
    EXPECT_TRUE(version_satisfies("1.0", "=", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0", "==", "1.0"));
    EXPECT_FALSE(version_satisfies("2.0", "=", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0.0", "=", "1.0.0"));
}

TEST(VersionSatisfies, NotEqual)
{
    EXPECT_FALSE(version_satisfies("1.0", "!=", "1.0"));
    EXPECT_TRUE(version_satisfies("2.0", "!=", "1.0"));
}

TEST(VersionSatisfies, GreaterThan)
{
    EXPECT_TRUE(version_satisfies("2.0", ">", "1.0"));
    EXPECT_FALSE(version_satisfies("1.0", ">", "1.0"));
    EXPECT_FALSE(version_satisfies("1.0", ">", "2.0"));
    EXPECT_TRUE(version_satisfies("1.0.1", ">", "1.0"));
    EXPECT_TRUE(version_satisfies("6.16.1", ">", "6.6.1"));
}

TEST(VersionSatisfies, GreaterThanOrEqual)
{
    EXPECT_TRUE(version_satisfies("1.0", ">=", "1.0"));
    EXPECT_TRUE(version_satisfies("2.0", ">=", "1.0"));
    EXPECT_FALSE(version_satisfies("1.0", ">=", "2.0"));
    EXPECT_TRUE(version_satisfies("1.0.1", ">=", "1.0"));

    // 2.0.0（release）>= 2.0.0-rc1 → true（release > rc）
    EXPECT_TRUE(version_satisfies("2.0.0", ">=", "2.0.0-rc1"));
}

TEST(VersionSatisfies, LessThan)
{
    EXPECT_TRUE(version_satisfies("1.0", "<", "2.0"));
    EXPECT_FALSE(version_satisfies("1.0", "<", "1.0"));
    EXPECT_FALSE(version_satisfies("2.0", "<", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0", "<", "1.0.1"));
}

TEST(VersionSatisfies, LessThanOrEqual)
{
    EXPECT_TRUE(version_satisfies("1.0", "<=", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0", "<=", "2.0"));
    EXPECT_FALSE(version_satisfies("2.0", "<=", "1.0"));
}

TEST(VersionSatisfies, PreReleaseConstraints)
{
    EXPECT_TRUE(version_satisfies("1.0-rc1", ">=", "1.0-alpha1"));
    EXPECT_FALSE(version_satisfies("1.0-rc1", ">=", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0", ">=", "1.0-rc1"));
    // 1.0-rc1（pre-release）< 1.0（release），所以 >= 不满足
}

TEST(VersionSatisfies, ComplexScenarios)
{
    EXPECT_TRUE(version_satisfies("2.0.0", ">=", "1.0.0"));
    EXPECT_TRUE(version_satisfies("2.0.0", ">=", "2.0.0"));
    EXPECT_FALSE(version_satisfies("1.0.0", ">=", "2.0.0"));

    // release > pre-release
    EXPECT_TRUE(version_satisfies("2.0.0", ">=", "2.0.0-rc1"));
    EXPECT_FALSE(version_satisfies("2.0.0-rc1", ">=", "2.0.0"));

    // pre-release vs pre-release
    EXPECT_TRUE(version_satisfies("2.0.0-rc2", ">", "2.0.0-rc1"));
}

// ===== Release revision (+) 测试 =====
// +后缀作为发行修订号，有修订号的版本 > 无后缀版本

TEST(VersionCompare, ReleaseSuffix)
{
    // 有 +N > 无后缀
    EXPECT_FALSE(version_compare("22.1.7+2", "22.1.7"));  // 22.1.7+2 < 22.1.7 → false
    EXPECT_TRUE(version_compare("22.1.7", "22.1.7+2"));   // 22.1.7 < 22.1.7+2 → true
    EXPECT_FALSE(version_compare("1.0+1", "1.0"));        // 1.0+1 < 1.0 → false

    // +N 数值比较
    EXPECT_TRUE(version_compare("22.1.7+1", "22.1.7+2"));  // +1 < +2 → true
    EXPECT_FALSE(version_compare("22.1.7+2", "22.1.7+1"));

    // +N > -pre-release
    EXPECT_FALSE(version_compare("1.0+1", "1.0-rc1"));  // +1 < -rc1 → false (release > pre)
    EXPECT_TRUE(version_compare("1.0-rc1", "1.0+1"));   // -rc1 < +1 → true

    // 完整排序链：-pre < base < +1 < +2
    EXPECT_TRUE(version_compare("1.0-rc1", "1.0"));
    EXPECT_TRUE(version_compare("1.0", "1.0+1"));
    EXPECT_TRUE(version_compare("1.0+1", "1.0+2"));
    // 链式确认
    EXPECT_TRUE(version_compare("1.0-rc1", "1.0+1"));
    EXPECT_FALSE(version_compare("1.0+1", "1.0-rc1"));

    // 多段 +N（如 +2.1）
    EXPECT_TRUE(version_compare("1.0+2", "1.0+2.1"));
    EXPECT_FALSE(version_compare("1.0+2.1", "1.0+2"));
    EXPECT_TRUE(version_compare("1.0+2.0", "1.0+2.1"));
}

// ===== 补丁后缀 (pN) 测试 =====
// pN 作为补丁后缀，有补丁 > 无补丁，优先级最高

TEST(VersionCompare, PatchSuffix)
{
    // 有 pN > 无后缀
    EXPECT_FALSE(version_compare("1.9.17p2", "1.9.17"));  // p2 < 1.9.17 → false
    EXPECT_TRUE(version_compare("1.9.17", "1.9.17p2"));   // 1.9.17 < p2 → true
    EXPECT_FALSE(version_compare("1.0p", "1.0"));         // p < 1.0 → false

    // pN 数值比较
    EXPECT_TRUE(version_compare("1.0p1", "1.0p2"));
    EXPECT_FALSE(version_compare("1.0p2", "1.0p1"));
    EXPECT_TRUE(version_compare("1.0p1", "1.0p10"));  // 数值比较，非字典序
    EXPECT_FALSE(version_compare("1.0p10", "1.0p1"));

    // pN 字母序比较
    EXPECT_TRUE(version_compare("1.0a", "1.0p"));
    EXPECT_FALSE(version_compare("1.0p", "1.0a"));

    // pN 无数字 vs 有数字
    EXPECT_TRUE(version_compare("1.0p", "1.0p2"));  // p < p2（无数字视为 0）
    EXPECT_FALSE(version_compare("1.0p2", "1.0p"));

    // pN 与 +N 相对顺序（rpm 语义）：base < pN < +N。
    // 注：rpm 按段比较，字母段 pN 排在数字段 +N 之前——与旧 lpkg"补丁优先级最高(p1>+1)"
    // 相反。这是 rpm 段比较模型的固有差异，非 `-`→`~` 可覆盖；随"完全使用 libsolv"采纳。
    EXPECT_TRUE(version_compare("1.0p1", "1.0+1"));   // p1 < +1 → true
    EXPECT_FALSE(version_compare("1.0+1", "1.0p1"));  // +1 < p1 → false

    // pN > -pre-release
    EXPECT_FALSE(version_compare("1.0p1", "1.0-rc1"));  // p1 < -rc1 → false
    EXPECT_TRUE(version_compare("1.0-rc1", "1.0p1"));

    // 完整排序链：-pre < base < pN < +N
    EXPECT_TRUE(version_compare("1.0-rc1", "1.0"));
    EXPECT_TRUE(version_compare("1.0", "1.0p1"));
    EXPECT_TRUE(version_compare("1.0p1", "1.0+1"));
    EXPECT_TRUE(version_compare("1.0+1", "1.0+2"));
}

// 回归测试：git hash 版本号（gn: 0.2385.9ece3f52+1）
TEST(VersionCompare, GitHashVersion)
{
    EXPECT_TRUE(version_compare("0.2385.9ece3f52+1", "0.2385.9ece3f52+2"));
    EXPECT_FALSE(version_compare("0.2385.9ece3f52+2", "0.2385.9ece3f52+1"));
    EXPECT_FALSE(version_compare("0.2385.9ece3f52+1", "0.2385.9ece3f52+1"));
    EXPECT_TRUE(version_compare("0.2385", "0.2385.9ece3f52"));
    EXPECT_FALSE(version_compare("0.2385.9ece3f52", "0.2385"));
    EXPECT_TRUE(version_compare("0.2385.9ece3f52", "0.2385.10"));
    EXPECT_FALSE(version_compare("0.2385.10", "0.2385.9ece3f52"));
    // hash + 补丁后缀同时存在几乎不会发生，不测试
}

// 回归：git hash 字母后缀不能被丢弃——两个不同修订必须判为不等。
// 1.0.0a1b2 的 "a1b2" 曾因不是合法补丁后缀被静默丢弃，与 1.0.0 判等。
TEST(VersionCompare, GitHashAlphaSuffixDistinctness)
{
    // 与自身相等
    EXPECT_FALSE(version_compare("1.0.0a1b2", "1.0.0a1b2"));
    // 与基础版不等：有后缀者大于无后缀者
    EXPECT_TRUE(version_compare("1.0.0", "1.0.0a1b2"));
    EXPECT_FALSE(version_compare("1.0.0a1b2", "1.0.0"));
    // 两个不同 git 修订不等（字典序）
    EXPECT_TRUE(version_compare("1.0.0a1b2", "1.0.0a1b3"));
    EXPECT_FALSE(version_compare("1.0.0a1b3", "1.0.0a1b2"));
    // 后缀不改变主版本比较（数字段仍主导）
    EXPECT_TRUE(version_compare("1.0.0a1b2", "1.0.1"));
    // 0.2385.9ece3f52 的现有语义保持不变（前导数字段仍参与数值比较）
    EXPECT_TRUE(version_compare("0.2385", "0.2385.9ece3f52"));
    EXPECT_FALSE(version_compare("0.2385.9ece3f52", "0.2385"));
}

#include "../../main/src/vercmp/dep_parser.hpp"

// 语义：= / == / != 按 libsolv EVRCMP 比较（归一化后）。
// 注意：与 rpm 一致不再补段——1.0 与 1.0.0 是**不同**版本（曾自研补 0 视为相等，已删除）。
TEST(VersionCompare, EqualConstraintUsesSemanticComparison)
{
    EXPECT_TRUE(version_satisfies("1.0", "=", "1.0"));
    EXPECT_FALSE(version_satisfies("2.0", "=", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0.0", "==", "1.0.0"));
    // 段数不同不等价（不再补 0）：
    EXPECT_FALSE(version_satisfies("1.0.0", "=", "1.0"));
    EXPECT_FALSE(version_satisfies("1.0", "==", "1.0.0"));
    EXPECT_TRUE(version_satisfies("1.0", "!=", "1.0.0"));  // 不同版本 → != 为真
    EXPECT_TRUE(version_satisfies("2.0", "!=", "1.0"));
    // 其它运算符：1.0 < 1.0.0
    EXPECT_TRUE(version_satisfies("1.0", "<=", "1.0.0"));
    EXPECT_TRUE(version_satisfies("1.0.0", ">=", "1.0"));
}

// to_libsolv_evr / from_libsolv_evr 往返 + 归一化使 EVRCMP 与 lpkg 语义一致
TEST(VersionCompare, LibsolvEvrRoundTrip)
{
    EXPECT_EQ(to_libsolv_evr("1.0-rc1"), "1.0~rc1");
    EXPECT_EQ(to_libsolv_evr("1.0"), "1.0");
    EXPECT_EQ(to_libsolv_evr("6.0.0+3.lpkg"), "6.0.0+3.lpkg");  // 无 `-` 不变
    EXPECT_EQ(from_libsolv_evr(to_libsolv_evr("1.0-rc1")), "1.0-rc1");
    EXPECT_EQ(from_libsolv_evr(to_libsolv_evr("22.1.7+2")), "22.1.7+2");
    // 归一化后：rc 旧于基础版
    EXPECT_TRUE(version_compare("1.0-rc1", "1.0"));
    EXPECT_FALSE(version_compare("1.0", "1.0-rc1"));
}

// 回归测试：parse_dep_strings 应当按字符串中出现的物理顺序匹配操作符，而非 ops 数组索引顺序
TEST(DepParser, OperatorAppearanceOrder)
{
    auto deps = detail::parse_dep_strings({"libfoo <= 2.0 >= 1.0"});
    ASSERT_EQ(deps.size(), 1u);
    EXPECT_EQ(deps[0].name, "libfoo");
    ASSERT_EQ(deps[0].constraints.size(), 2u);
    EXPECT_EQ(deps[0].constraints[0].op, "<=");
    EXPECT_EQ(deps[0].constraints[0].version, "2.0");
    EXPECT_EQ(deps[0].constraints[1].op, ">=");
    EXPECT_EQ(deps[0].constraints[1].version, "1.0");
}
