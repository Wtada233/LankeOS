/**
 * test_type_transition_matrix.cpp — 同一包同一路径的「类型转换全矩阵」（3×3 = 9 格）
 *
 * ── 要钉的语义 ──────────────────────────────────────────────────────────────
 * 同一个包的**同一个路径**，在新版本里从 {普通文件 file, 目录 dir, 符号链接 symlink} 变成
 * 三种里的任意一种，都应该：
 *   ① 安装成功；
 *   ② 批次中途失败时整批回滚，把旧形态**逐字节 / 逐类型**还原（内容、类型、链接目标逐项）。
 *
 * ── 每格的五步（见 run_cell）────────────────────────────────────────────────
 *   ① 装 v1（旧形态）→ 断言盘面 == v1 的形态 + 内容；
 *   ② 造"用户改动"留痕（文件写不同内容 / 符号链接换目标 / 改包里自己那个子条目的内容）——
 *      它是判"回滚是否逐字节保真"的锚点：新形态的字节与用户那份刻意不同，只断言"文件存在"
 *      无法区分"还原了用户那份"与"留下了新形态的残骸"；
 *   ③ 设断点 `copy_after_wal_<pkg>` 注入失败 → 断言抛出异常、且盘面**完全**回到
 *      "v1 + 用户改动"（类型 + 内容 + 链接目标逐项），DB 版本也回到 v1；
 *   ④ 清断点，干净地装 v2 → 断言盘面 == v2 的形态 + 内容；
 *   ⑤ 扫 test_root 断言没有 `.lpkg_bak_` 残留。
 *
 * ── 路径统一用**非 `/etc`**（`usr/share/thing`）───────────────────────────────
 * `/etc` 另有"配置保护（三哈希分流 / `.lpkgnew` / `.lpkgsave`）"一整套语义，会把 9 格的通用
 * 语义搅成"这格的差异来自类型转换还是来自 /etc 政策"。`/etc` 的差异行为在本文件末尾单独记录：
 *   · file → dir   （EtcFileToDirKeepsLpkgsave）、file → symlink（EtcFileToSymlinkGoesToLpkgnew）
 *   · dir → file / dir → symlink（EtcDirToFileKeepsLpkgsaveDir / EtcDirToSymlinkKeepsLpkgsaveDir）
 *     这两格曾经是**实测出来的缺陷**：`/etc` 的 dir → 非目录 根本装不上（详见那两条用例的注释）。
 *     已于 `9e91f2ba` 修好（决策表给这一格 `let_go = SaveConfig` + `write = WriteInPlace`），
 *     两条用例现在**是绿的**。（订正 2026-09-26：本行此前写"它们目前是红的，红即缺陷仍在"
 *     —— 那是修好之前的记录，别再照着它判断现状。）
 *
 * ── 为什么每格的 v2 都额外带一个普通文件 `usr/share/companion.txt` ─────────────
 * 断点 `copy_after_wal_<pkg>` 只挂在**普通文件**的 COPY 分支上（installation_task.cpp 的
 * `sink.commit_copy(tmp, dst, "copy_after_wal_" + pkg)`）：符号链接分支走
 * `new_file + create_symlink`（**无断点**）、目录分支走 `ensure_dir_exists`（**无断点**）。
 * 于是"v2 新形态只有符号链接"的格子（FileToSymlink / SymlinkToSymlink）根本够不到这个断点
 * —— 那不是被测语义失败，而是"这一格的失败注入没生效"，会把测试变成假红。
 * 让每个 v2 都带一个必然走 COPY 分支的普通文件，9 格才在**同一套注入手法**下可比。
 * （**订正 2026-09-26**：这里原记"符号链接条目的落位没有任何断点，该窗口不可测"—— 第③步已补
 *   `symlink_after_wal_<pkg>`（`NEW` 行已落、`create_symlink` 未做）与 `newdir_after_wal_<pkg>`，
 *   覆盖它们的用例见 `tests/integration/test_unstash_primitive.cpp`。）
 *
 * ── dir → 非目录（DirToFile / DirToSymlink）的**放行条件** ────────────────────
 * 归档条目是**非目录**、盘上是**真目录**时，`collect_content_conflicts` 的类型变更分支走
 * pacman `conflict.c: dir_belongsto_pkgs` 的判据（`dir_tree_entirely_ours`）：
 * **该目录下整棵子树（递归）里的每一个条目，都必须属于"本包"或"本批次正在升级的包"**——
 *   · 整棵树都是我们的 → **接管**：`backup_existing_files` 把目录整树搬进 stash（写 WAL
 *     `BACKUP` 行）让开路，随后才写文件/链接；回滚即 rename 回来（逐字节保真）；
 *   · 树里有**无人持有**的条目（用户自己往包里那个目录塞的文件）或属于**别的包**的条目
 *     → **拒绝**（整批预检，进事务之前；盘面一字未动）。这不是缺陷，是 pacman 的语义：
 *     整树搬走会**毁掉那个条目**，宁可拒绝升级、让用户自己处理。
 *   `/etc` 前缀另走 `save_config`（整树改名成 `<路径>.lpkgsave`，配置保留）。
 *
 * 故 DirToFile / DirToSymlink 两格的"用户改动"必须是**不阻断接管**的那种（改写包里自己那个
 * `a.txt` 的内容）—— 那才是在考**类型转换本身**。"目录里有无主内容 → 拒绝"是反方向的语义，
 * 由本文件 `DirToFileWithUnownedContentIsRefused` /
 * `DirToFileWithForeignPackageContentIsRefused` 两格单独钉（同方向的 DirToFile 已由
 * test_dir_entry_over_symlink.cpp 的 OwnDirReplacedByFileTakesOverWithWalBackup 钉住）。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class TypeTransitionMatrixTest : public IntegrationTestBase
{
protected:
    /// 迁移路径（**非 /etc**：/etc 的配置保护语义另有专门用例）
    static constexpr const char* REL = "usr/share/thing";
    /// 每格 v2 都带的"必然走 COPY 分支"的普通文件（见文件头说明）
    static constexpr const char* COMPANION = "usr/share/companion.txt";

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        IntegrationTestBase::TearDown();
    }

    // ── 通用小工具（与 test_etc_dir_upgrade.cpp 同一套写法）──────────────────

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

    /// 打一个包：content 由 fill 回调填（路径相对 content/）
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

    /**
     * 走真实入口 `install_packages({<本地 .lpkg>})`，返回**实际错误消息**（成功返回空串）。
     *
     * 刻意不用裸 `EXPECT_NO_THROW`：那只证明"抛了个异常"，失败时既看不到 `LpkgException`
     * 的 what、也分不清"预期的拒绝"与"测试自己写错了路径"。矩阵的核心产出之一就是**每格
     * 失败时的原文**，所以消息必须能被断言带进失败输出。
     */
    std::string install_err(const std::string& pkg_path)
    {
        try {
            install_packages({pkg_path});
        } catch (const LpkgException& e) {
            return e.what();
        } catch (const std::exception& e) {
            return std::string("（非 LpkgException）") + e.what();
        }
        return {};
    }

    /// 盘上该路径此刻的形态："absent" / "file" / "dir" / "symlink"（lstat 语义）
    static std::string shape_of(const fs::path& p)
    {
        std::error_code ec;
        if (fs::is_symlink(p, ec)) return "symlink";
        if (fs::is_directory(p, ec)) return "dir";
        if (fs::is_regular_file(p, ec)) return "file";
        return "absent";
    }

    /// 未清理的 stash 残留数（`.lpkg_bak_*`）
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

    // ── 盘面断言（类型 + 内容 + 链接目标，逐项）──────────────────────────────

    /// 盘上是**普通文件**（不是链接、不是目录）且内容逐字节等于 content
    void expect_regular(const std::string& content, const std::string& what)
    {
        const fs::path p = test_root / REL;
        EXPECT_EQ(shape_of(p), "file") << what;
        EXPECT_FALSE(fs::is_symlink(p)) << what << "（若是符号链接说明旧形态被穿透/未还原）";
        if (fs::is_regular_file(p)) {
            EXPECT_EQ(read_file(p), content) << what;
        }
    }

    /// 盘上是**符号链接**且目标逐字节等于 target
    void expect_link(const std::string& target, const std::string& what)
    {
        const fs::path p = test_root / REL;
        EXPECT_EQ(shape_of(p), "symlink") << what;
        if (fs::is_symlink(p)) {
            EXPECT_EQ(fs::read_symlink(p).string(), target) << what;
        }
    }

    /// v1 的目录形态：目录在、`a.txt` 内容 == a_content、v2 才有的 `b.txt` 不在
    /// （`a_content` 可传"用户改过的那份"，用于断言回滚是**逐字节**回到改动后的 v1）
    void expect_dir_v1(const std::string& what, const std::string& a_content = "v1 a\n")
    {
        EXPECT_EQ(shape_of(test_root / REL), "dir") << what;
        EXPECT_EQ(read_file(test_root / REL / "a.txt"), a_content) << what;
        EXPECT_FALSE(fs::exists(test_root / REL / "b.txt")) << what;
    }

    /// v2 的目录形态：目录在、`b.txt` 在、v1 独有的 `a.txt` 已作为废弃文件清掉
    void expect_dir_v2(const std::string& what)
    {
        EXPECT_EQ(shape_of(test_root / REL), "dir") << what;
        EXPECT_EQ(read_file(test_root / REL / "b.txt"), "v2 b\n") << what;
        EXPECT_FALSE(fs::exists(test_root / REL / "a.txt"))
            << what << "（a.txt 是 v1 独有、v2 不再发的废弃文件，应被 REMOVE_OLD 清掉）";
    }

    /** 符号链接格子的链接目标：**不属于任何包**的散落文件。
     *  留在盘上是为了让"链接目标不该被本包动过"这条断言有对象。 */
    void make_link_targets()
    {
        write_file(test_root / "usr/share/target-a.txt", "target a\n");
        write_file(test_root / "usr/share/target-b.txt", "target b\n");
        write_file(test_root / "usr/share/target-c.txt", "target c\n");
    }

    void expect_link_targets_untouched(const std::string& what)
    {
        EXPECT_EQ(read_file(test_root / "usr/share/target-a.txt"), "target a\n")
            << what << "：链接目标（无权属的散落文件）被动了";
        EXPECT_EQ(read_file(test_root / "usr/share/target-b.txt"), "target b\n") << what;
        EXPECT_EQ(read_file(test_root / "usr/share/target-c.txt"), "target c\n") << what;
    }

    // ── v1 的三种形态 ────────────────────────────────────────────────────────

    static void fill_v1_file(const fs::path& c)
    {
        write_file(c / REL, "v1 file\n");
    }

    static void fill_v1_dir(const fs::path& c)
    {
        write_file(c / REL / "a.txt", "v1 a\n");
    }

    static void fill_v1_symlink(const fs::path& c)
    {
        fs::create_directories(c / "usr/share");
        fs::create_symlink("target-a.txt", c / REL);
    }

    // ── v2 的三种形态（都带上 COMPANION，见文件头）───────────────────────────

    static void fill_v2_file(const fs::path& c)
    {
        write_file(c / REL, "v2 file\n");
        write_file(c / COMPANION, "companion\n");
    }

    static void fill_v2_dir(const fs::path& c)
    {
        write_file(c / REL / "b.txt", "v2 b\n");
        write_file(c / COMPANION, "companion\n");
    }

    static void fill_v2_symlink(const fs::path& c)
    {
        fs::create_directories(c / "usr/share");
        fs::create_symlink("target-c.txt", c / REL);
        write_file(c / COMPANION, "companion\n");
    }

    // ── 用户改动留痕（回滚保真的锚点）────────────────────────────────────────

    void user_edit_file()
    {
        write_file(test_root / REL, "user edited\n");
    }

    void user_add_note_in_dir()
    {
        write_file(test_root / REL / "user-note.txt", "user note\n");
    }

    /**
     * 用户改写**目录里包里自己那个 `a.txt`** 的内容。
     *
     * 用于 dir → 非目录 的两格：那里"盘上是真目录、新条目是文件/链接"，放行条件是
     * `dir_tree_entirely_ours`（整棵子树都归本包 / 本批次升级的包）。往里塞一个**无人持有**
     * 的文件会让整批被**正确拒绝**（pacman 的 `dir_belongsto_pkgs` 语义），那一格就考不到
     * 类型转换本身了。改我们自己那个条目的内容则归属不变 ⇒ 整树仍是我们的 ⇒ 接管照常放行，
     * 同时"用户改过的那份"仍是回滚保真的锚点（内容与新形态、与 v1 原始内容都不同）。
     */
    void user_edit_file_in_dir()
    {
        write_file(test_root / REL / "a.txt", "user edited a\n");
    }

    /// 用户把链接重新指向别处（目标另有一份散落文件）
    void user_repoint_symlink()
    {
        fs::remove(test_root / REL);
        fs::create_symlink("target-b.txt", test_root / REL);
    }

    /**
     * 跑完五步。各段断言由调用方给（它们的差别正是 9 格各自的语义）：
     *   prepare           — 装 v1 之前（铺散落的链接目标等）
     *   assert_v1         — 第 ① 步之后（盘面 == 全新 v1）
     *   assert_rolled_back — 第 ③ 步之后（盘面 == v1 + 用户改动；新形态**一点没留**）
     *   assert_v2         — 第 ④ 步之后（盘面 == v2）
     *
     * `expect_copy_reached`：本格 v2 是否**应当**通过预检、走到 COPY 分支让断点命中。
     * 九格全部为 true（含 dir→非目录 两格 —— 整棵树都是我们的，预检放行）；传 false 只有
     * "**应当被拒绝**"的格子用得上，那时它是反向断言：断点若命中了就说明盘面被动过，
     * 与"整批预检拒绝、一个文件都没动"矛盾。
     */
    void run_cell(const std::string& pkg, const std::function<void()>& prepare,
                  const std::function<void(const fs::path&)>& fill_v1,
                  const std::function<void(const fs::path&)>& fill_v2,
                  const std::function<void()>& user_change, const std::function<void()>& assert_v1,
                  const std::function<void()>& assert_rolled_back,
                  const std::function<void()>& assert_v2, bool expect_copy_reached = true)
    {
        SCOPED_TRACE("迁移格 / 包名：" + pkg);

        prepare();

        // ── ① 装 v1（旧形态）────────────────────────────────────────────────
        const std::string v1_pkg = pack(pkg, "1.0", fill_v1);
        const std::string e1 = install_err(v1_pkg);
        ASSERT_TRUE(e1.empty()) << "前置失败：v1 都装不上（本格其余步骤无意义）：" << e1;
        Cache::instance().load();
        ASSERT_EQ(Cache::instance().get_installed_version(pkg), "1.0");
        assert_v1();

        // ── ② 用户改动留痕 ──────────────────────────────────────────────────
        user_change();

        // ── ③ 注入"COPY 已写 WAL、rename 未做"的中途失败 ────────────────────
        const std::string v2_pkg = pack(pkg, "2.0", fill_v2);
        bool bp_hit = false;
        BreakpointManager::instance().set("copy_after_wal_" + pkg, [&bp_hit] {
            bp_hit = true;
            throw LpkgException("injected copy failure");
        });
        const std::string e2 = install_err(v2_pkg);
        BreakpointManager::instance().clear_all();

        EXPECT_FALSE(e2.empty()) << "断点 copy_after_wal_" << pkg
                                 << " 注入后 v2 竟然装成功（断点命中=" << bp_hit
                                 << "）—— 本格的失败注入没生效";
        if (expect_copy_reached) {
            EXPECT_TRUE(bp_hit) << "断点没命中：v2 没走到 COPY 分支（v1 形态被替换的那一步"
                                   "压根没发生，本格的\"回滚\"没被考到）";
        } else {
            EXPECT_FALSE(bp_hit) << "断点竟命中了：说明 v2 通过了整批预检、进了拷贝阶段 —— 与"
                                    "「整树归本包才接管、否则预检即拒」的现状不符（预检语义变了）";
        }
        if (!e2.empty()) {
            // 失败原文是矩阵的产出之一，单独打一遍（gtest 只在断言失败时打印 << 内容）
            std::cerr << "[type-matrix] " << pkg << " 步骤③ 异常：" << e2 << "\n";
        }
        assert_rolled_back();
        Cache::instance().load();
        EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0") << "回滚后 DB 版本没回到 v1";

        // ── ④ 清断点，干净地装 v2 ───────────────────────────────────────────
        const std::string e3 = install_err(v2_pkg);
        EXPECT_TRUE(e3.empty()) << "v2（新形态）安装失败，异常原文：" << e3;
        if (e3.empty()) {
            Cache::instance().load();
            EXPECT_EQ(Cache::instance().get_installed_version(pkg), "2.0");
        } else {
            std::cerr << "[type-matrix] " << pkg << " 步骤④ 异常：" << e3 << "\n";
        }
        assert_v2();

        // ── ⑤ 无残留 ────────────────────────────────────────────────────────
        EXPECT_EQ(bak_residue(), 0) << "root 下仍留有 .lpkg_bak_* 残留";
    }
};

// ============================================================================
// file → {file, dir, symlink}
// ============================================================================

/** file → file：最平凡的一格，兼作"矩阵脚手架本身没问题"的对照。 */
TEST_F(TypeTransitionMatrixTest, FileToFile)
{
    run_cell(
        "m_ff", [] {}, [](const fs::path& c) { fill_v1_file(c); },
        [](const fs::path& c) { fill_v2_file(c); }, [this] { user_edit_file(); },
        [this] { expect_regular("v1 file\n", "① v1 装完后 /usr/share/thing 应是 v1 的普通文件"); },
        [this] {
            // 回滚保真的锚点：用户改过的字节必须原样回来（不是 v1 原始字节、更不是 v2 的）
            expect_regular("user edited\n", "③ 回滚后必须逐字节回到**用户改过的那份**");
            EXPECT_FALSE(fs::exists(test_root / (std::string(REL) + ".lpkgsave")))
                << "非 /etc 路径不该产生 .lpkgsave（那是 /etc 配置的语义）";
        },
        [this] { expect_regular("v2 file\n", "④ v2 装完后应是 v2 的普通文件"); });
}

/**
 * file → dir（TODO E4 的正方向；usr 与 /etc 两腿已分别由 test_dir_entry_over_symlink.cpp /
 * test_etc_dir_upgrade.cpp 覆盖）。这里补的是**回滚**维度：挡路文件已进 stash、目录已建、
 * 新文件正落位时失败 —— 旧文件必须逐字节回到原位，而不是"目录没了、文件也没了"。
 */
TEST_F(TypeTransitionMatrixTest, FileToDir)
{
    run_cell(
        "m_fd", [] {}, [](const fs::path& c) { fill_v1_file(c); },
        [](const fs::path& c) { fill_v2_dir(c); }, [this] { user_edit_file(); },
        [this] { expect_regular("v1 file\n", "① v1 装完后应是 v1 的普通文件"); },
        [this] {
            expect_regular("user edited\n", "③ 回滚后挡路文件必须逐字节回到用户改过的那份");
            EXPECT_EQ(shape_of(test_root / (std::string(REL) + ".lpkgsave")), "absent")
                << "非 /etc 路径的挡路物该走 stash，不该留下 .lpkgsave";
        },
        [this] {
            EXPECT_EQ(shape_of(test_root / REL), "dir") << "④ v2 应把该路径接管成目录";
            EXPECT_FALSE(fs::is_symlink(test_root / REL));
            EXPECT_EQ(read_file(test_root / REL / "b.txt"), "v2 b\n");
        });
}

/** file → symlink：归档**非目录**条目接管本包自己的普通文件（不涉及目录，预检不拦）。 */
TEST_F(TypeTransitionMatrixTest, FileToSymlink)
{
    run_cell(
        "m_fs", [] {}, [](const fs::path& c) { fill_v1_file(c); },
        [](const fs::path& c) { fill_v2_symlink(c); }, [this] { user_edit_file(); },
        [this] { expect_regular("v1 file\n", "① v1 装完后应是 v1 的普通文件"); },
        [this] {
            expect_regular("user edited\n", "③ 回滚后旧普通文件必须逐字节回到用户改过的那份");
            EXPECT_FALSE(fs::is_symlink(test_root / REL)) << "回滚后不该留下 v2 的符号链接";
        },
        [this] { expect_link("target-c.txt", "④ v2 装完后该路径应是指向 target-c.txt 的链接"); });
}

// ============================================================================
// dir → {file, dir, symlink}
// ============================================================================

/**
 * dir → file：**接管成功**（整棵子树都归本包 → `dir_tree_entirely_ours` 放行）。
 *
 * 第 ③ 步是这一格的要害：挡路的**真目录**必须在 `backup_existing_files` 里整树搬进 stash
 * （WAL `BACKUP`），路径让开后才写新文件；中途失败时整棵树（含用户改过内容的 `a.txt`）
 * 必须逐字节 rename 回来。此前"目录由 `commit_without_file_ops` 的阶段 2 处理"是**赶不上**
 * 的（阶段 2 在拷贝之后）—— 那样 rename 撞 EISDIR，回滚还对空目录 rmdir，盘面/DB 脱节。
 */
TEST_F(TypeTransitionMatrixTest, DirToFile)
{
    run_cell(
        "m_df", [] {}, [](const fs::path& c) { fill_v1_dir(c); },
        [](const fs::path& c) { fill_v2_file(c); }, [this] { user_edit_file_in_dir(); },
        [this] { expect_dir_v1("① v1 装完后应是含 a.txt 的目录"); },
        [this] {
            expect_dir_v1("③ 回滚后整棵树必须原样搬回来（含**用户改过内容**的 a.txt）",
                          "user edited a\n");
            EXPECT_FALSE(fs::exists(test_root / COMPANION)) << "回滚后不该留下 v2 的伴生文件";
            EXPECT_EQ(shape_of(test_root / (std::string(REL) + ".lpkgsave")), "absent")
                << "非 /etc 路径的挡路目录该走 stash，不该留下 .lpkgsave（那是 /etc 的语义）";
        },
        [this] { expect_regular("v2 file\n", "④ v2 装完后该路径应是被接管成的普通文件"); });
}

/**
 * dir → file，但目录树里有**无人持有**的条目（用户往包里那个目录塞的文件）：**整批拒绝**。
 *
 * 这是 DirToFile 的反面 —— 放行条件是"**整棵**树都是我们的"（pacman 的
 * `conflict.c: dir_belongsto_pkgs`），不是"这个目录路径归本包"。整树搬走会毁掉那个文件，
 * 故宁可拒绝升级（用户得自己处理它）。
 *
 * 断言三件事：① 拒绝发生在**进事务之前**（盘面一字未动、连 v2 的伴生文件都没落、
 * 无 stash 残留）；② 报错点名**真实冲突源**（无主条目 → `error.unknown_manual_file`，
 * 而不是含糊地报"这个目录归本包"，那会把人引去查一个不存在的冲突源）；③ 无主文件原样还在。
 */
TEST_F(TypeTransitionMatrixTest, DirToFileWithUnownedContentIsRefused)
{
    const std::string pkg = "m_df_unowned";
    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) { fill_v1_dir(c); });
    ASSERT_TRUE(install_err(v1).empty()) << "前置：v1 装不上，本格其余步骤无意义";
    ASSERT_EQ(shape_of(test_root / REL), "dir");

    // ② 用户改动留痕 —— 但这次是往**包里那个目录**里塞一个**无人持有**的文件
    write_file(test_root / REL / "user-note.txt", "user note\n");

    const std::string v2 = pack(pkg, "2.0", [](const fs::path& c) { fill_v2_file(c); });
    const std::string msg = install_err(v2);

    ASSERT_FALSE(msg.empty())
        << "目录树里有无人持有的条目 ⇒ dir→file 必须被拒绝（整树搬走会毁掉那个文件）";
    std::cerr << "[type-matrix] " << pkg << " 拒绝原文：" << msg << "\n";
    EXPECT_NE(msg.find("usr/share/thing"), std::string::npos) << "拒绝信息没点名冲突路径：" << msg;
    EXPECT_NE(msg.find(get_string("error.unknown_manual_file")), std::string::npos)
        << "无主条目 → 判据必须是 " << get_string("error.unknown_manual_file") << "：" << msg;

    // ① 进事务之前就拒了：盘面一字未动
    expect_dir_v1("整批拒绝时盘面必须**一个文件都没动**");
    EXPECT_EQ(read_file(test_root / REL / "user-note.txt"), "user note\n")
        << "被拒时必须原样留着那个无主文件（pacman 语义：拒绝的理由就是不能毁掉它）";
    EXPECT_FALSE(fs::exists(test_root / COMPANION)) << "预检拒绝时不该落下 v2 的任何文件";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0") << "被拒后 DB 版本不该变";
    EXPECT_EQ(bak_residue(), 0) << "拒绝发生在进事务之前 → 不该有任何 stash 残留";
}

/**
 * dir → file，但目录树里有**别的包**持有的条目：同样整批拒绝，且报错要点名**那个包**。
 *
 * 与上一格同一判据（"整棵树都是我们的"），只是树里第一条"不是我们的"条目有主。这条路径
 * 正是"报错点名真实冲突源"最容易退化的地方 —— 如果只报"本包持有这个目录"，用户会去查一个
 * **不存在**的冲突（本包自己），永远找不到真正挡路的那个包。
 *
 * 目录键是**累加**持有者的：合租包能往本包装的目录里写自己的文件（共享目录），所以这一格
 * 是真实会发生、而非构造出来的。
 */
TEST_F(TypeTransitionMatrixTest, DirToFileWithForeignPackageContentIsRefused)
{
    const std::string pkg = "m_df_foreign";
    const std::string co_pkg = "m_df_foreign_co";
    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) { fill_v1_dir(c); });
    ASSERT_TRUE(install_err(v1).empty()) << "前置：v1 装不上";
    ASSERT_EQ(shape_of(test_root / REL), "dir");

    // 另一个包往**同一个目录**里放它自己的文件（目录可共享 → 两个包都持有这个目录键）
    const std::string co =
        pack(co_pkg, "1.0", [](const fs::path& c) { write_file(c / REL / "other.txt", "co\n"); });
    const std::string co_err = install_err(co);
    ASSERT_TRUE(co_err.empty()) << "前置：合租包装不进共享目录：" << co_err;
    ASSERT_EQ(read_file(test_root / REL / "other.txt"), "co\n");

    const std::string v2 = pack(pkg, "2.0", [](const fs::path& c) { fill_v2_file(c); });
    const std::string msg = install_err(v2);

    ASSERT_FALSE(msg.empty())
        << "目录树里有别的包的条目 ⇒ dir→file 必须被拒绝（整树搬走会搬走别人的文件）";
    std::cerr << "[type-matrix] " << pkg << " 拒绝原文：" << msg << "\n";
    EXPECT_NE(msg.find("usr/share/thing"), std::string::npos) << "拒绝信息没点名冲突路径：" << msg;
    EXPECT_NE(msg.find(co_pkg), std::string::npos)
        << "拒绝信息必须点名**真实冲突源** " << co_pkg << "（报成本包自己 = 把人引去查一个"
        << "不存在的冲突源）：" << msg;

    expect_dir_v1("整批拒绝时盘面必须**一个文件都没动**");
    EXPECT_EQ(read_file(test_root / REL / "other.txt"), "co\n") << "别的包的文件必须原样还在";
    EXPECT_FALSE(fs::exists(test_root / COMPANION)) << "预检拒绝时不该落下 v2 的任何文件";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");
    EXPECT_EQ(bak_residue(), 0);
}

/**
 * dir → symlink：**接管成功**（与 DirToFile 同一判据：整棵子树都归本包）。
 *
 * 注意 copy 阶段其实另有一道 `error.dir_replaced_by_symlink` 守卫（`fs::is_directory(dest)`
 * 时抛）—— 接管路径下它到不了，因为挡路目录已由 `backup_existing_files` 整树搬进 stash、
 * 路径已让开。它守的是"预检漏判"的兜底（第二道防线），不是正常路径。
 */
TEST_F(TypeTransitionMatrixTest, DirToSymlink)
{
    run_cell(
        "m_ds", [] {}, [](const fs::path& c) { fill_v1_dir(c); },
        [](const fs::path& c) { fill_v2_symlink(c); }, [this] { user_edit_file_in_dir(); },
        [this] { expect_dir_v1("① v1 装完后应是含 a.txt 的目录"); },
        [this] {
            expect_dir_v1("③ 回滚后整棵树必须原样搬回来（含**用户改过内容**的 a.txt）",
                          "user edited a\n");
            EXPECT_FALSE(fs::is_symlink(test_root / REL)) << "回滚后不该留下 v2 的符号链接";
            EXPECT_FALSE(fs::exists(test_root / COMPANION)) << "回滚后不该留下 v2 的伴生文件";
        },
        [this] { expect_link("target-c.txt", "④ v2 装完后该路径应是指向 target-c.txt 的链接"); });
}

/**
 * dir → dir：目录形态不变、只换内容。回滚维度：v2 独有的 `b.txt` 必须消失，v1 的 `a.txt`
 * 与用户加的 `user-note.txt` 必须都在（`user-note.txt` 无人持有 → 目录不能因"只剩它"被 rmdir）。
 */
TEST_F(TypeTransitionMatrixTest, DirToDir)
{
    run_cell(
        "m_dd", [] {}, [](const fs::path& c) { fill_v1_dir(c); },
        [](const fs::path& c) { fill_v2_dir(c); }, [this] { user_add_note_in_dir(); },
        [this] { expect_dir_v1("① v1 装完后应是含 a.txt 的目录"); },
        [this] {
            expect_dir_v1("③ 回滚后目录与 a.txt 都必须回到原位");
            EXPECT_EQ(read_file(test_root / REL / "user-note.txt"), "user note\n")
                << "回滚后用户加的文件必须还在（无人持有 → 目录不能被整树 rmdir 带走）";
            EXPECT_FALSE(fs::exists(test_root / (std::string(REL) + ".lpkgsave")));
        },
        [this] {
            expect_dir_v2("④ v2 装完后 b.txt 在位、a.txt 作为废弃文件被清掉");
            EXPECT_EQ(read_file(test_root / REL / "user-note.txt"), "user note\n")
                << "无人持有的用户文件不该被本包的内容替换连坐删掉";
        });
}

// ============================================================================
// symlink → {file, dir, symlink}
// ============================================================================

/**
 * symlink → file：链接换成一个实体普通文件。本格的关键是"被用户改过目标的那个链接"必须
 * 进 stash（否则回滚无从还原 —— `is_directory` 跟随链接会把 symlink→目录 误判成目录而跳过备份）。
 */
TEST_F(TypeTransitionMatrixTest, SymlinkToFile)
{
    run_cell(
        "m_sf", [this] { make_link_targets(); }, [](const fs::path& c) { fill_v1_symlink(c); },
        [](const fs::path& c) { fill_v2_file(c); }, [this] { user_repoint_symlink(); },
        [this] {
            expect_link("target-a.txt", "① v1 装完后应是指向 target-a.txt 的链接");
            expect_link_targets_untouched("①");
        },
        [this] {
            expect_link("target-b.txt", "③ 回滚后链接必须回到**用户改过的那个目标**（不是 v1 的）");
            expect_link_targets_untouched("③");
        },
        [this] {
            expect_regular("v2 file\n", "④ v2 装完后该路径应是 v2 的普通文件（链接被替换掉）");
            expect_link_targets_untouched("④");
        });
}

/** symlink → dir（E4 的正方向）。补回滚维度：被用户改过目标的链接必须**连目标一起**原样回来。 */
TEST_F(TypeTransitionMatrixTest, SymlinkToDir)
{
    run_cell(
        "m_sd", [this] { make_link_targets(); }, [](const fs::path& c) { fill_v1_symlink(c); },
        [](const fs::path& c) { fill_v2_dir(c); }, [this] { user_repoint_symlink(); },
        [this] {
            expect_link("target-a.txt", "① v1 装完后应是指向 target-a.txt 的链接");
            expect_link_targets_untouched("①");
        },
        [this] {
            expect_link("target-b.txt", "③ 回滚后链接必须回到用户改过的那个目标");
            expect_link_targets_untouched("③");
        },
        [this] {
            EXPECT_EQ(shape_of(test_root / REL), "dir") << "④ v2 应把链接接管成实体目录";
            EXPECT_FALSE(fs::is_symlink(test_root / REL)) << "链接必须被替换掉，而不是写穿";
            EXPECT_EQ(read_file(test_root / REL / "b.txt"), "v2 b\n");
            expect_link_targets_untouched("④");
        });
}

/** symlink → symlink：只有目标不同。最容易退化成"链接被删了重建、目标却没改"的一格。 */
TEST_F(TypeTransitionMatrixTest, SymlinkToSymlink)
{
    run_cell(
        "m_ss", [this] { make_link_targets(); }, [](const fs::path& c) { fill_v1_symlink(c); },
        [](const fs::path& c) { fill_v2_symlink(c); }, [this] { user_repoint_symlink(); },
        [this] {
            expect_link("target-a.txt", "① v1 装完后应是指向 target-a.txt 的链接");
            expect_link_targets_untouched("①");
        },
        [this] {
            expect_link("target-b.txt", "③ 回滚后链接必须回到用户改过的那个目标");
            expect_link_targets_untouched("③");
        },
        [this] {
            expect_link("target-c.txt", "④ v2 装完后链接目标必须是 v2 的 target-c.txt");
            expect_link_targets_untouched("④");
        });
}

// ============================================================================
// /etc 的差异行为（单独记录，不混进上面 9 格的"通用语义"）
// ============================================================================

/**
 * `/etc` 的 file → dir：与 `usr/share` 那一腿**语义不同** —— 被目录接管的挡路物不是"包自己的
 * 文件、进 stash 提交后清掉"，而是**用户改过的配置**：必须改名保留成 `/etc/foo.lpkgsave`
 * （与移除侧对 /etc 的政策一致），且回滚要把这次改名整条撤掉。
 *
 * 本用例不靠 COMPANION —— v2 的 `etc/foo/bar.conf` 自己就是普通文件、必然走 COPY 分支。
 */
TEST_F(TypeTransitionMatrixTest, EtcFileToDirKeepsLpkgsave)
{
    const std::string pkg = "m_etc_f2d";
    const std::string v1 =
        pack(pkg, "1.0", [](const fs::path& c) { write_file(c / "etc/foo", "v1 conf\n"); });
    ASSERT_TRUE(install_err(v1).empty());
    ASSERT_EQ(shape_of(test_root / "etc/foo"), "file");
    ASSERT_EQ(read_file(test_root / "etc/foo"), "v1 conf\n");

    // ② 用户改动留痕（/etc 配置保护的全部意义就在这一份）
    write_file(test_root / "etc/foo", "user edited\n");

    const std::string v2 = pack(
        pkg, "2.0", [](const fs::path& c) { write_file(c / "etc/foo/bar.conf", "v2 conf\n"); });

    // ③ 注入中途失败 → 回滚必须把 `/etc/foo` 逐字节还原、并把 .lpkgsave 撤销
    bool bp_hit = false;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&bp_hit] {
        bp_hit = true;
        throw LpkgException("injected copy failure");
    });
    const std::string e2 = install_err(v2);
    BreakpointManager::instance().clear_all();
    EXPECT_FALSE(e2.empty()) << "断点没生效（命中=" << bp_hit << "）";
    EXPECT_TRUE(bp_hit) << "断点没命中：etc/foo/bar.conf 应当走 COPY 分支";

    EXPECT_EQ(shape_of(test_root / "etc/foo"), "file") << "③ 回滚后 /etc/foo 不该还是目录";
    EXPECT_EQ(read_file(test_root / "etc/foo"), "user edited\n")
        << "③ 回滚后必须是用户改过的那份配置（逐字节）";
    EXPECT_FALSE(fs::exists(test_root / "etc/foo.lpkgsave"))
        << "③ 回滚必须撤销 save_config 的改名（.lpkgsave 是这次批次造出来的，不能留）";
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgsave"), "absent");
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");

    // ④ 干净装 v2
    const std::string e3 = install_err(v2);
    EXPECT_TRUE(e3.empty()) << "v2 安装失败，异常原文：" << e3;
    if (!e3.empty()) std::cerr << "[type-matrix] " << pkg << " 步骤④ 异常：" << e3 << "\n";
    EXPECT_EQ(shape_of(test_root / "etc/foo"), "dir") << "④ /etc/foo 应被接管成目录";
    EXPECT_EQ(read_file(test_root / "etc/foo/bar.conf"), "v2 conf\n");
    EXPECT_EQ(read_file(test_root / "etc/foo.lpkgsave"), "user edited\n")
        << "④ 被目录接管的 /etc 配置必须改名保留成 .lpkgsave（用户那份逐字节）";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "2.0");
    EXPECT_EQ(bak_residue(), 0);
}

/**
 * `/etc` 的 **file → symlink**：**类型变化** → 原物整份改名成 `/etc/foo.lpkgsave`，
 * 新链接**就地**落 `/etc/foo`。
 *
 * ── 本格 2026-09-26 换了语义（与 9 格里的同名迁移**现在一致了**）──────────────────
 * 改前：链接条目"一律按配置冲突处理"—— 不接管 `/etc/foo`，用户那份普通文件**留原样**，
 * 新链接退到 `/etc/foo.lpkgnew` 等审阅（旧用例名 `EtcFileToSymlinkGoesToLpkgnew`）。
 * 那条规则的理由是"`is_directory` 会跟随链接，不管住就会绕过整段配置保护"—— 防的是
 * **盘上是真目录**那一格，却顺手把"盘上是普通文件"也塞进了 `.lpkgnew` 分支。
 * 改后：**类型变化（符号链接 / 文件 / 目录 任意两者互换）统一走一条规则** ——
 * 原物 `.lpkgsave`（内容一个不丢、可回滚）+ 新物就地处。只有**类型未变**的配置才谈
 * "用户改没改过"（file→file 三哈希、symlink→symlink 退 `.lpkgnew`）。
 *
 * 这不是把一个断言放宽，而是**政策变了**：`.lpkgsave` 与 `.lpkgnew` 都保证"用户那份可寻回"，
 * 区别只在"谁占着原路径"。类型都换了，原路径上那份就已经不是"这个形态的配置"了，
 * 让新物就位、旧物留副本，比让新物退到 `.lpkgnew` 更贴近 pacman 的接管语义。
 */
TEST_F(TypeTransitionMatrixTest, EtcFileToSymlinkGoesToLpkgsave)
{
    const std::string pkg = "m_etc_f2s";
    const std::string v1 =
        pack(pkg, "1.0", [](const fs::path& c) { write_file(c / "etc/foo", "v1 conf\n"); });
    ASSERT_TRUE(install_err(v1).empty()) << "前置：v1 装不上，本格其余步骤无意义";
    ASSERT_EQ(shape_of(test_root / "etc/foo"), "file");

    write_file(test_root / "etc/foo", "user edited\n");  // ② 用户改动留痕

    const std::string v2 = pack(pkg, "2.0", [](const fs::path& c) {
        fs::create_directories(c / "etc");
        fs::create_symlink("foo.conf", c / "etc/foo");
        write_file(c / "etc/foo.conf", "v2 conf\n");
    });

    // ③ 注入中途失败：断点落在 etc/foo.conf 的 COPY 分支（链接条目自己的落位没有断点）
    bool bp_hit = false;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&bp_hit] {
        bp_hit = true;
        throw LpkgException("injected copy failure");
    });
    const std::string e2 = install_err(v2);
    BreakpointManager::instance().clear_all();
    EXPECT_FALSE(e2.empty()) << "断点没生效（命中=" << bp_hit << "）";
    EXPECT_TRUE(bp_hit) << "断点没命中：etc/foo.conf 应当走 COPY 分支";

    EXPECT_EQ(shape_of(test_root / "etc/foo"), "file")
        << "③ 回滚后 /etc/foo 应仍是用户那份普通文件";
    EXPECT_EQ(read_file(test_root / "etc/foo"), "user edited\n");
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgsave"), "absent")
        << "③ 回滚必须撤销这次 save_config（.lpkgsave 是本批次造的）";
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgnew"), "absent")
        << "③ 本格**不再**产生 .lpkgnew（类型变化走 .lpkgsave）";
    EXPECT_EQ(shape_of(test_root / "etc/foo.conf"), "absent")
        << "③ 回滚必须撤掉 v2 的 etc/foo.conf";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");

    // ④ 干净装 v2
    const std::string e3 = install_err(v2);
    EXPECT_TRUE(e3.empty()) << "v2 安装失败，异常原文：" << e3;
    if (!e3.empty()) std::cerr << "[type-matrix] " << pkg << " 步骤④ 异常：" << e3 << "\n";
    EXPECT_EQ(shape_of(test_root / "etc/foo"), "symlink") << "④ 新链接应**就地**落 /etc/foo";
    if (fs::is_symlink(test_root / "etc/foo")) {
        EXPECT_EQ(fs::read_symlink(test_root / "etc/foo").string(), "foo.conf");
    }
    EXPECT_EQ(read_file(test_root / "etc/foo.conf"), "v2 conf\n");
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgsave"), "file")
        << "④ 用户那份普通文件必须整份改名到 .lpkgsave 等用户处理";
    if (fs::is_regular_file(test_root / "etc/foo.lpkgsave")) {
        EXPECT_EQ(read_file(test_root / "etc/foo.lpkgsave"), "user edited\n")
            << "④ .lpkgsave 里必须逐字节是用户那份";
    }
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgnew"), "absent")
        << "④ 类型变化不再退 .lpkgnew（那是「类型未变的配置冲突」的落点）";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "2.0");
    EXPECT_EQ(bak_residue(), 0);
}

/**
 * `/etc` 的 **symlink → file**：反方向的类型变化，同一政策（原物 `.lpkgsave` + 新物就地处）。
 *
 * 改前这一格走的是**三哈希**（拿链接目标的内容当 `hash_local` 去和包内那份比）——
 * 类型都换了还谈"用户改没改过这份配置"没有意义，那条路径只是"链接碰巧被当成非目录条目"
 * 落进了三哈希的入口条件（`disk_exists && !disk_is_dir`）。改后与上一格对称。
 */
TEST_F(TypeTransitionMatrixTest, EtcSymlinkToFileGoesToLpkgsave)
{
    const std::string pkg = "m_etc_s2f";
    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        fs::create_directories(c / "etc");
        fs::create_symlink("foo.conf", c / "etc/foo");
        write_file(c / "etc/foo.conf", "v1 conf\n");
    });
    ASSERT_TRUE(install_err(v1).empty()) << "前置：v1 装不上";
    ASSERT_EQ(shape_of(test_root / "etc/foo"), "symlink");

    // ② 用户改动留痕：把链接**换成另一个目标**（这格里"用户那份"就是这条链接本身）
    std::error_code ec;
    fs::remove(test_root / "etc/foo", ec);
    fs::create_symlink("user-target.conf", test_root / "etc/foo");
    ASSERT_EQ(shape_of(test_root / "etc/foo"), "symlink");

    const std::string v2 =
        pack(pkg, "2.0", [](const fs::path& c) { write_file(c / "etc/foo", "v2 file\n"); });

    // ③ 注入中途失败：本格的 v2 只有 etc/foo 一个条目 → 它自己就是普通文件，走 COPY 分支
    bool bp_hit = false;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&bp_hit] {
        bp_hit = true;
        throw LpkgException("injected copy failure");
    });
    const std::string e2 = install_err(v2);
    BreakpointManager::instance().clear_all();
    EXPECT_FALSE(e2.empty()) << "断点没生效（命中=" << bp_hit << "）";
    EXPECT_TRUE(bp_hit) << "断点没命中：etc/foo 应当走 COPY 分支";

    EXPECT_EQ(shape_of(test_root / "etc/foo"), "symlink") << "③ 回滚后 /etc/foo 必须还是链接";
    if (fs::is_symlink(test_root / "etc/foo")) {
        EXPECT_EQ(fs::read_symlink(test_root / "etc/foo").string(), "user-target.conf")
            << "③ 回滚必须还原**用户改过的**链接目标";
    }
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgsave"), "absent");
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgnew"), "absent");
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");

    // ④ 干净装 v2
    const std::string e3 = install_err(v2);
    EXPECT_TRUE(e3.empty()) << "v2 安装失败，异常原文：" << e3;
    if (!e3.empty()) std::cerr << "[type-matrix] " << pkg << " 步骤④ 异常：" << e3 << "\n";
    EXPECT_EQ(shape_of(test_root / "etc/foo"), "file") << "④ 新文件应**就地**落 /etc/foo";
    if (fs::is_regular_file(test_root / "etc/foo")) {
        EXPECT_EQ(read_file(test_root / "etc/foo"), "v2 file\n");
    }
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgsave"), "symlink")
        << "④ 用户改过的那条链接必须改名到 .lpkgsave 等用户处理";
    if (fs::is_symlink(test_root / "etc/foo.lpkgsave")) {
        EXPECT_EQ(fs::read_symlink(test_root / "etc/foo.lpkgsave").string(), "user-target.conf")
            << "④ .lpkgsave 必须是用户改过的那个目标";
    }
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgnew"), "absent");
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "2.0");
    EXPECT_EQ(bak_residue(), 0);
}

/**
 * **类型未变**（symlink → symlink）仍走配置冲突那条路：不接管、v2 退 `.lpkgnew`。
 *
 * 与上面两格成对，专钉"类型变化"与"类型未变"的**分界**：如果实现方把"盘上被占"一律
 * 当成类型变化（或反之），这两格必有一格红。类型未变才谈"用户改没改过这条链接"，
 * 所以这里仍是**新物退让**（`.lpkgnew`），原路径上用户那份不动。
 */
TEST_F(TypeTransitionMatrixTest, EtcSymlinkToSymlinkStaysLpkgnew)
{
    const std::string pkg = "m_etc_s2s";
    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        fs::create_directories(c / "etc");
        fs::create_symlink("v1-target.conf", c / "etc/foo");
    });
    ASSERT_TRUE(install_err(v1).empty()) << "前置：v1 装不上";
    ASSERT_EQ(shape_of(test_root / "etc/foo"), "symlink");

    const std::string v2 = pack(pkg, "2.0", [](const fs::path& c) {
        fs::create_directories(c / "etc");
        fs::create_symlink("v2-target.conf", c / "etc/foo");
    });

    const std::string e2 = install_err(v2);
    EXPECT_TRUE(e2.empty()) << "v2 安装失败，异常原文：" << e2;
    if (!e2.empty()) std::cerr << "[type-matrix] " << pkg << " 异常：" << e2 << "\n";

    EXPECT_EQ(shape_of(test_root / "etc/foo"), "symlink")
        << "类型未变 ⇒ 不接管，原路径上仍是盘上那条链接";
    if (fs::is_symlink(test_root / "etc/foo")) {
        EXPECT_EQ(fs::read_symlink(test_root / "etc/foo").string(), "v1-target.conf")
            << "盘上那条链接不动";
    }
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgnew"), "symlink")
        << "类型未变的配置冲突 ⇒ 新链接退 .lpkgnew 等审阅";
    if (fs::is_symlink(test_root / "etc/foo.lpkgnew")) {
        EXPECT_EQ(fs::read_symlink(test_root / "etc/foo.lpkgnew").string(), "v2-target.conf");
    }
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgsave"), "absent")
        << "类型未变**不该**走 .lpkgsave（那是类型变化的落点）";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "2.0");
}

/**
 * **升级后新版本不再提供**该 `/etc` 配置 → 原物改名成 `.lpkgsave`（不再是"留原位不动"）。
 *
 * ── 本格 2026-09-26 换了语义（`DropOwnership` → `SaveConfigObsolete`）────────────────
 * 改前：废弃的 `/etc` 条目只撤所有权 + 撤配置哈希记录，**文件原地不动**（`DropOwnership`，
 * 决策表里那格注释写着"这正是 `/etc` 路径天然无法满足『盘面 == 新版本形态』的原因"）。
 * 改后：**只要一条路径会被彻底放弃，就把原物留成 `.lpkgsave`** —— 与"移除整包"侧
 * （`rm_save_conf_after_wal_`）和"类型变化"侧统一到同一条规则：`/etc` 下的东西
 * 永远不会被 lpkg 无声丢掉，也永远不会占着"新版本该用的那个名字"。
 *
 * 有意的**例外**：`/etc` 的废弃**目录**仍只撤所有权、不碰盘（维护者明确的范围是"要彻底删
 * 一个**文件**的路径"；且移除侧对 `/etc` 目录本来就有意的"`<dir>.lpkgsave` 会把不知名的
 * 目录整个搬走，宁可不碰（只告警）"）。由 `EtcObsoleteDirIsLeftInPlace` 单独钉。
 */
TEST_F(TypeTransitionMatrixTest, EtcObsoleteFileGoesToLpkgsave)
{
    const std::string pkg = "m_etc_obs";
    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        write_file(c / "etc/dropped.conf", "v1 conf\n");
        write_file(c / "etc/kept.conf", "kept v1\n");
    });
    ASSERT_TRUE(install_err(v1).empty()) << "前置：v1 装不上";
    ASSERT_EQ(shape_of(test_root / "etc/dropped.conf"), "file");

    // ② 用户改动留痕（废弃时同样不许丢）
    write_file(test_root / "etc/dropped.conf", "user edited\n");

    // v2 **不再提供** etc/dropped.conf（但保留另一个配置，保证 /etc 这一族真的走了一遍）
    const std::string v2 =
        pack(pkg, "2.0", [](const fs::path& c) { write_file(c / "etc/kept.conf", "kept v2\n"); });

    // ③ 注入中途失败：断点落在 kept.conf 的 COPY 分支 → 整批回滚
    bool bp_hit = false;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&bp_hit] {
        bp_hit = true;
        throw LpkgException("injected copy failure");
    });
    const std::string e2 = install_err(v2);
    BreakpointManager::instance().clear_all();
    EXPECT_FALSE(e2.empty()) << "断点没生效（命中=" << bp_hit << "）";
    EXPECT_TRUE(bp_hit) << "断点没命中：etc/kept.conf 应当走 COPY 分支";

    EXPECT_EQ(shape_of(test_root / "etc/dropped.conf"), "file")
        << "③ 回滚后废弃配置必须回到**原路径原名**";
    EXPECT_EQ(read_file(test_root / "etc/dropped.conf"), "user edited\n")
        << "③ 且必须逐字节是用户那份";
    EXPECT_EQ(shape_of(test_root / "etc/dropped.conf.lpkgsave"), "absent")
        << "③ 回滚必须撤销这次改名（.lpkgsave 是本批次造的）";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");

    // ④ 干净装 v2：废弃的那份改名保留
    const std::string e3 = install_err(v2);
    EXPECT_TRUE(e3.empty()) << "v2 安装失败，异常原文：" << e3;
    if (!e3.empty()) std::cerr << "[type-matrix] " << pkg << " 步骤④ 异常：" << e3 << "\n";
    EXPECT_EQ(shape_of(test_root / "etc/dropped.conf"), "absent")
        << "④ 废弃配置不该再占着原路径（它已经不是新版本的一部分）";
    EXPECT_EQ(shape_of(test_root / "etc/dropped.conf.lpkgsave"), "file")
        << "④ 用户那份必须留成 .lpkgsave（**不是**静默留在原位、更不是删掉）";
    if (fs::is_regular_file(test_root / "etc/dropped.conf.lpkgsave")) {
        EXPECT_EQ(read_file(test_root / "etc/dropped.conf.lpkgsave"), "user edited\n")
            << "④ .lpkgsave 里必须逐字节是用户那份";
    }
    EXPECT_EQ(read_file(test_root / "etc/kept.conf"), "kept v2\n") << "④ 保留的配置照常换新版";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "2.0");
    EXPECT_EQ(bak_residue(), 0);
}

/**
 * 废弃的 `/etc` **目录**：维持"只撤所有权、不碰盘"（上面的有意例外）。
 *
 * 与上一格成对：文件走 `.lpkgsave`、目录不动。若实现方把两者一起改，这条会红。
 */
TEST_F(TypeTransitionMatrixTest, EtcObsoleteDirIsLeftInPlace)
{
    const std::string pkg = "m_etc_obsdir";
    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        write_file(c / "etc/droppeddir/a.conf", "v1 conf\n");
        write_file(c / "etc/kept.conf", "kept v1\n");
    });
    ASSERT_TRUE(install_err(v1).empty()) << "前置：v1 装不上";
    ASSERT_EQ(shape_of(test_root / "etc/droppeddir"), "dir");

    const std::string v2 =
        pack(pkg, "2.0", [](const fs::path& c) { write_file(c / "etc/kept.conf", "kept v2\n"); });
    const std::string e2 = install_err(v2);
    EXPECT_TRUE(e2.empty()) << "v2 安装失败，异常原文：" << e2;

    EXPECT_EQ(shape_of(test_root / "etc/droppeddir"), "dir")
        << "废弃的 /etc 目录必须原地留着（有意例外：目录不搬、不删、也不改名）";
    EXPECT_EQ(shape_of(test_root / "etc/droppeddir.lpkgsave"), "absent")
        << "目录**不该**被改名成 .lpkgsave（那是文件/链接的落点）";
    // **但目录里的文件是文件** —— 它们各自是"被彻底放弃的 /etc 文件路径"，所以按同一条
    // 规则改名成 `.lpkgsave`（决策表按**单个路径**的事实做判断，不知道"父目录被有意留着"）。
    // 结果是这个被留着的目录里躺着 `.lpkgsave` 而不是原名 —— 行为与"废弃文件"一致，
    // 只是看起来有点怪（见实现报告的"待确认"一节）。
    EXPECT_EQ(shape_of(test_root / "etc/droppeddir/a.conf"), "absent")
        << "被废弃的 /etc 文件（哪怕它在一个被留着的目录里）也该改名让位";
    EXPECT_EQ(read_file(test_root / "etc/droppeddir/a.conf.lpkgsave"), "v1 conf\n")
        << "内容必须逐字节留在 .lpkgsave 里";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "2.0");
}

// ============================================================================
// `/etc` 的 dir → 非目录（反方向：盘上是**真目录**、新版本发文件/符号链接）
//
// ── 实测（2026-09-25，本文件加入这两格当天）：**两条腿都装不上** ─────────────────
//   · dir → file     ：`safe_rename failed: Is a directory`（EISDIR）—— 挡路目录没人让开，
//                      `.lpkgtmp` 被 rename 到那个**目录**上。
//   · dir → symlink  ：`Cannot replace a directory with a symlink.`
//                      （copy_package_files 里 `is_directory(dest) && !is_symlink(dest)` 的守卫）
//   两条都整批回滚、不丢数据，所以症状是"这条升级路径永远不可能成功"，而不是静默损坏。
//
// ── 根因：`backup_existing_files()` 的 `/etc` 早退**抢在**"真目录挡路"分支之前 ──────
//   `if (f.starts_with(DIR_ETC)) continue;` 在 `fs::exists(physical_path)` 那个 if **之前**，
//   于是归档条目是非目录、路径又在 `/etc` 下时，盘上那个**真目录**压根走不到"真目录挡路"
//   分支 —— 那个分支里 `/etc` 的 `sink.save_config(physical_path)` 因此是**死代码**
//   （控制流到那里时 `f` 绝不可能以 `/etc` 开头）。非 `/etc` 路径不受影响：它们能走到
//   `sink.backup`，所以 9 格里那两格是绿的（`4afb443d` 修的正是非 /etc 那一半）。
//   冲突预检是**放行**的（`dir_tree_entirely_ours` → 整棵树都归本包 ⇒ 接管），所以用户看到
//   的不是"被拒绝"，而是拷贝阶段一个 EISDIR —— 这条路径在预检与执行之间是脱节的。
//
//   **修法（已实施，`9e91f2ba`）**：早退结构本身被决策表取代 —— `decide_path` 对 `/etc` 非目录
//   条目先判 `disk_is_dir`：真目录 ⇒ `let_go = SaveConfig`（整树改名 `.lpkgsave`）+ `write =
//   WriteInPlace`（就地落位、不落 `.lpkgnew`）；其余 ⇒ `let_go = Noop`，三哈希仍在写入趟。
// ============================================================================

/**
 * `/etc` 的 **dir → file**：盘上是真目录、新版本发一个普通文件。
 *
 * 与非 `/etc` 的同名格（DirToFile）**语义相同、落点不同**：挡路目录整树改名成
 * `/etc/foo.lpkgsave/`（内容一个不丢），路径让开后新文件**就地**落 `/etc/foo`，
 * **不产生** `.lpkgnew`（`.lpkgnew` 是"盘上是**普通文件**、新版本要接管它"那条路的语义）。
 */
TEST_F(TypeTransitionMatrixTest, EtcDirToFileKeepsLpkgsaveDir)
{
    const std::string pkg = "m_etc_d2f";
    const std::string v1 =
        pack(pkg, "1.0", [](const fs::path& c) { write_file(c / "etc/foo/a.conf", "v1 conf\n"); });
    ASSERT_TRUE(install_err(v1).empty()) << "前置：v1 装不上，本格其余步骤无意义";
    ASSERT_EQ(shape_of(test_root / "etc/foo"), "dir");
    ASSERT_EQ(read_file(test_root / "etc/foo/a.conf"), "v1 conf\n");

    // ② 用户改动留痕：改**包里自己那个条目**的内容 —— 不引入无主文件（那会让整批被
    //    正确拒绝，见 DirToFileWithUnownedContentIsRefused），整树仍归本包 ⇒ 接管放行。
    write_file(test_root / "etc/foo/a.conf", "user edited\n");

    const std::string v2 =
        pack(pkg, "2.0", [](const fs::path& c) { write_file(c / "etc/foo", "v2 file\n"); });

    // ③ 注入中途失败：`etc/foo` 自己就是普通文件 → 必然走 COPY 分支（不靠伴生文件）
    bool bp_hit = false;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&bp_hit] {
        bp_hit = true;
        throw LpkgException("injected copy failure");
    });
    const std::string e2 = install_err(v2);
    BreakpointManager::instance().clear_all();
    std::cerr << "[type-matrix] " << pkg << " 步骤③ 异常（断点命中=" << bp_hit << "）：" << e2
              << "\n";
    EXPECT_FALSE(e2.empty()) << "断点没生效（命中=" << bp_hit << "）";

    EXPECT_EQ(shape_of(test_root / "etc/foo"), "dir") << "③ 回滚后 /etc/foo 必须还是目录";
    EXPECT_EQ(read_file(test_root / "etc/foo/a.conf"), "user edited\n")
        << "③ 回滚后目录里的配置必须是用户改过的那份（逐字节）";
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgsave"), "absent")
        << "③ 回滚必须撤销这次 save_config（.lpkgsave 是本批次造出来的）";
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgnew"), "absent");
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");

    // ④ 干净装 v2
    const std::string e3 = install_err(v2);
    EXPECT_TRUE(e3.empty()) << "v2（目录被文件接管）安装失败，异常原文：" << e3;
    if (!e3.empty()) std::cerr << "[type-matrix] " << pkg << " 步骤④ 异常：" << e3 << "\n";
    EXPECT_EQ(shape_of(test_root / "etc/foo"), "file") << "④ /etc/foo 应被接管成普通文件";
    // 读之前必须先判形态：对**目录**路径开 ifstream 会抛 basic_filebuf::underflow 异常，
    // 把测试从这一行直接打断，后面几条更有信息量的断言就再也不执行（实测踩过）。
    // 花括号不能省：gtest 的 EXPECT_EQ 展开成 if/else，裸写会被 -Werror=dangling-else 拦下
    if (fs::is_regular_file(test_root / "etc/foo")) {
        EXPECT_EQ(read_file(test_root / "etc/foo"), "v2 file\n");
    }
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgsave"), "dir")
        << "④ 被接管的 /etc 目录必须**整树**改名保留成 .lpkgsave（是目录、内容一个不丢）";
    if (fs::is_directory(test_root / "etc/foo.lpkgsave")) {
        EXPECT_EQ(read_file(test_root / "etc/foo.lpkgsave/a.conf"), "user edited\n")
            << "④ .lpkgsave 里必须逐字节是用户那份配置";
    }
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgnew"), "absent")
        << "④ 目录让开后新条目**就地**落位，不该退到 .lpkgnew";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "2.0");
    EXPECT_EQ(bak_residue(), 0);
}

/**
 * `/etc` 的 **dir → symlink**：与上一格同一判据、同一落点政策，只是新形态是符号链接。
 *
 * 链接条目在 `copy_package_files` 里没有故障注入断点（文件头记录的已知缺口），所以 v2 另带一个
 * 必然走 COPY 的 `etc/foo.conf`；不过**本格实测的失败点更早**（见下），断点是否命中不作断言，
 * 只打印。
 */
TEST_F(TypeTransitionMatrixTest, EtcDirToSymlinkKeepsLpkgsaveDir)
{
    const std::string pkg = "m_etc_d2s";
    const std::string v1 =
        pack(pkg, "1.0", [](const fs::path& c) { write_file(c / "etc/foo/a.conf", "v1 conf\n"); });
    ASSERT_TRUE(install_err(v1).empty()) << "前置：v1 装不上，本格其余步骤无意义";
    ASSERT_EQ(shape_of(test_root / "etc/foo"), "dir");
    ASSERT_EQ(read_file(test_root / "etc/foo/a.conf"), "v1 conf\n");

    write_file(test_root / "etc/foo/a.conf", "user edited\n");  // ② 用户改动留痕

    const std::string v2 = pack(pkg, "2.0", [](const fs::path& c) {
        fs::create_directories(c / "etc");
        fs::create_symlink("foo.conf", c / "etc/foo");
        write_file(c / "etc/foo.conf", "v2 conf\n");
    });

    // ③ 注入中途失败
    bool bp_hit = false;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&bp_hit] {
        bp_hit = true;
        throw LpkgException("injected copy failure");
    });
    const std::string e2 = install_err(v2);
    BreakpointManager::instance().clear_all();
    std::cerr << "[type-matrix] " << pkg << " 步骤③ 异常（断点命中=" << bp_hit << "）：" << e2
              << "\n";
    EXPECT_FALSE(e2.empty()) << "本批必须失败（断点命中=" << bp_hit << "）";

    EXPECT_EQ(shape_of(test_root / "etc/foo"), "dir") << "③ 回滚后 /etc/foo 必须还是目录";
    EXPECT_EQ(read_file(test_root / "etc/foo/a.conf"), "user edited\n")
        << "③ 回滚后目录里的配置必须是用户改过的那份（逐字节）";
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgsave"), "absent");
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgnew"), "absent");
    EXPECT_EQ(shape_of(test_root / "etc/foo.conf"), "absent")
        << "③ 回滚必须撤掉 v2 的 etc/foo.conf";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");

    // ④ 干净装 v2
    const std::string e3 = install_err(v2);
    EXPECT_TRUE(e3.empty()) << "v2（目录被符号链接接管）安装失败，异常原文：" << e3;
    if (!e3.empty()) std::cerr << "[type-matrix] " << pkg << " 步骤④ 异常：" << e3 << "\n";
    EXPECT_EQ(shape_of(test_root / "etc/foo"), "symlink") << "④ /etc/foo 应被接管成符号链接";
    if (fs::is_symlink(test_root / "etc/foo")) {
        EXPECT_EQ(fs::read_symlink(test_root / "etc/foo").string(), "foo.conf");
    }
    if (fs::is_regular_file(test_root / "etc/foo.conf")) {
        EXPECT_EQ(read_file(test_root / "etc/foo.conf"), "v2 conf\n");
    }
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgsave"), "dir")
        << "④ 被接管的 /etc 目录必须**整树**改名保留成 .lpkgsave";
    if (fs::is_directory(test_root / "etc/foo.lpkgsave")) {
        EXPECT_EQ(read_file(test_root / "etc/foo.lpkgsave/a.conf"), "user edited\n")
            << "④ .lpkgsave 里必须逐字节是用户那份配置";
    }
    EXPECT_EQ(shape_of(test_root / "etc/foo.lpkgnew"), "absent")
        << "④ 目录让开后新链接**就地**落位，不该退到 .lpkgnew";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "2.0");
    EXPECT_EQ(bak_residue(), 0);
}

/**
 * dir → file：树里那条**别的包持有的符号链接**，其**解析目标**归本包 —— 必须**拒绝**。
 *
 * 这是 `dir_tree_entirely_ours()` 的判据被 `fs::relative` **解析符号链接**打穿的形态
 * （2026-09-26 修）：算条目的 DB 键时若解析了链接，查到的是**目标**（`usr/share/other.txt`，
 * 归本包）而不是这个名字（`usr/share/thing/link`，归合租包）⇒ 判「整树都是我们的」⇒
 * 整树搬进 stash ⇒ 提交后 stash 被 `remove_all` ⇒ **合租包那份链接永久消失，而它的 DB
 * 归属还在原位**（所有权脱节 —— 正是这个判据存在的唯一意义）。
 *
 * 与上面 `DirToFileWithForeignPackageContentIsRefused` 是**同一判据的两个输入**：那条考
 * "别人的**普通文件**"，本条考"别人的**符号链接、且目标归我们**"（只有后者会被解析打穿）。
 * 注意现场有个必要条件：**链接目标必须存在**（`fs::relative` 解析得到才谈得上走错路）。
 */
TEST_F(TypeTransitionMatrixTest, DirToFileWithForeignSymlinkToOwnPathIsRefused)
{
    const std::string pkg = "m_df_link";
    const std::string co_pkg = "m_df_link_co";
    // 本包在**树外**持有一个路径：这就是"链接的解析目标归我们"那一半
    const auto fill_v1 = [](const fs::path& c) {
        write_file(c / REL / "a.txt", "v1 a\n");
        write_file(c / "usr/share/other.txt", "v1 other\n");
    };
    ASSERT_TRUE(install_err(pack(pkg, "1.0", fill_v1)).empty()) << "前置：v1 装不上";
    ASSERT_EQ(shape_of(test_root / REL), "dir");

    // 合租包在共享目录里放一个**符号链接**，其目标是本包持有的那个路径
    const std::string co_err = install_err(pack(co_pkg, "1.0", [](const fs::path& c) {
        fs::create_directories(c / REL);
        fs::create_symlink("../other.txt", c / REL / "link");
    }));
    ASSERT_TRUE(co_err.empty()) << "前置：合租包装不进共享目录：" << co_err;
    ASSERT_EQ(shape_of(test_root / REL / "link"), "symlink");
    ASSERT_TRUE(fs::exists(test_root / "usr/share/other.txt")) << "前置：链接目标必须存在";

    const auto fill_v2 = [](const fs::path& c) {
        write_file(c / REL, "v2 file\n");  // 目录 → 文件
        write_file(c / "usr/share/other.txt", "v2 other\n");
    };
    const std::string msg = install_err(pack(pkg, "2.0", fill_v2));

    ASSERT_FALSE(msg.empty())
        << "树里那条符号链接**属于别的包** ⇒ dir→file 必须被拒绝。"
           "（用 fs::relative 算键时会解析链接、查到**目标**的归属 = 本包 ⇒ 误判「整树都是"
           "我们的」⇒ 整树搬走 ⇒ 提交后别人那份被 remove_all 永久删掉）";
    std::cerr << "[type-matrix] " << pkg << " 外部链接拒绝原文：" << msg << "\n";
    EXPECT_NE(msg.find("usr/share/thing"), std::string::npos) << "拒绝信息没点名冲突路径：" << msg;
    EXPECT_NE(msg.find(co_pkg), std::string::npos) << "拒绝信息必须点名**真实持有者** " << co_pkg
                                                   << "（报成本包自己 = 把人引去查一个不存在"
                                                      "的冲突源）："
                                                   << msg;

    expect_dir_v1("整批拒绝时盘面必须一个文件都没动");
    EXPECT_EQ(shape_of(test_root / REL / "link"), "symlink") << "合租包那份链接必须原样还在";
    EXPECT_FALSE(fs::exists(test_root / COMPANION)) << "预检拒绝时不该落下 v2 的任何文件";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");
    EXPECT_EQ(Cache::instance().get_installed_version(co_pkg), "1.0");
    EXPECT_EQ(bak_residue(), 0);
}
