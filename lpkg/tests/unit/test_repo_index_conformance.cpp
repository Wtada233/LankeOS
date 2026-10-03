/**
 * test_repo_index_conformance.cpp — 索引格式的**跨语言契约**（C++ 侧）
 *
 * 索引格式 `name|ver:hash:deps:provides:needed_so;ver2:…|包级提供` 有两份读者：
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
 * 本 fixture 的**前两条**正是历史上两侧不一致的地方（2026-10-03 对齐）：
 *   ① 版本级 provides 为空 ⇒ **回退到行级**（Python 原先完全忽略）
 *   ② 只含版本号、**没有冒号**的版本块是合法的（Python 原先整块丢弃）
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
    "foo|1.0:aaaa:dep1,dep2:libfoo.so.1:libc.so.6|\n"  // 常规：5 字段齐全
    "bar|2.0:bbbb::|libbar.so.2\n"                     // 行级 provides 回退
    "baz|3.0\n"                                        // 1 字段版本块
    "qux|4.0:dddd::\n";                                // 4 字段、行级也为空

struct Fields {
    std::string sha256, deps, provides, needed_so;
};

/// 与 check_index_conformance.py 的 EXPECTED **逐字对应**
const std::map<std::string, std::map<std::string, Fields>>& expected()
{
    static const std::map<std::string, std::map<std::string, Fields>> kExpected = {
        {"foo", {{"1.0", {"aaaa", "dep1,dep2", "libfoo.so.1", "libc.so.6"}}}},
        {"bar", {{"2.0", {"bbbb", "", "libbar.so.2", ""}}}},
        {"baz", {{"3.0", {"", "", "", ""}}}},
        {"qux", {{"4.0", {"dddd", "", "", ""}}}},
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
        if (blocks.empty()) continue;  // 注释行 / 空行
        for (const auto& b : blocks) {
            got[b.name][b.version] = Fields{b.hash, b.deps, b.provides, b.needed_so};
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
            EXPECT_EQ(have.provides, want.provides) << name << " " << ver
                                                    << " 的 provides"
                                                       "（版本级为空时应回退到**行级**第 3 段）";
            EXPECT_EQ(have.needed_so, want.needed_so) << name << " " << ver << " 的 needed_so";
        }
    }
}

/** 正面点名两条历史分叉 —— 上面那套遍历若被改写，这两条仍会独立地把它钉住。 */
TEST(RepoIndexConformanceTest, RowLevelProvidesFallbackAndOneFieldBlock)
{
    const auto bar = parse_repo_index_line("bar|2.0:bbbb::|libbar.so.2");
    ASSERT_EQ(bar.size(), 1u);
    EXPECT_EQ(bar[0].provides, "libbar.so.2") << "版本级 provides 为空时必须回退到行级（Python "
                                                 "侧曾忽略它，见 check_index_conformance.py）";

    const auto baz = parse_repo_index_line("baz|3.0");
    ASSERT_EQ(baz.size(), 1u) << "只含版本号的版本块**是合法的**，不该整块丢弃";
    EXPECT_EQ(baz[0].version, "3.0");
    EXPECT_TRUE(baz[0].hash.empty()) << "没有哈希字段就该是空串，不是解析失败";
}
