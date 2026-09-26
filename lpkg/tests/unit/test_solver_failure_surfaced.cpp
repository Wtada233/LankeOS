/**
 * test_solver_failure_surfaced.cpp — "求解失败"绝不能被读成"无事可做"
 *
 * 缺口（CLAUDE.md §3「collect_problems 无兜底桶」）：`solver_solve` 的返回值是**问题数**
 * （0 = 成功），而 `collect_problems`（pkg/solver.cpp）把问题按"规则类型"分桶，末尾那类
 * （通用 JOB / DISTUPGRADE / CHOICE / BEST / YUMOBS / BLACK …）是**故意跳过**的。
 * 若 libsolv 报的问题**全部**落在跳过的那几类，`result.problems` 就是空的 →
 * `SolveResult::ok()` 为真 → 上层把"求解失败"读成"求解成功、无事可做"：
 * `lpkg upgrade` 打印"所有包都已是最新版本"并 **exit 0**，磁盘上什么都没变。
 *
 * 本文件两层：
 *  ① 不变量（对**所有**可达的失败类别）：`!ok()` ⟹ `problems` 非空 —— "失败必须可见"。
 *  ② 兜底分支本身（`res != 0` 却一个桶都没认领 → 合成一条 error.solve_failed_undiagnosed）。
 *
 * ⚠️ 关于 ② 的**可达性**：未证实（2026-09-25 静态分析：lpkg 的 pool 只建模 provides +
 * requires，没有 CONFLICTS/OBSOLETES；任何"装不上"的目标最终都由某条 requires 规则触发，
 * 而它的类型落在 PKG_* / JOB_* 桶里；weak 规则（choice 等）参与的问题会被 libsolv 的
 * analyze_unsolvable 直接撤销、根本不会留在 problems 队列里）。所以 ② 是**纵深防御**，
 * 不是已复现的缺陷 —— 本文件只用 ① 钉"失败可见"，并用 l10n 渲染钉住 ② 的消息通路
 * （真实触发要靠 libsolv 版本的规则分类变化，构造不出来就不假装构造）。
 */

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/solver.hpp"
#include "../../main/src/vercmp/dep_parser.hpp"

using namespace solv;

namespace
{
std::string join_problems(const SolveResult& r)
{
    std::string s;
    for (const auto& p : r.problems) s += p + " | ";
    return s;
}
}  // namespace

class SolverFailureSurfacedTest : public ::testing::Test
{
protected:
    Repository repo;
    std::map<std::string, InstalledPkg> installed;

    void SetUp() override
    {
        init_localization();
    }

    void add(const std::string& name, const std::string& ver,
             const std::vector<std::string>& deps = {},
             const std::vector<std::string>& provides = {},
             const std::vector<std::string>& needed_so = {})
    {
        std::vector<DependencyInfo> dep_infos;
        for (const auto& d : deps) {
            DependencyInfo di;
            di.name = d;
            dep_infos.push_back(std::move(di));
        }
        repo.update_package_info(name, ver, dep_infos, provides, needed_so);
    }

    /// 不变量：任何"求解失败"都必须带着**非空**的 problems 回来
    void expect_failure_visible(const SolveResult& r, const char* what)
    {
        if (r.ok()) {
            ADD_FAILURE() << what
                          << "：求解失败却被判成 ok()（problems 空）——上层会把它读成"
                             "「已是最新」并 exit 0";
            return;
        }
        EXPECT_FALSE(r.problems.empty())
            << what << "：!ok() 却没有任何 diagnostic —— " << join_problems(r);
    }
};

// ① 缺失的**命名依赖**（非 SONAME）
TEST_F(SolverFailureSurfacedTest, MissingNamedDepIsVisible)
{
    add("appB", "2.0", {"libgone"}, {}, {});
    expect_failure_visible(solve_install(repo, {}, installed, {{"appB", "latest"}}, {}),
                           "缺失命名依赖");
}

// ① 缺失的 SONAME 提供者（默认不容忍）
TEST_F(SolverFailureSurfacedTest, MissingSonameIsVisible)
{
    add("appB", "2.0", {}, {}, {"libmissing.so.1"});
    expect_failure_visible(solve_install(repo, {}, installed, {{"appB", "latest"}}, {}),
                           "缺失 SONAME");
}

// ① 顶层请求的包/能力不存在
TEST_F(SolverFailureSurfacedTest, MissingTargetIsVisible)
{
    expect_failure_visible(solve_install(repo, {}, installed, {{"ghost", "latest"}}, {}),
                           "顶层目标不存在");
}

// ① 指定版本不在仓库（真包存在但该版本没有）
TEST_F(SolverFailureSurfacedTest, MissingRequestedVersionIsVisible)
{
    add("libA", "1.0", {}, {}, {});
    add("libA", "2.0", {}, {}, {});
    auto r = solve_install(repo, {}, installed, {{"libA", "9.9"}}, {});
    expect_failure_visible(r, "指定版本不存在");
    EXPECT_NE(join_problems(r).find("9.9"), std::string::npos)
        << "报错必须点名请求的版本（拒绝类断言要落锚点）：" << join_problems(r);
}

// ① 升级破坏已装依赖（libsolv 的反向一致性）
TEST_F(SolverFailureSurfacedTest, DowngradeBreakingInstalledIsVisible)
{
    add("lib", "1.0", {}, {"lib.so"}, {});
    add("lib", "2.0", {}, {"lib.so"}, {});
    DependencyInfo dep;
    dep.name = "lib";
    Constraint c;
    c.op = ">=";
    c.version = "2.0";
    dep.constraints.push_back(c);
    installed["app"] = {"1.0", {dep}, {}, {}};
    installed["lib"] = {"2.0", {}, {}, {}};

    expect_failure_visible(solve_install(repo, {}, installed, {{"lib", "1.0"}}, {}),
                           "降级破坏已装依赖");
}

// ① 可容忍的缺 SONAME 也必须**有输出**：注入伪提供者后重解，不允许静默空事务
TEST_F(SolverFailureSurfacedTest, ToleratedMissingSonameStillProducesTheTarget)
{
    add("appB", "2.0", {}, {}, {"libmissing.so.1"});
    SolveOptions opts;
    opts.missing_so_no_error = true;
    auto r = solve_install(repo, {}, installed, {{"appB", "latest"}}, opts);

    ASSERT_TRUE(r.ok()) << "容错模式该成功：" << join_problems(r);
    ASSERT_FALSE(r.order.empty()) << "求解成功却空事务 = 静默什么都没做（这正是 D3/D5 的形态）";
    EXPECT_EQ(r.order[0].name, "appB");
}

// ① 容错模式**不得**把缺失命名依赖一起吞掉
TEST_F(SolverFailureSurfacedTest, ToleratedModeStillFailsOnNamedDep)
{
    add("appB", "2.0", {"libgone"}, {}, {});
    SolveOptions opts;
    opts.missing_so_no_error = true;
    expect_failure_visible(solve_install(repo, {}, installed, {{"appB", "latest"}}, opts),
                           "容错模式下的缺失命名依赖");
}

// ② 兜底消息的**渲染通路**：键存在、占位符被替换（键的存在性由 test_localization_keys.cpp
// 的源码 key 扫描保证；这里钉的是"渲染出来了、不是 [MISSING_STRING: ...] 占位"）
TEST_F(SolverFailureSurfacedTest, UndiagnosedFailureMessageRenders)
{
    const std::string msg = string_format("error.solve_failed_undiagnosed", std::to_string(3));
    EXPECT_NE(msg.find('3'), std::string::npos) << "问题数没渲染进去：" << msg;
    EXPECT_EQ(msg.find('{'), std::string::npos) << "占位符没被替换：" << msg;
    // 缺键时 get_string 返回 "[MISSING_STRING: <key>]"，**含键名** —— 所以不能拿"找得到键名"
    // 当断言（那是恒真的假绿），必须直接查这个哨兵
    EXPECT_EQ(msg.find("[MISSING_STRING"), std::string::npos) << "l10n 缺键：" << msg;
}
