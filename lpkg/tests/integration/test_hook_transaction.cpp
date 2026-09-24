/**
 * test_hook_transaction.cpp — 包钩子（postinst / prerm）与**批次事务**的边界
 *
 * 钉死三条语义（前两条是本次重构新增的不变量）：
 *
 *   ① **postinst 只在批次提交之后执行**。批次是"全或无"：回滚能撤销文件与 DB，却撤不回
 *      钩子的副作用（钩子以 root 跑 systemd-sysusers / tmpfiles --create / useradd，改的是
 *      系统状态）。批内执行 = 已经回滚掉的批次在系统上留下无法撤销的痕迹。
 *
 *   ② **hook 脚本文件进事务**。hooks_dir/<pkg>/ 是按**包名**存的实体：新版本直接
 *      `fs::copy(..., overwrite_existing)`（不进 WAL、无备份）覆盖旧脚本即永久丢失 ——
 *      批次回滚后包体回到旧版本、钩子却是新版本内容，之后 remove/upgrade 跑的是**错版本**
 *      的脚本。回滚后 hooks_dir/<pkg>/ 必须仍是旧版本的内容。
 *
 *   ③ **prerm 之前的检查全部前移**。prerm 按定义要在文件被删之前跑（不能挪到提交后：
 *      "停止服务 / 摘掉 catalog 条目"必须在文件还在时做），但"同一批次里后面的包还会因
 *      **安全检查**失败而整批回滚"这个窗口必须消除 —— 移除侧的安全检查（共享文件、
 *      DB 文件键撞实体目录）整体前移到**任何 prerm 之前**跑完。
 *
 *   ④ **hook 剪枝只对"真被处理过"的包记账**。批次循环把每个计划成员的
 *      `get_hook_files()` 记进 hook_sets，提交后据此剪枝（空集 = 新版本没有 hooks →
 *      整目录 remove_all）。但 `InstallationTask::run()` 在"已装同版本且非 force"时
 *      **早退**，那一支的 `hook_files_` 同样是空的 —— 这是"本包没被处理"而不是
 *      "新版本没有 hooks"。求解器确实会产出这种成员：批次里某个新包的依赖只有它提供、
 *      而盘上那份已装记录的能力集过期（老 lpkg 装的包 / DB 迁移丢过 provides_db 条目）时，
 *      libsolv 只能用 avail 的同名同版本包满足依赖 → 该包以 REINSTALL 步骤进计划，
 *      且它不是用户显式目标（force_reinstall=false）。记错账的后果是静默删掉一个
 *      **仍在册**的包的 hooks。故记账一律以 `did_process()` 为闸门。
 *
 * ── 取证方式：为什么用断点，而不是"让 hook 落一个标记文件" ───────────────────────
 * run_hook 在目标 root 里没有 /bin/bash 时**提前 return**（沙盒 root 里当然没有 bash），
 * 要让脚本真跑起来得往沙盒塞一整套 rootfs 与动态库 —— 那是在考环境，不是在考本文件的不变量
 * （同理，"测试通过与否取决于沙盒里恰好有没有 bash"本身就是不可接受的取证方式）。
 * 因此取证点是 run_hook 的**执行点断点** `hook_run_<hook 文件名>`，它写在"hooks 已启用、
 * 脚本确实存在、只剩 exec"那一行：命中 ⟺ 这个 hook 真的被执行，与沙盒里有没有 bash 无关。
 * 每一条"没命中"的断言都配一条**正向对照**（成功批次里同一个断点必须命中），否则"没命中"
 * 可能只是断点名字写错 —— 那样断言就退化成恒真。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/crypto/hash.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class HookTransactionTest : public IntegrationTestBase
{
protected:
    static constexpr const char* HOOK_A = "#!/bin/sh\n# hook v1\nexit 0\n";
    static constexpr const char* HOOK_B = "#!/bin/sh\n# hook v2\nexit 0\n";

    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        // 本文件测的是钩子**执行时机**：必须让 run_hook 走到执行点（no_hooks_mode 会在
        // 函数入口直接 return，那样断点永远不命中、全部断言恒真）
        Config::instance().set_no_hooks_mode(false);
        BreakpointManager::instance().clear_all();
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        // 复位到 **Config 的默认值**（false），不是"本用例想要的相反值"。
        // 这里曾经写成 `true`：本文件 SetUp 显式设 false 只为让 run_hook 走到执行点，而
        // TearDown 却把全局留在 true —— 于是"no_hooks_mode 默认是 false"这条前提在后续
        // 任何**忘了显式设 false** 的新套件里静默失效（钩子全被跳过、断点永不命中，
        // 断言恒真）。全局状态只有复位成默认值才不留隐性跨用例顺序依赖。
        Config::instance().set_no_hooks_mode(false);
        IntegrationTestBase::TearDown();
    }

    static std::string read_text(const fs::path& p)
    {
        std::ifstream f(p);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    /** 本包的 hooks 目录（按包名，与 Config::hooks_dir() 的布局一致） */
    static fs::path hooks_of(const std::string& pkg)
    {
        return Config::instance().hooks_dir() / pkg;
    }

    /** 打一个带 hooks/ 的包：content/usr/bin/<name> + hooks/<文件名> = 给定内容 */
    std::string pack_with_hooks(const std::string& name, const std::string& ver,
                                const std::vector<std::pair<std::string, std::string>>& hooks,
                                const std::vector<std::string>& extra_content = {},
                                const std::vector<std::string>& provides = {},
                                const std::vector<std::string>& needed_so = {})
    {
        const fs::path work = suite_work_dir / ("_hk_" + name + "_" + ver);
        fs::create_directories(work / "content/usr/bin");
        std::ofstream(work / "content/usr/bin" / name) << "#!/bin/sh\necho " << name << "\n";
        for (const auto& rel : extra_content) {
            fs::create_directories((work / "content" / rel).parent_path());
            std::ofstream(work / "content" / rel) << "extra\n";
        }
        fs::create_directories(work / "hooks");
        for (const auto& [file, content] : hooks) std::ofstream(work / "hooks" / file) << content;

        const std::string path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(path, work.string(), name, ver, {}, provides, "", needed_so);
        return path;
    }

    /** `.lpkg_bak_*` / `.lpkgtmp` 残留计数（备份与半成品都不该留在沙盒里） */
    static int count_residue(const fs::path& root)
    {
        int n = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const std::string name = it->path().filename().string();
            if (name.find(".lpkg_bak_") != std::string::npos || name.ends_with(".lpkgtmp")) ++n;
        }
        return n;
    }
};

// ============================================================================
// ① + ② 批次失败：既不许跑 postinst，也不许把旧 hook 脚本冲掉
// ============================================================================

TEST_F(HookTransactionTest, FailedBatchKeepsOldHookAndRunsNoPostinst)
{
    // v1：hook 内容 A
    const std::string v1 = pack_with_hooks("hk", "1.0", {{"postinst.sh", HOOK_A}});
    ASSERT_NO_THROW(install_packages({v1}));
    const fs::path hd = hooks_of("hk");
    ASSERT_EQ(read_text(hd / "postinst.sh"), HOOK_A)
        << "fixture 自检：v1 的 hook 没装上，下面两条断言都会退化成恒真";

    // v2：hook 内容 B（同包名、同 hook 名 —— 正是"覆盖旧脚本"的现场）
    const std::string v2 = pack_with_hooks("hk", "2.0", {{"postinst.sh", HOOK_B}});
    // 批次里第二个包在 BEGIN 之后注入失败：此刻 hk 的 v2 **已经完成**（文件 + DB + hook），
    // 正是"已成功的包要被整批回滚"那种局面
    const std::string bad = create_pkg("zfail", "1.0");

    bool postinst_reached = false;
    BreakpointManager::instance().set("hook_run_postinst.sh", [&] { postinst_reached = true; });
    std::map<std::string, std::string> mid;
    BreakpointManager::instance().set("install_after_begin_zfail", [&] {
        mid["hk_hook"] = read_text(hd / "postinst.sh");
        mid["hk_ver"] = Cache::instance().get_installed_version("hk");
        throw LpkgException("injected failure after hk committed");
    });

    EXPECT_THROW(install_packages({v2, bad}), LpkgException)
        << "批次中途注入的失败必须让整批中止并回滚";
    BreakpointManager::instance().clear_all();

    // 中途取证：证明本用例真的走到了"hk 已落地、失败尚未发生"那一刻（否则后面的断言是空转）
    ASSERT_FALSE(mid.empty()) << "断点没命中：批次没有走到 zfail 的 BEGIN —— 本用例失去了取证意义";
    EXPECT_EQ(mid["hk_hook"], HOOK_B) << "批次失败前 hk 的 hook 没有被 v2 覆盖（本用例没取到证）";
    EXPECT_EQ(mid["hk_ver"], "2.0");

    // ② hook 文件版本保真：回滚后仍是 v1 的内容（现状：被 v2 覆盖 → 旧脚本永久丢失）
    EXPECT_EQ(read_text(hd / "postinst.sh"), HOOK_A)
        << "回滚后 hooks_dir/<pkg>/ 里是新版本的脚本，旧版本内容已永久丢失";
    // ① 批次回滚 → 一个 postinst 都不许跑
    EXPECT_FALSE(postinst_reached) << "批次已回滚，hk 的 postinst 却被执行了（钩子副作用撤不回来）";
    EXPECT_EQ(count_residue(suite_work_dir), 0) << "回滚后仍有备份/半成品残留";
}

// ============================================================================
// ①② 的正向对照：成功批次里同一个断点必须命中、新 hook 必须生效
// ============================================================================

TEST_F(HookTransactionTest, SuccessfulUpgradeAppliesNewHookAndRunsPostinst)
{
    const std::string v1 = pack_with_hooks("hk", "1.0", {{"postinst.sh", HOOK_A}});
    ASSERT_NO_THROW(install_packages({v1}));
    const fs::path hd = hooks_of("hk");
    ASSERT_EQ(read_text(hd / "postinst.sh"), HOOK_A);

    const std::string v2 = pack_with_hooks("hk", "2.0", {{"postinst.sh", HOOK_B}});

    bool postinst_reached = false;
    BreakpointManager::instance().set("hook_run_postinst.sh", [&] { postinst_reached = true; });

    ASSERT_NO_THROW(install_packages({v2})) << "批次成功";
    BreakpointManager::instance().clear_all();

    EXPECT_EQ(read_text(hd / "postinst.sh"), HOOK_B)
        << "批次成功时新版本的 hook 内容必须生效（否则上面那条'回滚后仍是 A'没有证明力）";
    EXPECT_TRUE(postinst_reached)
        << "正向对照：成功批次里 hk 的 postinst 必须被执行（否则断点取证是死的）";
    EXPECT_EQ(count_residue(suite_work_dir), 0);
}

// ============================================================================
// ② 变体：回滚不许留下"新版本才有的 hook"（否则后续 remove/upgrade 会跑它）
// ============================================================================

TEST_F(HookTransactionTest, FailedBatchLeavesNoHookAddedByTheNewVersion)
{
    // v1 **不带**任何 hook → v2 新增 postinst.sh：批次失败后 hooks_dir/<pkg>/ 必须是干净
    // 的"没有这个东西"状态。留下的新版本脚本没有对应的包体（包还是 v1），却会在之后任何一次
    // remove/upgrade 里被执行 —— 与"旧脚本被覆盖丢失"是同一个 bug 的另一半。
    const std::string v1 = pack_with_hooks("hk", "1.0", {});
    ASSERT_NO_THROW(install_packages({v1}));
    const fs::path hd = hooks_of("hk");
    ASSERT_FALSE(fs::exists(hd)) << "fixture 自检：v1 不该有 hooks 目录";

    const std::string v2 = pack_with_hooks("hk", "2.0", {{"postinst.sh", HOOK_B}});
    const std::string bad = create_pkg("zfail", "1.0");

    std::map<std::string, std::string> mid;
    BreakpointManager::instance().set("install_after_begin_zfail", [&] {
        // 中途取证：此刻新 hook 已经落位（否则本用例是空转）
        mid["hook_exists"] = fs::exists(hd / "postinst.sh") ? "yes" : "no";
        mid["hk_ver"] = Cache::instance().get_installed_version("hk");
        throw LpkgException("injected failure after hk committed");
    });

    EXPECT_THROW(install_packages({v2, bad}), LpkgException);
    BreakpointManager::instance().clear_all();

    ASSERT_FALSE(mid.empty()) << "断点没命中：本用例失去了取证意义";
    EXPECT_EQ(mid["hook_exists"], "yes") << "批次失败前新 hook 并没有落位";
    EXPECT_EQ(mid["hk_ver"], "2.0");

    EXPECT_FALSE(fs::exists(hd / "postinst.sh"))
        << "回滚后留下了 v2 才有的 hook（包体是 v1，之后 remove/upgrade 会跑它）";
    EXPECT_FALSE(fs::exists(hd)) << "回滚后 hooks_dir/<pkg>/ 空壳目录仍在";
    EXPECT_EQ(count_residue(suite_work_dir), 0);
}

// ============================================================================
// ③ prerm 之前必须跑完成批检查：安全检查拒绝时一个 prerm 都不许跑
// ============================================================================

TEST_F(HookTransactionTest, RemovalSafetyCheckRunsBeforeAnyPrerm)
{
    // prek 带 prerm.sh；stale 的 DB 文件键 `usr/share/thing` 在盘上已被人换成实体目录
    // → 移除 stale 会被"陈旧文件键"检查拒绝（整批拒绝，什么都不动）
    const std::string prek = pack_with_hooks("prek", "1.0", {{"prerm.sh", HOOK_A}});
    const std::string stale = pack_with_hooks("stale", "1.0", {}, {"usr/share/thing"});
    ASSERT_NO_THROW(install_packages({prek, stale}));
    ASSERT_TRUE(fs::exists(hooks_of("prek") / "prerm.sh"))
        << "fixture 自检：prek 的 prerm 没装上，下面的断言会退化成恒真";

    fs::remove(test_root / "usr/share/thing");
    fs::create_directories(test_root / "usr/share/thing");
    std::ofstream(test_root / "usr/share/thing" / "other-pkg-data.txt") << "keep me\n";

    bool prerm_reached = false;
    BreakpointManager::instance().set("hook_run_prerm.sh", [&] { prerm_reached = true; });

    // prek 在批次里排在前面：旧实现会先跑它的 prerm，再撞上 stale 的检查 → 整批回滚，
    // 而 prerm 的副作用已经出去了。新实现把检查整体前移到任何 prerm 之前。
    EXPECT_THROW(remove_packages({"prek", "stale"}), LpkgException) << "陈旧文件键必须拒绝整批移除";
    BreakpointManager::instance().clear_all();

    EXPECT_FALSE(prerm_reached) << "批次被安全检查拒绝（什么都没删），prek 的 prerm 却已经跑了";
    EXPECT_TRUE(Cache::instance().is_installed("prek")) << "被拒绝的批次不该真删掉任何包";
    EXPECT_TRUE(fs::exists(hooks_of("prek") / "prerm.sh")) << "被拒绝时 hook 文件不该消失";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/prek")) << "被拒绝时必须什么都没删";
}

// ============================================================================
// ③ 的正向对照：成功的移除批次里 prerm 必须执行
// ============================================================================

TEST_F(HookTransactionTest, SuccessfulRemovalRunsPrerm)
{
    const std::string prek = pack_with_hooks("prek", "1.0", {{"prerm.sh", HOOK_A}});
    ASSERT_NO_THROW(install_packages({prek}));
    ASSERT_TRUE(fs::exists(hooks_of("prek") / "prerm.sh"));

    bool prerm_reached = false;
    BreakpointManager::instance().set("hook_run_prerm.sh", [&] { prerm_reached = true; });

    ASSERT_NO_THROW(remove_packages({"prek"}));
    BreakpointManager::instance().clear_all();

    EXPECT_TRUE(prerm_reached)
        << "正向对照：成功的移除必须执行 prerm（否则'被拒绝时不跑 prerm'那条是死的）";
    EXPECT_FALSE(Cache::instance().is_installed("prek"));
    EXPECT_FALSE(fs::exists(hooks_of("prek"))) << "移除后 hooks_dir/<pkg>/ 应被清掉";
    EXPECT_EQ(count_residue(suite_work_dir), 0);
}

// ============================================================================
// ④ 已装同版本的批次成员：它的 hooks 一个都不许被剪
// ============================================================================

/**
 * 现场构造（每一步都由公开 API 产生，且是真实可达的状态）：
 *
 *   1. ua 1.0 已装（provides `libua.so.1`、带 hooks/postinst.sh）——盘上 hooks 目录在。
 *   2. **盘上那份已装记录的 provides 过期**：删掉 provides_db 里 ua 提供 libua.so.1 的
 *      条目。真实来源：老 lpkg 装的包（当时不记 provides）、DB 迁移/损坏后重放、
 *      数据库被手工编辑。步骤 3 的前提正是这个"已装世界表达不出的能力"。
 *   3. 装 ub 1.0（needed_so `libua.so.1`，仓库索引里 ua 1.0 仍声明该 provides）。
 *      libsolv 无法用**已装的** ua 满足这条 requires（已装记录里没有它），唯一可行解是
 *      **装 avail 的同名同版本 ua** —— 于是 ua 1.0 以 `SOLVER_TRANSACTION_REINSTALL`
 *      步骤进计划（它只 obsoletes 那个已装 solvable），且它不是用户显式目标
 *      （targets 里只有 ub）→ `p.force_reinstall == false` → `run()` 在"已装同版本"处早退。
 *
 * 断言：ub 装上了 ⟹ 依赖链被满足 ⟹ solver 只能装 avail 的 ua（同包名同版本）⟹ ua 确实
 * 以"未被处理"的计划成员身份进过批次（这两步是推理，见下面的前提自检）；在此前提下，
 * ua 的 hooks 目录必须原样保留 —— 修复前这里会 `fs::remove_all` 整个删掉它。
 */
TEST_F(HookTransactionTest, AlreadyInstalledBatchMemberKeepsItsHooks)
{
    setup_local_mirror();

    // 1. ua 1.0 已装（本地文件装，与镜像无关）
    const std::string ua =
        pack_with_hooks("ua", "1.0", {{"postinst.sh", HOOK_A}}, {}, {"libua.so.1"});
    ASSERT_NO_THROW(install_packages({ua}));
    const fs::path hd = hooks_of("ua");
    ASSERT_EQ(read_text(hd / "postinst.sh"), HOOK_A)
        << "fixture 自检：ua 的 hook 没装上，下面的断言会退化成恒真";

    // 2. 已装记录的 provides 过期（模型见上）
    Cache::instance().remove_provider("libua.so.1", "ua");
    Cache::instance().write();

    // 3. 镜像：ua 1.0（声明 provides）+ ub 1.0（需要它）
    pack_with_hooks("ub", "1.0", {}, {}, {}, {"libua.so.1"});
    const fs::path mirror = suite_work_dir / "mirror" / "x86_64";
    {
        std::ofstream index(mirror / "index.txt");
        for (const auto& [n, v, prov, nso] :
             {std::tuple<std::string, std::string, std::string, std::string>{"ua", "1.0",
                                                                             "libua.so.1", ""},
              std::tuple<std::string, std::string, std::string, std::string>{"ub", "1.0", "",
                                                                             "libua.so.1"}}) {
            const fs::path built = pkg_dir / (n + "-" + v + ".lpkg");
            fs::create_directories(mirror / n);
            fs::copy(built, mirror / n / (v + ".lpkg"), fs::copy_options::overwrite_existing);
            index << n << "|" << v << ":" << calculate_sha256(built) << "::" << prov << ":" << nso
                  << "|\n";
        }
    }

    bool ua_began = false;
    bool ub_began = false;
    BreakpointManager::instance().set("install_after_begin_ua", [&] { ua_began = true; });
    BreakpointManager::instance().set("install_after_begin_ub", [&] { ub_began = true; });

    ASSERT_NO_THROW(install_packages({"ub"})) << "ub 的依赖必须能被满足";
    BreakpointManager::instance().clear_all();

    // 前提自检：ub 装上了（批次成功）+ ub 的任务真的开始了。
    // 推论：libua.so.1 的唯一提供者是 ua（已装记录里已无该 provides）→ solver 只能装
    // avail 的 ua → ua 作为计划成员进过批次。
    ASSERT_TRUE(ub_began) << "fixture 自检：ub 根本没开始装，本用例失去了取证意义";
    ASSERT_EQ(Cache::instance().get_installed_version("ub"), "1.0");
    // ua 仍是已装 1.0，且它的任务早退（连 WAL BEGIN 都没写）—— 这正是"未被处理"的计划成员
    EXPECT_EQ(Cache::instance().get_installed_version("ua"), "1.0");
    EXPECT_FALSE(ua_began) << "ua 的任务不该真的开始装（已装同版本 → run() 早退）";

    // 核心不变量：未被处理的包不进 hook 账，它的 hooks 目录因此一个字节都不许少
    EXPECT_TRUE(fs::exists(hd / "postinst.sh"))
        << "已装的同版本计划成员被当成'新版本没有 hooks'，它的 hook 被静默删掉了";
    EXPECT_EQ(read_text(hd / "postinst.sh"), HOOK_A);
    EXPECT_TRUE(Cache::instance().is_installed("ua")) << "ua 仍是在册状态，hooks 却没了";
    EXPECT_EQ(count_residue(suite_work_dir), 0);
}
