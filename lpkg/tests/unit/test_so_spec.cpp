/**
 * test_so_spec.cpp —— SONAME 规格（symbol version）的解析 / 校验 / 匹配 / 展开
 *
 * 这套语法让 `provides_soname` 与 `needed_so` 能表达**同一个 SONAME 内部的符号版本**
 * （`libc.so.6@GLIBC_2.40`、`libc.so.6@{GLIBC_2.40,GLIBC_2.39}`）。本文件是它的**唯一语义
 * 判据**的钉子：`base/so_spec.cpp` 里那份实现若不成立，下面的用例必须红。
 *
 * ⚠️ 三条**有意**的决策，写在这里免得下轮被当成缺陷报：
 *   1. **保守匹配**：带符号版本的 need **不**被"只声明裸 SONAME"的 provider 满足
 *      （`NeededSymbolVersionIsNotSatisfiedByABareProvider`）—— 否则今天 861 个裸包会让整个
 *      特性形同虚设；
 *   2. **provider 声明带版本时，裸条目仍然提供**（`ExpandProvidesKeepsTheBareEntry`）——
 *      否则裸 need 会被"声明得更细"的 provider 打破；
 *   3. **规格里不许有空白**（`WhitespaceAnywhereIsRejected`）—— 索引行"零空白"是可执行
 *      不变量（`wc -w` / 无参 `split()` / `awk '{print $k}'` 都靠它）。
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../../main/src/base/so_spec.hpp"

namespace
{

/// 断言"合法"，失败时把原串打出来（这类用例红起来都是"哪一条不合法"的问题）
void expect_wellformed(const std::string& raw)
{
    EXPECT_TRUE(so_spec_wellformed(raw)) << "应判合法却没判合法：'" << raw << "'";
}

void expect_malformed(const std::string& raw)
{
    EXPECT_FALSE(so_spec_wellformed(raw)) << "应判畸形却判合法：'" << raw << "'";
}

std::string join(const std::vector<std::string>& v)
{
    std::string s;
    for (const auto& x : v) {
        if (!s.empty()) s += ",";
        s += x;
    }
    return s;
}

}  // namespace

// ============================ 解析与裸名 ============================

TEST(SoSpec, BareNameHasNoSymbols)
{
    const SoSpec s = parse_so_spec("libc.so.6");
    EXPECT_EQ(s.soname, "libc.so.6");
    EXPECT_TRUE(s.bare());
    EXPECT_TRUE(s.symbols.empty());
    EXPECT_EQ(so_spec_key("libc.so.6"), "libc.so.6");
}

TEST(SoSpec, SingleSymbolVersion)
{
    const SoSpec s = parse_so_spec("libc.so.6@GLIBC_2.40");
    EXPECT_EQ(s.soname, "libc.so.6");
    EXPECT_FALSE(s.bare());
    ASSERT_EQ(s.symbols.size(), 1u);
    EXPECT_EQ(s.symbols[0], "GLIBC_2.40");
    // 裸名（建索引/查表/拼系统库路径）必须与带版本的写**指向同一个 SONAME**
    EXPECT_EQ(so_spec_key("libc.so.6@GLIBC_2.40"), "libc.so.6");
}

TEST(SoSpec, BraceFormDedupsAndCanonicalisesOrder)
{
    const SoSpec s = parse_so_spec("libc.so.6@{GLIBC_2.40,GLIBC_2.39,GLIBC_2.2.5}");
    EXPECT_EQ(s.soname, "libc.so.6");
    // **规范化 = 去重 + 字典序**：`X@{A,B}` 与 `X@{B,A}` 必须归一成同一个串，否则
    // 归档写一种、索引写另一种就会被 metadata 一致性校验判成不一致（同义不同串）。
    EXPECT_EQ(join(s.symbols), "GLIBC_2.2.5,GLIBC_2.39,GLIBC_2.40");
    EXPECT_EQ(join(parse_so_spec("X@{B,A}").symbols), join(parse_so_spec("X@{A,B}").symbols));
    EXPECT_EQ(join(parse_so_spec("X@{A,A,B}").symbols), "A,B");
}

TEST(SoSpec, WellformedForms)
{
    for (const char* raw : {"libc.so.6", "libc.so.6@GLIBC_2.40", "libc.so.6@{GLIBC_2.40}",
                            "libc.so.6@{GLIBC_2.40,GLIBC_2.39}", "libstdc++.so.6@GLIBCXX_3.4.30",
                            "ld-linux-x86-64.so.2@GLIBC_2.34", "libQt6Core.so.6@Qt_6_PRIVATE_API",
                            "libc.so.6@{a_b.c,9,D}"}) {
        expect_wellformed(raw);
    }
}

TEST(SoSpec, MalformedFormsAreRejected)
{
    for (const char* raw : {
             "",                                    // 空
             "@GLIBC_2.40",                         // 空 SONAME
             "libc.so.6@",                          // 空版本
             "libc.so.6@{}",                        // 空花括号块
             "libc.so.6@{GLIBC_2.40,}",             // 尾随逗号 ⇒ 空项
             "libc.so.6@{,GLIBC_2.40}",             // 开头逗号 ⇒ 空项
             "libc.so.6@{GLIBC_2.40",               // 花括号没闭合
             "libc.so.6@GLIBC_2.40}",               // 多余的 }
             "libc.so.6@{A}{B}",                    // 花括号段出现两次
             "libc.so.6@{A{B}}",                    // 嵌套
             "libc.so.6@@A",                        // 两个 @
             "libc.so.6@A@B",                       // 两个 @
             "libc.so.6@A{B}",                      // 花括号不在块位置
             "libc.so.6@GLIBC 2.40",                // 空格
             "libc.so.6 @GLIBC_2.40",               // 空格
             "libc.so.6@{GLIBC_2.40, GLIBC_2.39}",  // 花括号内的空格
             "libc.so.6\t@GLIBC_2.40",              // 制表符
             "libc.so.6@GLIBC_2.40\n",              // 换行
             "libc.so.6|x",                         // 分帧字符 |
             "libc.so.6;x",                         // 分帧字符 ;
             "libc.so.6:x",                         // 分帧字符 :（版本域早被判死）
             "libc.so.6,x",                         // 列表分隔符（逗号只能出现在花括号里）
         }) {
        expect_malformed(raw);
    }
    // 名字里的 `-` 与 `+` 是合法的（真实 SONAME 就有 `ld-linux-x86-64.so.2`）
    expect_wellformed("ld-linux-x86-64.so.2");
    expect_wellformed("libfoo.so.1+git");
    // 符号名与 SONAME 用**同一套**字符集（`-`/`+` 也放行）—— 真实 ELF 版本串偶尔带它们，
    // 收紧只会把合法数据挡在门外，而结构字符那几条才是格式真正需要的
    expect_wellformed("libc.so.6@GLIBC-2.40");
    expect_wellformed("libc.so.6-1@A");  // SONAME 带 `-`（如 ld-linux-x86-64.so.2）＋单符号版本
    expect_wellformed("libz.so.1@{ZLIB_1.2.9,XZ_5.2+}");
}

TEST(SoSpec, MalformedFallsBackToWholeStringAsBareName)
{
    // 宽容解析：判定路径遇到畸形串不抛、也不猜 —— 整串当裸名（落回"整串相等"的老语义）
    const SoSpec s = parse_so_spec("libc.so.6@{GLIBC_2.40");
    EXPECT_EQ(s.soname, "libc.so.6@{GLIBC_2.40");
    EXPECT_TRUE(s.bare());
    EXPECT_EQ(so_spec_key("libc.so.6@{GLIBC_2.40"), "libc.so.6@{GLIBC_2.40");
}

// ============================ 匹配（真值表） ============================

TEST(SoSpec, SatisfiesTruthTable)
{
    // provider 声明 | need 声明 | 满足？
    EXPECT_TRUE(so_spec_satisfies("X", "X"));          // 裸对裸
    EXPECT_TRUE(so_spec_satisfies("X@{A,B}", "X"));    // 库在就行
    EXPECT_TRUE(so_spec_satisfies("X@{A,B}", "X@A"));  // 约定含
    EXPECT_TRUE(so_spec_satisfies("X@{A,B}", "X@{A,B}"));
    EXPECT_TRUE(so_spec_satisfies("X@{A,B}", "X@{B,A}"));   // 集合语义，与顺序无关
    EXPECT_FALSE(so_spec_satisfies("X@{A,B}", "X@{A,C}"));  // 缺 C
    EXPECT_FALSE(so_spec_satisfies("X@A", "X@B"));          // 版本不同
    EXPECT_FALSE(so_spec_satisfies("X", "X@A"));            // **保守语义**（钉子）
    EXPECT_FALSE(so_spec_satisfies("X@{A}", "Y@A"));        // SONAME 不同
    EXPECT_FALSE(so_spec_satisfies("X", "Y"));
}

TEST(SoSpec, NeededSymbolVersionIsNotSatisfiedByABareProvider)
{
    // 这条是整个特性的**存在理由**：今天所有包都只声明裸 SONAME，如果裸 provider 放行，
    // 任何带符号版本的 need 都会被随便一个 provider 满足 ⇒ 特性形同虚设。
    // 这里连"自我声明"也一并钉住：
    EXPECT_FALSE(so_spec_satisfies("libc.so.6", "libc.so.6@GLIBC_2.40"));
    EXPECT_TRUE(so_spec_satisfies("libc.so.6@GLIBC_2.40", "libc.so.6@GLIBC_2.40"));
    EXPECT_TRUE(so_spec_satisfies("libc.so.6@{GLIBC_2.40,GLIBC_2.39}", "libc.so.6@GLIBC_2.40"));
}

TEST(SoSpec, MalformedSpecsOnlyMatchThemselves)
{
    // 宽容解析下畸形串退化成"裸名 = 整串"，于是只有**逐字相同**才算满足。
    // 不抛、不猜、也不把畸形串当成"通配任何版本"。
    EXPECT_TRUE(so_spec_satisfies("X@{A", "X@{A"));
    EXPECT_FALSE(so_spec_satisfies("X", "X@{A"));
    EXPECT_FALSE(so_spec_satisfies("X@{A", "X"));
    EXPECT_FALSE(so_spec_satisfies("X@{A,B", "X@A"));
}

// ============================ 灌池展开 ============================

TEST(SoSpec, ExpandProvidesKeepsTheBareEntry)
{
    // provider 侧：裸条目**必须有** —— 否则一个"声明了符号版本"的 provider 会让裸 need
    // （今天 100% 的 need 都是裸的）全部失效。
    EXPECT_EQ(join(expand_so_spec("libc.so.6@{GLIBC_2.40,GLIBC_2.39}", true)),
              "libc.so.6,libc.so.6@GLIBC_2.39,libc.so.6@GLIBC_2.40");
    EXPECT_EQ(join(expand_so_spec("libc.so.6@GLIBC_2.40", true)), "libc.so.6,libc.so.6@GLIBC_2.40");
    EXPECT_EQ(join(expand_so_spec("libc.so.6", true)), "libc.so.6");
}

TEST(SoSpec, ExpandNeedsOmitsTheBareEntry)
{
    // need 侧：带符号版本时**不带**裸条目 —— 这就是保守语义在 libsolv 池里的表达
    // （只登记 `so:X@A`，于是"只提供 `so:X` 的裸 provider"满足不了它）。
    EXPECT_EQ(join(expand_so_spec("libc.so.6@{GLIBC_2.40,GLIBC_2.39}", false)),
              "libc.so.6@GLIBC_2.39,libc.so.6@GLIBC_2.40");
    EXPECT_EQ(join(expand_so_spec("libc.so.6@GLIBC_2.40", false)), "libc.so.6@GLIBC_2.40");
    EXPECT_EQ(join(expand_so_spec("libc.so.6", false)), "libc.so.6");
}

TEST(SoSpec, ExpandOfMalformedEntryYieldsThatSingleString)
{
    // 畸形串不产生"半展开"的垃圾条目：整串一条进池（与宽容解析同口径）
    const auto items = expand_so_spec("X@{A,B", true);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0], "X@{A,B");
}

// ============================ 花括号感知切分 ============================

TEST(SoSpec, SplitKeepsBracesTogether)
{
    // 这条是**格式层面**的钉子：花括号里的逗号不是字段分隔符
    const auto items = split_so_list("libc.so.6@{GLIBC_2.40,GLIBC_2.39},libm.so.6,libz.so.1");
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0], "libc.so.6@{GLIBC_2.40,GLIBC_2.39}");
    EXPECT_EQ(items[1], "libm.so.6");
    EXPECT_EQ(items[2], "libz.so.1");
}

TEST(SoSpec, SplitMatchesTheOldCommaBehaviourOnPlainLists)
{
    // 与它取代的 `split_comma_list` 逐条一致：空片段丢弃、两端空白剥掉
    const auto items = split_so_list("a.so.1,,b.so.2,");
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0], "a.so.1");
    EXPECT_EQ(items[1], "b.so.2");
    EXPECT_TRUE(split_so_list("").empty());
    EXPECT_TRUE(split_so_list(",,,").empty());
    EXPECT_EQ(join(split_so_list(" a.so.1 , b.so.2 ")), "a.so.1,b.so.2");
}

TEST(SoSpec, SplitDegradesGracefullyOnUnbalancedBraces)
{
    // 不配对的 `{` 不能把整段的切分吞掉（此时读入处的严格校验早已把这条拒了，
    // 但"切分"本身必须仍是有界的、确定的行为）
    const auto items = split_so_list("X@{A,B,libm.so.6");
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0], "X@{A,B,libm.so.6");

    // 多出来的 `}` 同理（深度不会变成负数）：深度仍是 0 ⇒ 后面的逗号**照常**是分隔符。
    // ⚠️ 这里必须逐元素断言：`join()` 会把 ["X}A","b.so.1"] 也拼成 "X}A,b.so.1" ——
    // 拿它做断言，两种结果都"通过"，等于什么都没钉（写这条时踩过）。
    const auto stray = split_so_list("X}A,b.so.1");
    ASSERT_EQ(stray.size(), 2u);
    EXPECT_EQ(stray[0], "X}A");
    EXPECT_EQ(stray[1], "b.so.1");
}
