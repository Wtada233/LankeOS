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
    // rpm 不补段：1.0.1 < 1.0.1.0（段数不同即不等）
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
    // rpm 的**预发布标记是 `~`**：`1.0~beta < 1.0`。
    // ⚠️ `-` 之后是 **release**（修订号），不再是预发布 —— `1.0-beta` 现在意为
    // "1.0 的第 beta 号修订"，**大于** 1.0。旧的"`-` = 预发布"语义随版本桥一起删除。
    EXPECT_TRUE(version_compare("1.0~beta", "1.0"));
    EXPECT_FALSE(version_compare("1.0", "1.0~beta"));

    // alpha < beta
    EXPECT_TRUE(version_compare("1.0~alpha", "1.0~beta"));
    EXPECT_FALSE(version_compare("1.0~beta", "1.0~alpha"));

    // beta.1 < beta.2
    EXPECT_TRUE(version_compare("1.0~beta.1", "1.0~beta.2"));
    EXPECT_FALSE(version_compare("1.0~beta.2", "1.0~beta.1"));

    // beta < rc
    EXPECT_TRUE(version_compare("1.0~beta", "1.0~rc"));
    EXPECT_FALSE(version_compare("1.0~rc", "1.0~beta"));

    // 对照（语义变化的钉子）：`-rc1` 是 release ⇒ **新于**基础版
    EXPECT_FALSE(version_compare("1.0-rc1", "1.0"));
    EXPECT_TRUE(version_compare("1.0", "1.0-rc1"));
}

TEST(VersionCompare, PreReleaseMultipleIdentifiers)
{
    EXPECT_TRUE(version_compare("1.0~alpha", "1.0~alpha.1"));
    EXPECT_TRUE(version_compare("1.0~beta.1", "1.0~beta.2"));
    EXPECT_TRUE(version_compare("1.0~rc.2", "1.0~rc.3"));
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

    // `2.0.0`（候选不带 release）对 `>= 2.0.0-rc1` 成立 —— rpm 的匹配语义把"候选没写
    // release"当通配（见下面 ReleaseIsAWildcardInDependencyMatching）。
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
    // 预发布用 `~` 写
    EXPECT_TRUE(version_satisfies("1.0~rc1", ">=", "1.0~alpha1"));
    EXPECT_FALSE(version_satisfies("1.0~rc1", ">=", "1.0"));  // 预发布旧于基础版
    EXPECT_TRUE(version_satisfies("1.0", ">=", "1.0~rc1"));
}

// 约束判定用的是 **rpm 的依赖匹配语义**（`EVRCMP_MATCH_RELEASE`，与 libsolv 求解器判
// "谁满足这条依赖"是同一份代码），不是排序语义 —— 两者对"一侧有 release、另一侧没写"的
// 处置不同：rpm 把**没写 release 当通配**。下面每一条都以容器里的 libsolv 为准
// （它直接给答案），看着怪，但它们正是"求解器说能装、安装期说不满足"那类事故的防线本身。
TEST(VersionSatisfies, ReleaseIsAWildcardInDependencyMatching)
{
    // 候选有 release、约束没写 → 匹配 `=`/`>=`/`<=`，但**不**匹配 `>`/`<`
    EXPECT_TRUE(version_satisfies("1.0-5", "=", "1.0"));
    EXPECT_FALSE(version_satisfies("1.0-5", ">", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0-5", ">=", "1.0"));
    EXPECT_TRUE(version_satisfies("1.0-5", "<=", "1.0"));
    // 候选没写 release、约束有 → 反过来匹配**一切**（含 `>`/`<`/`!=`）
    EXPECT_TRUE(version_satisfies("1.0", "=", "1.0-5"));
    EXPECT_TRUE(version_satisfies("1.0", ">", "1.0-5"));
    EXPECT_TRUE(version_satisfies("1.0", "!=", "1.0-5"));
    // 两端都写了 release → 常规比较
    EXPECT_TRUE(version_satisfies("1.0-5", "=", "1.0-5"));
    EXPECT_TRUE(version_satisfies("1.0-5", ">", "1.0-3"));
    EXPECT_FALSE(version_satisfies("1.0-3", ">", "1.0-5"));
}

TEST(VersionSatisfies, ComplexScenarios)
{
    EXPECT_TRUE(version_satisfies("2.0.0", ">=", "1.0.0"));
    EXPECT_TRUE(version_satisfies("2.0.0", ">=", "2.0.0"));
    EXPECT_FALSE(version_satisfies("1.0.0", ">=", "2.0.0"));

    // 稳定版 > 预发布版
    EXPECT_TRUE(version_satisfies("2.0.0", ">=", "2.0.0~rc1"));
    EXPECT_FALSE(version_satisfies("2.0.0~rc1", ">=", "2.0.0"));

    // 预发布之间
    EXPECT_TRUE(version_satisfies("2.0.0~rc2", ">", "2.0.0~rc1"));
}

// ===== 发行修订号（release，`-N`）测试 =====
// `-` 之后就是 rpm 的 **release**（revision）：`1.0-1 > 1.0`。
// 旧写法（`1.0+1`，自有语义 + 编码桥）已随桥一起删除。

TEST(VersionCompare, ReleaseSuffix)
{
    // 有 release > 无 release
    EXPECT_FALSE(version_compare("22.1.7-2", "22.1.7"));
    EXPECT_TRUE(version_compare("22.1.7", "22.1.7-2"));
    EXPECT_FALSE(version_compare("1.0-1", "1.0"));

    // release 数值比较；release 也可以是多段
    EXPECT_TRUE(version_compare("22.1.7-1", "22.1.7-2"));
    EXPECT_FALSE(version_compare("22.1.7-2", "22.1.7-1"));
    EXPECT_TRUE(version_compare("1.0-2", "1.0-3"));
    EXPECT_TRUE(version_compare("1.0-2", "1.0-2.1"));

    // 完整排序链（rpm）：pre-release(`~`) < base < release-1 < release-2
    EXPECT_TRUE(version_compare("1.0~rc1", "1.0"));
    EXPECT_TRUE(version_compare("1.0", "1.0-1"));
    EXPECT_TRUE(version_compare("1.0-1", "1.0-2"));
    EXPECT_TRUE(version_compare("1.0~rc1", "1.0-1"));
    EXPECT_FALSE(version_compare("1.0-1", "1.0~rc1"));
}

// 回归：版本升级必须主导 release（systemd 261-3 → 261.2-3、tmux 3.7-2 → 3.7b-2）。
// 版本串**原样**交给 libsolv 的 rpm 比较器，它本身就按 epoch → version → release 分域比：
// 版本段不同时 release 根本不参与，于是版本升级必然主导 release 数字。
// （例：`1.0-9 < 1.0.1`。）
TEST(VersionCompare, VersionBumpDominatesRelease)
{
    EXPECT_TRUE(version_compare("261-3", "261.2-3"));
    EXPECT_FALSE(version_compare("261.2-3", "261-3"));
    EXPECT_TRUE(version_compare("3.7-2", "3.7b-2"));
    EXPECT_FALSE(version_compare("3.7b-2", "3.7-2"));
    EXPECT_TRUE(version_compare("1.0-9", "1.0.1"));
    EXPECT_FALSE(version_compare("1.0.1", "1.0-9"));
    // release 比较只在版本相等时才生效
    EXPECT_TRUE(version_compare("261.2-1", "261.2-3"));
    EXPECT_FALSE(version_compare("261.2-3", "261.2-1"));
}

// ===== 补丁后缀 (pN) 测试 =====
// pN 跟在**版本段**末尾（`1.9.17p2` 的版本段就是 `17p2`），所以比基础版新；
// release（`-1`）排在基础版**之后**、补丁版**之前**（rpm 的段语义）。

TEST(VersionCompare, PatchSuffix)
{
    EXPECT_FALSE(version_compare("1.9.17p2", "1.9.17"));
    EXPECT_TRUE(version_compare("1.9.17", "1.9.17p2"));

    // pN 数值比较（非字典序）
    EXPECT_TRUE(version_compare("1.0p1", "1.0p2"));
    EXPECT_FALSE(version_compare("1.0p2", "1.0p1"));
    EXPECT_TRUE(version_compare("1.0p1", "1.0p10"));
    EXPECT_FALSE(version_compare("1.0p10", "1.0p1"));

    // 字母序比较
    EXPECT_TRUE(version_compare("1.0a", "1.0p"));
    EXPECT_FALSE(version_compare("1.0p", "1.0a"));

    // 无数字 vs 有数字（无数字视为 0）
    EXPECT_TRUE(version_compare("1.0p", "1.0p2"));
    EXPECT_FALSE(version_compare("1.0p2", "1.0p"));

    // 与 release 的相对顺序：base < release < patch
    EXPECT_TRUE(version_compare("1.0", "1.0-1"));
    EXPECT_TRUE(version_compare("1.0-1", "1.0p1"));
    EXPECT_FALSE(version_compare("1.0p1", "1.0-1"));
    // 预发布仍排在这一切之前
    EXPECT_TRUE(version_compare("1.0~rc1", "1.0-1"));
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
// 注意：与 rpm 一致不再补段——1.0 与 1.0.0 是**不同**版本。
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

// ============================================================================
// dependency_name_of —— "一行 deps/ 元数据 → 依赖包名"的**唯一**实现
//
// 它存在之前，这条规则在四处各写了一遍、用了三种不同的判据（运算符感知 / 纯空白 /
// `ss >>`），于是同一个包在不同路径上会算出**不同的键**（真可达的一条见
// tests/integration/test_reverse_dep_key_consistency.cpp：纯空白规则把 `provb>=2.0`
// 整串当成包名，`get_reverse_deps("provb")` 于是永远查不到那个依赖者）。
//
// 本用例覆盖两件事：
//   ① 逐条列举的语义（含最容易写错的"约束紧贴包名"与 `!=`）；
//   ② **与 `parse_dep_strings` 交叉验证** —— 两者对同一个串必须给出同一个名字。
//      第 ② 条是关键：只要有人在任一头上改了判据，这里立刻红。
// ============================================================================
TEST(DepParser, DependencyNameOfMatchesParseDepStrings)
{
    const std::vector<std::string> lines = {
        "glibc",                 // 无约束
        "cmake >= 3.20",         // 常规带空格
        "cmake>=3.20",           // **约束紧贴包名**（纯空白规则在这里给出整串）
        "foo!=1.0",              // `!` 打头的二元运算符（`find_first_of(" \t<>=")` 给出 "foo!"）
        "libfoo <= 2.0 >= 1.0",  // 复合约束：名字是最早那个运算符之前那段
        "  cmake  ",             // 前后空白
        "cmake  >=  3.20",       // 运算符两侧多余空格
        "ninja",                 // 名字里没有运算符
        "gcc-libs",              // 名字含 `-`
    };
    for (const auto& line : lines) {
        const std::string name = detail::dependency_name_of(line);
        EXPECT_FALSE(name.empty())
            << "名字不该为空（空名会让调用方算出一个永远查不到的键）：" << line;
        // ② 交叉验证：同一个串，名字提取与语法解析必须一致
        const auto parsed = detail::parse_dep_strings({line});
        ASSERT_EQ(parsed.size(), 1u) << line;
        EXPECT_EQ(name, parsed[0].name)
            << "名字提取与 parse_dep_strings 不一致（两者是同一套判据的两面）：" << line;
    }

    // ① 逐条点名 —— 这三条正是旧的三份实现各错一条的地方
    EXPECT_EQ(detail::dependency_name_of("cmake>=3.20"), "cmake");
    EXPECT_EQ(detail::dependency_name_of("foo!=1.0"), "foo");
    EXPECT_EQ(detail::dependency_name_of("  cmake  "), "cmake");
    EXPECT_EQ(detail::dependency_name_of("provb>=2.0 <3.0"), "provb");
    // CRLF：`trim_copy` 只去空格与制表符，**不碰 `\r`** —— 行读的尾巴必须在里面剥掉
    EXPECT_EQ(detail::dependency_name_of("provb\r"), "provb");
    EXPECT_EQ(detail::dependency_name_of("provb>=2.0\r"), "provb");
}
