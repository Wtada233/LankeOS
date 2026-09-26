/**
 * test_symlink_loop_paths.cpp —— 符号链接环（ELOOP）不得把判定类调用变成异常
 *
 * 缺口（CLAUDE.md §3「符号链接环（ELOOP）使包永久无法卸载」）：
 * `std::filesystem` 的**判定类**调用在"末段符号链接解不开"时不是判 not-found，而是**抛**
 * `filesystem_error`。这条断言不靠记忆 —— 本文件的 Premise* 用例就是它的实测复现
 * （2026-09-25，libstdc++）：
 *
 *     fs::exists(loop)         → 抛 filesystem_error code=40 (ELOOP)
 *     fs::is_directory(loop)   → 抛 code=40
 *     fs::is_empty(loop)       → 抛 code=40
 *     fs::is_regular_file(loop)→ 抛 code=40
 *     fs::is_symlink(loop)     → 不抛（走 lstat）—— **但只在末段就是环时**：中间段成环
 *                                 （`self/x`、`self/x/`）时它照样抛 code=40（2026-09-26 补测，
 *                                 见 PremiseMiddleComponentLoopAlsoThrowsIsSymlink）
 *     directory_entry::is_directory()（迭代器条目）→ 对 symlink 会走 status()，**抛**
 *
 * 于是一个无害的环（自环、两跳环、上游包自带的环）能让"这路径归谁 / 该不该删 / 这个
 * 目录空不空"的判定把事务打断：卸载/升级/恢复在预检或删除阶段抛错 → 整批回滚 →
 * 那个包再也卸不掉。
 *
 * 修法：`base/utils.{hpp,cpp}` 新增**不抛**的谓词族（exists_no_follow / exists_follow /
 * is_directory_follow / is_real_directory / **is_symlink_no_follow** /
 * **is_regular_file_no_follow** —— 后两个是 2026-09-26 补的，因为"中间段成环"那一片
 * 是靠 `fs::is_symlink`/`fs::is_regular_file` 判的，而它们对那种形态会抛）。
 * 本文件钉住它们的语义与在本仓库里的落点。
 * **逐个调用点判断**，不做全局替换：`fs::read_symlink`/`fs::canonical` 这类**取值**调用在
 * 不可达时就该失败。
 *
 * 卸载路径上的同类漏点（`pkg/package_manager.cpp` 的 `do_remove_package()` 阶段 A 原为
 * `if (fs::exists(phys) || fs::is_symlink(phys))`）已改用不抛谓词。
 *
 * ⚠️ **订正 2026-09-26**：本行原写"并由其用 ec 重载修掉"——**那句话当时并不成立**，
 * `fs::exists(p, ec) || fs::is_symlink(p)` 里的右操作数是**抛型**，在**中间段**成环时必被
 * 求值、必抛（见上面 Premise 用例）。真正修好它的是"换成不抛谓词"，不是"换成 ec 重载"。
 * 端到端覆盖见文件末尾 RemovePackageFormingSymlinkLoopWithAnother（**硬断言**，不是 SKIP）。
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/install_common.hpp"
#include "../../main/src/pkg/package_manager.hpp"

namespace fs = std::filesystem;

namespace
{
/// 在 1 秒内能穷举完的"会不会抛"谓词包装
template <class F>
bool throws_fs_error(F&& f)
{
    try {
        (void)f();
        return false;
    } catch (const fs::filesystem_error&) {
        return true;
    }
}
}  // namespace

class SymlinkLoopPathsTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path root;
    fs::path probe_dir;  // 放环的地方（测试自己造，不属于任何包）

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_symlink_loop_test");
        fs::remove_all(suite_work_dir);
        root = suite_work_dir / "root";
        probe_dir = suite_work_dir / "probe";
        fs::create_directories(root);
        fs::create_directories(probe_dir);

        Config::instance().set_root_path(root.string());
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    /// 自环：`self -> self`
    fs::path make_self_loop(const fs::path& dir, const std::string& name = "self")
    {
        const fs::path p = dir / name;
        fs::create_symlink(name, p);
        return p;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  ① 前提实测：判定类调用在 ELOOP 上**抛**（本文件所有修复的立论基础）
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(SymlinkLoopPathsTest, PremiseThrowingPredicatesThrowOnLoop)
{
    const fs::path loop = make_self_loop(probe_dir);

    EXPECT_TRUE(throws_fs_error([&] { return fs::exists(loop); }))
        << "fs::exists 对符号链接环必须抛（判定类调用的漏判就是这个缺口）";
    EXPECT_TRUE(throws_fs_error([&] { return fs::is_directory(loop); }));
    EXPECT_TRUE(throws_fs_error([&] { return fs::is_empty(loop); }));
    EXPECT_TRUE(throws_fs_error([&] { return fs::is_regular_file(loop); }));

    // lstat 语义的调用**不**抛 —— 这是"名字被占"能被安全判定的原因
    EXPECT_FALSE(throws_fs_error([&] { return fs::is_symlink(loop); }));
    EXPECT_TRUE(fs::is_symlink(loop));
}

/**
 * ⚠️ **前提的补全（2026-09-26 新增，本轮才被发现）**：上面那条"`fs::is_symlink` 不抛"
 * **只在末段就是那个环时成立**。**中间段**成环时它照样抛，而且那才是真正危险的一种：
 * 判定"有没有环"的那个调用自己先炸了。
 *
 * 缺这条前提的后果（实测）：整片"中间段成环"的缺口在仓库里活了很久，因为**判据本身**
 * 只造了"末段是自环"一种形态、于是绿着 —— 一个不完整的前提比没有前提更坏（它给出虚假的
 * 安全感）。同类的"ec 版 + 抛版"**短路写法**（`fs::exists(p, ec) || fs::is_symlink(p)`）
 * 在中间段成环时**必然**求值抛型的右操作数（左边对 ELOOP 返回 false），必抛。
 */
TEST_F(SymlinkLoopPathsTest, PremiseMiddleComponentLoopAlsoThrowsIsSymlink)
{
    const fs::path loop = make_self_loop(probe_dir);  // probe_dir/self -> self

    // 末段就是环：不抛（老前提的那一半，仍然成立）
    EXPECT_FALSE(throws_fs_error([&] { return fs::is_symlink(loop); }));

    // **中间段**是环：`self/x` 与 `self/x/` 都抛
    const fs::path under = loop / "x";
    const fs::path under_slash = fs::path(loop.string() + "/x/");
    EXPECT_TRUE(throws_fs_error([&] { return fs::is_symlink(under); }))
        << "中间段成环时 fs::is_symlink 抛 —— 老前提（\"is_symlink 不抛\"）在中间段**不成立**";
    EXPECT_TRUE(throws_fs_error([&] { return fs::is_symlink(under_slash); }));

    // 而带 ec 的 `fs::exists` 对同一条路径**不抛**（返回 false + ELOOP）——于是
    // `fs::exists(p, ec) || fs::is_symlink(p)` 里那个 `||` 必然求值右半边 ⇒ 必抛。
    {
        std::error_code ec;
        const bool e = fs::exists(under, ec);
        EXPECT_FALSE(e);
        EXPECT_TRUE(ec) << "ec 应当被置成 ELOOP（这条是上面那句推理的前提）";
    }

    // 新增的不抛谓词对这**两种**形态都安全（`symlink_status` 把 ELOOP 放进 ec，不抛）
    EXPECT_NO_THROW(EXPECT_FALSE(is_symlink_no_follow(under)));
    EXPECT_NO_THROW(EXPECT_FALSE(is_real_directory(under)));
    EXPECT_NO_THROW(EXPECT_FALSE(exists_no_follow(under)));
    EXPECT_NO_THROW(EXPECT_FALSE(is_regular_file_no_follow(under)));
    // 末段是环时 lstat 语义仍然判"名字被占"（这是它存在的意义）
    EXPECT_NO_THROW(EXPECT_TRUE(exists_no_follow(loop)));
    EXPECT_NO_THROW(EXPECT_TRUE(is_symlink_no_follow(loop)));

    // ── "老写法 vs 新写法"的对照（这条是本轮修的**那两种表达式形状**的最小复现）────────
    // 形状甲（`installation_task.cpp` 的 probe_path、`op_sink.cpp` 的 save_config 移位搜索）：
    //   `fs::exists(p, ec) || fs::is_symlink(p)` —— 左边对 ELOOP 返回 false（**不抛**），
    //   于是 `||` **必然**求值右边那个抛型的 ⇒ 整条表达式抛。**判据是短路方向**，不是
    //   "用了 ec 重载没有"。
    EXPECT_TRUE(throws_fs_error([&] {
        std::error_code ec2;
        return fs::exists(under, ec2) || fs::is_symlink(under);
    })) << "老写法（甲）必须抛 —— 否则本轮对 probe_path / save_config 的修复理由不成立";
    // 形状乙（`package_manager.cpp` 的 cleanup_stashes）：**两个都是抛型**，且 `&&`/`||`
    // 的短路都会把它求值到。
    EXPECT_TRUE(throws_fs_error([&] {
        std::error_code ec3;
        return !fs::exists(under, ec3) && !fs::is_symlink(under);
    })) << "老写法（乙）必须抛 —— 否则 cleanup_stashes 的修复理由不成立";
    // 新写法：同一个位置、同一个语义，不抛。
    // （`!fs::is_symlink` 那一半在**已经短路掉**的形态里是安全的 ——
    //   `exists_no_follow(p) && !fs::is_symlink(p)` 只要 lstat 成功就不会遇到 ELOOP，
    //   所以 `Guard::TakenNotSymlink` 那种写法**不需要**改，别顺手统一。）
    EXPECT_NO_THROW(EXPECT_FALSE(exists_no_follow(under)));
    EXPECT_NO_THROW(EXPECT_FALSE(is_symlink_no_follow(under)));
}

TEST_F(SymlinkLoopPathsTest, PremiseDirectoryEntryIsDirectoryThrowsOnLoop)
{
    const fs::path loop = make_self_loop(probe_dir);

    // libstdc++ 的 directory_entry::is_directory() 对 **symlink** 条目会走 status()（跟随）→
    // 在环上抛。这就是"自带自环链接的包连扫描都过不去"的直接原因（见
    // scan_content_files 的修复与 ScanContentWithSymlinkLoopDoesNotThrow）。
    bool threw = false;
    for (const auto& e : fs::directory_iterator(probe_dir)) {
        if (e.path() != loop) continue;
        threw = throws_fs_error([&] { return e.is_directory(); });
    }
    EXPECT_TRUE(threw) << "directory_entry::is_directory() 对符号链接条目必须抛（否则本文件"
                          "对 scan_content_files 的修复理由不成立）";
}

// ═══════════════════════════════════════════════════════════════════════════
//  ② 新谓词族：解不开 → false，绝不抛
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(SymlinkLoopPathsTest, NoFollowPredicatesNeverThrowAndSayFalse)
{
    const fs::path loop = make_self_loop(probe_dir);
    fs::create_symlink("nope", probe_dir / "dangling");
    fs::create_directories(probe_dir / "realdir");
    fs::create_directories(probe_dir / "emptydir");
    std::ofstream(probe_dir / "realdir" / "f") << "x";
    const fs::path missing = probe_dir / "missing";

    // 自环：**名字被占**（lstat 成功），但目标不可达
    EXPECT_NO_THROW(EXPECT_TRUE(exists_no_follow(loop)));
    EXPECT_FALSE(exists_follow(loop)) << "解不开 = 不可达（不是 not-found，但判 false）";
    EXPECT_FALSE(is_real_directory(loop));

    // 悬空链接：同样"名字被占"
    EXPECT_TRUE(exists_no_follow(probe_dir / "dangling"));
    EXPECT_FALSE(exists_follow(probe_dir / "dangling"));

    // 真目录
    EXPECT_TRUE(exists_no_follow(probe_dir / "realdir"));
    EXPECT_TRUE(exists_follow(probe_dir / "realdir"));
    EXPECT_TRUE(is_real_directory(probe_dir / "realdir"));
    EXPECT_TRUE(is_real_directory(probe_dir / "emptydir"));
    // （没有 is_empty 的"不抛版"：handed-off 的几处空目录判定本来就走的 `fs::is_empty(p, ec)`
    //   重载 —— ec 非零时返回 false，保守方向天然正确，不需要新谓词。这里只钉住
    //   `is_real_directory` 对"空目录"也判 true。）

    // 符号链接指向真目录：**不是**真目录（不跟随末段链接），但 is_directory_follow 认它
    fs::create_directory_symlink("realdir", probe_dir / "link2dir");
    EXPECT_TRUE(exists_follow(probe_dir / "link2dir"));
    EXPECT_FALSE(is_real_directory(probe_dir / "link2dir"));
    // 这两个谓词的分工必须在测试里钉住：把它们"统一"成一个就会踩两头 ——
    // 用 lstat 判决"目录存不存在"会让 usr-merge 的 /lib → usr/lib 直接报"不是目录"；
    // 用跟随判决"归档条目是不是目录"会让符号链接条目走 status()（对环抛 ELOOP）。
    EXPECT_TRUE(is_directory_follow(probe_dir / "link2dir"))
        << "跟随语义：symlink→目录 要判 true（否则状态目录/父目录是符号链接的布局全线失效）";
    EXPECT_FALSE(is_directory_follow(loop)) << "解不开（ELOOP）→ false，不抛";
    EXPECT_FALSE(is_directory_follow(missing));

    // 不存在
    EXPECT_FALSE(exists_no_follow(missing));
    EXPECT_FALSE(exists_follow(missing));
    EXPECT_FALSE(is_real_directory(missing));
}

// ═══════════════════════════════════════════════════════════════════════════
//  ③ 落点：ensure_dir_exists / ensure_file_exists 报 LpkgException（点名路径），
//     而不是把 std::filesystem 的原始异常/半截状态抛给调用方
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(SymlinkLoopPathsTest, EnsureDirExistsOnLoopReportsPathNotFilesystemError)
{
    const fs::path loop = root / "usr" / "lib" / "loop";
    fs::create_directories(loop.parent_path());
    fs::create_symlink("loop", loop);

    std::string msg;
    try {
        ensure_dir_exists(loop);
        FAIL() << "在符号链接环上 ensure_dir_exists 必须报错（不能静默当成「已存在目录」）";
    } catch (const LpkgException& e) {
        msg = e.what();
    } catch (const fs::filesystem_error& e) {
        FAIL() << "仍然漏出 filesystem_error（判定不抛的修复没生效）: " << e.what();
    }
    EXPECT_NE(msg.find("loop"), std::string::npos) << "报错必须点名路径：" << msg;  // 锚点
}

// ensure_dir_exists 的判据**保持跟随语义**：symlink→目录 必须放行（usr-merge 的
// `/lib -> usr/lib`、管理员搬走的 `/var/lib/lpkg` 都是这个形态）。这条钉住"别把
// 谓词族无脑统一成 lstat 语义"。
TEST_F(SymlinkLoopPathsTest, EnsureDirExistsAcceptsSymlinkToDirectory)
{
    fs::create_directories(root / "usr" / "lib");
    fs::create_directory_symlink("usr/lib", root / "lib");

    EXPECT_NO_THROW(ensure_dir_exists(root / "lib"))
        << "symlink→目录 被 ensure_dir_exists 判成「不是目录」= usr-merge 布局崩掉";
}

TEST_F(SymlinkLoopPathsTest, EnsureFileExistsOnLoopReportsPath)
{
    const fs::path loop = root / "var" / "lib" / "loopfile";
    fs::create_directories(loop.parent_path());
    fs::create_symlink("loopfile", loop);

    std::string msg;
    try {
        ensure_file_exists(loop);
        FAIL() << "在符号链接环上 ensure_file_exists 必须报错";
    } catch (const LpkgException& e) {
        msg = e.what();
    } catch (const fs::filesystem_error& e) {
        FAIL() << "仍然漏出 filesystem_error: " << e.what();
    }
    EXPECT_NE(msg.find("loopfile"), std::string::npos) << "报错必须点名路径：" << msg;
}

// ═══════════════════════════════════════════════════════════════════════════
//  ④ scan_content_files：含环的 content/ 必须能被扫描（否则带环的包连装都装不上）
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(SymlinkLoopPathsTest, ScanContentWithSymlinkLoopDoesNotThrow)
{
    const fs::path content = suite_work_dir / "content";
    fs::create_directories(content / "usr" / "lib");
    std::ofstream(content / "usr" / "lib" / "libreal.so.1") << "elf";
    // 自环 + 两跳环（两个包各造一半的形态也能在单个 content 里出现）
    fs::create_symlink("self", content / "usr" / "lib" / "self");
    fs::create_symlink("b", content / "usr" / "lib" / "a");
    fs::create_symlink("a", content / "usr" / "lib" / "b");

    std::vector<std::string> entries;
    ASSERT_NO_THROW(entries = detail::scan_content_files(content))
        << "含符号链接环的 content/ 必须能扫描（libstdc++ 的 entry.is_directory() 会抛 ELOOP）";

    auto has = [&](const std::string& e) {
        return std::find(entries.begin(), entries.end(), e) != entries.end();
    };
    // 真目录 → 目录键（带尾斜杠）
    EXPECT_TRUE(has("usr/"));
    EXPECT_TRUE(has("usr/lib/"));
    EXPECT_TRUE(has("usr/lib/libreal.so.1"));
    // 符号链接（含环）→ **文件键**（不带尾斜杠）：符号链接是包的产物，按文件登记
    EXPECT_TRUE(has("usr/lib/self"));
    EXPECT_TRUE(has("usr/lib/a"));
    EXPECT_TRUE(has("usr/lib/b"));
    EXPECT_FALSE(has("usr/lib/self/")) << "环不得被登记成目录键";
}

// ═══════════════════════════════════════════════════════════════════════════
//  ⑤ WAL trim：bak 本身就是个环时，裁剪不能再抛（否则 WAL 永远裁不掉）
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(SymlinkLoopPathsTest, TrimCompletedKeepsWalWithLoopSymlinkBackup)
{
    // 造一个 stash 目录，里面放一个**符号链接环**当 bak —— 这正是"原路径是环、被 rename
    // 进 stash"之后的形态（rename 不改链接目标文本，环还是环）。
    const fs::path fsroot = root;
    const fs::path stash = fsroot / ".lpkg_bak_looppkg_1";
    fs::create_directories(stash);
    const fs::path bak = stash / "self.lpkg_bak_looppkg_abcd";
    fs::create_symlink("self.lpkg_bak_looppkg_abcd", bak);  // 自环

    // 全已配对（最后一个 COMMIT_PKGS 之后无未提交批次）→ 才会走到"bak 是否还在"的判定
    const std::string wpath = wal::wal_log_path();
    fs::create_directories(fs::path(wpath).parent_path());
    std::ofstream(wpath, std::ios::trunc)
        << "BEGIN_PKGS 1\n"
        << "BEGIN looppkg 1.0\n"
        << "BACKUP " << (root / "usr/lib/self").string() << " \xe2\x86\x92 " << bak.string() << "\n"
        << "COMMIT_PKGS\n";

    ASSERT_NO_THROW(trim_completed())
        << "bak 是符号链接环时 `fs::exists` 会抛 ELOOP —— trim 在每次启动都跑，抛出去就是"
           "WAL 永远裁剪不掉（判定必须走 lstat 语义的 exists_no_follow）";

    // 判据本身不变："bak 还占着这个名字 → 清理未完成 → **保留整个 WAL**"
    std::ifstream in(wpath);
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("BACKUP"), std::string::npos)
        << "bak 仍在 → 清理未完成 → 整个 WAL 必须保留（含 BACKUP 上下文），内容：" << content;
}

// ═══════════════════════════════════════════════════════════════════════════
//  ⑥ 端到端：**两个包各出一半**拼出一个符号链接环，然后卸载其中一个
//
//  为什么用"两个包"而不是"一个自带环的包"：**一个包自己的 content/ 里带环，今天根本装不
//  上** —— 扫描这一关（scan_content_files，本 agent 已修）过了，但安装的拷贝阶段会在
//  installation_task.cpp 的 `copy_package_files` 里用**会抛**的
//  `if (!fs::exists(src_path) && !fs::is_symlink(src_path)) continue;` 判定源路径，
//  `fs::exists` 对环抛 filesystem_error（2026-09-25 实测，异常原文见下）。那个文件不在本
//  agent 的文件集内，故本用例走**今天真正可达**的形态：
//
//      pkgA 装 `usr/lib/la -> lb`（悬空链接，装得进）
//      pkgB 装 `usr/lib/lb -> la`（悬空链接，装得进）
//      两个都装上之后：/usr/lib/la ⇄ /usr/lib/lb 互指成环 → 盘上出现 ELOOP
//
//  这就是现实里出现环的常见方式（两个包互相指、hook/用户后建的环），也是 CLAUDE.md §3
//  那条"包永久无法卸载"的可达路径。
//
//  卸载那一腿曾卡在**别人的**文件里（本 agent 只报不改）：
//      pkg/package_manager.cpp — do_remove_package() 阶段 A 原为
//          `if (fs::exists(phys) || fs::is_symlink(phys))`
//      phys 是环时 `fs::exists` 抛 filesystem_error → 整批回滚 → **这个包再也卸不掉**。
//      **持有者已修**（换成 ec 重载 `fs::exists(phys, ec) || fs::is_symlink(phys, ec)`：
//      环上 exists(ec) 判 false、is_symlink(ec) 走 lstat 判 true → 照常进 stash）。
//
//      所以本用例现在是**硬断言**：扫描 / 文件登记 / 卸载 / DB 与盘面收尾整条链一起钉住。
//      任何一处回退（417 改回抛版本、scan 回退成 `entry.is_directory()`、谓词回退成抛版本）
//      都会让它**响亮地红**，而不是悄悄跳过 —— 这正是当初用 SKIP 过渡时要避免的那件事。
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(SymlinkLoopPathsTest, RemovePackageFormingSymlinkLoopWithAnother)
{
    const auto make_side = [&](const std::string& name, const std::string& link_name,
                               const std::string& link_target) {
        const fs::path work = suite_work_dir / ("pkgwork_" + name);
        fs::create_directories(work / "content" / "usr" / "lib");
        std::ofstream(work / "content" / "usr" / "lib" / ("lib" + name + ".so.1")) << "elf";
        fs::create_symlink(link_target, work / "content" / "usr" / "lib" / link_name);
        const std::string pkg = (suite_work_dir / (name + "-1.0.lpkg")).string();
        pack_package(pkg, work.string(), name, "1.0", {}, {}, "man", {});
        return pkg;
    };
    const std::string pkgA = make_side("loopa", "la", "lb");
    const std::string pkgB = make_side("loopb", "lb", "la");

    ASSERT_NO_THROW(install_packages({pkgA})) << "悬空链接（半环）必须装得上";
    ASSERT_NO_THROW(install_packages({pkgB})) << "悬空链接（半环）必须装得上";
    write_cache();

    // 取证：两个包都装上之后，盘上确实是个**环**（自环也能形成，这里用更现实的互指）
    EXPECT_TRUE(fs::is_symlink(root / "usr/lib/la"));
    EXPECT_TRUE(fs::is_symlink(root / "usr/lib/lb"));
    EXPECT_TRUE(throws_fs_error([&] { return fs::exists(root / "usr/lib/lb"); }))
        << "前置条件：此刻 /usr/lib/lb 必须已经解不开（ELOOP）——否则这个用例什么都没测";

    // ── 卸载：整条链必须走通（含把环 rename 进 stash + 收尾清理）───────────────────
    EXPECT_NO_THROW(remove_package("loopb", /*force=*/true))
        << "含环的包必须卸得掉：`fs::exists` 对环抛 ELOOP 会把整批回滚，包从此永久无法卸载";
    write_cache();

    EXPECT_FALSE(Cache::instance().is_installed("loopb")) << "卸载后 DB 不该还记着这个包";
    // 判"盘上还在不在"必须用 lstat 语义：`fs::exists(p, ec)` 对符号链接环返回 false（ec=ELOOP），
    // 于是"环还留在盘上"也会让断言通过 —— 那是本文件自己要防的那种假绿。
    EXPECT_FALSE(exists_no_follow(root / "usr/lib/lb"))
        << "卸载后这个名字不该再被占着（含符号链接环形态的残留）";
}
