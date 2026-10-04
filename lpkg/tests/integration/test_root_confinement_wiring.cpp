/**
 * test_root_confinement_wiring.cpp — `detail::confine_target_path()` 的**生产接线**
 *
 * 2026-10-03 给"落位 / 让开目标的**祖先链**"加了约束：`detail::confine_target_path()`
 * （定义在 `install_common.cpp`），两个生产调用点 —— 写入趟
 * `installation_task_copy.cpp:copy_package_files()`、让开趟
 * `installation_task_letgo.cpp:backup_existing_files()`。此前用的是裸 `root_dir()/rel`，
 * 它只约束**词法**归属，而 `fs::copy` / `rename` / `create_symlink` 会**跟随中间段符号链接**
 * ⇒ `--root <R>` 下一条 `<R>/usr/lib -> /outside` 的链接，会让后续内容穿过它落到 root 之外。
 *
 * 判据本体另处已有单测；本文件证明**生产路径真的走了它**（单测不证明接线）。
 * 关键判据（见 install_common.hpp）：只解析**父目录**、末段不解析、解不开放行。
 *
 * ── 可达性（读码 + 推理，务必连同用例注释一起看）──
 * 要让 `confine_target_path` 成为**唯一**把关者，文件冲突预检必须先放行。而"归档**目录**
 * 条目撞别的包持有的**符号链接**"在预检里是判冲突的（见
 * tests/integration/test_dir_entry_over_symlink.cpp 的 `DirEntryOverForeignSymlinkIsRefused`）。
 * 因此可构造三条链：
 *   · 用例 1（A/B 两包）：靠 `--force-overwrite` 豁免预检里的"目录条目接管外来符号链接"
 *     （`collect_content_conflicts` 的 `force_exempts = entry_is_dir`）—— 这是审计给出的链；
 *   · 用例 2（**同包升级**：v1 发链接、v2 在链接之下发文件）：靠 pacman 的 E4 豁免
 *     （`ours_exempts`，本包旧版本以非目录形态持有）天然放行预检，**不需要 --force**，
 *     且**没有任何其他闸**能解释拒绝 ⇒ 最强的接线证据；
 *   · 用例 3：**不加 force** 时 A/B 那条链实际被**冲突预检**拦下 —— 诚实钉住"是哪一道闸"，
 *     免得把"被预检拒绝"误当成"接线生效"。
 *
 * ── 断言的可区分性 ──
 * 把两个调用点改回裸 `root_dir()/rel`，用例 1/2 会**绿转红**：不再抛 `install_escape_root`，
 * 安装会**成功**（让开趟会把挡路的符号链接先搬走，见下）。所以主判据是"抛且报错是
 * `install_escape_root`"。
 *
 * ⚠️ **"root 之外目录没多出东西"这条断言不具区分力**（照实说）：让开趟对这个链接的
 * 目录条目会走 `StashAndMkDir`（把链接 rename 进 stash、再建真目录），**在任何实现下都不会
 * 真写到外面**。它只作补充的不变量陈述，不作主判据。
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

namespace
{
/** 取 l10n 模板里最长的一段**字面**文本（`{}` 之间）—— 用作与语言无关的断言锚。 */
std::string longest_template_literal(const std::string& tmpl)
{
    std::string needle;
    for (size_t b = 0; b <= tmpl.size();) {
        const size_t e = tmpl.find("{}", b);
        const size_t end = (e == std::string::npos) ? tmpl.size() : e;
        if (end - b > needle.size()) needle = tmpl.substr(b, end - b);
        if (e == std::string::npos) break;
        b = e + 2;
    }
    return needle;
}
}  // namespace

class RootConfinementWiringTest : public IntegrationTestBase
{
protected:
    /** 打一个包：content 由 fill 回调填（相对 content/ 的路径）。 */
    template <typename F>
    std::string pack(const std::string& name, const std::string& ver, F fill)
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::remove_all(work);
        fs::create_directories(work / "content");
        fill(work / "content");
        const std::string path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(path, work.string(), name, ver, {}, {}, {}, "man " + name, {});
        return path;
    }

    static void write_file(const fs::path& p, const std::string& content = "x\n")
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }

    /** 跑一次"本该被拒绝"的安装并取回报错文本（只断言 THROW 无法区分"哪一道闸"）。 */
    template <typename F>
    static std::string install_refusal(F&& action)
    {
        try {
            action();
        } catch (const LpkgException& e) {
            return e.what();
        } catch (const std::exception& e) {
            ADD_FAILURE() << "安装抛的不是 LpkgException：" << e.what();
            return {};
        }
        ADD_FAILURE() << "本该被拒绝（内容会落到 root 之外）却成功了";
        return {};
    }

    /** `install_escape_root` 与 `file_conflict_header` 的长字面锚（区分"哪一道闸"）。 */
    static const std::string& escape_anchor()
    {
        static const std::string a = [] {
            const std::string tmpl = get_string("error.install_escape_root");
            return longest_template_literal(tmpl);
        }();
        return a;
    }
    static const std::string& conflict_anchor()
    {
        static const std::string a = [] {
            const std::string tmpl = get_string("error.file_conflict_header");
            return longest_template_literal(tmpl);
        }();
        return a;
    }
};

// ============================================================================
// 用例 1：A 发符号链接 usr/lib -> <root 外真目录>，B 在链接之下发普通文件；
//         用 --force-overwrite 让预检放行 → 只剩 confine_target_path 能拦。
// ============================================================================
TEST_F(RootConfinementWiringTest, ForceOverwriteInstallThroughForeignSymlinkAncestorIsRefused)
{
    const fs::path outside = suite_work_dir / "outside_dir";
    fs::create_directories(outside);

    // ── 装 A：内容含一条**绝对目标**指向 root 之外的符号链接 `usr/lib` ──
    const std::string a = pack("cfn_a", "1.0", [&](const fs::path& c) {
        write_file(c / "usr" / "share" / "cfn_a.txt");  // 让包非空（同时确保 content/usr 存在）
        fs::create_symlink(outside.string(), c / "usr" / "lib");
    });
    ASSERT_NO_THROW(install_packages({a})) << "A 只发一条合法的绝对目标链接，安装必须成功";
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("cfn_a")) << "前置：A 应已安装";

    // 前置（自检）：链接确实建在盘上、指向 root 之外的那个真目录、且目录此刻是空的。
    // 这几条不成立时用例会以别的原因假绿/假红。
    ASSERT_TRUE(fs::is_symlink(test_root / "usr" / "lib")) << "前置：A 应已建出 usr/lib 链接";
    ASSERT_EQ(fs::read_symlink(test_root / "usr" / "lib").string(), outside.string())
        << "前置：链接目标应是 root 之外的那个目录";
    ASSERT_TRUE(fs::is_empty(outside)) << "前置：root 之外的目录此刻应为空";

    // ── 装 B：在链接之下发一个普通文件 usr/lib/libb.so ──
    const std::string b =
        pack("cfn_b", "1.0", [&](const fs::path& c) { write_file(c / "usr" / "lib" / "libb.so"); });

    // 不加 force 时 B 会被**冲突预检**拦下（见用例 3）。加 force 豁免那条后，
    // 预检与逐包冲突检查都放行，剩下唯一能拦的闸就是 confine_target_path。
    Config::instance().set_force_overwrite_mode(true);
    const std::string msg = install_refusal([&] { install_packages({b}); });
    Config::instance().set_force_overwrite_mode(false);

    // ① 主判据：报错是 install_escape_root（不是冲突、不是别的）
    ASSERT_GE(escape_anchor().size(), 8u);
    EXPECT_NE(msg.find(escape_anchor()), std::string::npos)
        << "应报 error.install_escape_root（说明生产路径走了 confine_target_path）：\n"
        << msg;
    EXPECT_EQ(msg.find(conflict_anchor()), std::string::npos)
        << "不该是文件冲突预检的报错（那就证明不了接线）：\n"
        << msg;
    // ② 报错点名**相对路径**与 root（CLAUDE.md §8 第 7 条）
    EXPECT_NE(msg.find("usr/lib"), std::string::npos) << "报错没点名越界的相对路径：\n" << msg;
    EXPECT_NE(msg.find(test_root.string()), std::string::npos) << "报错没点名 root：\n" << msg;

    // ③ 补充不变量：root 之外什么也没多出来（**不具区分力**，见文件头说明）
    std::error_code ec;
    EXPECT_FALSE(fs::exists(outside / "libb.so", ec)) << "文件落到了 root 之外";
    EXPECT_TRUE(fs::is_empty(outside, ec)) << "root 之外的目录被写入了内容";

    // ④ 整批回滚：B 未安装，A 完好、链接仍在
    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("cfn_b")) << "拒绝后 B 不该在册";
    EXPECT_TRUE(Cache::instance().is_installed("cfn_a")) << "拒绝后 A 不该被动";
    EXPECT_TRUE(fs::is_symlink(test_root / "usr" / "lib")) << "拒绝后 A 的链接被动了";
}

// ============================================================================
// 用例 2：**同包升级** —— v1 发符号链接 usr/lib -> <root 外>，v2 在它之下发文件。
//         不需要 --force（预检靠 E4 豁免），除 confine 之外没有别的闸能解释拒绝。
// ============================================================================
TEST_F(RootConfinementWiringTest, SelfUpgradeThroughSymlinkAncestorIsRefused)
{
    const fs::path outside = suite_work_dir / "outside_dir2";
    fs::create_directories(outside);

    const std::string v1 = pack("cfn_self", "1.0", [&](const fs::path& c) {
        write_file(c / "usr" / "share" / "cfn_self.txt");
        fs::create_symlink(outside.string(), c / "usr" / "lib");
    });
    ASSERT_NO_THROW(install_packages({v1}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("cfn_self")) << "前置：v1 应已安装";
    ASSERT_TRUE(fs::is_symlink(test_root / "usr" / "lib")) << "前置：v1 应已建出 usr/lib 链接";
    ASSERT_TRUE(fs::is_empty(outside));

    // v2：旧链接变成真目录 + 一个文件（pacman 的 E4：本包旧版本以非目录形态持有 → 放行预检）
    const std::string v2 = pack("cfn_self", "2.0", [&](const fs::path& c) {
        write_file(c / "usr" / "share" / "cfn_self.txt");
        write_file(c / "usr" / "lib" / "libself.so");
    });

    const std::string msg = install_refusal([&] { install_packages({v2}); });

    ASSERT_GE(escape_anchor().size(), 8u);
    EXPECT_NE(msg.find(escape_anchor()), std::string::npos)
        << "同包升级走符号链接祖先时应报 error.install_escape_root：\n"
        << msg;
    EXPECT_EQ(msg.find(conflict_anchor()), std::string::npos) << msg;
    EXPECT_NE(msg.find("usr/lib"), std::string::npos) << msg;
    EXPECT_NE(msg.find(test_root.string()), std::string::npos) << msg;

    std::error_code ec;
    EXPECT_TRUE(fs::is_empty(outside, ec)) << "root 之外的目录被写入了内容";

    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("cfn_self"), "1.0")
        << "拒绝后应整批回滚、仍是 v1";
    EXPECT_TRUE(fs::is_symlink(test_root / "usr" / "lib")) << "拒绝后 v1 的链接被动了";
}

// ============================================================================
// 用例 3：**不加 force** 时，A/B 那条链实际被**文件冲突预检**拦下（不是 confine）。
//
// 这不是"接线生效"的证据 —— 它证明的是"审计给出的那条链需要 --force 才够得着 confine"。
// 钉下来，免得日后有人拿"B 被拒绝了"当成接线被测到了（那是预检的功劳）。
// ============================================================================
TEST_F(RootConfinementWiringTest, WithoutForceForeignSymlinkAncestorIsRefusedByConflictGate)
{
    const fs::path outside = suite_work_dir / "outside_dir3";
    fs::create_directories(outside);

    const std::string a = pack("cfn_c", "1.0", [&](const fs::path& c) {
        write_file(c / "usr" / "share" / "cfn_c.txt");
        fs::create_symlink(outside.string(), c / "usr" / "lib");
    });
    ASSERT_NO_THROW(install_packages({a}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed("cfn_c")) << "前置：A 应已安装";
    ASSERT_TRUE(fs::is_symlink(test_root / "usr" / "lib"));

    const std::string b =
        pack("cfn_d", "1.0", [&](const fs::path& c) { write_file(c / "usr" / "lib" / "libd.so"); });

    const std::string msg = install_refusal([&] { install_packages({b}); });

    // 报错来自**冲突预检**：点名冲突路径与真实持有者，而**不是** install_escape_root。
    EXPECT_NE(msg.find(conflict_anchor()), std::string::npos)
        << "不加 force 时应由文件冲突预检拦下：\n"
        << msg;
    EXPECT_EQ(msg.find(escape_anchor()), std::string::npos)
        << "这条链在预检就被拦了，走不到 confine_target_path：\n"
        << msg;
    EXPECT_NE(msg.find("usr/lib"), std::string::npos) << msg;
    EXPECT_NE(msg.find("cfn_c"), std::string::npos) << "冲突报告应点名真实持有者 cfn_c：\n" << msg;

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("cfn_d"));
    EXPECT_TRUE(fs::is_symlink(test_root / "usr" / "lib"));
}
