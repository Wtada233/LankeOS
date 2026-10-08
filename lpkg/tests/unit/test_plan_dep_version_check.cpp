/**
 * test_plan_dep_version_check.cpp — **计划版本复核**（`detail::check_planned_dep_version`）
 *
 * 判据：带约束的依赖若出现在**计划**里，其计划版本必须满足约束（按 **lpkg 语义**）。
 * 它是"批次结束后生效的是计划版本，不是盘上那份"这条不变量的落点。
 *
 * **为什么这个文件直接喂手搓的计划**（而不是让安装流程触发它）：在把版本语义换成
 * rpm EVR 之前（见 `vercmp/version.hpp`），libsolv 的 EVR 匹配会把"要求侧缺
 * release"当通配、选出 lpkg 认为不满足的版本 —— 那时这道复核是唯一能拦住的地方，用例只能借
 * "求解器产出违规计划"触达。换成 rpm EVR 之后求解器与安装期共用同一份判据、不再产出这种计划，
 * **那条路已经走不到**，
 * 再照着它写端到端用例就是假绿（分支根本不会走到，而用例照样绿）。
 * 判据本身仍然值得留（纵深防御：求解器选版本与 lpkg 判据是两条独立实现路径），
 * 所以在这里**直接**喂它一组构造出来的计划，把每一格钉死。
 */

#include <gtest/gtest.h>

#include <map>
#include <string>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/vercmp/dep_parser.hpp"

/**
 * 用例基类：**必须自己 `init_localization()`**。
 * 本套件断言的是 `error.dep_version_mismatch` 的**渲染结果**（点名依赖/版本/包），而 l10n 表
 * 是进程级惰性初始化 —— 不初始化就会拿到 `[MISSING_STRING: …]`，断言全红。
 * 它曾经只在"全量跑"时是绿的（别的套件先初始化过），**过滤单跑就红**：
 * 那正是"单跑红、全量绿"的顺序依赖，别靠别人替你把环境铺好。
 */
class PlanDepVersionCheckTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        init_localization();
    }
};

namespace
{

/// 造一个"计划里已解析出该依赖、版本是 planned"的场景。
InstallPlan plan_with(const std::string& name, const std::string& planned_version)
{
    InstallPlan p;
    p.name = name;
    p.actual_version = planned_version;
    return p;
}

/// 解析一条依赖串（`parse_dep_strings` 是唯一实现，别在这里手拼 Constraint）。
DependencyInfo dep_of(const std::string& dep_string)
{
    const auto parsed = detail::parse_dep_strings({dep_string});
    EXPECT_EQ(parsed.size(), 1u) << dep_string;
    return parsed.front();
}

/// 断言"被拒，且报错点名了依赖、计划版本、发起包"（本仓库的拒绝类断言要有多个锚点）。
void expect_refused(const std::string& dep_string, const std::string& planned,
                    const std::string& requester)
{
    const std::map<std::string, InstallPlan> plan{{"lib", plan_with("lib", planned)}};
    try {
        detail::check_planned_dep_version(dep_of(dep_string), plan, requester);
        FAIL() << "计划版本 " << planned << " 不满足 `" << dep_string << "`，必须拒绝";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("lib"), std::string::npos) << "报错必须点名依赖: " << msg;
        EXPECT_NE(msg.find(planned), std::string::npos) << "报错必须点名计划版本: " << msg;
        EXPECT_NE(msg.find(requester), std::string::npos) << "报错必须点名发起包: " << msg;
    }
}

}  // namespace

TEST_F(PlanDepVersionCheckTest, DifferentReleaseDoesNotSatisfyAnExactConstraint)
{
    // `= 1.0-2` 与计划版本 `1.0-5`：两端都写了 release → 常规比较 → 不满足。
    // ⚠️ `= 1.0` 与 `1.0-5` **是**满足的（rpm 把"没写 release"当通配，与求解器
    // 同一判据）—— 那条现在是**放行**用例，见本文件末尾的 ReleaseWildcardCasesAreNotRefused。
    expect_refused("lib = 1.0-2", "1.0-5", "app");
}

TEST_F(PlanDepVersionCheckTest, ReleaseAboveTheUpperBoundIsRefused)
{
    // `<= 1.0-2` 与 `1.0-5`：不满足（1.0-5 > 1.0-2）。
    expect_refused("lib <= 1.0-2", "1.0-5", "app");
}

TEST_F(PlanDepVersionCheckTest, LowerVersionIsRefusedWhenBothSidesCarryARelease)
{
    // `>= 1.0-5` 与 `1.0-2`：不满足。
    expect_refused("lib >= 1.0-5", "1.0-2", "app");
}

TEST_F(PlanDepVersionCheckTest, PrereleaseDoesNotSatisfyTheBaseVersionConstraint)
{
    // `= 1.0` 与 `1.0~rc1`：不满足（**预发布用 `~`**；`-rc1` 在 rpm 里是 release，比 1.0 新）。
    expect_refused("lib = 1.0", "1.0~rc1", "app");
}

TEST_F(PlanDepVersionCheckTest, ReleaseWildcardCasesAreNotRefused)
{
    // 与上面几组成对：**约束没写 release** 时按 rpm 规则是通配 —— `= 1.0` 与计划版本
    // `1.0-5` 满足；而计划版本 `1.0`（不带 release）反过来满足 `> 1.0-5`。这几条**必须**
    // 放行：求解器用的就是同一判据，拒掉它们等于"自己人打自己人"（这正是当初那 78
    // 处分叉的形态，只是方向反过来）。
    const std::map<std::string, InstallPlan> plan{{"lib", plan_with("lib", "1.0-5")}};
    EXPECT_NO_THROW(detail::check_planned_dep_version(dep_of("lib = 1.0"), plan, "app"));
    EXPECT_NO_THROW(detail::check_planned_dep_version(dep_of("lib >= 1.0"), plan, "app"));

    const std::map<std::string, InstallPlan> plan_no_rel{{"lib", plan_with("lib", "1.0")}};
    EXPECT_NO_THROW(detail::check_planned_dep_version(dep_of("lib > 1.0-5"), plan_no_rel, "app"));
}

TEST_F(PlanDepVersionCheckTest, SatisfyingPlanIsAccepted)
{
    // 正面对照：同样这几种约束，只要计划版本真的满足，一律放行 ——
    // 没有这一组，上面那些 EXPECT_THROW 在"函数见到什么都抛"时也会绿。
    const std::map<std::string, InstallPlan> plan{{"lib", plan_with("lib", "1.0+2")}};
    EXPECT_NO_THROW(detail::check_planned_dep_version(dep_of("lib >= 1.0"), plan, "app"));
    EXPECT_NO_THROW(detail::check_planned_dep_version(dep_of("lib > 1.0"), plan, "app"));
    EXPECT_NO_THROW(detail::check_planned_dep_version(dep_of("lib = 1.0+2"), plan, "app"));
    EXPECT_NO_THROW(detail::check_planned_dep_version(dep_of("lib <= 1.0+2"), plan, "app"));
    EXPECT_NO_THROW(detail::check_planned_dep_version(dep_of("lib != 1.0"), plan, "app"));
    // 复合区间（多个 Constraint 全部满足才算过）
    EXPECT_NO_THROW(detail::check_planned_dep_version(dep_of("lib >= 1.0, < 2.0"), plan, "app"));
}

TEST_F(PlanDepVersionCheckTest, UnconstrainedOrUnplannedDependencyIsNotThisFunctionsBusiness)
{
    // 无约束的依赖没有版本可复核；不在计划里的依赖由调用方走"盘面/能力"那条路 ——
    // 两者都不该在这里抛（抛了会把"没约束"误判成违规，那是"函数越权"）。
    const std::map<std::string, InstallPlan> plan{{"lib", plan_with("lib", "9.9")}};
    EXPECT_NO_THROW(detail::check_planned_dep_version(dep_of("lib"), plan, "app"));
    EXPECT_NO_THROW(detail::check_planned_dep_version(dep_of("other >= 99"), plan, "app"));
    EXPECT_NO_THROW(detail::check_planned_dep_version(dep_of("other >= 99"), {}, "app"));
}
