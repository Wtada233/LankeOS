/**
 * test_wal_newdir_symlink_guard.cpp — NEW_DIR 逆操作绝不穿过末尾符号链接 rmdir
 *
 * 缺陷：`fs::path p = op.arg1;`（写入侧 `OpSink::new_dir` 按**原样**记录归档条目形态，
 * 目录条目**恒带尾斜杠**），于是 `exists/is_directory/is_empty` 对末尾的符号链接**全部跟随**
 * （path_resolution(7)：尾斜杠要求前一分量是目录）——`/x/newdir/` 在 `newdir → <空目录>` 时
 * 三问全"是"，逆操作就此把一个**我们没建过的路径**当成"我们建的空目录"去删。同文件的
 * DIR_RM 分支与 OpSink::remove_empty_dir 都**先剥尾斜杠再判 is_symlink**，只有 NEW_DIR 漏了；
 * 修复即与之对齐（`strip_trailing_slash` + `!fs::is_symlink(p)`）。
 *
 * **实测边界（写测试时用真实 syscall 验过，务必先读）**：
 *   • 尾斜杠形态（①）在 Linux 上并不会真的删掉链接目标：`rmdir("link/")` 由**内核**拒绝
 *     （ENOTDIR，POSIX 特意这么定，正是为了防止"借尾斜杠删链接目标"），libstdc++ 的
 *     `fs::remove` 因此返回 false 且置 ec → 旧代码在这里是"想删但删不掉"的**侥幸**无害，
 *     审计行也不会写。这条用例钉的是**不变量**（不跟随、不谎报），不是"旧代码在此崩溃"。
 *   • 真正的行为差异在无尾斜杠形态（③）：旧代码 `fs::remove` 直接 unlink 掉**链接本身**
 *     （实测旧 wal_op.cpp 下 ③ 失败：链接消失），修复后一律不碰。
 *
 * 本文件钉住三条：
 *   ① 尾斜杠形态（= 真实写入形态）指向 symlink→空目录 → 链接与链接目标都不动，
 *      且**不写** RESTORE_DIR_RM 审计行（不谎报"删掉了我们建的目录"）；
 *   ② 同一形态指向**真**空目录 → 照旧删除，并写入 RESTORE_DIR_RM —— 与 ① 成对，
 *      否则"一律跳过带尾斜杠的路径"这种把正常路径一起挡掉的实现也能让 ① 变绿；
 *   ③ 路径本身是符号链接（写入侧只对目录条目发 NEW_DIR，我们从不创建符号链接，
 *      也就绝不该删除一个）→ 一律不处理；这条是修复前后的**唯一**可观测差异。
 *
 * 只用 wal::reverse_execute，不需要装包，故与 tests/unit/test_wal_core.cpp 的
 * ReverseExecuteNewDir 同属"直接驱动逆向引擎"的单元级用例。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/wal_op.hpp"

namespace fs = std::filesystem;

class WalNewDirSymlinkGuardTest : public ::testing::Test
{
protected:
    fs::path suite_dir;
    fs::path test_root;

    void SetUp() override
    {
        suite_dir = fs::absolute("tmp_wal_newdir_symlink_test");
        if (fs::exists(suite_dir)) fs::remove_all(suite_dir);
        test_root = suite_dir / "root";
        fs::create_directories(test_root);

        Config::instance().set_root_path(test_root.string());
        Config::instance().set_testing_mode(true);
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_dir);
    }

    std::string read_wal()
    {
        const std::string wpath = wal::wal_log_path();
        std::ifstream f(wpath);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    /**
     * 造一条 NEW_DIR 位移，arg1 形如 `/root/usr/share/newdir/`（**带尾斜杠**）。
     *
     * 尾斜杠不是随手加的：`OpSink::new_dir(physical_path)` 记录的是归档目录条目路径，
     * 本来就带尾斜杠（见 op_sink.hpp 的说明）。没有尾斜杠就复现不了这个缺陷。
     */
    wal::WALOp new_dir_op(const fs::path& dir_key)
    {
        wal::WALOp op = wal::parse_op("NEW_DIR dummy");
        op.arg1 = dir_key.string();
        EXPECT_EQ(op.type, wal::WALOpType::NEW_DIR);
        return op;
    }

    /** 建 symlink（fs::create_directory_symlink 无返回值，只能事后断言） */
    void make_dir_symlink(const fs::path& target, const fs::path& link)
    {
        fs::create_directories(link.parent_path());
        fs::create_directory_symlink(target, link);
        ASSERT_TRUE(fs::is_symlink(link));
    }
};

// ── ① 尾斜杠 + symlink→空目录：链接与目标都不动，也不写审计行 ──────────────
TEST_F(WalNewDirSymlinkGuardTest, TrailingSlashNewDirNeverRmdirsThroughSymlink)
{
    // 注：Linux 内核本身也拒绝 rmdir("link/")（ENOTDIR），所以旧代码在这条上并不会真的
    // 删到目标 —— 本用例钉的是"不跟随末尾链接、也不谎报"这个不变量（见文件头说明）。
    const fs::path target = test_root / "usr/share/other-pkg-dir";  // 别人的（空）目录
    const fs::path link = test_root / "usr/share/newdir";
    fs::create_directories(target);
    make_dir_symlink("other-pkg-dir", link);

    // 写入侧形态：目录键带尾斜杠
    std::vector<wal::WALOp> ops = {new_dir_op(fs::path(link.string() + "/"))};
    wal::reverse_execute(ops, /*write_audit=*/true);

    EXPECT_TRUE(fs::is_symlink(link)) << "符号链接被删掉了";
    EXPECT_TRUE(fs::is_directory(target))
        << "链接的**目标**目录被删了：尾斜杠让判定全跟随，删到了我们没建过的路径上";
    EXPECT_EQ(read_wal().find("RESTORE_DIR_RM"), std::string::npos)
        << "写了 RESTORE_DIR_RM 审计行：声称删掉了我们建的目录";
}

// ── ② 正向对照：同一形态指向真空目录 → 照旧删除（且审计行正常写） ──────────
TEST_F(WalNewDirSymlinkGuardTest, TrailingSlashNewDirStillRemovesRealEmptyDir)
{
    const fs::path dir = test_root / "usr/share/ours";
    fs::create_directories(dir);
    ASSERT_FALSE(fs::is_symlink(dir));

    std::vector<wal::WALOp> ops = {new_dir_op(fs::path(dir.string() + "/"))};
    wal::reverse_execute(ops, /*write_audit=*/true);

    EXPECT_FALSE(fs::exists(dir)) << "尾斜杠被剥过头了：真实空目录也该被删掉";
    EXPECT_NE(read_wal().find("RESTORE_DIR_RM"), std::string::npos)
        << "真删了目录却不写审计行，回滚链就断了";
}

// ── ③ 防御面：路径本身是符号链接（无论带不带尾斜杠）→ 一律不处理 ──────────
TEST_F(WalNewDirSymlinkGuardTest, NewDirOnSymlinkIsNeverRemovedInAnyForm)
{
    const fs::path target = test_root / "usr/share/real-target";
    const fs::path link = test_root / "usr/share/newdir2";
    fs::create_directories(target);
    make_dir_symlink("real-target", link);

    // 无尾斜杠（写入侧不会这样发，纯防御）：旧代码会把**链接本身**删掉
    std::vector<wal::WALOp> ops = {new_dir_op(link)};
    wal::reverse_execute(ops, /*write_audit=*/false);

    EXPECT_TRUE(fs::is_symlink(link)) << "符号链接被删：我们从不创建链接，也就绝不该删它";
    EXPECT_TRUE(fs::is_directory(target));
}
