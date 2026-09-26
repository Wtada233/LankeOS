/**
 * test_wal_confinement.cpp — `reverse_execute` 的**路径 confinement**（纵深防御）
 *
 * ── 防的是什么 ──────────────────────────────────────────────────────────────
 * `reverse_execute` 按 WAL 行里的**字面路径**直接 rename/remove/chmod。输入端已经堵死
 * （归档成员名/包名/版本号都消毒，见 `installation_task.cpp` 的说明），要触发得先能写 WAL
 * 文件 —— 那已经是 root。所以这是**纵深防御**，不是可达漏洞：万一 WAL 被别的工具/编辑器
 * 改过，回滚不许照着行里的路径去动系统上任意位置的东西。
 *
 * 判据（`main/src/db/wal_op.cpp` 的 `wal_line_paths_confined`）：涉及的每个路径都必须落在
 * `Config::instance().root_dir()` 之内，或落在**已知的 stash 根**之内；不满足 → **告警 +
 * 跳过该行**（与"bak 不存在 → 跳过"同一个保守方向）。**绝不因此让恢复失败** —— 恢复失败
 * 意味着每次启动都重试、所有 lpkg 命令起不来。
 *
 * ── 本文件钉的三件事 ────────────────────────────────────────────────────────
 *   ① 越界的行被跳过、且**真的什么都没动**（词法逃逸 `../`、绝对路径指向 root 外、
 *      以及"root 内的符号链接指向 root 外"这种词法合法、实际穿透的形状）；
 *   ② 合法的行**一条都不许被拒**（root 内的 BACKUP/RESTORE、分量级前缀的边界
 *      `/a/bc` vs `/a/b`、以及 ELOOP 自环 —— canonical 对环恒失败，绝不能据此判越界）；
 *   ③ 跳过的只是**越界那一行**，同批次里合法的行照旧执行。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

namespace
{
constexpr const char* ARROW = " \xe2\x86\x92 ";
}  // namespace

class WalConfinementTest : public IntegrationTestBase
{
protected:
    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        IntegrationTestBase::TearDown();
    }

    static void write_file(const fs::path& p, const std::string& content)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }

    static std::string read_file(const fs::path& p)
    {
        std::ifstream f(p);
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    /// root 之外的"受害文件"：`test_root` 的同级目录（`suite_work_dir` 不在 root 里）
    fs::path outside(const std::string& name) const
    {
        return suite_work_dir / name;
    }
};

// ============================================================================
// 1) 绝对路径指向 root 之外 → 跳过（原位那份什么都不该发生）
// ============================================================================

TEST_F(WalConfinementTest, AbsolutePathOutsideRootIsSkipped)
{
    const fs::path victim = outside("victim_abs.txt");
    const fs::path stash = test_root / ".lpkg_bak_pkgx_1";
    const fs::path bak = stash / "victim.txt.lpkg_bak_pkgx_1";
    write_file(victim, "OUTSIDE\n");
    write_file(bak, "BACKED-UP\n");  // 这条行的 arg1 在 root 外，arg2 在 root 内

    std::vector<wal::WALOp> ops;
    ops.push_back(wal::parse_op("BACKUP " + victim.string() + ARROW + bak.string()));
    const wal::RollbackStats st = wal::reverse_execute(ops, /*write_audit=*/false);

    EXPECT_EQ(st.files_restored, 0) << "越界行必须被跳过（不得按字面路径去动 root 外的文件）";
    EXPECT_TRUE(fs::exists(victim)) << "root 外的文件必须原样存在";
    EXPECT_EQ(read_file(victim), "OUTSIDE\n");
    EXPECT_TRUE(fs::exists(bak)) << "跳过 = 什么都不做（备份也留在 stash 里，等人工处置）";
}

// ============================================================================
// 2) `../` 词法逃逸 → 跳过
// ============================================================================

TEST_F(WalConfinementTest, DotDotEscapeIsSkipped)
{
    const fs::path victim = outside("victim_dotdot.txt");
    const fs::path escape = test_root / ".." / "victim_dotdot.txt";  // 词法上逃出 root
    const fs::path stash = test_root / ".lpkg_bak_pkgx_1";
    const fs::path bak = stash / "v.lpkg_bak_pkgx_1";
    write_file(victim, "OUTSIDE\n");
    write_file(bak, "BACKED-UP\n");

    std::vector<wal::WALOp> ops;
    ops.push_back(wal::parse_op("BACKUP " + escape.string() + ARROW + bak.string()));
    wal::reverse_execute(ops, false);

    EXPECT_TRUE(fs::exists(victim)) << "`../` 逃逸的路径必须被规范化后判为越界";
    EXPECT_EQ(read_file(victim), "OUTSIDE\n");
    EXPECT_TRUE(fs::exists(bak));
}

// ============================================================================
// 3) root 内的符号链接指向 root 外（词法合法、实际穿透）→ 跳过
// ============================================================================

TEST_F(WalConfinementTest, SymlinkEscapeIsSkipped)
{
    const fs::path victim = outside("victim_link.txt");
    write_file(victim, "OUTSIDE\n");
    // root 内造一条指向 root 外的链接：`<root>/evil -> <suite_work_dir>`
    const fs::path link = test_root / "evil";
    std::error_code ec;
    fs::create_directory_symlink(suite_work_dir, link, ec);
    ASSERT_FALSE(ec) << "造链接失败：" << ec.message();

    std::vector<wal::WALOp> ops;
    ops.push_back(wal::parse_op("NEW " + (link / "victim_link.txt").string()));
    wal::reverse_execute(ops, false);

    EXPECT_TRUE(fs::exists(victim))
        << "词法在 root 内、canonical 穿透到 root 外的路径必须被跳过（否则任意删除）";
    EXPECT_EQ(read_file(victim), "OUTSIDE\n");
}

// ============================================================================
// 4) 合法的行**一条都不能被拒**：root 内的行照旧执行
// ============================================================================

TEST_F(WalConfinementTest, LegitimateInRootLinesStillExecute)
{
    const fs::path orig = test_root / "usr" / "bin" / "tool";
    const fs::path stash = test_root / ".lpkg_bak_pkgx_1";
    const fs::path bak = stash / "tool.lpkg_bak_pkgx_1";
    write_file(bak, "OLD\n");
    const fs::path new_file = test_root / "usr" / "bin" / "fresh";
    write_file(new_file, "NEW\n");
    const fs::path dead_dir = test_root / "usr" / "share" / "dead";
    fs::create_directories(dead_dir);
    fs::remove(dead_dir);

    std::vector<wal::WALOp> ops;
    ops.push_back(wal::parse_op("BACKUP " + orig.string() + ARROW + bak.string()));
    ops.push_back(wal::parse_op("NEW " + new_file.string()));
    ops.push_back(wal::parse_op("DIR_RM " + dead_dir.string() + " 493 0 0"));
    const wal::RollbackStats st = wal::reverse_execute(ops, false);

    EXPECT_EQ(read_file(orig), "OLD\n") << "root 内的 BACKUP 逆操作照旧";
    EXPECT_FALSE(fs::exists(new_file)) << "root 内的 NEW 逆操作照旧（删掉本批次新建的）";
    EXPECT_EQ(st.dirs_recreated, 1) << "root 内的 DIR_RM 逆操作照旧（重建目录）";
    EXPECT_TRUE(fs::is_directory(dead_dir));
}

// ============================================================================
// 5) 分量级前缀的边界：`/a/bc` **不是** `/a/b` 的子路径
// ============================================================================

TEST_F(WalConfinementTest, SiblingPrefixIsNotInsideRoot)
{
    // 与 root 同前缀的兄弟目录（`<root>x`）不该被当成"在 root 内"
    const fs::path sibling = suite_work_dir / (test_root.filename().string() + "_sibling");
    fs::create_directories(sibling);
    const fs::path victim = sibling / "victim.txt";
    write_file(victim, "SIBLING\n");
    const fs::path stash = test_root / ".lpkg_bak_pkgx_1";
    const fs::path bak = stash / "v.lpkg_bak_pkgx_1";
    write_file(bak, "BACKED-UP\n");

    std::vector<wal::WALOp> ops;
    ops.push_back(wal::parse_op("BACKUP " + victim.string() + ARROW + bak.string()));
    wal::reverse_execute(ops, false);

    EXPECT_TRUE(fs::exists(victim)) << "同前缀的兄弟路径必须判为越界（按分量比，不按字符串比）";
    EXPECT_EQ(read_file(victim), "SIBLING\n");
}

// ============================================================================
// 6) ELOOP 自环：canonical 失败**不**能变成"判越界"
// ============================================================================

TEST_F(WalConfinementTest, LoopPathIsNotRejected)
{
    const fs::path orig = test_root / "usr" / "share" / "selfloop";
    const fs::path stash = test_root / ".lpkg_bak_pkgx_1";
    const fs::path bak = stash / "selfloop.lpkg_bak_pkgx_1";
    fs::create_directories(stash);
    std::error_code ec;
    fs::create_symlink("selfloop", bak, ec);  // 被备份的那份本身就是自环（selfloop -> selfloop）
    ASSERT_FALSE(ec) << "造自环失败：" << ec.message();

    std::vector<wal::WALOp> ops;
    ops.push_back(wal::parse_op("BACKUP " + orig.string() + ARROW + bak.string()));
    const wal::RollbackStats st = wal::reverse_execute(ops, false);

    EXPECT_EQ(st.files_restored, 1)
        << "自环路径解不开（canonical 失败）绝不能因此被跳过 —— 那份备份必须能搬回原位"
           "（test_symlink_loop_install.cpp 钉着这条语义）";
    EXPECT_TRUE(fs::is_symlink(orig)) << "搬回来的必须还是那条自环（形态逐项保真）";
    // 环上 `fs::exists` 会抛（ELOOP），用 lstat 语义的谓词判"这个名字还占着没有"
    EXPECT_FALSE(exists_no_follow(bak)) << "stash 里那份已被搬走（备份已消费）";
}

// ============================================================================
// 7) 只跳过越界的那一行：同批次里合法的行照旧执行
// ============================================================================

TEST_F(WalConfinementTest, OnlyTheEscapingLineIsSkipped)
{
    const fs::path victim = outside("victim_mixed.txt");
    write_file(victim, "OUTSIDE\n");
    const fs::path good_orig = test_root / "usr" / "share" / "good";
    const fs::path stash = test_root / ".lpkg_bak_pkgx_1";
    const fs::path good_bak = stash / "good.lpkg_bak_pkgx_1";
    write_file(good_bak, "GOOD-OLD\n");

    std::vector<wal::WALOp> ops;
    ops.push_back(wal::parse_op("BACKUP " + victim.string() + ARROW + stash.string() +
                                "/victim.lpkg_bak_pkgx_1"));
    ops.push_back(wal::parse_op("BACKUP " + good_orig.string() + ARROW + good_bak.string()));
    const wal::RollbackStats st = wal::reverse_execute(ops, false);

    EXPECT_EQ(st.files_restored, 1) << "越界的那条被跳过、合法的这条照旧执行";
    EXPECT_EQ(read_file(good_orig), "GOOD-OLD\n");
    EXPECT_TRUE(fs::exists(victim)) << "root 外那份一个字节都不许动";
    EXPECT_EQ(read_file(victim), "OUTSIDE\n");
}

// ============================================================================
// 8) stash 根也算允许范围（备份侧的目标）
// ============================================================================

TEST_F(WalConfinementTest, StashRootIsAllowedTarget)
{
    // 合成一条"备份落在 root 内、原位在 root 外"的**畸形**行：两侧任一越界都应整条跳过，
    // 而反过来（原位在 root 内、备份在 stash 内）必须照旧执行 —— 后者由上面第 4 条覆盖。
    // 这里钉的是"stash 根被显式纳入允许范围"不会把 root 外当成合法原位。
    const fs::path victim = outside("victim_stash.txt");
    write_file(victim, "OUTSIDE\n");
    const fs::path stash = test_root / ".lpkg_bak_pkgx_1";
    const fs::path bak = stash / "victim.lpkg_bak_pkgx_1";
    write_file(bak, "BACKED-UP\n");

    std::vector<wal::WALOp> ops;
    ops.push_back(wal::parse_op("UNSTASH " + bak.string() + ARROW + victim.string()));
    const wal::RollbackStats st = wal::reverse_execute(ops, false);

    EXPECT_EQ(st.files_restored, 0) << "原位在 root 外 → 整条跳过";
    EXPECT_TRUE(fs::exists(victim));
    EXPECT_EQ(read_file(victim), "OUTSIDE\n");
    EXPECT_TRUE(fs::exists(bak));
}

// ============================================================================
// 9) 末段是"绝对目标符号链接"时**不判越界**（canonical 只解析父目录）
// ============================================================================

/**
 * `--root` 安装里包可以发绝对目标的符号链接（`<root>/usr/bin/abs -> /etc/abs`）。判据若解析
 * **末段**，它在宿主上会落到 root 之外 —— 而回滚对这条路径做的是"删掉那个链接本身"
 * （`rename`/`unlink` **不跟随末段链接**），与链接指向哪里无关。误判会让这类链接在回滚后
 * 留下来（盘面 != 批次前）。这条钉子钉住"只解析父目录"的选择。
 */
TEST_F(WalConfinementTest, AbsoluteSymlinkFinalComponentIsNotAnEscape)
{
    const fs::path link = test_root / "usr" / "bin" / "abs";
    fs::create_directories(link.parent_path());
    std::error_code ec;
    fs::create_symlink("/etc/abs", link, ec);  // 绝对目标（指向**目标系统**的 /etc/abs）
    ASSERT_FALSE(ec) << "造链接失败：" << ec.message();

    std::vector<wal::WALOp> ops;
    ops.push_back(wal::parse_op("NEW " + link.string()));
    const wal::RollbackStats st = wal::reverse_execute(ops, false);

    EXPECT_EQ(st.files_cleaned, 1)
        << "末段是绝对目标链接不该被当成越界 —— 逆操作删的是链接本身（不跟随末段）";
    EXPECT_FALSE(exists_no_follow(link)) << "本批次新建的链接必须被撤销";
}
