/**
 * test_plan_repo_miss.cpp — 求解计划里的包在仓库索引里找不到时**必须硬报错**
 *
 * 缺口（CLAUDE.md §3「空 sha256 → 静默跳过哈希校验」）：
 * `detail::resolve_with_solver`（pkg/install_common.cpp）把求解结果映射成 InstallPlan 时，
 * 非本地包从 `ctx.repo.find_package(name, version)` 取 PackageInfo；**未命中就什么都不做**，
 * `info` 保持默认构造 —— 其中 `sha256` 为空。空的 sha256 一路流到 installation_task 的
 * `download_and_verify_package()`，那里两处 `!expected_hash_.empty()` 守卫直接放行，
 * 于是**从镜像下载下来的包文件一个字节都没被校验**（而哈希是下载内容唯一的完整性依据）。
 *
 * 修法（落点在本测试对应的文件中，installation_task.cpp 的守卫不动）：
 * 仓库解析路径上取不到 info 就抛 LpkgException —— "求解结果里有、索引里没有"本来就是
 * 索引/求解不一致的信号（两者用的是同一个 Repository），静默降级只是把"索引坏了"
 * 伪装成"装好了"。
 *
 * 可达性：**已证实**。走 `solve_install` 末尾的 `--force` 回填 ——
 * `install --force <已装但已不在索引里的包>`：libsolv 不会为"已装同版"产生事务步骤，
 * solve_install 会把它补回 order，而它不在 `repo.packages()` 里 → 修复前静默装一个
 * 未校验的包，修复后明确拒绝（fail-closed 是有意的取舍）。
 *
 * 边界的另一侧**不动**：本地 `.lpkg` 文件安装本来就允许没有哈希（离线安装），
 * 见 LocalPackageHasNoHashAndIsStillAllowed——它钉住"别把这条路一起打死"。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/install_common.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/repo/repository.hpp"

namespace fs = std::filesystem;

class PlanRepoMissTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path root;
    fs::path index_dir;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_plan_repo_miss_test");
        fs::remove_all(suite_work_dir);

        root = suite_work_dir / "root";
        index_dir = suite_work_dir / "mirror" / "x86_64";
        fs::create_directories(index_dir);
        fs::create_directories(root / "etc" / "lpkg");

        Config::instance().set_root_path(root.string());
        Config::instance().set_architecture("x86_64");
        Config::instance().init_filesystem();
        Cache::instance().load();

        std::ofstream(root / "etc/lpkg/mirror.conf")
            << "file://" << (suite_work_dir / "mirror").string() << "/" << std::endl;
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    void write_index(const std::string& content)
    {
        std::ofstream f(index_dir / "index.txt");
        f << content;
    }

    /// 就地构造一个 InstallContext（只借用调用方的容器）
    struct Ctx {
        std::map<std::string, InstallPlan> plan;
        std::vector<std::string> order;
        std::map<std::string, fs::path> locals;
        std::vector<std::pair<std::string, std::string>> targets;
    };

    static InstallContext make_ctx(Repository& repo, Ctx& c, bool force)
    {
        // 逐字段列全：InstallContext 有 9 个成员，少写一个就触发
        // -Wmissing-field-initializers（-Werror 下直接编译失败）
        InstallContext ctx{repo,
                           c.plan,
                           c.order,
                           c.locals,
                           c.targets,
                           force,
                           /*top_level=*/true,
                           /*successfully_installed=*/{},
                           /*installed_set=*/{}};
        return ctx;
    }

    /// 造一个只有 usr/bin/<name> 的最小本地包，返回 .lpkg 路径
    std::string make_local_pkg(const std::string& name, const std::string& version)
    {
        fs::path work = suite_work_dir / ("_pkg_" + name);
        fs::remove_all(work);
        fs::create_directories(work / "content" / "usr" / "bin");
        std::ofstream(work / "content" / "usr" / "bin" / name) << "#!/bin/sh\n";
        const std::string path = (suite_work_dir / (name + "-" + version + ".lpkg")).string();
        pack_package(path, work.string(), name, version, {}, {}, "man", {});
        return path;
    }
};

// ── 主锚点：仓库解析路径上取不到 info → 硬报错（不是静默的空 PackageInfo） ──────────
TEST_F(PlanRepoMissTest, PlanEntryMissingFromRepoIndexIsHardError)
{
    // 索引里只有 present；Cache 说 ghost 1.0 已装（但仓库索引里没有它）
    write_index("present|1.0:deadbeefcafe::libpresent.so.1:\n");
    Cache::instance().add_installed("ghost", "1.0", true);

    Repository repo;
    repo.load_index();
    ASSERT_FALSE(repo.find_package("ghost", "1.0").has_value())
        << "前置条件：索引里不该有 ghost（否则这个用例什么都没测）";

    Ctx c;
    // --force 重装 ghost：libsolv 对"已装同版"不产生事务步骤，solve_install 末尾的
    // force 回填会把它补进 order —— 这正是"求解结果里有、索引里没有"的可达路径。
    c.targets = {{"ghost", "latest"}};
    InstallContext ctx = make_ctx(repo, c, /*force=*/true);

    std::string msg;
    try {
        detail::resolve_with_solver(ctx);
        FAIL() << "求解计划里的 ghost 1.0 不在仓库索引中，必须硬报错；"
                  "静默降级成空 PackageInfo = 空 sha256 = 下载的包文件无人校验";
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    // 拒绝类断言必须落**两个锚点**：点名哪个包 + 哪个版本（否则"拒绝"与"查错了东西"
    // 不可区分）
    EXPECT_NE(msg.find("ghost"), std::string::npos) << "报错没点名包： " << msg;
    EXPECT_NE(msg.find("1.0"), std::string::npos) << "报错没点名版本： " << msg;
}

// ── 对照：索引里有的目标照常建计划，且 sha256 来自索引 ────────────────────────────
TEST_F(PlanRepoMissTest, RepoResidentTargetKeepsIndexHash)
{
    write_index("present|1.0:deadbeefcafe::libpresent.so.1:\n");

    Repository repo;
    repo.load_index();

    Ctx c;
    c.targets = {{"present", "latest"}};
    InstallContext ctx = make_ctx(repo, c, /*force=*/false);

    ASSERT_NO_THROW(detail::resolve_with_solver(ctx));
    ASSERT_EQ(c.plan.count("present"), 1u) << "索引里有的包必须进计划";
    EXPECT_EQ(c.plan["present"].sha256, "deadbeefcafe")
        << "计划里的 sha256 必须来自索引（哈希校验的唯一依据）";
    EXPECT_EQ(c.plan["present"].actual_version, "1.0");
    EXPECT_TRUE(c.plan["present"].local_path.empty());
}

// ── 边界（**不许打死**）：本地 .lpkg 安装允许没有哈希 ─────────────────────────────
// 离线安装的完整性由用户手里那份文件本身负责，不是仓库路径的哈希。这条路上的
// PackageInfo 从不带 sha256，若把"sha256 为空"当错误，本地安装会全线崩掉。
TEST_F(PlanRepoMissTest, LocalPackageHasNoHashAndIsStillAllowed)
{
    write_index("");  // 空索引：本地包安装不该依赖仓库里有它
    const std::string pkg = make_local_pkg("localpkg", "3.1");

    Repository repo;
    repo.load_index();

    Ctx c;
    c.targets = {{"localpkg", "latest"}};
    c.locals["localpkg"] = pkg;  // = InstallContext::local_candidates
    InstallContext ctx = make_ctx(repo, c, /*force=*/false);

    ASSERT_NO_THROW(detail::resolve_with_solver(ctx))
        << "本地 .lpkg 这条路必须继续可用（它的 sha256 本来就是空的）";
    ASSERT_EQ(c.plan.count("localpkg"), 1u);
    EXPECT_TRUE(c.plan["localpkg"].sha256.empty())
        << "本地包的 sha256 为空是**预期**形态（离线安装）";
    EXPECT_EQ(c.plan["localpkg"].local_path, fs::path(pkg))
        << "本地路径必须落进计划（下载/校验走 local_path 那条路）";
}
