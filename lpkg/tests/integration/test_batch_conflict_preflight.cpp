/**
 * test_batch_conflict_preflight.cpp — 整批文件冲突预检：冲突必须在**一个文件都没动**之前拦下
 *
 * 冲突判定原先只在**逐包**的 prepare() → check_for_file_conflicts 里做（批次循环内），
 * 语义后果是"该批次前面若干包已经完整落地（文件 + DB 里程碑）之后才发现后面某包的冲突"——
 * 整批回滚能撤文件，撤不回来已经出去的副作用。上游 libalpm 相反：`alpm_trans_commit` 的
 * 第一步就把**整笔事务**（`trans->add`）的文件冲突检完，要么全不动要么全动。
 *
 * 本文件钉住预检的收益、它的边界、以及改写后的 `all_upgrading` 顺序语义：
 *   ① 收益：批次 [A, B] 里 B 与盘上某文件冲突（B **不是**第一个）→ 抛错，且 A 的文件
 *      **一个都没落地**、DB 里没有 A、A 的 postinst 没跑。
 *      ⚠ "WAL 里没有 BEGIN_PKGS"**不是**这条收益的证据（成功/回滚收尾的 trim 会把整个
 *      WAL 清空 → 两种实现下都为空，恒真）；证据是断点取证，见下。
 *   ② 判据不变：冲突信息点名**真实持有者**；无人持有但盘上已有物 → "未知（手动文件）"；
 *      批次内成员之间的同路径冲突（盘上谁都还没有）同样在装第一个包之前拦下。
 *   ③ `all_upgrading` 的顺序语义改写后**仍然生效**（升级时新增依赖与旧版本文件冲突 →
 *      放行），且**两个方向都成立**：持有者排在后面（依赖先装，所有权提前交棒）与持有者
 *      排在前面（旧包先升级、把废弃文件交棒给后面的成员）。
 *
 * ── 取证方式：为什么"断点没命中"能区分"没落地"与"落地后回滚" ──────────────────
 * `install_after_begin_<pkg>` 写在 `InstallationTask::run()` 的 WAL BEGIN 之后 —— 命中
 * ⟺ 这个包真的开始装了。预检拒绝时批次循环**一次都没进**，断点必然不命中；而"逐包检出 +
 * 整批回滚"的旧行为里它会命中（那时 A 已完整装完）。终态（包未安装、文件不在盘上）在两种
 * 行为下**完全相同**（回滚保证），所以只有断点取证能区分二者 —— 这正是本文件的核心断言。
 * 每条"没命中"的断言都配一条**正向对照**（同一套 fixture 里冲突消失后同一个断点必须命中、
 * 且顺序符合预期），否则"没命中"可能只是断点名字写错。正向对照里还在**批次内**读一次
 * WAL（`install_after_begin_prefirst` 回调），钉住"跑起来的批次此刻确实有 BEGIN_PKGS 与
 * 包级行" —— 那是否定式 WAL 断言唯一可能的判别力来源（见 expect_no_batch_rows）。
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
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class BatchConflictPreflightTest : public IntegrationTestBase
{
protected:
    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        // 本文件用 `hook_run_<hook>` 执行点断点取证"postinst 到底跑没跑"（no_hooks_mode 会在
        // run_hook 入口直接 return，断点就永远不命中）
        Config::instance().set_no_hooks_mode(false);
        // force-overwrite 是**进程级**开关：本文件的用例都必须在"不 force"下判定
        Config::instance().set_force_overwrite_mode(false);
        BreakpointManager::instance().clear_all();
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        Config::instance().set_force_overwrite_mode(false);
        // 复位到 **Config 的默认值**（false），而不是本文件想要的相反值 `true`：
        // `set_no_hooks_mode(true)` 会把"钩子全跳过"这个状态泄漏给后续套件 —— 那时
        // `hook_run_*` 断点永不命中，任何"钩子没跑"的断言都恒真。全局开关只有复位成
        // 默认值才不留隐性跨用例顺序依赖。
        Config::instance().set_no_hooks_mode(false);
        // 架构覆盖同理：默认是**空**（= 走 uname 探测），本文件只有一个用例
        // （UpgradeBatchIsGatedToo）显式设 x86_64，不复位就会把架构钉死给后续套件，
        // 而那些套件里"当前架构"决定仓库解析行为。
        Config::instance().set_architecture("");
        IntegrationTestBase::TearDown();
    }

    static void write_file(const fs::path& p, const std::string& content = "x\n")
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }

    /**
     * 打一个包：content 由 fill 回调填（相对 content/ 的路径），hooks 非空时把同名脚本打进
     * hooks/。work 目录先清空 —— 同 name+version 的多个变体（"冲突版"/"对照版"）复用它时，
     * 残留的上一个变体的内容会被一起打进新包（那会让"对照版"凭空带上冲突条目）。
     */
    template <typename F>
    std::string pack(const std::string& name, const std::string& ver,
                     const std::vector<std::string>& deps, F fill,
                     const std::vector<std::string>& hooks = {})
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::remove_all(work);
        fs::create_directories(work / "content");
        fill(work / "content");
        if (!hooks.empty()) {
            fs::create_directories(work / "hooks");
            for (const auto& h : hooks) std::ofstream(work / "hooks" / h) << "#!/bin/sh\nexit 0\n";
        }
        const std::string path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(path, work.string(), name, ver, deps, {}, "man " + name, {});
        return path;
    }

    static std::string read_text(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    std::string wal_text() const
    {
        return read_text(wal::wal_log_path());
    }

    /** 未清理的 stash / 半成品残留数 */
    int count_residue() const
    {
        int n = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(test_root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const std::string name = it->path().filename().string();
            if (name.find(".lpkg_bak_") != std::string::npos || name.ends_with(".lpkgtmp")) ++n;
        }
        return n;
    }

    /**
     * 失败批次收尾后的**终态形态**检查：WAL 里既没有未提交批次，也没有本批次的行。
     *
     * ⚠ **本断言不具判别力**（"预检拒绝"与"批次内拒绝→整批回滚"两条路径都能让它绿）：
     * 成功/回滚成功的批次收尾都会 `trim_completed()`，而它在"已提交且无残留 bak"时把
     * **整个 WAL 文件删掉** —— 于是拒绝之后 WAL 恒为空，这些 `find(...) == npos` 恒真。
     * **判别力来自每个用例里的断点取证**：`install_after_begin_<第一个成员>` 命中 ⟺ 批次
     * 真的进过事务（旧行为"逐包检出→整批回滚"）；预检拒绝时它必然不命中，且每例都配了
     * 正向对照。下面只断言"拒绝后没有留下未提交批次"这一条形态不变量。
     */
    void expect_no_batch_rows(const std::string& ctx) const
    {
        const std::string w = wal_text();
        EXPECT_EQ(w.find("BEGIN_PKGS"), std::string::npos)
            << ctx << "：WAL 里残留了未提交批次（拒绝后没收干净）";
        EXPECT_EQ(w.find("BEGIN "), std::string::npos)
            << ctx << "：WAL 里残留了包级行（拒绝后没收干净）：" << w;
    }
};

// ============================================================================
// ① 核心收益：批次里**后面的**包冲突 → 前面的包一个文件都没落地
// ============================================================================

TEST_F(BatchConflictPreflightTest, ConflictInLaterBatchMemberLeavesEarlierMemberUntouched)
{
    // ownerpkg 先装好并持有 usr/share/data.txt —— 它是**真实持有者**，且不在本批次里
    const std::string owner = pack("ownerpkg", "1.0", {}, [&](const fs::path& c) {
        write_file(c / "usr" / "share" / "data.txt", "owned by ownerpkg\n");
    });
    ASSERT_NO_THROW(install_packages({owner}));
    ASSERT_EQ(Cache::instance().get_installed_version("ownerpkg"), "1.0");
    ASSERT_EQ(read_text(test_root / "usr/share/data.txt"), "owned by ownerpkg\n");

    // 批次 [prefirst, prelater]：prelater **依赖** prefirst → prefirst 必定排在前面
    // （依赖先处理）。冲突落在**第二个**成员上。
    const std::string a = pack(
        "prefirst", "1.0", {},
        [&](const fs::path& c) { write_file(c / "usr" / "bin" / "prefirst", "prefirst v1\n"); },
        {"postinst.sh"});
    const std::string b = pack("prelater", "1.0", {"prefirst"}, [&](const fs::path& c) {
        write_file(c / "usr" / "share" / "data.txt", "from prelater\n");
    });

    bool a_install_began = false;
    bool any_postinst = false;
    // 断点写在 InstallationTask::run() 的 WAL BEGIN 之后 → 命中 ⟺ prefirst 真的开始装了
    BreakpointManager::instance().set("install_after_begin_prefirst",
                                      [&] { a_install_began = true; });
    BreakpointManager::instance().set("hook_run_postinst.sh", [&] { any_postinst = true; });

    std::string msg;
    try {
        install_packages({a, b});
        FAIL() << "prelater 撞上 ownerpkg 持有的 usr/share/data.txt，整批必须被拒绝";
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    BreakpointManager::instance().clear_all();

    // (a) 拒绝的理由：点名**真实持有者**（而不是泛化的 "unknown (manual file)"）
    EXPECT_NE(msg.find("data.txt"), std::string::npos) << "冲突信息没点名冲突路径：" << msg;
    EXPECT_NE(msg.find("ownerpkg"), std::string::npos)
        << "冲突信息必须点名真实持有者 ownerpkg：" << msg;

    // (b) **核心收益**：prefirst 的文件一个都没落地（不是"落地后回滚"）。
    //     预检拒绝时批次循环一次都没进 → 这个断点必然不命中。
    EXPECT_FALSE(a_install_began)
        << "prefirst 已经开始装了 —— 冲突是批次**中途**才发现的，不是预检在一个文件都没动时拦下的";
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/prefirst")) << "prefirst 的文件落到了盘上";
    EXPECT_TRUE(Cache::instance().get_installed_version("prefirst").empty())
        << "DB 里不该有 prefirst 的记录";
    EXPECT_FALSE(fs::exists(Config::instance().hooks_dir() / "prefirst"))
        << "prefirst 的 hook 文件不该落位（它根本没开始装）";
    // postinst 只在批次**提交之后**跑；批次被预检拒绝时一遍都没提交 → 不许有执行
    EXPECT_FALSE(any_postinst) << "批次被拒绝，postinst 却跑到了执行点";
    // 失败包自己也没有任何痕迹
    EXPECT_FALSE(fs::exists(test_root / "usr/share/prelater"));
    // 真实持有者的文件毫发无损
    EXPECT_EQ(read_text(test_root / "usr/share/data.txt"), "owned by ownerpkg\n")
        << "被拒绝的批次动了别人的文件";
    EXPECT_TRUE(Cache::instance().get_file_owners("/usr/share/data.txt").contains("ownerpkg"));
    EXPECT_EQ(count_residue(), 0) << "拒绝时必须什么都没动（无 stash/半成品残留）";
    // 终态形态：拒绝后不留未提交批次（**不具判别力**，理由与出处见 expect_no_batch_rows）
    expect_no_batch_rows("整批预检拒绝后");

    // ── 正向对照：同一套 fixture，把冲突条目去掉 → 必须装成功且断点命中 ──────────
    // 两件事一起证：断点机制是活的（否则上面的"没命中"是恒真），以及 prefirst 确实排在
    // prelater 之前（本用例的前提：冲突的包不是第一个）。
    const std::string b_ok = pack("prelater", "1.0", {"prefirst"}, [&](const fs::path& c) {
        write_file(c / "usr" / "share" / "prelater-only.txt", "no conflict this time\n");
    });
    std::vector<std::string> order_log;
    // 批次内 WAL 取证：同一断点在**拒绝用例**里不会命中，而批次真的跑起来时那一刻 WAL 里
    // **确实**有批次行与包级行 —— 这正是否定式断言（"WAL 里没有这些行"）的意义来源。
    std::string wal_in_batch;
    BreakpointManager::instance().set("install_after_begin_prefirst", [&] {
        order_log.push_back("prefirst");
        wal_in_batch = wal_text();
    });
    BreakpointManager::instance().set("install_after_begin_prelater",
                                      [&] { order_log.push_back("prelater"); });
    BreakpointManager::instance().set("hook_run_postinst.sh", [&] { any_postinst = true; });
    ASSERT_NO_THROW(install_packages({a, b_ok})) << "不冲突的同一批次必须能成功";
    BreakpointManager::instance().clear_all();

    EXPECT_NE(wal_in_batch.find("BEGIN_PKGS"), std::string::npos)
        << "批次内取证：跑起来的批次那一刻 WAL 里必须有 BEGIN_PKGS（否则拒绝后的'没有批次行'"
           "是一条恒真的空话）："
        << wal_in_batch;
    EXPECT_NE(wal_in_batch.find("BEGIN prefirst"), std::string::npos)
        << "批次内取证：prefirst 的包级 WAL 行在该断点处必须已经写下：" << wal_in_batch;
    EXPECT_EQ(order_log, (std::vector<std::string>{"prefirst", "prelater"}))
        << "prelater 必须排在 prefirst 之后（依赖先处理）—— 这是本用例的前提";
    EXPECT_TRUE(any_postinst) << "正向对照：成功批次里 postinst 必须真的跑到执行点";
    // 把 expect_no_batch_rows 那条注释的**前提**钉住：同一套 fixture 里批次真的进过事务
    // （上面刚在批次内读到 BEGIN_PKGS 与包级行），而成功的批次收尾 trim_completed() 会把
    // 整个 WAL 清空 —— 所以"事后 WAL 为空"在结构上无法区分"从没进事务"与"进过又收尾"。
    // 这条断言失败 ⇒ expect_no_batch_rows 里那段"不具判别力"的说明需要重写。
    EXPECT_TRUE(wal_text().empty())
        << "成功批次收尾后 WAL 应被 trim 清空（残留：" << wal_text() << "）";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/prefirst"));
    EXPECT_TRUE(fs::exists(test_root / "usr/share/prelater-only.txt"));
    EXPECT_EQ(count_residue(), 0);
}

// ============================================================================
// ② 判据不变：无人持有、盘上已有同名物 → "未知（手动文件）"，同样在装之前拦下
// ============================================================================

TEST_F(BatchConflictPreflightTest, ManualFileConflictIsRefusedByPreflightToo)
{
    write_file(test_root / "usr" / "share" / "manual.txt", "手工放的文件\n");

    const std::string a =
        pack("mfirst", "1.0", {},
             [&](const fs::path& c) { write_file(c / "usr" / "bin" / "mfirst", "mfirst\n"); },
             {"postinst.sh"});
    const std::string b = pack("mlater", "1.0", {"mfirst"}, [&](const fs::path& c) {
        write_file(c / "usr" / "share" / "manual.txt", "from mlater\n");
    });

    bool a_install_began = false;
    BreakpointManager::instance().set("install_after_begin_mfirst",
                                      [&] { a_install_began = true; });

    std::string msg;
    try {
        install_packages({a, b});
        FAIL() << "mlater 撞上无人持有的手工文件，必须被拒绝";
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    BreakpointManager::instance().clear_all();

    // 无人持有 → 泛化消息（与逐包检查同一份措辞）
    EXPECT_NE(msg.find(get_string("error.unknown_manual_file")), std::string::npos)
        << "无人持有时应报 " << get_string("error.unknown_manual_file") << "：" << msg;
    EXPECT_FALSE(a_install_began) << "预检必须在批次第一个包开始装之前拦下";
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/mfirst"));
    EXPECT_EQ(read_text(test_root / "usr/share/manual.txt"), "手工放的文件\n")
        << "手工文件被搬走/覆盖";
    EXPECT_EQ(count_residue(), 0);
    expect_no_batch_rows("手工文件冲突被拒绝后");
}

// ============================================================================
// ②' 判据不变：批次**内**两个成员发同一路径（盘上谁都还没有）→ 也必须在装之前拦下
//     （这条完全靠"本批次接管顺序"的模拟：预检时盘上还没有这个文件，所有权只存在于
//       第一个成员装完之后）
// ============================================================================

TEST_F(BatchConflictPreflightTest, SamePathBetweenBatchMembersIsRefusedBeforeAnyInstall)
{
    const std::string a = pack("sfirst", "1.0", {}, [&](const fs::path& c) {
        write_file(c / "usr" / "share" / "same.txt", "from sfirst\n");
    });
    const std::string b = pack("slater", "1.0", {"sfirst"}, [&](const fs::path& c) {
        write_file(c / "usr" / "share" / "same.txt", "from slater\n");
    });
    ASSERT_FALSE(fs::exists(test_root / "usr/share/same.txt")) << "fixture 自检：盘上不该有它";

    bool a_install_began = false;
    BreakpointManager::instance().set("install_after_begin_sfirst",
                                      [&] { a_install_began = true; });

    std::string msg;
    try {
        install_packages({a, b});
        FAIL() << "同批次两个成员发同一路径（非 force）必须判冲突";
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    BreakpointManager::instance().clear_all();

    EXPECT_NE(msg.find("sfirst"), std::string::npos)
        << "冲突信息必须点名批次内的真实持有者 sfirst：" << msg;
    EXPECT_FALSE(a_install_began)
        << "批内的同路径冲突必须在**第一个包**开始装之前就拦下（否则就是旧行为：装完再回滚）";
    EXPECT_FALSE(fs::exists(test_root / "usr/share/same.txt"));
    EXPECT_TRUE(Cache::instance().get_installed_version("sfirst").empty());
    EXPECT_EQ(count_residue(), 0);
    expect_no_batch_rows("批内同路径冲突被拒绝后");
}

// ============================================================================
// ③ all_upgrading 顺序语义（改写后）—— 持有者排在**后面**：依赖先装，所有权提前交棒
//    这正是 test_upgrade_deps_resolution.cpp 的 BundledLibTransitionsToExternalDep 场景，
//    由本文件的预检路径再钉一遍：预检若把这条判成冲突，升级新依赖会**在什么都没动时**被
//    误拒（比旧行为更糟：旧行为至少能装成功）。
// ============================================================================

TEST_F(BatchConflictPreflightTest, UpgradeExemptionHoldsWhenNewDependencyShipsTheOldFile)
{
    const std::string v1 = pack("app", "1.0", {}, [&](const fs::path& c) {
        write_file(c / "usr" / "bin" / "app", "app v1\n");
        write_file(c / "usr" / "lib" / "libhelper.so.1", "helper lib v1\n");
    });
    ASSERT_NO_THROW(install_packages({v1}));
    ASSERT_TRUE(Cache::instance().get_file_owners("/usr/lib/libhelper.so.1").contains("app"));

    // app v2 不再自带 libhelper.so.1，改为依赖 libprovider（它发同一个文件）
    const std::string app2 = pack("app", "2.0", {"libprovider"}, [&](const fs::path& c) {
        write_file(c / "usr" / "bin" / "app", "app v2\n");
    });
    const std::string provider = pack("libprovider", "1.0", {}, [&](const fs::path& c) {
        write_file(c / "usr" / "lib" / "libhelper.so.1", "helper from provider\n");
    });

    // 依赖先处理 → libprovider 先装、app（**持有者**）后升级：libprovider 判定时
    // "该 owner 也在本批次 plan 里、且比本包晚轮到" → 放行（预检的等价改写）
    ASSERT_NO_THROW(install_packages({app2, provider}))
        << "升级时新增依赖与旧版本文件冲突必须放行（all_upgrading 的顺序语义）";

    EXPECT_EQ(Cache::instance().get_installed_version("app"), "2.0");
    EXPECT_EQ(Cache::instance().get_installed_version("libprovider"), "1.0");
    EXPECT_TRUE(fs::exists(test_root / "usr/lib/libhelper.so.1")) << "文件不该被删";
    EXPECT_EQ(read_text(test_root / "usr/lib/libhelper.so.1"), "helper from provider\n")
        << "后装的 libprovider 没有接管该文件";
    EXPECT_TRUE(
        Cache::instance().get_file_owners("/usr/lib/libhelper.so.1").contains("libprovider"));
    EXPECT_EQ(count_residue(), 0);
}

// ============================================================================
// ④ 升级批次同样受这道闸门保护 —— `lpkg upgrade` 一次几百个包，冲突同样必须在**装第一个
//    包之前**拦下（这正是审查关注的最坏场景：升级到第 800 个包才发现冲突）
// ============================================================================

TEST_F(BatchConflictPreflightTest, UpgradeBatchIsGatedToo)
{
    Config::instance().set_architecture("x86_64");

    // 已装：ualpha v1（待升级）、uvictim v1（待升级，新版本会撞上别人的文件）、
    // uowner（持有 usr/share/data.txt，**不参与升级**）
    const std::string a1 =
        pack("ualpha", "1.0", {},
             [&](const fs::path& c) { write_file(c / "usr" / "bin" / "ualpha", "ualpha v1\n"); },
             {"postinst.sh"});
    const std::string v1 = pack("uvictim", "1.0", {}, [&](const fs::path& c) {
        write_file(c / "usr" / "bin" / "uvictim", "uvictim v1\n");
    });
    const std::string owner = pack("uowner", "1.0", {}, [&](const fs::path& c) {
        write_file(c / "usr" / "share" / "data.txt", "owned by uowner\n");
    });
    ASSERT_NO_THROW(install_packages({a1, v1, owner}));
    ASSERT_EQ(Cache::instance().get_installed_version("ualpha"), "1.0");
    ASSERT_EQ(Cache::instance().get_installed_version("uvictim"), "1.0");

    // 仓库：ualpha 2.0（干净）、uvictim 2.0（**撞上 uowner 的文件**，并依赖 ualpha >= 2.0
    // 以保证升级顺序里 ualpha 排在前面 —— 冲突的包不是第一个）
    pack("ualpha", "2.0", {},
         [&](const fs::path& c) { write_file(c / "usr" / "bin" / "ualpha", "ualpha v2\n"); });
    pack("uvictim", "2.0", {"ualpha >= 2.0"}, [&](const fs::path& c) {
        write_file(c / "usr" / "bin" / "uvictim", "uvictim v2\n");
        write_file(c / "usr" / "share" / "data.txt", "from uvictim v2\n");
    });
    const fs::path mirror = setup_local_mirror();
    std::ofstream index(mirror / "index.txt");
    for (const auto& [n, v] :
         {std::pair<std::string, std::string>{"ualpha", "2.0"}, {"uvictim", "2.0"}}) {
        const fs::path built = pkg_dir / (n + "-" + v + ".lpkg");
        fs::create_directories(mirror / n);
        fs::copy(built, mirror / n / (v + ".lpkg"), fs::copy_options::overwrite_existing);
        index << n << "|" << v << ":" << calculate_sha256(built) << ":"
              << (n == "uvictim" ? "ualpha >= 2.0" : "") << "::\n";
    }
    index.close();

    bool a_install_began = false;
    bool any_postinst = false;
    BreakpointManager::instance().set("install_after_begin_ualpha",
                                      [&] { a_install_began = true; });
    BreakpointManager::instance().set("hook_run_postinst.sh", [&] { any_postinst = true; });

    std::string msg;
    try {
        upgrade_packages();
        FAIL() << "升级批次里 uvictim 2.0 撞上 uowner 持有的文件，必须被拒绝";
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    BreakpointManager::instance().clear_all();

    EXPECT_NE(msg.find("uowner"), std::string::npos)
        << "冲突信息必须点名真实持有者 uowner：" << msg;
    EXPECT_FALSE(a_install_began)
        << "ualpha 已经开始升级了 —— 冲突是批次**中途**才发现的，不是整批预检拦下的";
    EXPECT_EQ(Cache::instance().get_installed_version("ualpha"), "1.0") << "被拒绝时不该升级";
    EXPECT_EQ(Cache::instance().get_installed_version("uvictim"), "1.0");
    EXPECT_EQ(read_text(test_root / "usr/bin/ualpha"), "ualpha v1\n");
    EXPECT_FALSE(fs::exists(test_root / "usr/share/data.txt.lpkgnew"));
    EXPECT_EQ(read_text(test_root / "usr/share/data.txt"), "owned by uowner\n");
    EXPECT_FALSE(any_postinst) << "整批被拒绝，postinst 却跑到了执行点";
    EXPECT_EQ(count_residue(), 0);
    expect_no_batch_rows("升级批次被预检拒绝后");
}

// ============================================================================
// ③' 同一顺序语义的**反方向**：持有者排在**前面**（旧包先升级、丢掉该文件，后面的新成员
//     接管）。逐包语义下这条靠"前一个包已经把废弃文件搬走"自然成立；预检时"谁都还没装"，
//     必须显式模拟"前序成员释放了哪些路径"，否则会把这种**合法**批次误判成
//     "未知（手动文件）"冲突而拒绝 —— 比旧行为更糟（旧行为能装成功）。
// ============================================================================

TEST_F(BatchConflictPreflightTest, UpgradeExemptionHoldsWhenOldFileMovesToALaterMember)
{
    const std::string v1 = pack("migrator", "1.0", {}, [&](const fs::path& c) {
        write_file(c / "usr" / "bin" / "migrator", "migrator v1\n");
        write_file(c / "usr" / "lib" / "libmoved.so.1", "libmoved v1\n");
    });
    ASSERT_NO_THROW(install_packages({v1}));
    ASSERT_TRUE(Cache::instance().get_file_owners("/usr/lib/libmoved.so.1").contains("migrator"));

    // migrator v2 丢掉 libmoved.so.1；接手者 taker 依赖 migrator，把顺序钉成
    // [migrator, taker]（依赖先处理）—— 于是"释放者在前面、接手者在后面"
    const std::string v2 = pack("migrator", "2.0", {}, [&](const fs::path& c) {
        write_file(c / "usr" / "bin" / "migrator", "migrator v2\n");
    });
    const std::string taker = pack("taker", "1.0", {"migrator"}, [&](const fs::path& c) {
        write_file(c / "usr" / "lib" / "libmoved.so.1", "libmoved from taker\n");
    });

    ASSERT_NO_THROW(install_packages({v2, taker}))
        << "前面的包升级时丢掉的废弃文件交给后面的成员接管，是合法批次，预检不得误拒";
    EXPECT_EQ(Cache::instance().get_installed_version("migrator"), "2.0");
    EXPECT_EQ(Cache::instance().get_installed_version("taker"), "1.0");
    EXPECT_TRUE(fs::exists(test_root / "usr/lib/libmoved.so.1"));
    EXPECT_EQ(read_text(test_root / "usr/lib/libmoved.so.1"), "libmoved from taker\n");
    EXPECT_TRUE(Cache::instance().get_file_owners("/usr/lib/libmoved.so.1").contains("taker"));
    EXPECT_FALSE(Cache::instance().get_file_owners("/usr/lib/libmoved.so.1").contains("migrator"));
    EXPECT_EQ(count_residue(), 0);
}
