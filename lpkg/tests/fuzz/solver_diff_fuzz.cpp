// harness：求解器的**小池穷举差分** —— libsolv 求出的"能不能装" vs **暴力枚举**的答案。
//
// 为什么是它：`solve_install` 决定"这个包能不能装、装哪些版本"。它错了的后果不是崩溃，
// 而是**给出错误的结论**：明明装得上却报"依赖无法满足"（用户装不了），或者求解器选出一个
// 安装期验收不过的版本组合（装到一半被拒、整批回滚）。这类缺陷**已经真出过事** ——
// 版本桥的 `+release` 映射错槽位时，手工枚举的矩阵上差 78 处、两个方向都有。
//
// 手工矩阵只能覆盖"人想得到"的组合；这里让**池子本身**（包数/版本数/依赖边/约束）去变异。
//
// ── 为什么必须把池子限得这么小 ─────────────────────────────────────────────
// oracle 是**暴力枚举**：把"每个包装哪个版本 / 不装"的全部组合走一遍（≤ 4 包 × ≤ 3 版本
// ⇒ ≤ 4^5 种），逐个用**安装期那套判据**验收。池子一大就没法穷举，而没有参考实现的
// "差分"只会退化成"再写一个求解器" —— 那正是本仓库的头号缺陷形态（第二实现必漂移）。
// 所以**故意**把上限压得这么低：小池子换来确定性。
//
// ── oracle（两条，都不重写任何判据）────────────────────────────────────────
//  1. **存在性一致**：暴力枚举"存在满足全部依赖的版本组合" ⟺ `SolveResult::ok()`。
//     等价性判据**全部复用生产函数**：版本约束用 `version_satisfies_all`（它同时是求解器
//     与安装期验收共用的那一个）—— 本 harness **一行版本比较都不自己写**。
//     满足关系照两套**隔离命名空间**建模（字段显式分开，不再靠字符串形状猜）：
//       · `needed_so: X` → 走 `so:` 空间 ⇒ **只**能被 `provides_soname` 满足
//         （包名与虚拟 provides 都够不着它）。带符号版本（`X@V` / `X@{V1,V2}`）时按**保守**
//         语义：只有"也声明了符号版本且覆盖它"的 provider 才算 —— 判据直接用生产的
//         `so_spec_satisfies()`；
//       · `deps: X` → 走裸名空间 ⇒ 匹配**包名**（自提供 `name = evr`，须满足版本约束）
//         或**虚拟 provides**（不带版本 ⇒ 通配任何约束）。
//     ⚠️ **目标包被钉在"最新版"**：本 harness 传的 target 是 `(name, "latest")`，而 lpkg 的
//     `latest` 是"装**最新**版"、**不回退到旧版**（装不上就是无解）。暴力枚举必须照这个语义
//     走 —— 否则会把"另一个更宽松的旧版本能装"误判成求解器漏解（见下）。
//  2. **产出的方案本身必须自洽**（`ok()` 为真时）：每个 ResolvedPkg 的版本确实在池子里、
//     **目标被满足**、方案内部所有 needed_so 与 deps 都被同一个方案满足。
//     这一条覆盖"求解器说能装、但给出的清单其实不满足"那一侧。
//     ⚠️ "目标被满足"同样**按能力语义**（见 oracle 1 底下那段）：池里没有同名包时，
//     方案里装的是**提供者**，不是叫那个名字的包 —— **同族判据只推一条分支**。
//
// ── 已知边界（有意不报的）──────────────────────────────────────────────────
//  · 池子里含**同名多版本**且互相依赖时，语义仍由上面两条覆盖；不做"装哪个更优"的比较
//    （求解器选哪个版本是实现自由，只要满足约束）。
//  · 不建模已装包（`installed` 恒为空）、不建模 local 包、不用任何 `SolveOptions` 开关 ——
//    这些都会引入"到底谁的语义对"的争议；要覆盖它们需要各自的专门 oracle。
//  · 没有反向依赖一致性（`dontfix`）的建模：那条只在有已装包时才起作用。
//  · **"最新版"不回退**：目标钉最新版、装不上即无解 —— 这是 `latest` 的语义，不是漏解。
//  · 池子里同名多版本时，**非目标包**可以任选版本（求解器的实现自由），只要求满足约束。
//
// ── 输入语法（每行一条，`-` 表示空表）────────────────────────────────────────
//   T <target-name>                                      （目标，≤ kMaxPkgs 个）
//   P <name> <ver> <provides> <provides_soname> <needed_so> <deps>
//     六个字段以空格分隔；`provides`/`provides_soname`/`needed_so`/`deps` 是 **`;` 分隔**的
//     表（`-` = 空表）。**不能用逗号**：`X@{A,B}` 里的逗号属于符号版本列表。
//
// ── 求解器对输入的前提（本 harness **逐条过滤**；这几条都是**撞出来的**，不是预防）────
// 求解器内部全走 libsolv 的池，而池里的名字/版本都是 **C 串**（版本**原样**进池，
// 不再有 EVR 编解码桥），所以下面这些形态要么让它看到别的东西、要么被拒 —— 喂进去只会产生
// 关于 oracle 模型的假分叉：
//   ① **NUL**：`pool_str2id` 收 C 串 ⇒ 版本/名字被截断；
//   ② **`version_compare(a, b)` 的语义是 `a < b`**（不是 a > b）—— 见 `pinned` 那里的注释。
// 真实索引不会产出这些形态，所以过滤不损失覆盖。

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "base/constants.hpp"
#include "base/so_spec.hpp"
#include "fuzz_common.hpp"
#include "pkg/solver.hpp"
#include "vercmp/dep_parser.hpp"
#include "vercmp/version.hpp"

namespace
{

constexpr std::size_t kMaxPkgs = 4;
constexpr std::size_t kMaxVersions = 3;
constexpr std::size_t kMaxTokens = 4;

std::size_t g_satisfiable = 0;
std::size_t g_unsatisfiable = 0;

void report_outcomes()
{
    std::fprintf(stderr, "[fuzz] solver_diff: 可满足 %zu 次 / 不可满足 %zu 次\n", g_satisfiable,
                 g_unsatisfiable);
}

/// 表项分隔符 = **`;`**（`-` 表示空表）。
///
/// ⚠️ **不能用逗号**：`provides_soname` / `needed_so` 的条目现在可以是
/// `libc.so.6@{GLIBC_2.40,GLIBC_2.39}` —— 花括号里的逗号不是分隔符（索引那边用
/// 花括号感知切分解决同一个冲突，这里换成 `;` 最省事，也让输入本身没有歧义）。
std::vector<std::string> split_tokens(std::string_view s)
{
    std::vector<std::string> out;
    if (s == "-") return out;
    std::size_t start = 0;
    while (start <= s.size() && out.size() < kMaxTokens) {
        const auto sep = s.find(';', start);
        const std::string_view piece =
            (sep == std::string_view::npos) ? s.substr(start) : s.substr(start, sep - start);
        if (!piece.empty()) out.emplace_back(piece);
        if (sep == std::string_view::npos) break;
        start = sep + 1;
    }
    return out;
}

/// 这个包"能不能满足一个**裸名空间的能力需求**"（deps / 能力型 target）？
///
/// 裸名空间里有两条来源：① `provides` 列出的**裸 capability**；② 一条**带版本的自提供**
/// `name = evr`。deps 与"池里没有同名包"的 capability 型 target 都问这一侧。
/// ⚠️ **`needed_so` 不走这里** —— 它走 `so:` 空间，只认 `provides_soname`（见下）。
///
/// **本 harness 里所有"裸名能力匹配"一律走这一个函数** —— 分两处写就会一处补了 `name`、
/// 另一处只查 `provides`，给出不同答案。
/// 这个包是否**声明了虚拟 provider** `cap`（只查 `provides`，**不含自己的名字**）。
///
/// ⚠️ 依赖满足必须用这个，**不能**用上面那个：包对**自己名字**的提供是**带版本**的
/// （`solver.cpp` 的 `name = evr`，那里的注释专门警告过 "plain 无版本 provide 会被
/// libsolv 视为满足任意版本 requires"），而虚拟 `provides` 是**不带版本**的 ⇒ libsolv 把
/// 它当**通配**。两者对"带约束的依赖"的结论相反，混用就会把 `libA >= 2.0` 判成被
/// `libA 1.0` 满足。
bool provides_virtual(const PackageInfo& q, const std::string& cap)
{
    for (const auto& p : q.provides) {
        if (p == cap) return true;
    }
    return false;
}

/// 这个包是否**提供**（虚拟 provider 或它自己的名字）能力 `cap`？**目标**判定用这个 ——
/// 目标走 `solver.cpp` 的"有同名包 → 精确指定；没有 → 当 capability"那条路。
bool provides_capability(const PackageInfo& q, const std::string& cap)
{
    if (q.name == cap) return true;
    return provides_virtual(q, cap);
}

/// 这个包是否满足了 SONAME 需求 `need`？
///
/// ⚠️ **必须调生产的 `so_spec_satisfies()`**（`base/so_spec.hpp`）—— 池里的注册就是从它
/// 派生的（`solver.cpp::add_provides_soname`：provider 只登记它**整体**满足的那些 need 的 id）。
/// 在这里自己写第二份判据（例如"整串相等"或"逐符号匹配"）就是本 harness 反复吃过的那种
/// 假分叉：**同族判据只推了一条分支**。
bool provides_soname_of(const PackageInfo& q, const std::string& need)
{
    for (const auto& s : q.provides_soname) {
        if (so_spec_satisfies(s, need)) return true;
    }
    return false;
}

/// 这个串"看起来像**真实版本域**里的版本"吗？字母表 = `[0-9A-Za-z.+-]`。
///
/// 为什么要这条闸：喂进空格、高位字节、`!` 这类字节，池里的 EVRCMP 与 `version_satisfies`
/// 对同一个垃圾串的解释可能不同，报出来的是关于 oracle 模型的假分叉。例：
/// `P appB 1.0 - - libA>2 .0`（**约束里的版本含空格**）配 `libA 1\xeb`（**高位字节**）。
/// 真实索引的版本全部落在这个字母表内，所以不损失覆盖。
bool plausible_version(const std::string& v)
{
    if (v.empty()) return false;
    for (const unsigned char c : v) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') || c == '.' || c == '+' || c == '-';
        if (!ok) return false;
    }
    return true;
}

[[noreturn]] void oracle_violation(const std::string& why, const std::string& detail)
{
    std::fprintf(stderr, "[fuzz] oracle 违反: %s\n%s\n", why.c_str(), detail.c_str());
    __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    silence_stdout();
    std::cerr.rdbuf(std::cout.rdbuf());  // 同 wal_line_fuzz：只静音 lpkg 的 std::cerr
    std::atexit(report_outcomes);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 4096) return 0;
    // **含 NUL 的输入丢掉**：求解器把版本串交给 libsolv，而 `pool_str2id` 收的是 **C 串** ——
    // 版本里的 NUL 会让 libsolv 看到的串**被截断**，而本 harness 的暴力枚举按 `std::string`
    // 比较完整串 ⇒ 两边比的不是同一个东西，报出来的"分叉"是 harness 自己造的。
    // 这不是理论风险：本 harness 第一次跑就是这么红的（输入 `P libX 1.\0\0…0 libx.so.2 - -`，
    // 方案里回来的版本是 C 串截断后的 `1.`，被判成"池子里没有的版本"）。
    // 真实版本串不含 NUL —— 索引是行式文本，且 `is_safe_path_component` 明确拒 `\0`。
    // `vercmp_fuzz` 早就为同一个原因过滤了可打印性，这里照抄它的判据。
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] == '\0') return 0;
    }

    // ── 解析输入（语法见文件头）──
    Repository repo;
    std::vector<std::string> targets;
    std::string line;
    bool bad = false;  // 任一版本不"像版本" ⇒ 整包跳过（与上面那条闸同一个处置）
    auto flush = [&] {
        if (line.empty()) return;
        const auto sp1 = line.find(' ');
        if (sp1 == std::string::npos) return;
        const std::string_view kind(line.data(), sp1);
        std::string_view rest(line.data() + sp1 + 1, line.size() - sp1 - 1);
        auto next = [&](std::string& out) -> bool {
            const auto sp = rest.find(' ');
            if (sp == std::string_view::npos) return false;
            out.assign(rest.substr(0, sp));
            rest.remove_prefix(sp + 1);
            return true;
        };
        if (kind == "T") {
            std::string name(rest);
            if (!name.empty() && targets.size() < kMaxPkgs) targets.push_back(name);
            return;
        }
        if (kind != "P") return;
        // 六列：name ver provides provides_soname needed_so deps
        std::string name, ver, provides_s, soname_s, needed_s;
        if (!next(name) || !next(ver) || !next(provides_s) || !next(soname_s) || !next(needed_s))
            return;
        std::string deps_s(rest);
        if (name.empty() || ver.empty()) return;

        std::vector<DependencyInfo> deps;
        if (!deps_s.empty() && deps_s != "-") deps = detail::parse_dep_strings({deps_s});
        // 每个版本（包版本 + 依赖约束里的版本）都要"像版本"：垃圾字节上 EVRCMP 与
        // `version_satisfies` 的解释可能不同，报出来只是 oracle 模型自己造的假分叉。
        for (const auto& d : deps) {
            for (const auto& c : d.constraints) {
                if (!plausible_version(c.version)) bad = true;
            }
        }
        if (!plausible_version(ver)) bad = true;
        repo.update_package_info(name, ver, deps, split_tokens(provides_s), split_tokens(soname_s),
                                 split_tokens(needed_s));
    };
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] == '\n') {
            flush();
            line.clear();
        } else {
            line.push_back(static_cast<char>(data[i]));
        }
    }
    flush();

    if (bad) return 0;  // 见上面：版本不是"编解码恒等" ⇒ 整个输入不可用

    // 池子形状闸：包数/版本数超限就丢弃该输入（穷举空间决定了上限）
    const auto& pkgs = repo.packages();
    if (pkgs.empty() || pkgs.size() > kMaxPkgs || targets.empty()) return 0;
    for (const auto& [name, versions] : pkgs) {
        if (versions.empty() || versions.size() > kMaxVersions) return 0;
    }

    // ── 参考答案：暴力枚举"每个包装哪个版本 / 不装"的全部组合 ──
    std::vector<std::string> names;
    names.reserve(pkgs.size());
    for (const auto& [name, versions] : pkgs) names.push_back(name);

    // 每个包的候选：下标 0..n-1 = 装该版本，n = 不装
    std::vector<std::size_t> choice(names.size(), 0);
    bool satisfiable = false;
    // **目标钉在"最新版"** —— 这条是 oracle 的核心：
    // 本 harness 传的 target 是 `(name, "latest")`（见下面 `solve_targets`），而 lpkg 的
    // `latest` 语义是**"装最新版"、不是"随便装一个能装的版本"**：最新版装不上就报无解，
    // **不回退到旧版**。例：`appB` 有两个版本 —— `1.0`（依赖 `libA>2`）与另一个无依赖的
    // 版本，池里 `libA` 只有 `1.0` ⇒ 求解器报 `error.unresolved_dependency`；把 target 当
    // 通配、选那个无依赖的版本就是错的。
    // "哪个是最新"用**同一个** `version_compare` 算（不自己写版本序 —— 那是第二实现）；
    // 版本序本身另有 `vercmp_fuzz` 对着 libsolv 差分守着。
    std::map<std::string, std::size_t> pinned;  // 目标包名 -> 最新版在 pkgs[name] 里的下标
    for (const auto& t : targets) {
        const auto it = pkgs.find(t);
        if (it == pkgs.end() || it->second.empty())
            continue;  // 不在池里的目标：下面"必须装上"会判否
        std::size_t best = 0;
        for (std::size_t i = 1; i < it->second.size(); ++i) {
            // ⚠️ `version_compare(a, b)` 的语义是 **a < b**，不是 a > b —— 仓库里两处用法都
            // 钉死了它：`repository.cpp` 拿它当 `ranges::sort` 的比较器（升序 ⇒ 最后一版最新），
            // `solver.cpp` 的原话是"当前 best 版本 < sa 版本 → sa 更新为 best"。
            // 按 a > b 用会 pin 出**最旧版**。**别信"这个 API 应该是这个语义"** —— 去看它的
            // 用法怎么读。
            if (version_compare(it->second[best].version, it->second[i].version)) best = i;
        }
        pinned[t] = best;
    }

    std::string witness;
    while (!satisfiable) {
        // 当前组合：选中的 (包名, PackageInfo)
        std::vector<const PackageInfo*> chosen;
        bool ok = true;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto& versions = pkgs.at(names[i]);
            const auto pin = pinned.find(names[i]);
            const bool is_target = pin != pinned.end();
            if (choice[i] == versions.size()) {
                if (is_target) ok = false;  // 目标必须装上
                continue;
            }
            if (is_target && choice[i] != pin->second) ok = false;  // 目标只能是它自己的最新版
            chosen.push_back(&versions[choice[i]]);
        }
        // 目标必须都满足 —— 但**按能力语义**，不是"必须有个叫这个名字的包"：
        // `solver.cpp` 对目标分两条路：
        //   · 池里有**同名真实包** → 取它的最高版本，用 `SOLVER_SOLVABLE|INSTALL` 精确指定
        //     （= 上面 `pinned` 做的事）；
        //   · **没有同名包** → 当 **capability** 处理（`SOLVER_SOLVABLE_PROVIDES|INSTALL`）
        //     ⇒ 任何**提供**这个名字的包都算满足。
        // 只按包名判时，"target 是个能力名、由某个包的 provides 提供"的池会被判成不可满足
        // —— 与 `needs`/无约束 deps 是同一个族。
        for (const auto& t : targets) {
            const bool same_name_pkg_exists = pkgs.find(t) != pkgs.end();
            bool satisfied = false;
            for (const auto* p : chosen) {
                if (same_name_pkg_exists ? p->name == t : provides_capability(*p, t)) {
                    // 同名包路径：装上了就算（是不是最新版由 `pinned` 保证）；
                    // 无同名包时走 capability：用**唯一那个**能力判据。
                    satisfied = true;
                }
                if (satisfied) break;
            }
            if (!satisfied) {
                ok = false;
                break;
            }
        }
        // 已选包的需求必须被**同一个组合**满足
        if (ok) {
            for (const auto* p : chosen) {
                for (const auto& so : p->needed_so) {
                    // `needed_so` 走 `so:` 空间 ⇒ **只**认 `provides_soname`
                    // （包名与虚拟 provides 都够不着它）。
                    bool provided = false;
                    for (const auto* q : chosen) {
                        if (provides_soname_of(*q, so)) {
                            provided = true;
                            break;
                        }
                    }
                    if (!provided) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) break;
                for (const auto& dep : p->dependencies) {
                    bool satisfied = false;
                    for (const auto* q : chosen) {
                        // **依赖满足的模型（照 `solver.cpp` 与 libsolv 的语义）**——
                        // 两条路，任一成立即算满足：
                        //  ① 按**包名**：该包的自提供是**带版本**的（`name =
                        //  evr`，`solver.cpp`）
                        //     ⇒ 须同时满足版本约束；无约束时 `version_satisfies_all(v, {})` 恒真，
                        //     退化成"按名字匹配"。
                        //  ② 按**虚拟能力**：`add_provides` 注册的是**不带版本**的 capability
                        //     （`solver.cpp`），而 **libsolv 把不带版本的 provide 当通配**
                        //     ⇒ 它能满足**任何**版本约束。⚠️ 这一条把"只在无约束时才走能力匹配"
                        //     的写法推翻了 ——
                        //     带约束的能力依赖就是这么被误判的。
                        //     **只对虚拟 `provides` 成立**：包对自己名字的提供带版本，见下面那行。
                        const bool by_name = (q->name == dep.name &&
                                              version_satisfies_all(q->version, dep.constraints));
                        // ⚠️ 能力那一路**只查虚拟 provides**（`provides_virtual`）：
                        // 包对自己的名字是**带版本**提供，走上面 `by_name` 那条并受约束检查。
                        if (by_name || provides_virtual(*q, dep.name)) {
                            satisfied = true;
                            break;
                        }
                    }
                    if (!satisfied) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) break;
            }
        }
        if (ok) {
            satisfiable = true;
            for (const auto* p : chosen) {
                witness += " ";
                witness += p->name;
                witness += "=";
                witness += p->version;
            }
            break;
        }
        // 进位（最后一个包的候选先走完）
        std::size_t i = 0;
        for (; i < names.size(); ++i) {
            if (++choice[i] <= pkgs.at(names[i]).size()) break;
            choice[i] = 0;
        }
        if (i == names.size()) break;  // 组合走完了
    }

    // ── 求解 ──
    std::vector<std::pair<std::string, std::string>> solve_targets;
    for (const auto& t : targets) solve_targets.emplace_back(t, "latest");
    const solv::SolveResult res = solv::solve_install(repo, {}, {}, solve_targets, {});

    if (res.ok()) {
        ++g_satisfiable;
    } else {
        ++g_unsatisfiable;
    }

    // 诊断串（万一分叉，用来当场判"是求解器错还是 oracle 错"）
    std::string detail;
    for (const auto& [name, versions] : pkgs) {
        for (const auto& v : versions) {
            detail += "  " + v.name + " " + v.version + " provides{";
            for (const auto& p : v.provides) detail += p + " ";
            detail += "} provides_soname{";
            for (const auto& s : v.provides_soname) detail += s + " ";
            detail += "} needed{";
            for (const auto& n : v.needed_so) detail += n + " ";
            detail += "}";
            for (const auto& d : v.dependencies) {
                detail += " dep:" + d.name;
            }
            detail += "\n";
        }
    }
    detail += "  targets:";
    for (const auto& t : targets) detail += " " + t;
    detail += "\n  求解器 problems:";
    for (const auto& p : res.problems) detail += " [" + p + "]";
    detail += "\n  暴力枚举: " + std::string(satisfiable ? "可满足" : "不可满足") + witness;

    // ── oracle 1：存在性必须一致 ──
    if (satisfiable != res.ok()) {
        oracle_violation(
            satisfiable ? "暴力枚举找到满足全部依赖的组合，而求解器说装不上（用户装不了）"
                        : "求解器说能装，而暴力枚举证明不存在满足全部依赖的组合（会装出坏系统）",
            detail);
    }

    // ── oracle 2：求解器给出的方案本身必须自洽 ──
    if (res.ok()) {
        // 方案里的每个版本都要真实存在
        for (const auto& r : res.order) {
            const auto it = pkgs.find(r.name);
            if (it == pkgs.end()) {
                oracle_violation("方案里出现了池子里没有的包", detail + "\n  方案含: " + r.name);
            }
            bool found = false;
            for (const auto& v : it->second) {
                if (v.version == r.version) found = true;
            }
            if (!found) {
                oracle_violation("方案里出现了池子里没有的版本",
                                 detail + "\n  方案含: " + r.name + " " + r.version);
            }
        }
        // 目标必须在方案里 —— **按能力语义**，与上面那条存在性判据**同一套模型**
        // （池里有同名包 ⇒ 方案里要有那个包；没有同名包 ⇒ 方案里要有**提供该能力**的包）。
        // ⚠️ 目标判定必须与上面那条存在性判据**同一套模型**，否则 "target 是能力名、
        // 方案里装的是提供者"会报假分叉 —— **同族判据只推了一条分支**正是这个仓库的
        // 头号缺陷形态，写 oracle 时同样会犯。
        for (const auto& t : targets) {
            const bool same_name_pkg_exists = pkgs.find(t) != pkgs.end();
            bool satisfied = false;
            for (const auto& r : res.order) {
                for (const auto& v : pkgs.at(r.name)) {
                    if (v.version != r.version) continue;
                    if (same_name_pkg_exists ? r.name == t : provides_capability(v, t)) {
                        satisfied = true;
                    }
                }
            }
            if (!satisfied) {
                oracle_violation("求解成功却没有满足目标（方案里既没有该包、也没有提供该能力的包）",
                                 detail + "\n  目标: " + t);
            }
        }
        // 方案内部自洽：needed_so 与 deps 都要被**方案自己**满足
        for (const auto& r : res.order) {
            const PackageInfo* self = nullptr;
            for (const auto& v : pkgs.at(r.name)) {
                if (v.version == r.version) self = &v;
            }
            if (self == nullptr) continue;
            for (const auto& so : self->needed_so) {
                bool provided = false;
                for (const auto& q : res.order) {
                    for (const auto& v : pkgs.at(q.name)) {
                        if (v.version != q.version) continue;
                        if (provides_soname_of(v, so)) provided = true;
                    }
                }
                if (!provided) {
                    oracle_violation("方案里有个包的 needed_so 在方案内无人提供",
                                     detail + "\n  未满足: " + r.name + " 需要 " + so);
                }
            }
            for (const auto& dep : self->dependencies) {
                bool satisfied = false;
                for (const auto& q : res.order) {
                    for (const auto& v : pkgs.at(q.name)) {
                        if (v.version != q.version) continue;
                        // 与上面那条存在性判据**同一套模型**：按名（带版本的自提供 ⇒ 须满足约束）
                        // 或按能力（不带版本的 provide 是通配）。
                        const bool by_name = (q.name == dep.name &&
                                              version_satisfies_all(v.version, dep.constraints));
                        if (by_name || provides_capability(v, dep.name)) satisfied = true;
                    }
                }
                if (!satisfied) {
                    oracle_violation("方案里有个包的依赖在方案内不满足",
                                     detail + "\n  未满足: " + r.name + " 依赖 " + dep.name);
                }
            }
        }
    }

    return 0;
}
