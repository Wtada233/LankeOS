#include "solver.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>

// libsolv 的 Solvable 结构有 `requires` 字段，是 C++20 关键字 → 宏改名绕开。
// 我们只用 pool_id2solvable/solvable_add_deparray 等 API，不直接访问 s->requires，改名安全。
#define requires solv_requires
#include <solv/evr.h>
#include <solv/pool.h>
#include <solv/pooltypes.h>
#include <solv/problems.h>
#include <solv/repo.h>
#include <solv/rules.h>
#include <solv/solvable.h>
#include <solv/solver.h>
#include <solv/transaction.h>
#undef requires

#include "../base/constants.hpp"
#include "../base/so_spec.hpp"
#include "../base/utils.hpp"
#include "../i18n/localization.hpp"
#include "../repo/repository.hpp"
#include "../vercmp/version.hpp"

// libsolv 内部有全局状态，非线程安全；lpkg 的 install/remove 串行执行，
// 用一把全局锁兜底（未来可拆成 per-pool 独立状态）。
static std::mutex g_solv_mutex;

namespace solv
{

namespace
{

// 判定依赖名是否为 SONAME（needed_so）：".so" 后紧跟数字/点或结尾。
//
// ⚠️ **已删除（2026-10-04，维护者拍板）**：这里曾有一个**按字符串形状猜**的
// `looks_like_soname()`。它存在的前提是"`deps` 与 `needed_so` 被压进同一个命名空间"
// —— 于是拿到一个池里的 Id 时，代码**分不清**它来自哪个字段，只能猜形状。而这个启发式
// **本来就不可靠**（它自己的旧注释就记着：`libfoo.so-dev` 这类包名中间含 `.so` 会被误判，
// 于是"缺失的命名依赖"被静默容忍、还多出一条 ABI 排序边）。
//
// **类型信息在源头就有**：`needed_so` 与 `provides` 在 `index.txt` / `metadata.json` 里是
// **两个独立字段**，从来不是"需要猜"的东西 —— 是池化时被压平才丢的。现在
// `add_requires` 把 `needed_so` 的需求登记进 **`so:` 空间**（`POOL_SONAME_PREFIX`），
// 类型随名字回来了：**带前缀 ⟺ 来自 `needed_so` 字段 ⟺ 按定义就是 SONAME**。
// 所以三处判定全部换成下面这个**精确谓词**，形状猜测整个消失。

/**
 * 把 libsolv **自己拼出来的字符串**处理成用户能看的形态。
 *
 * ⚠️ **订正 2026-10-04（8.0.0）——本函数只剩一件事做。** 它此前还要把**编码过的 EVR**
 * 解回 lpkg 版本域（`^^`→`+`、`~`→`-`），因为池里那份是桥的编码串，而用户拿编码串既
 * grep 不到仓库里的版本、也对应不回"到底哪两个版本冲突了"（那时 861 个版本里 807 个带
 * `+`，几乎每条冲突消息都长这样：`cannot install both lib-2.0^^1 and lib-1.0^^1`）。
 * **桥拆掉之后版本串原样进池**，libsolv 打出来的就是 lpkg 自己的版本 ⇒ 那两步不但多余，
 * 而且**有害**（`~` 现在是合法的预发布标记，再把它换成 `-` 会把版本改错）。
 * 现在只剩"剥掉池里的内部前缀"。
 */
std::string decode_libsolv_message(std::string msg)
{
    // `so:` 是**SONAME 空间**的内部标记（见 constants::POOL_SONAME_PREFIX）：它会出现在
    // libsolv 拼的规则描述里（`nothing provides so:libc.so.6 needed by …`），而用户该看到的
    // 是裸的 SONAME。
    //
    // ⚠️ **订正 2026-10-04（8.0.0）**：这里此前还要把**编码过的 EVR** 解回 lpkg 版本域
    // （`^^`→`+`、`~`→`-`），因为池里那份是编码串。桥拆掉之后版本串**原样进池**，
    // libsolv 打出来的就是 lpkg 自己的版本 ⇒ 那两步不但多余，而且**有害**
    // （`~` 现在是合法的预发布标记，再把它换成 `-` 会把版本改错）。
    for (std::size_t pos = msg.find(constants::POOL_SONAME_PREFIX); pos != std::string::npos;
         pos = msg.find(constants::POOL_SONAME_PREFIX, pos))
        msg.erase(pos, constants::POOL_SONAME_PREFIX.size());
    return msg;
}

/**
 * SONAME → 池里的 **`so:` 空间 Id**（见 `constants::POOL_SONAME_PREFIX`）。
 *
 * **凡是与 SONAME 打交道的地方都必须用这个入口**：`provides_soname` 的登记与 `needed_so` 的
 * 需求。直接 `pool_str2id(pool, soname)` 拿到的是**包名/虚拟能力空间**里的 id ——
 * 两个空间不通用，混用就会让 `needed_so: liba.so` 被一个**名叫 `liba.so` 的包**满足
 * （8.0.0 拆分要根除的正是这个）。
 */
Id scoped_soname(Pool* pool, const std::string& soname)
{
    const std::string scoped = std::string(constants::POOL_SONAME_PREFIX) + soname;
    return pool_str2id(pool, scoped.c_str(), 1);
}

/**
 * 池里的名字 → lpkg 的裸名（剥掉 `constants::POOL_SONAME_PREFIX`）。
 *
 * 与 `scoped_soname()` 是**一对**：一个进池、一个出池，各只有这一处实现。
 *
 * ⚠️ **凡是把池里的 requires/provides 名字读回来**用于判定或拼消息的地方都必须过它 ——
 * 否则 `so:` 会漏进用户可见的报错。**改编码就要连"所有读回它的地方"一起数。**
 */
std::string unscoped_soname(const char* pool_name)
{
    std::string s = pool_name ? pool_name : "";
    const std::string_view prefix = constants::POOL_SONAME_PREFIX;
    if (s.size() >= prefix.size() && std::string_view(s).substr(0, prefix.size()) == prefix)
        s.erase(0, prefix.size());
    return s;
}

/**
 * 这个池里的需求名是不是**来自 `needed_so` 字段的那一类**（带 `constants::POOL_SONAME_PREFIX`）？
 *
 * **精确分类，不做任何形状猜测** —— 这就是取代 `looks_like_soname()` 的那个谓词。
 * `so:` 前缀只由 `add_requires()` 的 `needed_so` 那一支产生，所以：
 *   带 `so:` ⟺ 这条需求来自 `needed_so` ⟺ 它按定义就是一个 **SONAME**；
 *   裸名 ⟺ 来自 `deps`（或 capability 型 target）⟺ **包名/虚拟能力语义**
 *   （永远不是 SONAME，哪怕名字里有 `.so`）。
 */
bool is_soname_requirement(const char* pool_name)
{
    if (pool_name == nullptr) return false;
    const std::string_view prefix = constants::POOL_SONAME_PREFIX;
    const std::string_view name = pool_name;
    return name.size() >= prefix.size() && name.substr(0, prefix.size()) == prefix;
}

/// 登记**虚拟 provider**（`provides` 字段）—— 裸名（无版本），与 `deps` 的裸需求同一个命名空间。
void add_provides(Solvable* s, Pool* pool, const std::vector<std::string>& provides)
{
    for (const auto& cap : provides) {
        solvable_add_deparray(s, SOLVABLE_PROVIDES, pool_str2id(pool, cap.c_str(), 1), 0);
    }
}

/**
 * 池里**所有"带符号版本"的 need**（规范化整串，如 `libc.so.6@{GLIBC_2.39,GLIBC_2.40}`）。
 *
 * 它只为回答一个问题：「哪些 need id 需要被登记成 provides」。见下面的 ②。
 */
using SoNeedSet = std::set<std::string>;

/**
 * 登记**导出的 SONAME**（`provides_soname` 字段）—— 走 `so:` 空间，只有 `needed_so` 需求认它。
 *
 * ── 符号版本（symbol version）的两条规则 ─────────────────────────────────────
 * ① 裸 id `so:X` **永远登记**（只要声明了 `X…`）：`needed_so: X` 认它。少了它，一个"声明得
 *    更细"的 provider 反倒会把所有裸 need 打破。
 * ② 带符号版本的条目**只登记它整体满足的那些 need 的 id**（`versioned_needs`）——
 *    **不是**逐个符号登记。`needed_so: X@{A,B}` 的语义是"**某一个** provider 必须同时给出
 *    A 与 B"（ELF 里消费方链的是**一个**库文件）。而 libsolv 的多条 requires 是 AND、却允许
 *    被**不同** solvable 分别满足 —— 逐符号登记会造出"P1 给 A、P2 给 B ⇒ 求解器说能装"而
 *    安装期按单 provider 判**必失败**的分叉。need 侧登记整串 id + provider 侧只登记整体满足
 *    的那些 id，两边就**字面一致**：池里能匹配 ⟺ `so_spec_satisfies()` 成立。
 */
void add_provides_soname(Solvable* s, Pool* pool, const std::vector<std::string>& sonames,
                         const SoNeedSet& versioned_needs)
{
    for (const auto& so : sonames) {
        const SoSpec spec = parse_so_spec(so);
        if (spec.soname.empty()) continue;
        solvable_add_deparray(s, SOLVABLE_PROVIDES, scoped_soname(pool, spec.soname), 0);
        if (spec.symbols.empty()) continue;  // 裸声明：到此为止（保守语义：拿不出符号版本）
        for (const auto& need : versioned_needs) {
            if (so_spec_satisfies(so, need)) {
                solvable_add_deparray(s, SOLVABLE_PROVIDES, scoped_soname(pool, need), 0);
            }
        }
    }
}

void add_requires(Solvable* s, Pool* pool, const std::vector<DependencyInfo>& deps,
                  const std::vector<std::string>& needed_so)
{
    for (const auto& dep : deps) {
        // `deps` 走**能力命名空间**（`cap:`）：于是它既能匹配**包名**（靠包的 `cap:<名> = evr`
        // 自提供），也能匹配**虚拟 provides**（虚拟包语义）。
        const Id nid = pool_str2id(pool, dep.name.c_str(), 1);
        if (dep.constraints.empty()) {
            solvable_add_deparray(s, SOLVABLE_REQUIRES, nid, 0);
        } else {
            // 复合约束（如 ">=2 <3"）→ 每个约束一个 requires（libsolv 全部满足 = AND）。
            // 版本串**原样**进池（8.0.0 起无编码）——`version_satisfies()` 那边走的是同一个
            // libsolv EVR 比较，所以"求解器判的"与"安装期验的"不可能不一致。
            //
            // ⚠️ **订正 2026-10-04**：本行原写"约束版本串要归一化（`-预发布`→`~`），否则
            // EVRCMP 会把 `-` 后当 release 误判（`>= 1.0` 被 `1.0-rc1` 满足）"。那**两半都不再
            // 成立**：归一化已删；而 `1.0-rc1` 在 rpm 语义里就是"1.0 的 release rc1"，
            // `>= 1.0` 匹配它**是正确语义**（未指定 release = 任何 release），不是误判。
            for (const auto& c : dep.constraints) {
                Id evr = pool_str2id(pool, c.version.c_str(), 1);
                solvable_add_deparray(s, SOLVABLE_REQUIRES,
                                      pool_rel2id(pool, nid, evr, version_op_flags(c.op), 1), 0);
            }
        }
    }
    for (const auto& soname : needed_so) {
        // SONAME 需求走**独立的 `so:` 空间**：它只能被 `provides_soname`
        // 满足（`add_provides_soname` 在那边登记），**包名永远进不来**。不加这层隔离时，`needed_so:
        // o` 会被一个
        // **名叫 `o` 的包**满足（靠它的自提供）—— 实测复现过。
        // **带符号版本时登记整串（规范化后）的 id**，不逐符号 —— 与 `add_provides_soname`
        // 的 ② 配套：只有"整体覆盖这个 need"的 provider 才登记这个 id。
        const SoSpec spec = parse_so_spec(soname);
        if (spec.soname.empty()) continue;
        solvable_add_deparray(s, SOLVABLE_REQUIRES, scoped_soname(pool, format_so_spec(spec)), 0);
    }
}

// 收集 solve 失败的问题：
//   missing_so       — nothing-provides 且缺失的 needed_so（SONAME）；
//   soname_conflicts — PKG 冲突形态的缺 SONAME（libsolv 对无提供者的 SONAME requires
//                     有时报 PKG 冲突而非 nothing-provides，如 "qt6-base requires
//                     libgbm.so.1"；dep 名是 SONAME 且全池确无提供者时归入本桶）；
//   missing_dep      — 缺失的命名依赖（**不可容忍**，即使开 missing-so-no-error 也报错）；
//   missing_target   — **用户直接请求**的包/能力无提供者（JOB 规则，报"包未找到"而非"依赖"）；
//   fatal            — 真冲突（版本不符/CONFLICTS/同名等）。
// missing_so 与 soname_conflicts 在 `--missing-so-no-error` 下都可容忍（同一性质：缺
// SONAME 提供者，只是 libsolv 措辞不同）；missing_dep/missing_target/fatal 不可容忍。
// 判定约定：与 order_by_dependencies 相同——requires 名含 ".so" 视为 needed_so（SONAME），
// 否则视为命名依赖。防止 --missing-so-no-error 把缺失命名依赖一起"伪提供"吞掉。
// 区分 JOB vs PKG 规则：JOB_NOTHING_PROVIDES_DEP = 顶层请求无提供者（如 `install foo`
// 而 foo 不在仓库，报"仓库中未找到软件包"）；PKG_NOTHING_PROVIDES_DEP = 传递依赖缺失
// （报"依赖无提供者"）。曾把两者都当依赖报，`install foo` 缺包时错报"依赖 'foo' 无提供者"。
void collect_problems(Solver* solv, Pool* pool, std::vector<std::string>& missing_so,
                      std::vector<std::string>& soname_conflicts,
                      std::vector<std::string>& missing_dep,
                      std::vector<std::string>& missing_target, std::vector<std::string>& fatal)
{
    unsigned int count = solver_problem_count(solv);
    Id problem = 0;
    for (unsigned int i = 0; i < count; ++i) {
        problem = solver_next_problem(solv, problem);
        Queue rules;
        queue_init(&rules);
        solver_findallproblemrules(solv, problem, &rules);
        for (int ri = 0; ri < rules.count; ++ri) {
            Id from = 0, to = 0, dep = 0;
            SolverRuleinfo info = solver_ruleinfo(solv, rules.elements[ri], &from, &to, &dep);
            if (info == SOLVER_RULE_PKG_NOTHING_PROVIDES_DEP ||
                info == SOLVER_RULE_JOB_NOTHING_PROVIDES_DEP) {
                const char* raw_dep = dep ? pool_id2str(pool, dep) : nullptr;
                const std::string dep_name = unscoped_soname(raw_dep);
                if (!dep_name.empty()) {
                    // **先看 JOB/PKG，再看它来自哪个字段**：顶层请求（JOB）是"用户要的包/能力
                    // 不存在"，必须硬报错（此前先判形状 → 形似 SONAME 的顶层目标被归入可容忍的
                    // missing_so，配 --missing-so-no-error 就"求解成功但事务为空"）。
                    if (info == SOLVER_RULE_JOB_NOTHING_PROVIDES_DEP)
                        missing_target.emplace_back(dep_name);  // 直接请求的包/能力
                    else if (is_soname_requirement(raw_dep))
                        missing_so.emplace_back(dep_name);  // 来自 needed_so ⇒ 就是 SONAME
                    else
                        missing_dep.emplace_back(dep_name);  // 来自 deps ⇒ 包名依赖
                }
            } else if (info == SOLVER_RULE_JOB_UNKNOWN_PACKAGE) {
                // 请求的包不存在 → 真错误（走 l10n）。**必须点名包**：libsolv 的 job 规则里
                // `dep` 就是 job 的选择 Id（包名）—— 不带名字的 "does not exist" 让人查不出
                // 是哪个包。注：lpkg 的 job 全用真实 solvable / 名字 id 构造，这条分支当前
                // **不可达**（纵深防御）；一旦 libsolv 改了规则分类，这里也要能定位。
                const std::string unknown = unscoped_soname(dep ? pool_id2str(pool, dep) : nullptr);
                fatal.emplace_back(string_format("error.requested_package_not_exist",
                                                 unknown.empty() ? "?" : unknown));

            } else if ((info & SOLVER_RULE_TYPEMASK) == SOLVER_RULE_PKG) {
                // PKG 规则。判据用**类型掩码**：曾写成
                // `info >= SOLVER_RULE_PKG && info < SOLVER_RULE_JOB`，而 libsolv 的
                // UPDATE(0x200) 与 FEATURE(0x300) 也落在 [0x100,0x400) 这个数值区间里 ——
                // 它们会被当成真冲突报成 fatal，与下面"其余（UPDATE…）跳过"的注释矛盾
                // （2026-10-02 修）。
                // 若 dep 是 SONAME 且全池确无提供者 → libsolv 把它当冲突报
                // （qt6-base requires libgbm.so.1 之类），归 soname_conflicts 供容忍；
                // 否则才是真冲突（版本不符/CONFLICTS/SAME_NAME/OBSOLETES...）。
                const char* raw_dn = dep ? pool_id2str(pool, dep) : nullptr;
                const std::string dn = unscoped_soname(raw_dn);
                if (!dn.empty() && is_soname_requirement(raw_dn)) {
                    const Id* w = pool_whatprovides_ptr(pool, dep);
                    if (!w || !*w) {
                        soname_conflicts.emplace_back(dn);
                        continue;
                    }
                }
                // `desc` 为 null（libsolv 给不出规则描述）时回退到 l10n 的 "(conflict)"。
                // 三元里 const char* 与 std::string 混合，结果类型是 std::string（旧写法直接
                // 塞硬编码英文，且本处是用户可见的冲突原因）。
                // ⚠️ **该回退近不可达**：实测 libsolv 对本处收集的这几类规则都给出了描述，
                // 所以它**没有对应用例 —— 这是有意的，不是漏测**。保留是纵深防御：
                // `solver_ruleinfo2str` 的契约允许返回 NULL，直接解引用会崩。
                const char* desc = solver_ruleinfo2str(solv, info, from, to, dep);
                // `desc` 是 libsolv 拼的串。8.0.0 起池里的版本串**原样**就是 lpkg 版本，
                // 所以只需剥掉内部前缀（见 decode_libsolv_message）。
                fatal.emplace_back(desc ? decode_libsolv_message(desc)
                                        : get_string("info.solver_rule_conflict"));
            }
            // 其余（通用 JOB、UPDATE、DISTUPGRADE 等）→ 缺依赖的症状/结构性，跳过
        }
        queue_free(&rules);
    }
}

/**
 * libsolv Pool 的 RAII 持有者。
 *
 * **拷贝必须禁掉**：它持有裸 `Pool*`，隐式拷贝（用户声明了析构 ⇒ 拷贝构造仍被隐式声明、
 * 只是 deprecated）会造成两个对象各自 `pool_free` 同一个 pool —— double free。此前只靠
 * `build_pool` 的 `return ps;` 走 NRVO/移动语义侥幸不触发（2026-10-02 修）。这里显式
 * 删除拷贝、补上移动（`return ps;` 即使不 NRVO 也走移动）。
 */
struct PoolState {
    Pool* pool = nullptr;
    Repo* avail = nullptr;

    PoolState() = default;
    PoolState(const PoolState&) = delete;
    PoolState& operator=(const PoolState&) = delete;
    PoolState(PoolState&& other) noexcept : pool(other.pool), avail(other.avail)
    {
        other.pool = nullptr;
        other.avail = nullptr;
    }
    PoolState& operator=(PoolState&& other) noexcept
    {
        if (this != &other) {
            if (pool) pool_free(pool);
            pool = other.pool;
            avail = other.avail;
            other.pool = nullptr;
            other.avail = nullptr;
        }
        return *this;
    }
    ~PoolState()
    {
        if (pool) pool_free(pool);
    }
};

// 三色 DFS：在剩余子图（未 done 且 indeg>0）中找一条构成环的后向边 (u→v)。
// u 依赖 v（v 在 DFS 栈上同栈即成环）。子图无环返回 false。确定性（节点按名序）。
bool find_cycle_edge(const std::vector<ResolvedPkg>& order,
                     const std::vector<std::vector<size_t>>& edges, const std::vector<int>& indeg,
                     const std::vector<bool>& done, size_t& out_u, size_t& out_v)
{
    const size_t n = order.size();
    std::vector<size_t> nodes;
    for (size_t i = 0; i < n; ++i)
        if (!done[i] && indeg[i] > 0) nodes.push_back(i);
    if (nodes.empty()) return false;
    std::sort(nodes.begin(), nodes.end(),
              [&](size_t a, size_t b) { return order[a].name < order[b].name; });
    std::vector<char> color(n, 0);  // 0=白 1=灰 2=黑
    for (size_t root : nodes) {
        if (color[root]) continue;
        color[root] = 1;
        std::vector<std::pair<size_t, size_t>> st;  // (node, 邻接索引)
        st.emplace_back(root, 0);
        while (!st.empty()) {
            auto& top = st.back();
            const size_t u = top.first;
            if (top.second < edges[u].size()) {
                const size_t v = edges[u][top.second++];
                if (done[v] || indeg[v] == 0) continue;  // 不在剩余子图
                if (color[v] == 1) {
                    out_u = u;
                    out_v = v;
                    return true;
                }
                if (color[v] == 0) {
                    color[v] = 1;
                    st.emplace_back(v, 0);
                }
            } else {
                color[u] = 2;
                st.pop_back();
            }
        }
    }
    return false;
}

// 安装序：稳定拓扑排序，任何 needed_so 提供者先于依赖者。
// libsolv 的 transaction_order 是启发式，对真实大图会漏排依赖边（bootstrap 里
// job 包 bash 被留在最前、gcc 先于 gmp/mpfr/mpc），不能直接依赖。
//
// 核心参考 farm/build/sched.rs：
// - **只用 needed_so（SONAME/ABI）边**排序，不用命名依赖边。命名依赖图（尤其
//   deps/ 被污染的）含大量循环，断边会让 glibc 这类根掉到中间；needed_so 边代表
//   真实二进制链接顺序，基本无环。判定：裸名（非 REL）且名字含 ".so" 的 requires。
// - 就绪队列按**名字升序**确定性弹出（不依赖原始事务序）。
// - 环用三色 DFS 找一条环边逐条切断（u 依赖 v → 断 u→v），而非任选节点兜底
//   （naive 兜底会把被环阻塞的 bash 这类依赖者提前装掉）。
// sids[i] 与 order[i] 一一对应（sids 是 order[i] 在 pool 里的 solvable id）。
void order_by_dependencies(Pool* pool, const std::vector<Id>& sids, std::vector<ResolvedPkg>& order)
{
    const size_t n = order.size();
    if (n < 2) return;

    std::map<std::string, size_t> idx;
    for (size_t i = 0; i < n; ++i) idx[order[i].name] = i;

    // edges[i] = 必须先于 i 的计划内提供者（needed_so 边；去重）
    std::vector<std::vector<size_t>> edges(n), rev(n);
    std::vector<int> indeg(n, 0);
    for (size_t i = 0; i < n; ++i) {
        Solvable* s = pool_id2solvable(pool, sids[i]);
        Id* data = s->repo ? s->repo->idarraydata : nullptr;
        if (!data) continue;
        for (Offset o = s->solv_requires; data[o]; ++o) {
            Id req = data[o];
            if (ISRELDEP(req)) continue;  // 带版本约束的命名依赖，不用于 ABI 排序
            // 只保留 `needed_so` 的边（= 能力需求）。**精确判据**：带前缀 ⟺ 来自该字段。
            // 此前按名字形状猜（`looks_like_soname`），会把包名里含 `.so` 的**命名依赖**
            // 也造一条 ABI 排序边。
            if (!is_soname_requirement(pool_id2str(pool, req))) continue;
            Id* dp = pool_whatprovides_ptr(pool, req);
            for (; *dp; dp++) {
                Solvable* prov = pool_id2solvable(pool, *dp);
                auto it = idx.find(pool_id2str(pool, prov->name));
                if (it == idx.end() || it->second == i) continue;  // 不在计划 / 自引用
                edges[i].push_back(it->second);
            }
        }
        std::sort(edges[i].begin(), edges[i].end());
        edges[i].erase(std::unique(edges[i].begin(), edges[i].end()), edges[i].end());
        indeg[i] = static_cast<int>(edges[i].size());
        for (size_t e : edges[i]) rev[e].push_back(i);
    }

    // Kahn：就绪队列按名字升序；环切边后继续
    auto name_less = [&](size_t a, size_t b) { return order[a].name < order[b].name; };
    std::set<size_t, decltype(name_less)> ready(name_less);
    std::vector<bool> done(n, false);
    for (size_t i = 0; i < n; ++i)
        if (indeg[i] == 0) ready.insert(i);

    std::vector<size_t> seq;
    while (seq.size() < n) {
        if (ready.empty()) {
            size_t u = 0, v = 0;
            if (!find_cycle_edge(order, edges, indeg, done, u, v)) break;
            edges[u].erase(std::remove(edges[u].begin(), edges[u].end(), v), edges[u].end());
            rev[v].erase(std::remove(rev[v].begin(), rev[v].end(), u), rev[v].end());
            --indeg[u];
            if (indeg[u] == 0) ready.insert(u);
            continue;
        }
        const size_t pick = *ready.begin();
        ready.erase(ready.begin());
        done[pick] = true;
        seq.push_back(pick);
        for (size_t k : rev[pick])
            if (!done[k] && --indeg[k] == 0) ready.insert(k);
    }
    if (seq.size() < n)  // 理论不可达兜底：剩余按名序追加
        for (size_t i = 0; i < n; ++i)
            if (!done[i]) seq.push_back(i);

    std::vector<ResolvedPkg> ordered;
    ordered.reserve(n);
    for (size_t i : seq) ordered.push_back(std::move(order[i]));
    order = std::move(ordered);
}

PoolState build_pool(const Repository& repo, const std::vector<PackageInfo>& local,
                     const std::map<std::string, InstalledPkg>& installed, const SolveOptions& opts,
                     const std::vector<std::string>& extra_provides)
{
    PoolState ps;
    ps.pool = pool_create();

    // ── 预扫：池里所有"带符号版本"的 need（见 `add_provides_soname` 的 ②）──────────────
    // 三处来源都要扫：avail（候选）/ local（本地 .lpkg）/ installed（**已装消费者** ——
    // "已装的老二进制要求某符号版本、而仓库里的 provider 得能覆盖它"正是要抓的那类）。
    // 成本与**真实出现的 distinct need 数**成正比：今天全是裸 need ⇒ 集合为空、后面那段
    // 循环一次都不跑。
    SoNeedSet versioned_needs;
    const auto collect_needs = [&versioned_needs](const std::vector<std::string>& needed) {
        for (const auto& raw : needed) {
            const SoSpec spec = parse_so_spec(raw);
            if (!spec.symbols.empty()) versioned_needs.insert(format_so_spec(spec));
        }
    };
    // 不设 pool arch：LankeOS 单 arch，solver 不关心 arch。
    // 注意：pool_setarch("x86_64") + arch-less solvable 会让 SOLVER_SOLVABLE_NAME
    // 找不到任何包（whatprovides 按 arch 过滤，全空）——arch 要么都不设要么都设，不能混合。

    // available repo：权威 provider 源（needed_so/provides/deps）
    ps.avail = repo_create(ps.pool, "available");
    for (const auto& [name, versions] : repo.packages()) {
        for (const auto& pkg : versions) collect_needs(pkg.needed_so);
    }
    for (const auto& pkg : local) collect_needs(pkg.needed_so);
    for (const auto& [name, pkg] : installed) collect_needs(pkg.needed_so);

    for (const auto& [name, versions] : repo.packages()) {
        for (const auto& pkg : versions) {
            Id sid = repo_add_solvable(ps.avail);
            Solvable* s = pool_id2solvable(ps.pool, sid);
            s->name = pool_str2id(ps.pool, pkg.name.c_str(), 1);
            s->evr = pool_str2id(ps.pool, pkg.version.c_str(), 1);
            // 自提供必须**带版本**（`<名> = evr`）—— plain 无版本 provide 会被 libsolv
            // 视为满足任意版本 requires（"lib >= 2.0" 会被 lib 1.0 误满足）。
            // ⚠️ 名字走**能力命名空间**（`cap:<包名>`）：`deps` 的需求也在那一侧，于是
            // "按包名依赖"照旧成立；而 `needed_so` 在 `so:` 那侧，包名**够不着**它。
            solvable_add_deparray(s, SOLVABLE_PROVIDES,
                                  pool_rel2id(ps.pool, s->name, s->evr, REL_EQ, 1), 0);
            add_provides(s, ps.pool, pkg.provides);
            add_provides_soname(s, ps.pool, pkg.provides_soname, versioned_needs);
            // --no-deps：不建模候选包的 requires → solver 不会拉依赖（只装目标自身）。
            // installed repo 的 requires 仍保留（"不破坏已装依赖"的一致性照旧）。
            if (!opts.no_deps) add_requires(s, ps.pool, pkg.dependencies, pkg.needed_so);
        }
    }
    // 本地候选包（.lpkg 元数据）也进 available repo
    for (const auto& pkg : local) {
        Id sid = repo_add_solvable(ps.avail);
        Solvable* s = pool_id2solvable(ps.pool, sid);
        s->name = pool_str2id(ps.pool, pkg.name.c_str(), 1);
        s->evr = pool_str2id(ps.pool, pkg.version.c_str(), 1);
        solvable_add_deparray(s, SOLVABLE_PROVIDES,
                              pool_rel2id(ps.pool, s->name, s->evr, REL_EQ, 1), 0);
        add_provides(s, ps.pool, pkg.provides);
        add_provides_soname(s, ps.pool, pkg.provides_soname, versioned_needs);
        if (!opts.no_deps) add_requires(s, ps.pool, pkg.dependencies, pkg.needed_so);
    }

    // installed repo：已装包。requires 建模 deps + needed_so，provides 建模
    // capabilities（dontfix 反向一致性需要"已装 provider"存在才算健康依赖）。
    // 取代旧的手动校验（check_plan_consistency 等）。
    Repo* inst = repo_create(ps.pool, "installed");
    ps.pool->installed = inst;
    for (const auto& [name, pkg] : installed) {
        Id sid = repo_add_solvable(inst);
        Solvable* s = pool_id2solvable(ps.pool, sid);
        s->name = pool_str2id(ps.pool, name.c_str(), 1);
        s->evr = pool_str2id(ps.pool, pkg.version.c_str(), 1);
        solvable_add_deparray(s, SOLVABLE_PROVIDES,
                              pool_rel2id(ps.pool, s->name, s->evr, REL_EQ, 1), 0);
        add_provides(s, ps.pool, pkg.provides);
        add_provides_soname(s, ps.pool, pkg.provides_soname, versioned_needs);
        add_requires(s, ps.pool, pkg.deps, pkg.needed_so);
    }

    // --use-system-soname：系统 .so 作为 installed 伪 solvable 的 provides
    // 以及 missing-so 容忍注入的缺 SONAME（都视为"已装满足"）
    if (opts.use_system_soname && !opts.system_sonames.empty()) {
        Id sid = repo_add_solvable(inst);
        Solvable* s = pool_id2solvable(ps.pool, sid);
        s->name = pool_str2id(ps.pool, "@system-sonames", 1);
        add_provides_soname(s, ps.pool, opts.system_sonames, versioned_needs);
    }
    if (!extra_provides.empty()) {
        Id sid = repo_add_solvable(inst);
        Solvable* s = pool_id2solvable(ps.pool, sid);
        s->name = pool_str2id(ps.pool, "@missing-tolerated", 1);
        add_provides_soname(s, ps.pool, extra_provides, versioned_needs);
    }

    pool_createwhatprovides(ps.pool);
    return ps;
}

}  // namespace

SolveResult solve_install(const Repository& repo, const std::vector<PackageInfo>& local,
                          const std::map<std::string, InstalledPkg>& installed,
                          const std::vector<std::pair<std::string, std::string>>& targets,
                          const SolveOptions& opts)
{
    std::lock_guard<std::mutex> lock(g_solv_mutex);
    SolveResult result;

    std::vector<std::string> injected;  // 容忍模式注入的缺 SONAME
    const int rounds = opts.missing_so_no_error ? 2 : 1;
    for (int round = 0; round < rounds; ++round) {
        PoolState ps = build_pool(repo, local, installed, opts, injected);

        Queue jobs;
        queue_init(&jobs);
        for (const auto& [name, vspec] : targets) {
            Id nid = pool_str2id(ps.pool, name.c_str(), 1);
            if (vspec == std::string(constants::VER_LATEST)) {
                // "装到最新可用版本"。SOLVER_SOLVABLE_NAME|INSTALL 只"确保已装"，
                // 已装时不升级（空 transaction）；加 SOLVER_UPDATE 又会让未装包变空操作。
                // 正解：有同名真实包时找 available 最高版本 solvable，用
                // SOLVER_SOLVABLE|INSTALL 精确指定（新装=装它，已装且更高=升级到它）；
                // 无同名包时当 capability 处理（SOLVER_SOLVABLE_PROVIDES|INSTALL——
                // 已装满足则 no-op，否则装 provider，缺则报错）。
                // "最新"用 `version_compare()` 选。
                // ⚠️ **订正 2026-10-04（8.0.0）**：这条注释此前写着"必须用 lpkg 的版本语义
                // （version_compare），**不能**用 libsolv 的 EVRCMP —— 两者对预发布判序相反
                // （`1.0-rc1` vs `1.0`）"。**那个前提已经不存在**：桥拆掉之后
                // `version_compare()` 的实现**就是** libsolv 的 EVR 比较，两边是同一个判据，
                // 不可能"选错版本/升不到稳定版"（原回归 S2 的场景随桥一起消失）。
                Id best = 0;
                int pi;
                Solvable* sa;
                FOR_REPO_SOLVABLES(ps.avail, pi, sa)
                {
                    if (sa->name != nid) continue;
                    // 版本串**原样**进池（8.0.0 起不再有编码），所以直接拿池里的 EVR 去比即可。
                    if (!best ||
                        version_compare(pool_id2str(ps.pool, pool_id2solvable(ps.pool, best)->evr),
                                        pool_id2str(ps.pool, sa->evr)))
                        best = pi;  // 当前 best 版本 < sa 版本 → sa 更新为 best
                }
                // 已装版本 >= available 最高版本时不降级（仓库暂缺该新版本 / 已最新）
                std::string installed_ver;
                if (Repo* ir = ps.pool->installed) {
                    int ip;
                    Solvable* is;
                    FOR_REPO_SOLVABLES(ir, ip, is)
                    if (is->name == nid) {
                        installed_ver = pool_id2str(ps.pool, is->evr);
                        break;
                    }
                }
                const std::string best_ver =
                    best ? pool_id2str(ps.pool, pool_id2solvable(ps.pool, best)->evr)
                         : std::string{};
                if (best && (installed_ver.empty() || version_compare(installed_ver, best_ver)))
                    queue_push2(&jobs, SOLVER_SOLVABLE | SOLVER_INSTALL, best);  // 新装 / 升级
                else if (best)
                    queue_push2(&jobs, SOLVER_SOLVABLE_NAME | SOLVER_INSTALL,
                                nid);  // 已装同版/更高 → no-op
                else
                    // capability —— ⚠️ 用**带命名空间前缀**的 id（能力与包名不共用命名空间，
                    // 见 constants::POOL_SONAME_PREFIX）。这里 `nid` 是裸包名，不能直接拿来问
                    // "谁提供这个能力"。
                    queue_push2(&jobs, SOLVER_SOLVABLE_PROVIDES | SOLVER_INSTALL, nid);
            } else {
                // 指定版本（`pkg:版本` / 本地 .lpkg）。libsolv 对"已装 identical"的
                // SOLVER_SOLVABLE|INSTALL 会产出 REINSTALL 步骤，导致非 --force 的
                // 同版本安装也进计划（回归 S1）。这里在 job 层做策略：
                //   已装同版本且非 force → 不发 job（上层报"已安装"）；
                //   真包但版本不在 avail → 明确报错，附可用版本（回归 S3）；
                //   无同名包 → 当 capability 处理（同 latest 分支：装提供者/缺则报错）。
                Id evr = pool_str2id(ps.pool, vspec.c_str(), 1);
                Id target_sid = 0;
                bool name_exists = false;
                std::string avail_versions;
                {
                    int pi2;
                    Solvable* sa2;
                    FOR_REPO_SOLVABLES(ps.avail, pi2, sa2)
                    {
                        if (sa2->name != nid) continue;
                        name_exists = true;
                        if (!avail_versions.empty()) avail_versions += ", ";
                        avail_versions += pool_id2str(ps.pool, sa2->evr);
                        if (sa2->evr == evr) target_sid = pi2;
                    }
                }
                if (target_sid) {
                    bool same_installed = false;
                    if (Repo* ir = ps.pool->installed) {
                        int ip;
                        Solvable* is;
                        FOR_REPO_SOLVABLES(ir, ip, is)
                        if (is->name == nid && is->evr == evr) {
                            same_installed = true;
                            break;
                        }
                    }
                    if (!same_installed || opts.force_reinstall)
                        queue_push2(&jobs, SOLVER_SOLVABLE | SOLVER_INSTALL, target_sid);
                } else if (name_exists) {
                    // 真包存在但指定版本不在 avail → 报错，附可用版本（不再静默"已安装"）
                    result.problems.push_back(string_format("error.package_version_not_found", name,
                                                            vspec, avail_versions));
                } else {
                    // 无同名真实包 → capability 回退：提供者由 libsolv 选（同 latest 分支），
                    // 无提供者时产生 SOLVER_RULE_JOB_NOTHING_PROVIDES_DEP → collect_problems 报错。
                    // 指定版本对能力无意义——提示找不到同名包、忽略版本约束，仍装提供者。
                    log_warning(string_format("warning.capability_version_ignored", name, vspec));
                    queue_push2(&jobs, SOLVER_SOLVABLE_PROVIDES | SOLVER_INSTALL, nid);
                }
            }
        }

        if (!result.problems.empty()) {
            queue_free(&jobs);
            return result;  // S3：真包指定版本不存在 → 直接报错，不再静默"已安装"
        }

        Solver* solv = solver_create(ps.pool);
        int res = solver_solve(solv, &jobs);
        queue_free(&jobs);

        if (res != 0) {
            std::vector<std::string> missing_so, soname_conflicts, missing_dep, missing_target,
                fatal;
            collect_problems(solv, ps.pool, missing_so, soname_conflicts, missing_dep,
                             missing_target, fatal);

            if (round == 0 && opts.missing_so_no_error && missing_dep.empty() &&
                missing_target.empty()) {
                // --missing-so-no-error 容忍（round0 注入一次后重解）：
                // 注入 = nothing-provides 缺 SONAME（missing_so）∪ PKG 冲突形态的缺
                // SONAME（soname_conflicts，libsolv 措辞不同、性质相同）。命名依赖/顶层
                // 请求缺失仍不可容忍（回归 M1）。真冲突不进这两个桶 → 注入集空 → 落到
                // 下方照常报错；若某 fatal 实为缺 SONAME 引起，注入后 round1 重解即消解。
                std::vector<std::string> inject = missing_so;
                for (const auto& c : soname_conflicts) {
                    if (std::find(inject.begin(), inject.end(), c) == inject.end())
                        inject.push_back(c);
                }
                if (!inject.empty()) {
                    injected = std::move(inject);
                    solver_free(solv);
                    continue;
                }
            }
            for (const auto& p : fatal) result.problems.push_back(p);
            // 顶层请求的包/能力无提供者 → "仓库中未找到软件包"，不是"依赖"
            for (const auto& t : missing_target)
                result.problems.push_back(string_format("error.package_not_in_repo", t));
            for (const auto& d : missing_dep)
                result.problems.push_back(string_format("error.unresolved_dependency", d));
            for (const auto& c : missing_so)
                result.problems.push_back(string_format("error.unresolved_soname", c));
            for (const auto& c : soname_conflicts)
                result.problems.push_back(string_format("error.unresolved_soname", c));

            // ── 兜底桶：libsolv 报了问题，却**没有任何桶认领** ────────────────────────
            //
            // `solver_solve` 的返回值是**问题数**（0 = 求解成功），所以 `res != 0` 意味着
            // libsolv 确实有一个它无法满足的约束。而本函数把问题按"规则类型"分桶，
            // 末尾那段 `else`（通用 JOB、DISTUPGRADE、CHOICE、BEST、YUMOBS、BLACK …）
            // 是**故意跳过**的：那些规则在 lpkg 的 pool 里不该成为最终冲突（可容忍/结构性）。
            //
            // 但"跳过"不能变成"什么都不报"：`result.problems` 空 = `ok()` = 上层把它读成
            // **求解成功、无事可做** —— `upgrade` 会打印"所有包都已是最新版本"并以 0 退出，
            // 而磁盘上什么都没变。这是把"失败"伪装成"成功"的最恶劣形态（脚本无法区分）。
            // 所以此处兜底：诊断不出具体原因也要**报一次失败**，绝不放行。
            //
            // 可达性：**未证实**（2026-09-25 静态分析：lpkg 的 pool 只建模 provides +
            // requires，没有 CONFLICTS/OBSOLETES；任何装不上的目标最终都由某条 requires 规则
            // 触发，而它的类型落在 PKG_* / JOB_* 桶里；weak 规则（choice 等）参与的问题会被
            // libsolv 的 analyze_unsolvable 直接撤销、不会留在 problems 队列里）。
            // 这是**纵深防御**：不在上面加桶的那天起，这段就只是把"漏报"钉成"必报"。
            if (result.problems.empty()) {
                result.problems.push_back(
                    string_format("error.solve_failed_undiagnosed", std::to_string(res)));
            }
            solver_free(solv);
            return result;
        }

        Transaction* trans = solver_create_transaction(solv);
        // 不用 transaction_order：libsolv 的启发式对真实大图会漏排依赖边
        // （bootstrap 里 job 包 bash 被留在最前、gcc 先于 gmp/mpfr/mpc）。
        // 只取原始步骤，随后自己做稳定拓扑排序（order_by_dependencies）。
        std::vector<Id> order_sids;
        for (int i = 0; i < trans->steps.count; ++i) {
            Id step = trans->steps.elements[i];
            Solvable* s = pool_id2solvable(ps.pool, step);
            // 旧包（被替换/删除，repo==installed）跳过：升级时 libsolv 会同时产出
            // 旧包（UPGRADED/DOWNGRADED/OBSOLETED/ERASE）与新包（UPGRADE/INSTALL）
            // 两个 step，旧包必须忽略，否则幽灵"app 1.0"进 order 破坏升级计划。
            if (s->repo == ps.pool->installed) continue;
            int type = transaction_type(trans, step, 0);
            if (type == SOLVER_TRANSACTION_ERASE) continue;  // 兜底
            ResolvedPkg r;
            r.name = pool_id2str(ps.pool, s->name);
            // pool 内 evr **原样**就是 lpkg 版本（8.0.0 起无编码），直接读回即可
            r.version = pool_id2str(ps.pool, s->evr);
            r.is_install = (type == SOLVER_TRANSACTION_INSTALL);
            result.order.push_back(std::move(r));
            order_sids.push_back(step);
        }
        transaction_free(trans);
        order_by_dependencies(ps.pool, order_sids, result.order);
        solver_free(solv);
        break;
    }

    // --force 的"已是最新版本"目标：同版本无需安装，libsolv 不会为它们产生事务步骤，
    // 必须逐目标补回。**不能只在 result.order 为空时补**——`install --force A B`
    // （A 已当前版本、B 新装）会因为 B 让 order 非空而静默漏掉 A（历史 TODO.md E1）。
    if (opts.force_reinstall) {
        std::set<std::string> in_order;
        for (const auto& r : result.order) in_order.insert(r.name);
        for (const auto& [name, vspec] : targets) {
            if (in_order.contains(name)) continue;  // 已在事务里（升级/新装）→ 不重复补
            auto it = installed.find(name);
            if (it == installed.end()) continue;  // 未安装 → 由 solver 负责
            result.order.push_back(ResolvedPkg{name, it->second.version, /*is_install=*/false,
                                               /*is_explicit=*/true});
            in_order.insert(name);
        }
    }
    return result;
}

}  // namespace solv
