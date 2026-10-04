/**
 * test_repo_index_conformance.cpp — 索引格式的**跨语言契约**（C++ 侧）
 *
 * 索引格式 `name|ver:hash:deps:provides:provides_soname:needed_so;ver2:…|` 有两份读者：
 *   · **C++**：`base/utils.cpp` 的 `parse_repo_index_line()`（lpkg 唯一的解析器，
 *     `repository.cpp` 与 `depend_scanner.cpp` 共用）
 *   · **Python**：`main/scripts/lrepo-mgr.py` 的 `parse_aggregated_index()` —— 它的结果会被
 *     `push` 用来**读-改-写整份索引**、被 `cleanup` 用来决定删哪些包文件
 *
 * 少读一条不是显示问题，是**数据丢失 + 删掉仍被引用的文件**。所以两侧必须同语义。
 *
 * **契约由"同一份 fixture + 同一份期望"维持**，两侧各自断言、谁也不调谁：
 *   · C++ 侧：本文件
 *   · Python 侧：`main/scripts/check_index_conformance.py`（`make check-index-format`）
 * 任一侧漂移，都会被其中一边抓住。
 *
 * ⚠️ **订正 2026-10-04（8.0.0，破坏性）**：本 fixture 曾覆盖两种**兼容形态** —— 版本级
 * provides 为空时回退到**行级**（第 3 个 `|` 段），以及容忍 4/5 字段的版本块。两者都已
 * **废除**：版本块**恰好 6 个字段**、**没有行级 provides**。旧格式的块被**跳过**（不是被
 * 误读），与 Python 侧同判据。原有两条点名这些兼容行为的用例已随之删除/改写。
 *
 * ⚠️ 改本文件的 fixture 或期望时，**必须同时改** `check_index_conformance.py` 里那一份 ——
 *    两处逐字相同是这套契约的**全部**机制（没有生成器，也不建依赖）。
 */

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../main/src/base/utils.hpp"

namespace
{
/// 与 check_index_conformance.py 的 FIXTURE **逐字相同**
constexpr const char* kFixture =
    "# 注释行与空行应被两侧同样忽略\n"
    "\n"
    "foo|1.0:aaaa:dep1,dep2:libssl:libfoo.so.1:libc.so.6|\n"  // 常规：6 字段齐全
    "bar|2.0:bbbb:::libbar.so.2:|\n"                          // 各字段可为空
    "multi|1.0:aaaa:::libm.so.1:;2.0:bbbb::::libm.so.2|\n"    // 版本块间 `;`，共享包名
    "qux|4.0\n"  // 字段数不是 6 → 整块跳过（不做兼容读取）
    // SONAME 规格：花括号里的逗号**不是**字段分隔符（两侧都必须原样保留）
    "ver|3.0:cccc:::libc.so.6@{GLIBC_2.40,GLIBC_2.39}:libm.so.6@GLIBC_2.2.5|\n";

struct Fields {
    std::string sha256, deps, provides, provides_soname, needed_so;
};

/// 与 check_index_conformance.py 的 EXPECTED **逐字对应**
const std::map<std::string, std::map<std::string, Fields>>& expected()
{
    static const std::map<std::string, std::map<std::string, Fields>> kExpected = {
        {"foo", {{"1.0", {"aaaa", "dep1,dep2", "libssl", "libfoo.so.1", "libc.so.6"}}}},
        {"bar", {{"2.0", {"bbbb", "", "", "libbar.so.2", ""}}}},
        {"multi",
         {{"1.0", {"aaaa", "", "", "libm.so.1", ""}}, {"2.0", {"bbbb", "", "", "", "libm.so.2"}}}},
        {"ver",
         {{"3.0", {"cccc", "", "", "libc.so.6@{GLIBC_2.40,GLIBC_2.39}", "libm.so.6@GLIBC_2.2.5"}}}},
    };
    return kExpected;
}
}  // namespace

TEST(RepoIndexConformanceTest, CParserMatchesTheSharedContract)
{
    std::map<std::string, std::map<std::string, Fields>> got;
    std::string line;
    for (const char* p = kFixture; *p != '\0';) {
        // 逐行喂给解析器（它接受**单行**，注释/空行返回空表）
        const char* nl = std::strchr(p, '\n');
        line.assign(p, nl ? static_cast<std::size_t>(nl - p) : std::strlen(p));
        p = nl ? nl + 1 : p + std::strlen(p);

        const auto blocks = parse_repo_index_line(line);
        if (blocks.empty()) continue;  // 注释行 / 空行 / 字段数不是 6 的块
        for (const auto& b : blocks) {
            got[b.name][b.version] =
                Fields{b.hash, b.deps, b.provides, b.provides_soname, b.needed_so};
        }
    }

    // ① 包的集合必须一致（多一个少一个都要报）
    ASSERT_EQ(got.size(), expected().size())
        << "解析出的包数与契约不符（少了 = 整条被丢；多了 = 不该有）";
    for (const auto& [name, versions] : expected()) {
        ASSERT_TRUE(got.count(name)) << "包 '" << name << "' 整条缺失";
        ASSERT_EQ(got.at(name).size(), versions.size())
            << "包 '" << name << "' 的版本块数与契约不符";

        for (const auto& [ver, want] : versions) {
            ASSERT_TRUE(got.at(name).count(ver)) << "包 '" << name << "' 缺版本 '" << ver << "'";
            const Fields& have = got.at(name).at(ver);
            EXPECT_EQ(have.sha256, want.sha256) << name << " " << ver << " 的 sha256";
            EXPECT_EQ(have.deps, want.deps) << name << " " << ver << " 的 deps";
            EXPECT_EQ(have.provides, want.provides) << name << " " << ver << " 的 provides";
            EXPECT_EQ(have.provides_soname, want.provides_soname)
                << name << " " << ver << " 的 provides_soname";
            EXPECT_EQ(have.needed_so, want.needed_so) << name << " " << ver << " 的 needed_so";
        }
    }
}

/**
 * 正面点名两条 8.0.0 判据（上面那套遍历若被改写，这两条仍会独立地把它钉住）：
 *   ① 版本块**恰好 6 字段**：4/5 字段的旧块整块跳过（不再误读、也没有行级回退）；
 *   ② `;` 分隔的第二个版本块照常解析（与第一个共享包名）。
 */
TEST(RepoIndexConformanceTest, SixFieldBlocksOnlyAndAggregatedVersions)
{
    // 4 字段（旧写入器把 provides 写在 vh[3]）→ 跳过；5 字段同理
    EXPECT_TRUE(parse_repo_index_line("qux|4.0\n").empty())
        << "4 字段（只有版本号）的块必须整块跳过，不做兼容读取";
    EXPECT_TRUE(parse_repo_index_line("old|1.0:aaaa:dep1:libfoo.so.1\n").empty())
        << "5 字段的旧块必须整块跳过（少一个字段会让后面整体错位）";

    const auto multi = parse_repo_index_line("multi|1.0:aaaa:::libm.so.1:;2.0:bbbb::::libm.so.2|");
    ASSERT_EQ(multi.size(), 2u);
    EXPECT_EQ(multi[0].version, "1.0");
    EXPECT_EQ(multi[0].provides_soname, "libm.so.1");
    EXPECT_EQ(multi[1].version, "2.0");
    EXPECT_EQ(multi[1].needed_so, "libm.so.2");
}
