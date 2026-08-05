#include "solver.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>

// libsolv 的 Solvable 结构有 `requires` 字段，是 C++20 关键字 → 宏改名绕开。
// 我们只用 pool_id2solvable/solvable_add_deparray 等 API，不直接访问 s->requires，改名安全。
#define requires solv_requires
#include <solv/pool.h>
#include <solv/pooltypes.h>
#include <solv/evr.h>
#include <solv/problems.h>
#include <solv/repo.h>
#include <solv/rules.h>
#include <solv/solvable.h>
#include <solv/solver.h>
#include <solv/transaction.h>
#undef requires

#include "../base/constants.hpp"
#include "../i18n/localization.hpp"
#include "../repo/repository.hpp"

// libsolv 内部有全局状态，非线程安全；lpkg 的 install/remove 串行执行，
// 用一把全局锁兜底（未来可拆成 per-pool 独立状态）。
static std::mutex g_solv_mutex;

namespace solv {

namespace {

// lpkg 约束 op → libsolv REL 标志（REL_GT=1, REL_EQ=2, REL_LT=4；GE=EQ|GT, LE=EQ|LT）
// 注：dep_parser 原样保留 "=="（lpkg 的 == 即精确等于），必须与 "=" 同样映射 REL_EQ，
// 否则 "lib == 1.0" 落到 fallback 会变成"不等于"语义，装 2.0 反而满足。
int rel_op(const std::string& op)
{
    if (op == ">=") return REL_EQ | REL_GT;
    if (op == "<=") return REL_EQ | REL_LT;
    if (op == ">") return REL_GT;
    if (op == "<") return REL_LT;
    if (op == "==" || op == "=") return REL_EQ;
    // "!="：不等于 = 大于或小于（libsolv 无独立 REL_NE）。未知 op 兜底同此——
    // 宽松处理避免误判冲突，比卡死更安全。
    return REL_EQ | REL_GT | REL_LT;
}

void add_provides(Solvable* s, Pool* pool, const std::vector<std::string>& provides)
{
    for (const auto& cap : provides) {
        // 裸 provides（无版本）：直接 push 名字 id 即"提供 cap"（匹配任何 requires cap）
        solvable_add_deparray(s, SOLVABLE_PROVIDES, pool_str2id(pool, cap.c_str(), 1), 0);
    }
}

void add_requires(Solvable* s, Pool* pool, const std::vector<DependencyInfo>& deps,
                  const std::vector<std::string>& needed_so)
{
    for (const auto& dep : deps) {
        Id nid = pool_str2id(pool, dep.name.c_str(), 1);
        if (dep.constraints.empty()) {
            solvable_add_deparray(s, SOLVABLE_REQUIRES, nid, 0);
        } else {
            // 复合约束（如 ">=2 <3"）→ 每个约束一个 requires（libsolv 全部满足 = AND）
            for (const auto& c : dep.constraints) {
                Id evr = pool_str2id(pool, c.version.c_str(), 1);
                solvable_add_deparray(s, SOLVABLE_REQUIRES,
                                      pool_rel2id(pool, nid, evr, rel_op(c.op), 1), 0);
            }
        }
    }
    for (const auto& soname : needed_so) {
        solvable_add_deparray(s, SOLVABLE_REQUIRES, pool_str2id(pool, soname.c_str(), 1), 0);
    }
}

// 收集 solve 失败的问题：missing_caps（可容忍的缺 provider）与 fatal（真冲突）
void collect_problems(Solver* solv, Pool* pool, std::vector<std::string>& missing_caps,
                      std::vector<std::string>& fatal)
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
                // 缺 provider → 容忍候选（missing-so-no-error 时注入伪提供者）
                const char* dep_name = dep ? pool_id2str(pool, dep) : "";
                if (dep_name && *dep_name) missing_caps.emplace_back(dep_name);
            } else if (info == SOLVER_RULE_JOB_UNKNOWN_PACKAGE) {
                // 请求的包不存在 → 真错误
                fatal.emplace_back("requested package does not exist");
            } else if (info >= SOLVER_RULE_PKG && info < SOLVER_RULE_JOB) {
                // 真实包级冲突（REQUIRES 版本不符/CONFLICTS/SAME_NAME/OBSOLETES...）
                const char* desc = solver_ruleinfo2str(solv, info, from, to, dep);
                fatal.emplace_back(desc ? desc : "(conflict)");
            }
            // 其余（通用 JOB、UPDATE、DISTUPGRADE 等）→ 缺依赖的症状/结构性，跳过
        }
        queue_free(&rules);
    }
}

struct PoolState {
    Pool* pool = nullptr;
    Repo* avail = nullptr;
    ~PoolState()
    {
        if (pool) pool_free(pool);
    }
};

// 三色 DFS：在剩余子图（未 done 且 indeg>0）中找一条构成环的后向边 (u→v)。
// u 依赖 v（v 在 DFS 栈上同栈即成环）。子图无环返回 false。确定性（节点按名序）。
bool find_cycle_edge(const std::vector<ResolvedPkg>& order,
                     const std::vector<std::vector<size_t>>& edges,
                     const std::vector<int>& indeg, const std::vector<bool>& done,
                     size_t& out_u, size_t& out_v)
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
                if (color[v] == 1) { out_u = u; out_v = v; return true; }
                if (color[v] == 0) { color[v] = 1; st.emplace_back(v, 0); }
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
void order_by_dependencies(Pool* pool, const std::vector<Id>& sids,
                           std::vector<ResolvedPkg>& order)
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
            if (ISRELDEP(req)) continue;              // 带版本约束的命名依赖，不用于 ABI 排序
            const char* rn = pool_id2str(pool, req);
            if (!rn || strstr(rn, ".so") == nullptr) continue;  // 只保留 needed_so（SONAME）
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

PoolState build_pool(const Repository& repo,
                     const std::vector<PackageInfo>& local,
                     const std::map<std::string, InstalledPkg>& installed,
                     const SolveOptions& opts,
                     const std::vector<std::string>& extra_provides)
{
    PoolState ps;
    ps.pool = pool_create();
    // 不设 pool arch：LankeOS 单 arch，solver 不关心 arch。
    // 注意：pool_setarch("x86_64") + arch-less solvable 会让 SOLVER_SOLVABLE_NAME
    // 找不到任何包（whatprovides 按 arch 过滤，全空）——arch 要么都不设要么都设，不能混合。

    // available repo：权威 provider 源（needed_so/provides/deps）
    ps.avail = repo_create(ps.pool, "available");
    for (const auto& [name, versions] : repo.packages()) {
        for (const auto& pkg : versions) {
            Id sid = repo_add_solvable(ps.avail);
            Solvable* s = pool_id2solvable(ps.pool, sid);
            s->name = pool_str2id(ps.pool, pkg.name.c_str(), 1);
            s->evr = pool_str2id(ps.pool, pkg.version.c_str(), 1);
            // 自提供名字必须带版本（provides name = evr）——plain 无版本 provide 会被
            // libsolv 视为满足任意版本 requires（"lib >= 2.0" 会被 lib 1.0 误满足）
            solvable_add_deparray(s, SOLVABLE_PROVIDES,
                                  pool_rel2id(ps.pool, s->name, s->evr, REL_EQ, 1), 0);
            add_provides(s, ps.pool, pkg.provides);
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
        add_requires(s, ps.pool, pkg.deps, pkg.needed_so);
    }

    // --use-system-soname：系统 .so 作为 installed 伪 solvable 的 provides
    // 以及 missing-so 容忍注入的缺 SONAME（都视为"已装满足"）
    if (opts.use_system_soname && !opts.system_sonames.empty()) {
        Id sid = repo_add_solvable(inst);
        Solvable* s = pool_id2solvable(ps.pool, sid);
        s->name = pool_str2id(ps.pool, "@system-sonames", 1);
        add_provides(s, ps.pool, opts.system_sonames);
    }
    if (!extra_provides.empty()) {
        Id sid = repo_add_solvable(inst);
        Solvable* s = pool_id2solvable(ps.pool, sid);
        s->name = pool_str2id(ps.pool, "@missing-tolerated", 1);
        add_provides(s, ps.pool, extra_provides);
    }

    pool_createwhatprovides(ps.pool);
    return ps;
}

}  // namespace

SolveResult solve_install(const Repository& repo,
                          const std::vector<PackageInfo>& local,
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
                Id best = 0;
                int pi;
                Solvable* sa;
                FOR_REPO_SOLVABLES(ps.avail, pi, sa) {
                    if (sa->name != nid) continue;
                    if (!best ||
                        pool_evrcmp(ps.pool, sa->evr, pool_id2solvable(ps.pool, best)->evr,
                                    EVRCMP_COMPARE) > 0)
                        best = pi;
                }
                // 已装版本高于 available 最高版本时不降级（仓库暂缺该新版本）
                Id installed_evr = 0;
                if (Repo* ir = ps.pool->installed) {
                    int ip;
                    Solvable* is;
                    FOR_REPO_SOLVABLES(ir, ip, is)
                        if (is->name == nid) { installed_evr = is->evr; break; }
                }
                if (best &&
                    (!installed_evr ||
                     pool_evrcmp(ps.pool, pool_id2solvable(ps.pool, best)->evr, installed_evr,
                                 EVRCMP_COMPARE) > 0))
                    queue_push2(&jobs, SOLVER_SOLVABLE | SOLVER_INSTALL, best);
                else if (best)
                    queue_push2(&jobs, SOLVER_SOLVABLE_NAME | SOLVER_INSTALL, nid);
                else
                    queue_push2(&jobs, SOLVER_SOLVABLE_PROVIDES | SOLVER_INSTALL, nid);
            } else {
                // 指定版本：找 name+evr 精确匹配的 solvable
                Id evr = pool_str2id(ps.pool, vspec.c_str(), 1);
                Id off = pool_whatprovides(ps.pool, nid);
                Id target_sid = 0;
                for (Id vp = off; vp && (target_sid = ps.pool->whatprovidesdata[vp++]) != 0;) {
                    if (pool_id2solvable(ps.pool, target_sid)->evr == evr) break;
                    target_sid = 0;
                }
                if (target_sid) queue_push2(&jobs, SOLVER_SOLVABLE | SOLVER_INSTALL, target_sid);
            }
        }

        Solver* solv = solver_create(ps.pool);
        int res = solver_solve(solv, &jobs);
        queue_free(&jobs);

        if (res != 0) {
            std::vector<std::string> missing_caps, fatal;
            collect_problems(solv, ps.pool, missing_caps, fatal);

            if (round == 0 && opts.missing_so_no_error && !missing_caps.empty() && fatal.empty()) {
                // 纯缺 SONAME 且容忍 → 注入伪提供者重解
                injected = std::move(missing_caps);
                solver_free(solv);
                continue;
            }
            for (const auto& p : fatal) result.problems.push_back(p);
            for (const auto& c : missing_caps)
                result.problems.push_back(string_format("error.unresolved_soname", c));
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

    // 纯 force_reinstall（全部已装同版本）：solver 无操作，逐目标补回
    if (result.order.empty() && opts.force_reinstall) {
        for (const auto& [name, vspec] : targets) {
            auto it = installed.find(name);
            if (it != installed.end()) {
                result.order.push_back(
                    ResolvedPkg{name, it->second.version, /*is_install=*/false, /*is_explicit=*/true});
            }
        }
    }
    return result;
}

std::set<std::string> repo_revrequires(const Repository& repo, const std::string& target)
{
    std::set<std::string> result;
    // target 被谁依赖：deps 指名 target，或 needed_so 由 target 提供（repo 权威 provider 源）
    for (const auto& [name, versions] : repo.packages()) {
        bool hit = false;
        for (const auto& pkg : versions) {
            for (const auto& dep : pkg.dependencies)
                if (dep.name == target) { hit = true; break; }
            if (hit) break;
            for (const auto& so : pkg.needed_so) {
                auto prov = repo.find_provider(so);
                if (prov && prov->name == target) { hit = true; break; }
            }
            if (hit) break;
        }
        if (hit) result.insert(name);
    }
    return result;
}

}  // namespace solv
