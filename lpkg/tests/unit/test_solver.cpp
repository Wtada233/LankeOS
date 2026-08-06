#include <gtest/gtest.h>

#include "../../main/src/pkg/solver.hpp"

using namespace solv;

class SolverTest : public ::testing::Test
{
protected:
    Repository repo;
    std::map<std::string, InstalledPkg> installed;

    // 添加仓库包：deps 为依赖名列表（无约束），provides/needed_so 为能力列表
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

    SolveResult solve(const std::vector<std::pair<std::string, std::string>>& targets,
                      const SolveOptions& opts = {})
    {
        return solve_install(repo, {}, installed, targets, opts);
    }

    bool order_has(const SolveResult& r, const std::string& name) const
    {
        for (const auto& p : r.order)
            if (p.name == name) return true;
        return false;
    }
};

// 基本：装 appB（needed_so liba）→ libA 被自动拉入
TEST_F(SolverTest, PullsProviderViaNeededSo)
{
    add("libA", "1.0", {}, {"liba.so.1"}, {});
    add("appB", "2.0", {}, {}, {"liba.so.1"});

    auto r = solve({{"appB", "latest"}});
    ASSERT_TRUE(r.ok())
        << "problems: " << [&r] { std::string s; for (auto& p : r.problems) s += p + "; "; return s; }();
    EXPECT_TRUE(order_has(r, "appB"));
    EXPECT_TRUE(order_has(r, "libA"));
    // 依赖先装：libA 在 appB 之前
    size_t libA_pos = r.order.size(), appB_pos = r.order.size();
    for (size_t i = 0; i < r.order.size(); ++i) {
        if (r.order[i].name == "libA") libA_pos = i;
        if (r.order[i].name == "appB") appB_pos = i;
    }
    EXPECT_LT(libA_pos, appB_pos);
}

// 基本：装 appB（deps 指名 libA）→ libA 被自动拉入
TEST_F(SolverTest, PullsDepsByName)
{
    add("libA", "1.0", {}, {"liba.so.1"}, {});
    add("appB", "2.0", {"libA"}, {}, {});

    auto r = solve({{"appB", "latest"}});
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(order_has(r, "libA"));
}

// qt6-base 场景：已装 sqlite 提供 libsqlite3.so（available repo 也有）→ 重装 qt6-base 成功
TEST_F(SolverTest, InstalledProviderSatisfiesSoname)
{
    add("sqlite", "3.53.4", {}, {"libsqlite3.so"}, {});
    add("qt6-base", "6.11.1", {}, {}, {"libsqlite3.so"});
    installed["sqlite"] = {"3.53.4", {}, {}, {}};  // 已装（即使无 Cache provider 记录，available repo 兜底）

    auto r = solve({{"qt6-base", "latest"}});
    ASSERT_TRUE(r.ok()) << "problems: " << (r.problems.empty() ? "" : r.problems[0]);
    // sqlite 已装满足 → 只需 qt6-base
    EXPECT_TRUE(order_has(r, "qt6-base"));
}

// 缺 provider 且非容忍 → 报错（缺 SONAME 在 problems 里）
TEST_F(SolverTest, MissingProviderErrorsByDefault)
{
    add("appB", "2.0", {}, {}, {"libmissing.so"});

    auto r = solve({{"appB", "latest"}});
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(r.problems.empty());
}

// 缺 provider 且 --missing-so-no-error → 注入伪提供者，求解成功
TEST_F(SolverTest, MissingProviderTolerated)
{
    add("appB", "2.0", {}, {}, {"libmissing.so"});

    SolveOptions opts;
    opts.missing_so_no_error = true;
    auto r = solve_install(repo, {}, installed, {{"appB", "latest"}}, opts);
    ASSERT_TRUE(r.ok()) << "problems: " << (r.problems.empty() ? "" : r.problems[0]);
    EXPECT_TRUE(order_has(r, "appB"));
}

// M1 回归：缺失**命名依赖**（非 SONAME）默认即报错
TEST_F(SolverTest, MissingNamedDepErrorsByDefault)
{
    add("appB", "2.0", {"libgone"}, {}, {});

    auto r = solve({{"appB", "latest"}});
    EXPECT_FALSE(r.ok()) << "缺失命名依赖应报错，而非静默装上";
    EXPECT_FALSE(r.problems.empty());
}

// M1 回归：--missing-so-no-error **只**容忍缺失 needed_so（SONAME），缺失命名依赖仍报错
TEST_F(SolverTest, MissingNamedDepNotToleratedBySoFlag)
{
    add("appB", "2.0", {"libgone"}, {}, {});

    SolveOptions opts;
    opts.missing_so_no_error = true;
    auto r = solve_install(repo, {}, installed, {{"appB", "latest"}}, opts);
    EXPECT_FALSE(r.ok())
        << "--missing-so-no-error 只允许 needed_so 缺失，缺失命名依赖不能靠伪提供者吞掉（回归 M1）";
}

// 传递闭包：C → B → A，装 C 全部拉入
TEST_F(SolverTest, TransitiveClosure)
{
    add("libA", "1.0", {}, {"liba.so.1"}, {});
    add("libB", "1.0", {}, {"libb.so.1"}, {"liba.so.1"});
    add("appC", "1.0", {}, {}, {"libb.so.1"});

    auto r = solve({{"appC", "latest"}});
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(order_has(r, "libA"));
    EXPECT_TRUE(order_has(r, "libB"));
    EXPECT_TRUE(order_has(r, "appC"));
}

// 核心场景（紧跟上游）：装 chromium 需要 libxml2.so.16，机器只有 libxml2.so.2，
// 且已装 app 依赖旧 libxml2.so.2 → 自动升级 libxml2 + 连带升级 app
TEST_F(SolverTest, AutoUpgradesReverseDepsForSoname)
{
    add("libxml2", "2.0", {}, {"libxml2.so.2"}, {});
    add("libxml2", "3.0", {}, {"libxml2.so.16"}, {});   // 新版丢 libxml2.so.2
    add("app", "1.0", {}, {}, {"libxml2.so.2"});         // 旧 app 依赖旧 soname
    add("app", "2.0", {}, {}, {"libxml2.so.16"});        // 新 app 依赖新 soname
    add("chromium", "1.0", {}, {}, {"libxml2.so.16"});
    installed["libxml2"] = {"2.0", {}, {}, {"libxml2.so.2"}};  // 已装旧版提供旧 soname
    installed["app"] = {"1.0", {}, {"libxml2.so.2"}, {}};      // 已装 app 依赖旧 soname

    auto r = solve({{"chromium", "latest"}});
    ASSERT_TRUE(r.ok()) << "problems: " << (r.problems.empty() ? "" : r.problems[0]);
    EXPECT_TRUE(order_has(r, "libxml2"));   // 升级 libxml2
    EXPECT_TRUE(order_has(r, "app"));       // 连带升级 app
    EXPECT_TRUE(order_has(r, "chromium"));
    // 依赖序：libxml2 在 app 与 chromium 之前（两者都依赖新 libxml2.so.16；
    // app 与 chromium 是兄弟节点，彼此相对顺序不保证）
    size_t lx = r.order.size(), ap = r.order.size(), cr = r.order.size();
    for (size_t i = 0; i < r.order.size(); ++i) {
        if (r.order[i].name == "libxml2") lx = i;
        if (r.order[i].name == "app") ap = i;
        if (r.order[i].name == "chromium") cr = i;
    }
    EXPECT_LT(lx, ap);
    EXPECT_LT(lx, cr);
}

// 冲突变体：app 没有可升级的新版本 → 装 chromium 会破坏已装 app → 硬报错
TEST_F(SolverTest, ReverseDepWithoutNewVersionConflicts)
{
    add("libxml2", "2.0", {}, {"libxml2.so.2"}, {});
    add("libxml2", "3.0", {}, {"libxml2.so.16"}, {});
    add("app", "1.0", {}, {}, {"libxml2.so.2"});   // 只有旧版，无新版可升
    add("chromium", "1.0", {}, {}, {"libxml2.so.16"});
    installed["libxml2"] = {"2.0", {}, {}, {"libxml2.so.2"}};
    installed["app"] = {"1.0", {}, {"libxml2.so.2"}, {}};

    auto r = solve({{"chromium", "latest"}});
    EXPECT_FALSE(r.ok()) << "app 无新版可升时应报冲突，而非硬装破坏它";
}

// --no-deps：只装目标自身，不拉依赖
TEST_F(SolverTest, NoDepsSkipsDependencyPulling)
{
    add("libA", "1.0", {}, {"liba.so.1"}, {});
    add("appB", "2.0", {"libA"}, {}, {"liba.so.1"});

    SolveOptions opts;
    opts.no_deps = true;
    auto r = solve_install(repo, {}, installed, {{"appB", "latest"}}, opts);
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(order_has(r, "appB"));
    EXPECT_FALSE(order_has(r, "libA")) << "no_deps 时不得拉入依赖 libA";
}

// 回归测试：指定特定版本安装（lpkg install pkg:1.0）
TEST_F(SolverTest, InstallSpecifiedVersion)
{
    add("appX", "1.0", {}, {}, {});
    add("appX", "2.0", {}, {}, {});

    auto r = solve({{"appX", "1.0"}});
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(r.order.size(), 1u);
    EXPECT_EQ(r.order[0].name, "appX");
    EXPECT_EQ(r.order[0].version, "1.0");
}

// 回归测试：!= 约束排除相同版本
TEST_F(SolverTest, NotEqualOperatorMapping)
{
    // appY 依赖 libK != 1.0
    DependencyInfo dep;
    dep.name = "libK";
    dep.constraints.push_back({"!=", "1.0"});
    repo.update_package_info("appY", "1.0", {dep}, {}, {});

    add("libK", "1.0", {}, {}, {});
    add("libK", "2.0", {}, {}, {});

    auto r = solve({{"appY", "latest"}});
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(order_has(r, "appY"));
    // 应选 2.0 而非 1.0
    for (const auto& p : r.order) {
        if (p.name == "libK") {
            EXPECT_EQ(p.version, "2.0");
        }
    }
}

// 回归测试：repo_revrequires 不包含自身
TEST_F(SolverTest, RepoRevRequiresExcludesSelf)
{
    add("glibc", "2.34", {}, {"libc.so.6"}, {"libc.so.6"});
    add("appZ", "1.0", {}, {}, {"libc.so.6"});

    auto rev = repo_revrequires(repo, "glibc");
    EXPECT_TRUE(rev.contains("appZ"));
    EXPECT_FALSE(rev.contains("glibc")) << "repo_revrequires 不应把 glibc 自身记为自己的反向依赖";
}

// S1 回归：裸名 install 已装最新版 → no-op（既有语义，防回归）
TEST_F(SolverTest, InstallLatestAlreadyInstalledIsNoop)
{
    add("pkgA", "1.0", {}, {}, {});
    installed["pkgA"] = {"1.0", {}, {}, {}};

    auto r = solve({{"pkgA", "latest"}});
    ASSERT_TRUE(r.ok()) << "problems: " << (r.problems.empty() ? "" : r.problems[0]);
    EXPECT_TRUE(r.order.empty()) << "已装最新版 install pkg 应为空计划（报'已安装'）";
}

// S1 回归：指定版本（本地文件/`pkg:ver`）已装同版本 → no-op，而非 REINSTALL 重装计划
TEST_F(SolverTest, InstallSpecifiedVersionAlreadyInstalledIsNoop)
{
    add("pkgA", "1.0", {}, {}, {});
    installed["pkgA"] = {"1.0", {}, {}, {}};

    auto r = solve({{"pkgA", "1.0"}});
    ASSERT_TRUE(r.ok()) << "problems: " << (r.problems.empty() ? "" : r.problems[0]);
    EXPECT_TRUE(r.order.empty())
        << "已装同版本时 install pkg:ver / 本地 .lpkg 不应再产出重装计划（回归 S1）";
}

// S1 变体：--force 时同版本仍要重装（reinstall 语义保留）
TEST_F(SolverTest, ForceReinstallSameVersionProducesPlan)
{
    add("pkgA", "1.0", {}, {}, {});
    installed["pkgA"] = {"1.0", {}, {}, {}};

    SolveOptions opts;
    opts.force_reinstall = true;
    auto r = solve_install(repo, {}, installed, {{"pkgA", "1.0"}}, opts);
    ASSERT_TRUE(r.ok()) << "problems: " << (r.problems.empty() ? "" : r.problems[0]);
    EXPECT_TRUE(order_has(r, "pkgA")) << "--force 重装应保留计划";
}

// S2 回归：latest 必须用 lpkg 版本语义（预发布 -rc 比稳定版旧），已装 rc 应升到稳定版
TEST_F(SolverTest, LatestPrefersStableOverPrerelease)
{
    add("pkgB", "1.0", {}, {}, {});
    add("pkgB", "1.0-rc1", {}, {}, {});
    installed["pkgB"] = {"1.0-rc1", {}, {}, {}};

    auto r = solve({{"pkgB", "latest"}});
    ASSERT_TRUE(r.ok()) << "problems: " << (r.problems.empty() ? "" : r.problems[0]);
    ASSERT_EQ(r.order.size(), 1u);
    EXPECT_EQ(r.order[0].name, "pkgB");
    EXPECT_EQ(r.order[0].version, "1.0")
        << "lpkg 语义下 1.0 > 1.0-rc1，latest 应选稳定版（回归 S2，EVRCMP 会选错 rc）";
}

// S2 变体：已装稳定版，仓库只有预发布 → 不应降级到 rc
TEST_F(SolverTest, LatestDoesNotDowngradeToPrerelease)
{
    add("pkgB", "1.0-rc1", {}, {}, {});
    installed["pkgB"] = {"1.0", {}, {}, {}};

    auto r = solve({{"pkgB", "latest"}});
    ASSERT_TRUE(r.ok()) << "problems: " << (r.problems.empty() ? "" : r.problems[0]);
    EXPECT_TRUE(r.order.empty()) << "已装 1.0 而仓库只有 1.0-rc1，不得降级到预发布";
}

// S3 回归：真包但指定版本不存在 → 明确报错（不再静默"已安装"）
TEST_F(SolverTest, InstallNonexistentVersionErrors)
{
    add("pkgC", "1.0", {}, {}, {});
    add("pkgC", "2.0", {}, {}, {});

    auto r = solve({{"pkgC", "9.9.9"}});
    EXPECT_FALSE(r.ok()) << "真包版本不存在应报错，而非静默'已安装'（回归 S3）";
    EXPECT_FALSE(r.problems.empty());
}

// S3 回归：能力名 + 指定版本 → 回退 capability 装提供者（不再静默"已安装"）
TEST_F(SolverTest, InstallCapabilityWithVersionFallsBackToProvider)
{
    add("provPkg", "1.0", {}, {"somecap"}, {});

    auto r = solve({{"somecap", "1.0"}});
    ASSERT_TRUE(r.ok()) << "problems: " << (r.problems.empty() ? "" : r.problems[0]);
    EXPECT_TRUE(order_has(r, "provPkg"))
        << "无同名包的能力名+版本应回退装提供者（回归 S3）";
}

// S3 回归：包不存在且无 capability → 报错（而非静默"已安装"）
TEST_F(SolverTest, InstallNonexistentPackageWithVersionErrors)
{
    auto r = solve({{"nosuchpkg", "1.0"}});
    EXPECT_FALSE(r.ok()) << "不存在的包+版本应报错（回归 S3）";
}

