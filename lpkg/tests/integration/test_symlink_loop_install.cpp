/**
 * test_symlink_loop_install.cpp — **自带符号链接环的包必须能装上**，且环不得让事务收不了尾
 *
 * ── 缺口（CLAUDE.md §3「符号链接环（ELOOP）使包永久无法卸载」的安装侧一半）────────
 * `std::filesystem` 的**判定类**调用在"末段符号链接解不开"时不是判 not-found，而是**抛**
 * `filesystem_error`（实测记录与前提复现见 `tests/unit/test_symlink_loop_paths.cpp` 的
 * `Premise*` 用例）。安装路径上还剩两个炸点：
 *
 *   ① `InstallationTask::copy_package_files()` 的第一行判定
 *        `if (!fs::exists(src_path) && !fs::is_symlink(src_path)) continue;`
 *      —— `src_path` 是**包内内容**：包自己的 `content/` 里带一个自环符号链接时，
 *      `fs::exists` 直接抛
 *      `filesystem error: status: Symbolic link loop [.../content/usr/lib/self]`
 *      （异常原文见下）。**自带环的包因此根本装不上** —— 扫描那一关（`scan_content_files`）
 *      已修，但拷贝阶段这一关还在抛。等价的不抛写法就是 `exists_no_follow()`（lstat 语义，
 *      与原来的 `exists || is_symlink` 逐字同义）。
 *
 *   ② `db/wal_op.cpp` 的 `reverse_execute()` 里 BACKUP/REMOVE_OLD/SAVE_CONF 的**逆操作**：
 *        `if (fs::exists(bak_path) || fs::is_symlink(bak_path)) { safe_rename(bak_path, orig); }`
 *      —— `bak_path` 可以是**环本身**（原路径是环，rename 进 stash 之后还是环）。
 *      `fs::exists` 抛 ELOOP 意味着**回滚走不完**：升级中途失败后事务收不了尾，盘面停在
 *      "环被搬走、新版本没落位"的中间态（那一行同样是不抛的 `exists_no_follow()`）。
 *      同族的还有 NEW / NEW_DIR / COPY 的逆操作与 DIR_RM 的重建分支。
 *
 * ── 为什么本文件用"**一个包自带自环**"而不是"两个包互指" ─────────────────────────
 * `tests/unit/test_symlink_loop_paths.cpp` 的 `RemovePackageFormingSymlinkLoopWithAnother`
 * 走的是"两个包各出一半拼成环"的**绕道** —— 它当时的注释写明了理由：自带环的包装不上，
 * 所以只能绕过安装那一腿。安装侧修好之后，这条**直路**才是用户真会遇到、也最该钉住的形态。
 * 两者互补，不重复：那条钉"两个包互指 + 卸载"，本条钉"单包自带环 + 安装 + 回滚"。
 *
 * ── 两条用例分别是两个炸点的**红→绿**判别项 ─────────────────────────────────────
 *   ① 自带自环的包装得上、盘上留下那个环、随后卸得掉（判别 ①）；
 *   ② 环被让开（`BACKUP` 进 stash）之后中途失败 → 回滚必须把环原样搬回来（判别 ②；
 *      只修 ① 不够 —— 那时回滚会在 `fs::exists(bak_path)` 上抛）。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class SymlinkLoopInstallTest : public IntegrationTestBase
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

    template <typename F>
    std::string pack(const std::string& name, const std::string& ver, F fill) const
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::create_directories(work / "content");
        fill(work / "content");
        const std::string path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(path, work.string(), name, ver, {}, {}, "man " + name, {});
        return path;
    }

    int bak_residue() const
    {
        int n = 0;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(test_root, ec)) {
            if (ec) break;
            if (e.path().filename().string().find(".lpkg_bak_") != std::string::npos) ++n;
        }
        return n;
    }
};

/**
 * ① 包自带自环符号链接（`usr/lib/self -> self`）：安装必须成功、环必须原样落在盘上、
 * 随后必须卸得掉。
 *
 * 改前：`copy_package_files` 的 `fs::exists(src_path)` 抛
 * `filesystem error: status: Symbolic link loop` → `install_packages` 整体失败。
 */
TEST_F(SymlinkLoopInstallTest, PackageWithSelfLoopSymlinkInstallsAndCanBeRemoved)
{
    const std::string pkg = "looppkg";
    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        // ⚠️ 刻意**不带** `usr/lib/**.so*`：那会激活 `triggers.conf` 的 ldconfig 规则，
        //    提交后跑 `apply_soname_links(<root>/usr/lib)` —— 它遍历该目录时用
        //    `directory_entry::is_regular_file()`（`elf/lib_utils.cpp`），对环**抛**
        //    （实测异常原文：`filesystem error: status: Symbolic link loop
        //    [.../root/usr/lib/self]`）。 那是**另一处**缺口（`elf/*`
        //    不在本次改动的文件集内，已单独上报），不该把本用例
        //    变成它的代言人：本用例钉的是**拷贝阶段**那条炸点。
        //    此处若换成 `usr/lib/real.so`，本用例在"全量"里红、在"单跑"里绿 —— 因为
        //    ldconfig 规则来自 `TriggerManager` 这个进程级单例（`tests/unit/test_trigger_manager`
        //    跑过之后规则才在册），是测试隔离问题，不是被测语义问题。
        write_file(c / "usr/share/real.txt", "real\n");
        fs::create_directories(c / "usr/lib");
        fs::create_symlink("self", c / "usr/lib/self");  // 自环：解不开
    });

    EXPECT_NO_THROW(install_packages({v1}))
        << "自带符号链接环的包必须装得上（拷贝阶段用会抛的 fs::exists 判源路径就会在这里炸）";

    ASSERT_TRUE(fs::is_symlink(test_root / "usr/lib/self")) << "环本身必须原样落在盘上";
    // 前提复现（与 tests/unit/test_symlink_loop_paths.cpp 的 Premise* 同一断言，故放在环
    // **确实在盘上之后**）：判定类调用今天对环**抛**。没有这条前提，"路径不存在"与
    // "路径不可达"就分不开，本用例考的东西也就无从谈起。
    EXPECT_THROW(
        { (void)fs::exists(test_root / "usr/lib/self"); }, fs::filesystem_error)
        << "前提变了：libstdc++ 不再对 ELOOP 抛 —— 请一并复核 base/utils.hpp 的谓词族说明";
    EXPECT_EQ(fs::read_symlink(test_root / "usr/lib/self").string(), "self");
    EXPECT_EQ(read_file(test_root / "usr/share/real.txt"), "real\n")
        << "同一个包里的普通文件不受影响";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");
    EXPECT_TRUE(Cache::instance().is_file_owned_by("/usr/lib/self", pkg))
        << "环是**文件**（lstat 语义），DB 键不带尾斜杠";

    // 卸得掉：环不得让"这个包永远卸不掉"（CLAUDE.md §3 那条缺口的另一半）
    EXPECT_NO_THROW(remove_package(pkg, /*force=*/true))
        << "持有环的包必须卸得掉（卸载路径对环同样不能抛）";
    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed(pkg));
    EXPECT_FALSE(exists_no_follow(test_root / "usr/lib/self")) << "环必须被 unlink 掉";
}

/**
 * ② 环被让开之后中途失败 → 回滚必须把环**原样**搬回来。
 *
 * 时序：v2 把 `usr/lib/self` 从环换成普通文件 → 让开趟 `sink.backup()` 把环 rename 进 stash
 * （WAL `BACKUP`）→ 拷贝阶段注入失败 → 逆序回滚执行 `BACKUP` 的逆操作
 * （`safe_rename(bak → 原位)`）。**改前那一行会被 `fs::exists(bak_path)` 上的 ELOOP 打断**
 * （bak 就是那个环），事务收不了尾。
 *
 * 断言三件：异常是**注入的那个**（不是 ELOOP 顶掉的）、盘面逐项回到批次前（环 + 目标）、
 * 无 stash 残留且 DB 回到 v1。
 */
TEST_F(SymlinkLoopInstallTest, RollbackRestoresBackedUpLoopVerbatim)
{
    const std::string pkg = "looppkg2";
    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        fs::create_directories(c / "usr/lib");
        fs::create_symlink("self", c / "usr/lib/self");
    });
    ASSERT_NO_THROW(install_packages({v1}));
    ASSERT_TRUE(fs::is_symlink(test_root / "usr/lib/self"));

    // v2：同一个路径改成普通文件（+ 一个必然走 COPY 的伴生文件，保证断点命中）
    const std::string v2 = pack(pkg, "2.0", [](const fs::path& c) {
        write_file(c / "usr/lib/self", "no longer a loop\n");
        write_file(c / "usr/share/companion.txt", "companion\n");
    });

    bool bp_hit = false;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&bp_hit] {
        bp_hit = true;
        throw LpkgException("injected copy failure");
    });
    const std::string err = [&] {
        try {
            install_packages({v2});
        } catch (const LpkgException& e) {
            return std::string(e.what());
        } catch (const std::exception& e) {
            return std::string("（非 LpkgException）") + e.what();
        }
        return std::string{};
    }();
    BreakpointManager::instance().clear_all();

    EXPECT_TRUE(bp_hit) << "断点没命中：本用例没考到「让开已发生、写入未完成」的中间态";
    EXPECT_NE(err.find("injected copy failure"), std::string::npos)
        << "回滚路径自己在半路抛了别的异常（环把逆操作打断了），注入的错误被顶掉。实际异常："
        << err;

    // 盘面逐项回到批次前：那个环必须**原样**搬回来（连目标一起）
    ASSERT_TRUE(fs::is_symlink(test_root / "usr/lib/self"))
        << "回滚没能把环从 stash 搬回来（盘面停在「环被搬走、新版本没落位」的中间态）";
    EXPECT_EQ(fs::read_symlink(test_root / "usr/lib/self").string(), "self");
    EXPECT_FALSE(fs::exists(test_root / "usr/share/companion.txt")) << "回滚必须撤掉 v2 的新文件";
    EXPECT_EQ(bak_residue(), 0) << "回滚后不该留下 stash 残留";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");
}
