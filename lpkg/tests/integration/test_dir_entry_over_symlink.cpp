/**
 * test_dir_entry_over_symlink.cpp — 目录 / 符号链接的类型变更语义（对齐 pacman）
 *
 * 现场事故：发行版布局 `/var/run -> ../run`，某包归档里带实体 `var/run/` 目录条目，
 * reinstall 后 `/var/run` 被换成实体目录（链接被 rename 进 stash、CLEANUP 删掉）。
 *
 * 采用 pacman 的语义（libalpm）：
 *   · 判定"盘上是什么"一律 **lstat**（不跟随末段）且**先剥尾斜杠**（pacman 为此写了
 *     `llstat()`，见 FS#51377 / commit `16b91f79`）—— 尾斜杠会把末尾链接解引用，
 *     让 `is_symlink` 恒假、"别动 symlink→目录"的守卫集体失效。
 *   · **symlink 一律算「非目录」**："We do not support treating symlinks to directories as
 *     directories. They are considered a file."（pacman-dev）
 *   · 类型不一致（归档目录 vs 盘上非目录、归档文件 vs 盘上真目录）→ **默认判冲突、整批
 *     中止**；只有"该路径由**本包旧版本**以另一形态持有"（文件→目录升级，TODO E4）才放行 ——
 *     pacman 的 "Check if the directory was a file in dbpkg"，`fileconflict00x.py` 钉的就是这条。
 *   · 删除侧：symlink 一律 unlink、绝不 rmdir 也不跟随（commit `b1e495b8`）；DB 记为文件键
 *     而盘上已是目录 → 拒绝（`--force` 才跳过，且仍然只跳过、不搬目录）。
 *
 * 本文件把这些边界钉死：该拒绝的必须拒绝（且**什么都没动**），该放行的必须放行。
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
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"

namespace fs = std::filesystem;

class DirEntryOverSymlinkTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;
    int seq = 0;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        Config::instance().set_no_hooks_mode(true);
        init_localization();
        BreakpointManager::instance().clear_all();

        suite_work_dir = fs::absolute("tmp_dir_entry_symlink_test");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    /** 打一个包：content 由 fill 回调填（相对 content/ 的路径） */
    template <typename F>
    std::string pack(const std::string& name, const std::string& ver, F fill)
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::create_directories(work / "content");
        fill(work / "content");
        const std::string path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(path, work.string(), name, ver, {}, {}, "man " + name, {});
        return path;
    }

    static void write_file(const fs::path& p, const std::string& content = "x\n")
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }

    /**
     * 跑一个"必须被拒绝"的动作，返回报错文本。
     *
     * **为什么不能只用 EXPECT_THROW**：那只证明"抛了个 LpkgException"，而"包压根没找到 /
     * 路径写错 / 别的什么原因也抛"同样能让它绿。冲突拒绝的判据是**报错点名了谁** ——
     * 冲突路径 + 真实持有者（无人持有时是"unknown (manual file)"），所以本文件里每一处
     * 拒绝都必须落到这两个锚点上（否则"拒绝"与"测试自己写错了"不可区分）。
     */
    template <typename F>
    static std::string refusal_message(F&& action, const char* what)
    {
        try {
            action();
        } catch (const LpkgException& e) {
            return e.what();
        } catch (const std::exception& e) {
            ADD_FAILURE() << what << "：抛的不是 LpkgException：" << e.what();
            return {};
        }
        ADD_FAILURE() << what << "：本该被拒绝（类型变更冲突必须整批中止），却成功了";
        return {};
    }

    /** 未清理的 stash 残留数（`.lpkg_bak_*`） */
    int count_bak_residue() const
    {
        int n = 0;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(test_root, ec)) {
            if (ec) break;
            if (e.path().filename().string().find(".lpkg_bak_") != std::string::npos) ++n;
        }
        return n;
    }

    /** 造出"包 A 拥有符号链接 var/run -> ../run"的真实盘面 + DB 状态 */
    std::string install_owner_of_var_run()
    {
        fs::create_directories(test_root / "run");
        const std::string a = pack("fslayout", "1.0", [&](const fs::path& c) {
            fs::create_directories(c / "var");
            fs::create_symlink("../run", c / "var" / "run");
        });
        install_packages({a});
        EXPECT_TRUE(fs::is_symlink(test_root / "var/run"));
        return a;
    }
};

// ============================================================================
// 安装侧：类型变更必须判冲突、整批中止，且不碰盘上任何东西
// ============================================================================

TEST_F(DirEntryOverSymlinkTest, DirEntryOverForeignSymlinkIsRefused)
{
    install_owner_of_var_run();  // var/run 归别的包所有（符号链接）

    const std::string b = pack("qemu-like", "1.0", [&](const fs::path& c) {
        fs::create_directories(c / "var" / "run");
        std::ofstream(c / "var" / "run" / "qemu.pid") << "1234\n";
    });

    const std::string msg =
        refusal_message([&] { install_packages({b}); }, "归档目录条目撞别的包持有的 symlink→目录");
    // 冲突判据是"点名了谁"：路径 + **真实持有者**（fslayout 那两个字符的差别，正是
    // "判对了冲突"与"测试自己写错路径"的区别）
    EXPECT_NE(msg.find("var/run"), std::string::npos) << "拒绝信息没点名冲突路径：" << msg;
    EXPECT_NE(msg.find("fslayout"), std::string::npos)
        << "拒绝信息必须点名真实持有者 fslayout：" << msg;

    // 什么都没动：链接在、目标目录在、包没装上、没有 stash 残留
    EXPECT_TRUE(fs::is_symlink(test_root / "var/run")) << "符号链接被替换/删除";
    EXPECT_TRUE(fs::is_directory(test_root / "run")) << "链接目标目录被删";
    EXPECT_FALSE(Cache::instance().is_installed("qemu-like"));
    EXPECT_EQ(count_bak_residue(), 0);
}

TEST_F(DirEntryOverSymlinkTest, DirEntryOverUnownedSymlinkIsRefused)
{
    // 无人持有的 symlink→目录（管理员手工建的 / 老版本 lpkg 留下的）同样判冲突：
    // pacman 的 "exists in filesystem"（其 CHECK 2 的闸门是 lstat 意义上的 S_ISDIR，
    // 符号链接不享受这个豁免）
    fs::create_directories(test_root / "var");
    fs::create_directories(test_root / "run");
    fs::create_directory_symlink("../run", test_root / "var/run");

    const std::string b = pack("pkg-a", "1.0", [&](const fs::path& c) {
        fs::create_directories(c / "var" / "run");
        std::ofstream(c / "var" / "run" / "app.pid") << "1\n";
    });

    const std::string msg =
        refusal_message([&] { install_packages({b}); }, "归档目录条目撞无人持有的 symlink→目录");
    EXPECT_NE(msg.find("var/run"), std::string::npos) << "拒绝信息没点名冲突路径：" << msg;
    EXPECT_NE(msg.find(get_string("error.unknown_manual_file")), std::string::npos)
        << "无人持有 → 判据文本必须是 " << get_string("error.unknown_manual_file") << "：" << msg;
    EXPECT_TRUE(fs::is_symlink(test_root / "var/run"));
    EXPECT_TRUE(fs::is_directory(test_root / "run"));
    EXPECT_EQ(count_bak_residue(), 0);
}

TEST_F(DirEntryOverSymlinkTest, DirEntryOverUnownedFileIsRefused)
{
    write_file(test_root / "var/run", "manual file\n");

    const std::string b = pack("pkg-b", "1.0", [&](const fs::path& c) {
        fs::create_directories(c / "var" / "run");
        std::ofstream(c / "var" / "run" / "app.pid") << "1\n";
    });

    const std::string msg =
        refusal_message([&] { install_packages({b}); }, "归档目录条目撞无人持有的盘上文件");
    EXPECT_NE(msg.find("var/run"), std::string::npos) << "拒绝信息没点名冲突路径：" << msg;
    EXPECT_NE(msg.find(get_string("error.unknown_manual_file")), std::string::npos)
        << "无人持有 → 判据文本必须是 " << get_string("error.unknown_manual_file") << "：" << msg;
    EXPECT_TRUE(fs::is_regular_file(test_root / "var/run")) << "手工文件被搬走/覆盖";
    EXPECT_EQ(count_bak_residue(), 0);
}

TEST_F(DirEntryOverSymlinkTest, FileEntryOverRealDirIsRefused)
{
    // pacman 的 case 5："not overwriting dir with file" —— --overwrite 也不放行
    write_file(test_root / "usr/share/thing/inner.txt");

    const std::string b = pack("pkg-c", "1.0", [&](const fs::path& c) {
        write_file(c / "usr" / "share" / "thing", "now a file\n");
    });

    const std::string msg =
        refusal_message([&] { install_packages({b}); }, "归档文件撞盘上真目录（不 force）");
    EXPECT_NE(msg.find("usr/share/thing"), std::string::npos) << "拒绝信息没点名冲突路径：" << msg;
    EXPECT_NE(msg.find(get_string("error.unknown_manual_file")), std::string::npos)
        << "手工建的目录无人持有 → 判据文本必须是 " << get_string("error.unknown_manual_file")
        << "：" << msg;
    EXPECT_TRUE(fs::is_directory(test_root / "usr/share/thing"));
    EXPECT_TRUE(fs::exists(test_root / "usr/share/thing/inner.txt")) << "目录里的内容被删";

    // `--force-overwrite` **也不能**放行这一条：pacman 文档原话 "Using --overwrite will not
    // allow overwriting a directory with a file"，其闸门对 lstat 意义上的目录不生效。
    // （此前的实现把**整类**类型变更都交给 force 豁免 —— 那样真目录会连同内容被删掉。）
    // 注意方向性：force 仍可放行反方向（归档目录接管盘上符号链接/文件），见
    // tests/integration/test_upgrade_rollback_fidelity.cpp 的
    // ForceOverwriteStillTakesOverForeignSymlinkDir。
    Config::instance().set_force_overwrite_mode(true);
    const std::string forced_msg =
        refusal_message([&] { install_packages({b}); }, "归档文件撞盘上真目录（force 也不该豁免）");
    EXPECT_NE(forced_msg.find("usr/share/thing"), std::string::npos)
        << "拒绝信息没点名冲突路径：" << forced_msg;
    EXPECT_NE(forced_msg.find(get_string("error.unknown_manual_file")), std::string::npos)
        << "force 下判据文本也必须是 " << get_string("error.unknown_manual_file") << "："
        << forced_msg;
    EXPECT_TRUE(fs::is_directory(test_root / "usr/share/thing"));
    EXPECT_TRUE(fs::exists(test_root / "usr/share/thing/inner.txt")) << "force 下目录内容被删";
    Config::instance().set_force_overwrite_mode(false);
}

// ============================================================================
// 安装侧：本包自己旧版本以另一形态持有 → 放行（pacman 的 dbpkg 豁免 = TODO E4）
// ============================================================================

TEST_F(DirEntryOverSymlinkTest, OwnedFileBecomesDirOnUpgrade)
{
    const std::string v1 = pack("evolve", "1.0", [&](const fs::path& c) {
        write_file(c / "usr/share/thing", "was a file\n");
    });
    install_packages({v1});
    ASSERT_TRUE(fs::is_regular_file(test_root / "usr/share/thing"));

    const std::string v2 = pack("evolve", "2.0", [&](const fs::path& c) {
        write_file(c / "usr/share/thing" / "inner.txt", "now a dir\n");
    });
    install_packages({v2});

    std::error_code ec;
    EXPECT_TRUE(fs::is_directory(test_root / "usr/share/thing", ec))
        << "文件→目录升级（E4）被误判成冲突";
    EXPECT_TRUE(fs::exists(test_root / "usr/share/thing/inner.txt"));
    EXPECT_EQ(count_bak_residue(), 0);
}

TEST_F(DirEntryOverSymlinkTest, OwnedSymlinkBecomesDirOnUpgrade)
{
    const std::string v1 = pack("evolve-sym", "1.0", [&](const fs::path& c) {
        write_file(c / "usr/share/target.txt");
        fs::create_symlink("target.txt", c / "usr/share/thing");
    });
    install_packages({v1});
    ASSERT_TRUE(fs::is_symlink(test_root / "usr/share/thing"));

    const std::string v2 = pack("evolve-sym", "2.0", [&](const fs::path& c) {
        write_file(c / "usr/share/target.txt");  // 新版仍持有链接目标，才能断言它没被动过
        write_file(c / "usr/share/thing" / "inner.txt", "now a dir\n");
    });
    install_packages({v2});

    std::error_code ec;
    EXPECT_TRUE(fs::is_directory(test_root / "usr/share/thing", ec))
        << "自己发的链接应当被实体目录取代";
    EXPECT_FALSE(fs::is_symlink(test_root / "usr/share/thing"));
    EXPECT_TRUE(fs::exists(test_root / "usr/share/thing/inner.txt"));
    EXPECT_TRUE(fs::exists(test_root / "usr/share/target.txt")) << "链接目标（另一个文件）不该被动";
    EXPECT_EQ(count_bak_residue(), 0);
}

// ============================================================================
// 删除侧
// ============================================================================

TEST_F(DirEntryOverSymlinkTest, RemoveRefusesStaleFileKeyPointingAtDirectory)
{
    // 盘上已是目录、DB 里仍记为本包的文件：默认拒绝整批（什么都不改）
    const std::string p = pack(
        "stale", "1.0", [&](const fs::path& c) { write_file(c / "usr/share/thing", "file\n"); });
    install_packages({p});

    fs::remove(test_root / "usr/share/thing");
    write_file(test_root / "usr/share/thing/other-pkg-data.txt", "keep me\n");

    const std::string msg =
        refusal_message([&] { remove_packages({"stale"}); }, "DB 文件键撞实体目录的移除");
    EXPECT_NE(msg.find("usr/share/thing"), std::string::npos) << "拒绝信息没点名冲突路径：" << msg;
    EXPECT_NE(msg.find(get_string("error.remove_path_is_dir_header")), std::string::npos)
        << "拒绝理由必须点名「陈旧文件键撞实体目录」这一类（而不是别的失败）：" << msg;
    EXPECT_TRUE(fs::exists(test_root / "usr/share/thing/other-pkg-data.txt"))
        << "拒绝时必须什么都没删";
    EXPECT_TRUE(Cache::instance().is_installed("stale")) << "拒绝时包仍是已安装状态";
}

TEST_F(DirEntryOverSymlinkTest, RemoveForceSkipsForeignDirectory)
{
    const std::string p = pack(
        "stale2", "1.0", [&](const fs::path& c) { write_file(c / "usr/share/thing", "file\n"); });
    install_packages({p});

    fs::remove(test_root / "usr/share/thing");
    write_file(test_root / "usr/share/thing/other-pkg-data.txt", "keep me\n");

    EXPECT_NO_THROW(remove_packages({"stale2"}, /*force=*/true));
    EXPECT_TRUE(fs::exists(test_root / "usr/share/thing/other-pkg-data.txt"))
        << "--force 也只跳过，绝不把别人的目录搬进 stash 再 remove_all";
    EXPECT_FALSE(Cache::instance().is_installed("stale2"));
    EXPECT_EQ(count_bak_residue(), 0);
}

TEST_F(DirEntryOverSymlinkTest, RemoveNeverRmdirsThroughDirSymlink)
{
    // DB 记的是目录键 `srv/`，盘上已被（管理员）换成 symlink→空目录：
    // 尾斜杠会让 is_symlink 恒假、rmdir 落在链接目标上 —— 必须既不删链接也不删目标
    const std::string p = pack("withdir", "1.0", [&](const fs::path& c) {
        fs::create_directories(c / "srv");
        write_file(c / "srv/app.log");
    });
    install_packages({p});
    ASSERT_TRUE(fs::is_directory(test_root / "srv"));

    fs::remove_all(test_root / "srv");
    fs::create_directories(test_root / "target");  // 空目录：老代码会把 rmdir 穿过去删掉它
    fs::create_directory_symlink("../target", test_root / "srv");

    remove_packages({"withdir"});

    EXPECT_TRUE(fs::is_symlink(test_root / "srv")) << "符号链接被删/被替换";
    EXPECT_TRUE(fs::is_directory(test_root / "target")) << "rmdir 穿过了尾斜杠，把链接目标删了";
    EXPECT_FALSE(Cache::instance().is_installed("withdir"));
    EXPECT_EQ(count_bak_residue(), 0);
}

// ============================================================================
// 备份目录名被符号链接占住：绝不写穿（否则备份跑到链接目标里、清理侧看不见）
// ============================================================================

TEST_F(DirEntryOverSymlinkTest, StashDirectorySymlinkIsRejected)
{
    const std::string v1 = pack("stashguard", "1.0",
                                [&](const fs::path& c) { write_file(c / "usr/bin/tool", "v1\n"); });
    install_packages({v1});
    // 注意用**升级**而不是再装一遍同版本：install_packages 对"已装的同版本"是 no-op，
    // 那种情况下压根不会走到备份阶段（一开始就是被这条误导，测试假红了一次）。
    const std::string v2 = pack("stashguard", "2.0",
                                [&](const fs::path& c) { write_file(c / "usr/bin/tool", "v2\n"); });

    // 备份目录名 = <root>/.lpkg_bak_<pkg>_<pid>（stash_parent_dir 走到 root_dir 顶层）
    const fs::path elsewhere = test_root / "elsewhere";
    fs::create_directories(elsewhere);
    fs::create_directory_symlink("elsewhere",
                                 test_root / (".lpkg_bak_stashguard_" + std::to_string(getpid())));

    // 升级 → 需要把已存在的文件搬进 stash → 必须报错而不是写穿链接
    const std::string msg =
        refusal_message([&] { install_packages({v2}); }, "升级时备份目录名被符号链接占住");
    EXPECT_NE(msg.find("stashguard"), std::string::npos)
        << "拒绝信息必须点名包名 stashguard（否则与'包没找到/路径写错'不可区分）：" << msg;
    EXPECT_NE(msg.find("usr/bin/tool"), std::string::npos)
        << "拒绝信息没点名要备份哪个文件：" << msg;
    std::error_code ec;
    EXPECT_TRUE(fs::is_empty(elsewhere, ec)) << "备份被写进了链接目标目录";
    EXPECT_TRUE(fs::is_regular_file(test_root / "usr/bin/tool"));
}

// ============================================================================
// 本包自己的"目录 → 文件"升级：**接管成功**（走"整棵树都是我们的"这条正路，与 --force
// 无关），且目录是带 WAL `BACKUP` 行搬进 stash 的 → 可回滚。
// ============================================================================
//
// 历史与订正（2026-09-25）：
//   这条路径**曾经**被无条件拒绝，理由写的是"pacman 的 case 5 对任何持有者都不放行"。
//   复核上游源码后这个理由**不成立**：`add.c` 那条 "extract: not overwriting dir with
//   file" 是**解压层**的无条件拒绝，而 `conflict.c` 的**冲突层**（`dir_belongsto_pkgs`）
//   在"目录里外全属于本包/本次要升级的包"时是**放行**的 —— pacman 先删旧包文件（含目录）
//   再解压，解压时那个路径已经不存在，所以撞不到 add.c 那道闸。
//
//   当年撞 EISDIR 崩溃的真实原因不是"不该放行"，而是**放行了却没先把目录让开**：第③步
//   `rename(.lpkgtmp → 该路径)` 撞 EISDIR；崩在事务中途后，回滚的 COPY 逆操作对**空目录**
//   `fs::remove` 会 rmdir 成功 → 盘面路径凭空消失、files.db 却仍声称持有它（且没有
//   BACKUP 行可还原）。现在第②步 `backup_existing_files` 会先把挡路的真目录搬进 stash
//   （写 `BACKUP` 行），路径让开后才写文件：崩溃点消失、回滚有依据、所有权不再脱节。
//
//   许可条件也比"本包持有"更严：`dir_tree_entirely_ours`（对齐 pacman 的
//   `dir_belongsto_pkgs`）要求**整棵子树**都只属于本包或本批次升级的包 —— 目录键是累加
//   持有者的，共享目录整树让开会搬走别人的文件。共享/无主目录仍被拒绝（见本文件
//   `FileEntryOverRealDirIsRefused` 与 `test_overwrite_globs.cpp` 的对应用例）。
TEST_F(DirEntryOverSymlinkTest, OwnDirReplacedByFileTakesOverWithWalBackup)
{
    const std::string v1 = pack("evolve-f2d", "1.0", [&](const fs::path& c) {
        fs::create_directories(c / "usr/share/thing");
    });
    install_packages({v1});
    ASSERT_TRUE(fs::is_directory(test_root / "usr/share/thing"));

    const std::string v2 = pack("evolve-f2d", "2.0", [&](const fs::path& c) {
        write_file(c / "usr/share/thing", "now a file\n");
    });

    // ① 回滚保真：注入"COPY 的 WAL 行已写、尚未 rename"处的失败 → 空目录必须被搬回来
    //    （这一条同时证明"目录确实是被 `sink.backup` 搬走的"——否则没有 BACKUP 行可逆）
    BreakpointManager::instance().set("copy_after_wal_evolve-f2d",
                                      [] { throw LpkgException("injected copy failure"); });
    EXPECT_THROW(install_packages({v2}), LpkgException);
    BreakpointManager::instance().clear_all();

    EXPECT_TRUE(fs::is_directory(test_root / "usr/share/thing")) << "回滚没把被搬走的目录还原回来";
    EXPECT_FALSE(fs::is_symlink(test_root / "usr/share/thing"));
    EXPECT_FALSE(fs::exists(test_root / "usr/share/thing.lpkgtmp")) << "留下半成品 .lpkgtmp";
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(test_root, ec))
        EXPECT_EQ(e.path().filename().string().find(".lpkg_bak_"), std::string::npos)
            << "回滚后残留备份: " << e.path();
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("evolve-f2d"), "1.0");

    // ② 干净升级：不带 --force 也要成功（走的是"整棵树都是我们的"，与 --overwrite 无关）
    ASSERT_NO_THROW(install_packages({v2})) << "本包自己的 dir→文件 应当接管成功";
    EXPECT_TRUE(fs::is_regular_file(test_root / "usr/share/thing"));
    EXPECT_FALSE(fs::is_directory(test_root / "usr/share/thing"));
    EXPECT_FALSE(fs::is_symlink(test_root / "usr/share/thing"));
    EXPECT_FALSE(fs::exists(test_root / "usr/share/thing.lpkgtmp")) << "留下半成品 .lpkgtmp";
    // 让开的那份进了 stash、提交后被清理 → 不残留
    for (const auto& e : fs::recursive_directory_iterator(test_root, ec))
        EXPECT_EQ(e.path().filename().string().find(".lpkg_bak_"), std::string::npos)
            << "提交后残留备份: " << e.path();
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("evolve-f2d"), "2.0");
}
