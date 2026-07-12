#include <gtest/gtest.h>
#include "../../main/src/vercmp/version.hpp"
#include "../../main/src/base/exception.hpp"

// ===== version_compare 测试 =====
// version_compare(v1, v2) 返回 true 当且仅当 v1 < v2

TEST(VersionCompare, SimpleNumeric) {
    EXPECT_TRUE(version_compare("1.0", "2.0"));   // 1.0 < 2.0 → true
    EXPECT_FALSE(version_compare("2.0", "1.0"));  // 2.0 < 1.0 → false
    EXPECT_FALSE(version_compare("1.0", "1.0"));  // equal → false
}

TEST(VersionCompare, MultiSegment) {
    EXPECT_TRUE(version_compare("1.0", "1.0.1"));    // 1.0 < 1.0.1 → true
    // 缺失段视为 0，所以 1.0.1 == 1.0.1.0 → false（less-than 不成立）
    EXPECT_FALSE(version_compare("1.0.1", "1.0.1.0"));
    // 1.0.1.0 == 1.0.1 → false（less-than 不成立）
    EXPECT_FALSE(version_compare("1.0.1.0", "1.0.1"));
    EXPECT_FALSE(version_compare("1.0.1", "1.0"));    // 1.0.1 < 1.0 → false
    EXPECT_TRUE(version_compare("1.0.0.0", "1.0.0.1")); // 1.0.0.0 < 1.0.0.1 → true
}

TEST(VersionCompare, NumericHandling) {
    // 6.16.1 > 6.6.1 — 纯字符串比较会错误，数字比较正确
    EXPECT_FALSE(version_compare("6.16.1", "6.6.1"));  // 6.16.1 < 6.6.1 → false
    EXPECT_TRUE(version_compare("6.6.1", "6.16.1"));   // 6.6.1 < 6.16.1 → true
    EXPECT_FALSE(version_compare("10.0", "9.9.9"));    // 10.0 < 9.9.9 → false
    EXPECT_FALSE(version_compare("2.10", "2.9"));       // 2.10 < 2.9 → false
    EXPECT_FALSE(version_compare("1.20", "1.3"));       // 1.20 < 1.3 → false
}

TEST(VersionCompare, DifferentLength) {
    EXPECT_TRUE(version_compare("1.0", "1.0.1"));   // 1.0 < 1.0.1 → true
    EXPECT_FALSE(version_compare("1.0.1", "1.0"));  // 1.0.1 < 1.0 → false
    // 缺失段视为 0，因此 1.0.0 == 1.0 → false
    EXPECT_FALSE(version_compare("1.0.0", "1.0"));  // equal → false
    EXPECT_FALSE(version_compare("1.0", "1.0.0"));  // equal → false
    EXPECT_FALSE(version_compare("2.0.0", "1.0.0.0.0.1")); // 2.0.0 < 1.x → false
}

TEST(VersionCompare, PreRelease) {
    // beta < release
    EXPECT_TRUE(version_compare("1.0-beta", "1.0"));   // beta < release → true
    EXPECT_FALSE(version_compare("1.0", "1.0-beta"));   // release < beta → false

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

TEST(VersionCompare, PreReleaseMultipleIdentifiers) {
    EXPECT_TRUE(version_compare("1.0-alpha", "1.0-alpha.1"));
    EXPECT_TRUE(version_compare("1.0-beta.1", "1.0-beta.2"));
    EXPECT_TRUE(version_compare("1.0-rc.2", "1.0-rc.3"));
}

TEST(VersionCompare, InvalidVersionThrows) {
    // 空字符串无法通过版本格式验证
    EXPECT_THROW(version_compare("", "1.0"), LpkgException);
    EXPECT_THROW(version_compare("1.0", ""), LpkgException);
    EXPECT_THROW(version_compare("", ""), LpkgException);
    // 非法版本字符串
    EXPECT_THROW(version_compare("abc", "1.0"), LpkgException);
    EXPECT_THROW(version_compare("1.0", "abc"), LpkgException);
    // +后缀中不允许含连字符
    EXPECT_THROW(version_compare("1.0+2-rc1", "1.0"), LpkgException);
    EXPECT_THROW(version_compare("1.0+2.1-beta", "1.0"), LpkgException);
    // 空后缀
    EXPECT_THROW(version_compare("1.0-", "1.0"), LpkgException);
    EXPECT_THROW(version_compare("1.0+", "1.0"), LpkgException);
}

// ===== version_satisfies 测试 =====

TEST(VersionSatisfies, Equal) {
    EXPECT_TRUE(version_satisfies("1.0", "=", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0", "==", "1.0"));
    EXPECT_FALSE(version_satisfies("2.0", "=", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0.0", "=", "1.0.0"));
}

TEST(VersionSatisfies, NotEqual) {
    EXPECT_FALSE(version_satisfies("1.0", "!=", "1.0"));
    EXPECT_TRUE(version_satisfies("2.0", "!=", "1.0"));
}

TEST(VersionSatisfies, GreaterThan) {
    EXPECT_TRUE(version_satisfies("2.0", ">", "1.0"));
    EXPECT_FALSE(version_satisfies("1.0", ">", "1.0"));
    EXPECT_FALSE(version_satisfies("1.0", ">", "2.0"));
    EXPECT_TRUE(version_satisfies("1.0.1", ">", "1.0"));
    EXPECT_TRUE(version_satisfies("6.16.1", ">", "6.6.1"));
}

TEST(VersionSatisfies, GreaterThanOrEqual) {
    EXPECT_TRUE(version_satisfies("1.0", ">=", "1.0"));
    EXPECT_TRUE(version_satisfies("2.0", ">=", "1.0"));
    EXPECT_FALSE(version_satisfies("1.0", ">=", "2.0"));
    EXPECT_TRUE(version_satisfies("1.0.1", ">=", "1.0"));

    // 2.0.0（release）>= 2.0.0-rc1 → true（release > rc）
    EXPECT_TRUE(version_satisfies("2.0.0", ">=", "2.0.0-rc1"));
}

TEST(VersionSatisfies, LessThan) {
    EXPECT_TRUE(version_satisfies("1.0", "<", "2.0"));
    EXPECT_FALSE(version_satisfies("1.0", "<", "1.0"));
    EXPECT_FALSE(version_satisfies("2.0", "<", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0", "<", "1.0.1"));
}

TEST(VersionSatisfies, LessThanOrEqual) {
    EXPECT_TRUE(version_satisfies("1.0", "<=", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0", "<=", "2.0"));
    EXPECT_FALSE(version_satisfies("2.0", "<=", "1.0"));
}

TEST(VersionSatisfies, PreReleaseConstraints) {
    EXPECT_TRUE(version_satisfies("1.0-rc1", ">=", "1.0-alpha1"));
    EXPECT_FALSE(version_satisfies("1.0-rc1", ">=", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0", ">=", "1.0-rc1"));
    // 1.0-rc1（pre-release）< 1.0（release），所以 >= 不满足
}

TEST(VersionSatisfies, ComplexScenarios) {
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

TEST(VersionCompare, ReleaseSuffix) {
    // 有 +N > 无后缀
    EXPECT_FALSE(version_compare("22.1.7+2", "22.1.7"));  // 22.1.7+2 < 22.1.7 → false
    EXPECT_TRUE(version_compare("22.1.7", "22.1.7+2"));   // 22.1.7 < 22.1.7+2 → true
    EXPECT_FALSE(version_compare("1.0+1", "1.0"));         // 1.0+1 < 1.0 → false

    // +N 数值比较
    EXPECT_TRUE(version_compare("22.1.7+1", "22.1.7+2")); // +1 < +2 → true
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

TEST(VersionCompare, PatchSuffix) {
    // 有 pN > 无后缀
    EXPECT_FALSE(version_compare("1.9.17p2", "1.9.17"));  // p2 < 1.9.17 → false
    EXPECT_TRUE(version_compare("1.9.17", "1.9.17p2"));   // 1.9.17 < p2 → true
    EXPECT_FALSE(version_compare("1.0p", "1.0"));          // p < 1.0 → false

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

    // pN > +N（补丁优先级最高）
    EXPECT_FALSE(version_compare("1.0p1", "1.0+1"));   // p1 < +1 → false
    EXPECT_TRUE(version_compare("1.0+1", "1.0p1"));    // +1 < p1 → true

    // pN > -pre-release
    EXPECT_FALSE(version_compare("1.0p1", "1.0-rc1")); // p1 < -rc1 → false
    EXPECT_TRUE(version_compare("1.0-rc1", "1.0p1"));

    // 完整排序链：-pre < base < +1 < p1 < p2
    EXPECT_TRUE(version_compare("1.0-rc1", "1.0"));
    EXPECT_TRUE(version_compare("1.0", "1.0+1"));
    EXPECT_TRUE(version_compare("1.0+1", "1.0p1"));
    EXPECT_TRUE(version_compare("1.0p1", "1.0p2"));
}
