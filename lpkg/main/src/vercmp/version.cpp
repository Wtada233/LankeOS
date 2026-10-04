#include "version.hpp"

// libsolv 的 `Solvable` 有 `requires` 字段，而它在 C++20 里是关键字 → 宏改名绕开。
// **必须在 libsolv 头之后立刻 `#undef`**（`<type_traits>` 自己用 `requires` 当关键字，
// 漏了 `#undef` 就会炸在标准库头里）。`solver.cpp` 就是这么收的，别发明第二种。
#define requires solv_requires
#include <solv/evr.h>
#include <solv/pool.h>
#undef requires

#include <mutex>
#include <string>
#include <vector>

#include "../base/exception.hpp"
#include "../i18n/localization.hpp"

namespace
{

/**
 * **比较用的独立小池**：`pool_evrcmp_str()` 需要一个 `Pool` 去取 disttype，而 lpkg 的比较
 * 可能发生在任何地方（排序仓库版本、读配置、比较约束……），那里没有求解器的池。
 *
 * disttype = **RPM**（libsolv 的默认，也是 lpkg 从 8.0.0 起**采用**的版本语义）。
 * 本池只用来解析字符串、不做 interning；但 libsolv 整体不是线程安全的，故仍串行化
 * （与 `pkg/solver.cpp` 同一取向）。
 */
const Pool* comparison_pool()
{
    static Pool* pool = [] {
        Pool* p = pool_create();
        pool_setdisttype(p, DISTTYPE_RPM);
        return p;
    }();
    return pool;
}

/**
 * EVR 比较（rpm 语义：`[epoch:]version[-release]`）。返回 <0 / 0 / >0。
 *
 * ⚠️ **8.0.0 起不再有"桥"**（推翻了什么的订正痕迹在 `version.hpp`）。此前这里先把 lpkg 版本
 * 编码成 libsolv EVR（`-`→`~`、`+release`→`^^release`）再自己拆 release，为的是让
 * **libsolv 的依赖匹配**与 **lpkg 的 `version_satisfies()`** 逐条一致 —— 而这两套判据
 * **不一致过**：2026-10-03 在 4032 组合的 (候选 × 约束) 矩阵上差 **78 处、两个方向都有**
 * （`= 1.0` 匹配 `1.0+1` 是假满足；`> 1.0` 不匹配 `1.0+1` 是假不满足）。
 *
 * 现在只留**一份实现**：libsolv 自己的 EVR 比较，**版本串原样进**。求解器内部用的也是它
 * ⇒ "两边必须手动保持一致"这件事从根上不存在了。
 */
int evr_cmp(const std::string& a, const std::string& b)
{
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    return pool_evrcmp_str(comparison_pool(), a.c_str(), b.c_str(), EVRCMP_COMPARE);
}

}  // namespace

bool version_compare(const std::string& v1_str, const std::string& v2_str)
{
    return evr_cmp(v1_str, v2_str) < 0;
}

int version_op_flags(const std::string& op)
{
    // 与 `solver.cpp` 往池里灌依赖时用的是**同一张表**（本函数就是它的唯一实现）。
    if (op == ">=") return REL_EQ | REL_GT;
    if (op == "<=") return REL_EQ | REL_LT;
    if (op == ">") return REL_GT;
    if (op == "<") return REL_LT;
    if (op == "==" || op == "=") return REL_EQ;
    if (op == "!=") return REL_GT | REL_LT;
    // 未知 op 兜底——宽松处理避免误判冲突，比卡死更安全（`solver.cpp` 的旧注释原样保留）。
    return REL_EQ | REL_GT | REL_LT;
}

bool version_satisfies(const std::string& current_version, const std::string& op,
                       const std::string& required_version)
{
    static const std::vector<std::string> kKnownOps = {"=", "==", "!=", "<", "<=", ">", ">="};
    bool known = false;
    for (const auto& k : kKnownOps) known = known || k == op;
    if (!known) throw LpkgException(string_format("error.invalid_version_format", op));

    const int flags = version_op_flags(op);
    const int lt = (flags & REL_LT) != 0;
    const int gt = (flags & REL_GT) != 0;
    const int eq = (flags & REL_EQ) != 0;

    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    // ⚠️ **必须用 `EVRCMP_MATCH_RELEASE`（= `pooldep.c` 的 `EVRCMP_DEPCMP` 在 RPM disttype 下
    // 的取值），不是 `evr_cmp` 的 `EVRCMP_COMPARE`。** 两者对"缺 release"的处置不同：
    // `EVRCMP_COMPARE` 只用来**排序**（有 release 者更大）；依赖匹配则按 rpm 的规则把
    // "缺 release"当**通配**（`evr.c` 在 MATCH_RELEASE 下返回 ±2）。
    //
    // 下面这张三元表**逐字对应** `pooldep.c` 的 `pool_match_nevr_rel()`（libsolv 判定
    // "某个 solvable 是否满足某个依赖"用的就是它，`pool_whatprovides` 同一份）：
    //   -2  候选没有 release、约束有  → **匹配一切**（含 `>`/`<`）
    //    2  候选有 release、约束没有  → **只匹配 `=`/`<=`/`>=`**，不匹配 `>`/`<`
    //    -1/0/1 → 常规的 `<` / `=` / `>` 判据
    // 这是 rpm 依赖语义里那处**有名的非对称**（未写 release 视为"任何 release"）。它只在
    // 一侧有 release 时才出现，而 lpkg 的版本现在原生就是 rpm EVR ⇒ 真实数据里可达。
    const int c = pool_evrcmp_str(comparison_pool(), current_version.c_str(),
                                  required_version.c_str(), EVRCMP_MATCH_RELEASE);
    if (c == -2) return true;
    if (c == 2) return eq;
    if (c < 0) return lt;
    if (c > 0) return gt;
    return eq;
}

bool version_satisfies_all(const std::string& current_version,
                           const std::vector<Constraint>& constraints)
{
    for (const auto& c : constraints)
        if (!version_satisfies(current_version, c.op, c.version)) return false;
    return true;
}
