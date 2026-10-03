// harness #3：版本桥的**差分 fuzz** —— libsolv 的依赖匹配 vs `version_satisfies()`。
//
// 为什么是它：这条桥保证"**求解器按 libsolv 出方案、安装期按 lpkg 的判据验收**"两边一致。
// 不一致的后果是实打实的：假不满足 ⇒ 明明装得上却报缺依赖（事务无解）；假满足 ⇒ 求解器选了
// 一个 lpkg 认为不满足的版本，装到一半被拒、整批回滚，或更糟。**它已经真出过事**：当年
// `+release` 被映射进 libsolv 的 release 槽位，在**手工枚举**的 4032 组合矩阵上差 **78 处、
// 两个方向都有**（`= 1.0` 匹配 `1.0+1`；`> 1.0+1` 反过来匹配 `1.0`）。
//
// 那个矩阵是**枚举**的（`tests/unit/test_vercmp_libsolv_bridge.cpp`，24 个版本 × 7 算子）；
// 这里把**同一套判据**搬进 fuzzer，让**版本串本身**去变异 —— 多段、`~`/`^`/`:`、超长数字段、
// 退化串（`1.0+`）、混合分隔符这些枚举覆盖不到的形态才有机会被走到。
//
// 输入 = 若干版本串（换行分隔，最多 8 个）。差分机制与那个测试文件**逐字同款**：
// 建池 → 每个版本一个 solvable、**自提供 `name = evr`**（自提供不带版本会让 `>= 2.0` 被 1.0 满足）
// → `pool_whatprovides_ptr` 问 libsolv"谁满足这个依赖" → 与 `version_satisfies` 逐格对。

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

/// 算子表：`flags` 与 `solver.cpp` 的 `rel_op()` 逐条对应（照抄桥接测试）。
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

[[noreturn]] void oracle_violation(const std::string& why)
{
    std::fprintf(stderr, "[fuzz] 版本桥分叉: %s\n", why.c_str());
    __builtin_trap();
}

}  // namespace

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

    // 编码可能**有意**抛异常：`^`（release 分隔符）/`~`（预发布）/`:`（epoch）是版本域的保留
    // 字符，`to_libsolv_evr` 拒它们是有据可查的行为（真实索引 678 个版本里这三者一个都没有）
    // —— 那是"拒绝"，不是缺陷，所以整包跳过。
    std::vector<std::string> evrs;
    evrs.reserve(vers.size());
    for (const auto& v : vers) {
        try {
            evrs.push_back(to_libsolv_evr(v));
        } catch (const std::exception&) {
            return 0;
        }
    }

    Pool* pool = pool_create();
    pool_setdisttype(pool, DISTTYPE_RPM);  // libsolv 的默认 disttype 就是 RPM，与 lpkg 实际一致
    Repo* repo = repo_create(pool, "fuzz");
    const Id name = pool_str2id(pool, "fuzz-pkg", 1);

    std::vector<Id> evr_ids;
    evr_ids.reserve(evrs.size());
    for (const auto& e : evrs) {
        Solvable* s = pool_id2solvable(pool, repo_add_solvable(repo));
        s->name = name;
        s->evr = pool_str2id(pool, e.c_str(), 1);
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
