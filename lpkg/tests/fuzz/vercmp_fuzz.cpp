// harness #3：版本约束判定的**差分 fuzz** —— libsolv 的依赖匹配 vs `version_satisfies()`。
//
// 为什么是它：历史上这一族真出过事。lpkg 曾有**自己的一套**版本语义（`-` = 预发布、
// `+N` = 发行修订号）并用编码桥把它映射进 libsolv 的 rpm 语义，两套判据在手工枚举的
// 4032 组合矩阵上差 **78 处、两个方向都有**（`= 1.0` 匹配 `1.0+1` 是假满足 ⇒ 装出坏系统；
// `> 1.0+1` 反过来匹配 `1.0` 是假不满足 ⇒ 事务无解）。**假满足与假不满足都是实打实的
// 事故**，而那个矩阵是**枚举**的，覆盖不到形态空间。
//
// 8.0.0 取消了第二套语义：lpkg 原生用 rpm 的 EVR，版本串**原样**进池。本 harness 因此
// 不再测"编码往返"（那个 API 已删），只留**差分** —— 它现在守的是**剩下的那层判据**：
//
//   1. 算子映射（`version_op_flags`，`solver.cpp` 灌依赖用的就是它）；
//   2. "缺 release = 通配"那张 ±2 三元表（`version.cpp` 的 `version_satisfies`，
//      逐条对应 `pooldep.c` 的 `pool_match_nevr_rel`）。
//
// 两侧的**比较器**如今是同一个（`pool_evrcmp_str`），所以这个 harness **不再**能发现
// "比较器分叉"（那类缺陷已从根上不存在），但仍然能抓住上面两层里任何一处写错 ——
// 而写错的后果与当年一样重。别再把它读成"桥的回归闸门"。
//
// 输入 = 若干版本串（换行分隔，最多 8 个）。差分机制：建池 → 每个版本一个 solvable、
// **自提供 `name = evr`**（自提供不带版本会让 `>= 2.0` 被 1.0 满足）→
// `pool_whatprovides_ptr` 问 libsolv"谁满足这个依赖" → 与 `version_satisfies` 逐格对。

// libsolv 的 `Solvable` 有 `requires` 字段，而它在 C++20 里是关键字 → 宏改名绕开。
// **必须在 libsolv 头之后立刻 `#undef`**：这宏一旦泄漏到 C++ 标准库头里就炸
// （`<type_traits>` 自己用 `requires` 当关键字 —— 实测报 `unknown type name 'solv_requires'`）。
// `solver.cpp` 就是这么收的，照抄，别在这里发明第二种。
#define requires solv_requires
#include <solv/evr.h>
#include <solv/pool.h>
#include <solv/repo.h>
#undef requires

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "vercmp/version.hpp"

namespace
{

/// 算子表：`flags` 就是 `version_op_flags()` 的产物（这里**再写一遍**是有意的：
/// 若哪天有人改了 `version_op_flags` 的映射却不改池侧的期望，这张表与它的不一致
/// 就会以"分叉"的形式炸出来 —— 它不是第二份实现，是本 harness 的期望值）。
struct RelOp {
    const char* text;
    int flags;
};

const std::vector<RelOp>& ops()
{
    static const std::vector<RelOp> v = {
        {"=", REL_EQ},           {"==", REL_EQ}, {"!=", REL_GT | REL_LT}, {">", REL_GT},
        {">=", REL_EQ | REL_GT}, {"<", REL_LT},  {"<=", REL_EQ | REL_LT},
    };
    return v;
}

/// 每次迭代最多参与比对的版本数：够表达形态，又让池保持小（差分是 O(算子数 × n²)）。
constexpr std::size_t kMaxVersions = 8;

std::size_t g_checks = 0;
std::size_t g_wildcard_side = 0;  ///< 命中"缺 release"那一侧的次数（证明危险形态真被走到）

[[noreturn]] void oracle_violation(const std::string& why)
{
    std::fprintf(stderr, "[fuzz] 版本判定分叉: %s\n", why.c_str());
    __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    std::atexit([] {
        std::fprintf(stderr,
                     "[fuzz] vercmp: 比对 %zu 格 / 其中两侧 release 形态不同（±2 通配那侧）"
                     "%zu 格\n",
                     g_checks, g_wildcard_side);
    });
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 4096) return 0;

    // **只接受可打印 ASCII（外加作为分隔符的换行）**：`pool_str2id` 收的是 **C 串** —— 输入里的
    // NUL 会让 libsolv 看到的版本串**被截断**，而 `version_satisfies` 看的是完整串 ⇒ 两边比的
    // **根本不是同一个字符串**，报出来的"分叉"是这个 harness 造出来的。实测踩过：一条带
    // `1.<0x80>\0\0\x16...` 的输入被报成分叉。真实版本串全部落在可打印 ASCII 内（索引 678 个
    // 版本实测过），所以这条过滤不损失覆盖。
    for (std::size_t i = 0; i < size; ++i) {
        const uint8_t c = data[i];
        if (c == '\n') continue;
        if (c < 0x20 || c >= 0x7f) return 0;
    }

    // 换行分隔的版本串。空行丢弃（否则会让 `vers` 里出现空串，那不是有意义的版本）。
    std::vector<std::string> vers;
    std::string cur;
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] == '\n') {
            if (!cur.empty()) vers.push_back(cur);
            cur.clear();
            if (vers.size() >= kMaxVersions) break;
        } else {
            cur.push_back(static_cast<char>(data[i]));
        }
    }
    if (!cur.empty() && vers.size() < kMaxVersions) vers.push_back(cur);
    if (vers.size() < 2) return 0;

    Pool* pool = pool_create();
    pool_setdisttype(pool, DISTTYPE_RPM);  // libsolv 的默认 disttype 就是 RPM，与 lpkg 实际一致
    Repo* repo = repo_create(pool, "fuzz");
    const Id name = pool_str2id(pool, "fuzz-pkg", 1);

    std::vector<Id> evr_ids;
    evr_ids.reserve(vers.size());
    for (const auto& e : vers) {
        Solvable* s = pool_id2solvable(pool, repo_add_solvable(repo));
        s->name = name;
        s->evr = pool_str2id(pool, e.c_str(), 1);  // 8.0.0 起版本串**原样**进池，无编码
        solvable_add_deparray(s, SOLVABLE_PROVIDES, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1),
                              0);
        evr_ids.push_back(s->evr);
    }
    pool_createwhatprovides(pool);

    for (const auto& op : ops()) {
        for (std::size_t d = 0; d < vers.size(); ++d) {
            const Id dep_id = pool_rel2id(pool, name, evr_ids[d], op.flags, 1);

            std::vector<bool> matched(vers.size(), false);
            for (Id* p = pool_whatprovides_ptr(pool, dep_id); *p; ++p) {
                const Solvable* s = pool_id2solvable(pool, *p);
                if (s->name != name) continue;
                for (std::size_t i = 0; i < evr_ids.size(); ++i) {
                    if (s->evr == evr_ids[i]) matched[i] = true;
                }
            }

            for (std::size_t i = 0; i < vers.size(); ++i) {
                ++g_checks;
                // 统计"一侧有 release、另一侧没有"的格数：那正是 ±2 通配规则的适用面，
                // 也是真实仓库里可达的形态（如候选 `6.5-20250809` 对约束 `>= 6.5`）。
                const bool a_has_rel = vers[i].find('-') != std::string::npos;
                const bool b_has_rel = vers[d].find('-') != std::string::npos;
                if (a_has_rel != b_has_rel) ++g_wildcard_side;

                const bool libsolv_says = matched[i];
                const bool lpkg_says = version_satisfies(vers[i], op.text, vers[d]);
                if (libsolv_says != lpkg_says) {
                    char buf[512];
                    std::snprintf(
                        buf, sizeof buf,
                        "libsolv 说 `fuzz-pkg %s %s` %s版本 %s 满足，"
                        "而 version_satisfies 说相反（求出的方案会被自己拒掉 / 装出坏系统）",
                        op.text, vers[d].c_str(), libsolv_says ? "被" : "不被", vers[i].c_str());
                    oracle_violation(buf);
                }
            }
        }
    }
    pool_free(pool);
    return 0;
}
