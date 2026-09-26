/**
 * test_unstash_primitive.cpp — `un_stash` 原语（`ARCH.md` §9.2 有它的 WAL 行与逆操作）
 *
 * ── 为什么需要这个原语 ──────────────────────────────────────────────────────
 * 第③步把升级改成"**先移除旧版本的全部触碰面，再安装新版本**"。`/etc` 配置的三哈希分流
 * 也要吃这一口：判定 `hash_local`（盘上那份的内容哈希）原先在写入趟读**盘上**那份，而
 * 让开趟一旦把路径统一清空（那是第③步的语义），盘上就没有那份可读了 —— 所以改成
 * 「让开趟先把它搬进 stash → 从 **stash 副本**读 `hash_local` →
 * 判定为保留（`KeepLocal` / `SaveLpkgnew`）时再**搬回原位**"。
 *
 * 搬回去这个动作就是 `OpSink::un_stash()`：WAL 行 `UNSTASH <bak> → <orig>`，
 * **逆操作 = 再搬进 stash**（与 `BACKUP` 恰好互为逆，参考 `reverse_execute` 的说明）。
 *
 * ── 本文件的两半 ─────────────────────────────────────────────────────────────
 *   A. **WAL 层**（`UnstashWalTest`）：前向搬回、bak 缺失时的幂等、逆操作、成对行
 *      （`BACKUP` + `UNSTASH`）在回滚里的收敛、以及 `purge_consumed_stashes` 的
 *      "已消费"判据在引入 UNSTASH 后的加固（不收敛 ⇒ 保留 stash，绝不删唯一一份数据）。
 *   B. **端到端**（`UnstashBreakpointTest`）：三个**新补的 write-ahead 断点**
 *      （`symlink_after_wal_` / `newdir_after_wal_` / `unstash_after_wal_`）各注入一次
 *      失败，断言整批回滚后盘面逐项回到批次前 —— 这三个窗口此前**根本注入不进去**
 *      （`lpkg/CLAUDE.md` §2 记着这个空档：符号链接分支与目录分支都没有断点）。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/install_common.hpp"
#include "../../main/src/pkg/op_sink.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

namespace
{
constexpr const char* ARROW = " \xe2\x86\x92 ";  // " → "（WAL 行分帧用的箭头）
}  // namespace

class UnstashWalTest : public IntegrationTestBase
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

    std::string read_wal() const
    {
        std::ifstream f(wal::wal_log_path());
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
};

// ============================================================================
// A1) 前向：bak → 原位（并留下 UNSTASH 行）
// ============================================================================

TEST_F(UnstashWalTest, ForwardMovesBakBackAndWritesWalLine)
{
    const fs::path orig = test_root / "etc" / "keep.conf";
    const fs::path stash = test_root / ".lpkg_bak_pkgx_1";
    const fs::path bak = stash / "keep.conf.lpkg_bak_pkgx_ab12";
    write_file(bak, "USER-EDITED\n");  // 让开趟搬走之后，原位上什么都没有

    detail::OpSink sink("pkgx", nullptr);
    bool moved = false;
    ASSERT_NO_THROW(moved = sink.un_stash(bak, orig));

    EXPECT_TRUE(moved) << "stash 里有那份 → 必须真的搬回来";
    EXPECT_FALSE(fs::exists(bak));
    ASSERT_TRUE(fs::exists(orig)) << "原位那份必须回来了（保留用户文件的唯一凭据）";
    EXPECT_EQ(read_file(orig), "USER-EDITED\n") << "内容必须逐字节不变";
    // 文件回到原位、stash 里那份没了 —— 这就是"已消费"
    EXPECT_NE(read_wal().find("UNSTASH " + bak.string() + ARROW + orig.string()), std::string::npos)
        << "搬回动作没进 WAL：崩溃后无法回滚（批次内 WAL:\n"
        << read_wal() << ")";
}

// ============================================================================
// A2) 前向幂等：bak 不存在 → 跳过，原位那份**原样不动**
// ============================================================================

TEST_F(UnstashWalTest, ForwardSkipsWhenBakMissingAndLeavesOrigAlone)
{
    const fs::path orig = test_root / "etc" / "keep.conf";
    const fs::path bak = test_root / ".lpkg_bak_pkgx_1" / "keep.conf.lpkg_bak_pkgx_ab12";
    write_file(orig, "STILL-HERE\n");  // 原位那份还在（压根没被搬走过）
    ASSERT_FALSE(fs::exists(bak));

    detail::OpSink sink("pkgx", nullptr);
    bool moved = true;
    ASSERT_NO_THROW(moved = sink.un_stash(bak, orig));

    EXPECT_FALSE(moved) << "没有可搬的 → 返回 false（调用方据此知道没发生搬家）";
    EXPECT_EQ(read_file(orig), "STILL-HERE\n")
        << "**绝不因为找不到备份去碰原位那份** —— 这是幂等分支的安全底线";
    EXPECT_EQ(read_wal().find("UNSTASH "), std::string::npos)
        << "没有真的发生搬家时不该写 UNSTASH 行（行 = 承诺）";
}

// ============================================================================
// A3) 逆操作：UNSTASH 的逆向 = 再搬进 stash（与 BACKUP 互为逆）
// ============================================================================

TEST_F(UnstashWalTest, ReverseOfUnstashMovesItBackIntoStash)
{
    const fs::path orig = test_root / "etc" / "keep.conf";
    const fs::path stash = test_root / ".lpkg_bak_pkgx_1";
    const fs::path bak = stash / "keep.conf.lpkg_bak_pkgx_ab12";
    write_file(orig, "USER-EDITED\n");  // 前向已做：原位有那份，stash 里没有
    fs::create_directories(stash);      // stash 目录本体还在（让开趟只是把文件搬走了）
    ASSERT_FALSE(fs::exists(bak));

    std::vector<wal::WALOp> ops;
    ops.push_back(wal::parse_op("UNSTASH " + bak.string() + ARROW + orig.string()));
    const wal::RollbackStats st = wal::reverse_execute(ops, /*write_audit=*/false);

    EXPECT_EQ(st.files_restored, 1) << "逆操作必须真的搬了一次";
    EXPECT_FALSE(fs::exists(orig)) << "逆操作 = 再搬进 stash（撤销'搬回'）";
    ASSERT_TRUE(fs::exists(bak));
    EXPECT_EQ(read_file(bak), "USER-EDITED\n");
}

// ============================================================================
// A4) 成对行（BACKUP + UNSTASH）在回滚里收敛回原点
//     —— 含"UNSTASH 行已写、rename 未做"这个 write-ahead 窗口
// ============================================================================

TEST_F(UnstashWalTest, BackupPlusUnstashConvergesBackToOrig)
{
    const fs::path orig = test_root / "etc" / "keep.conf";
    const fs::path stash = test_root / ".lpkg_bak_pkgx_1";
    const fs::path bak = stash / "keep.conf.lpkg_bak_pkgx_ab12";

    // 现场 ①（UNSTASH 的前向没做）：那份还在 stash 里 —— 崩在 write-ahead 窗口
    write_file(bak, "USER-EDITED\n");
    std::vector<wal::WALOp> ops;
    ops.push_back(wal::parse_op("BACKUP " + orig.string() + ARROW + bak.string()));
    ops.push_back(wal::parse_op("UNSTASH " + bak.string() + ARROW + orig.string()));
    wal::reverse_execute(ops, false);
    ASSERT_TRUE(fs::exists(orig)) << "现场①：UNSTASH 没做 → BACKUP 的逆操作应该把它搬回原位";
    EXPECT_EQ(read_file(orig), "USER-EDITED\n");
    EXPECT_FALSE(fs::exists(bak));

    // 现场 ②（UNSTASH 的前向已做）：原位有那份、stash 里没有 → 逆序撤两次回到原点
    std::vector<wal::WALOp> ops2;
    ops2.push_back(wal::parse_op("BACKUP " + orig.string() + ARROW + bak.string()));
    ops2.push_back(wal::parse_op("UNSTASH " + bak.string() + ARROW + orig.string()));
    wal::reverse_execute(ops2, false);
    EXPECT_TRUE(fs::exists(orig)) << "现场②：先撤 UNSTASH（→ stash）再撤 BACKUP（→ 原位）";
    EXPECT_EQ(read_file(orig), "USER-EDITED\n");
    EXPECT_FALSE(fs::exists(bak));

    // 再跑一遍（幂等）：结果必须一模一样
    wal::reverse_execute(ops2, false);
    EXPECT_TRUE(fs::exists(orig)) << "重复回滚必须幂等（第二次同样收敛回原位）";
    EXPECT_EQ(read_file(orig), "USER-EDITED\n");
    EXPECT_FALSE(fs::exists(bak));
}

// ============================================================================
// A5) purge 的"已消费"判据：UNSTASH 不收敛时**保留** stash（绝不删唯一一份数据）
// ============================================================================

TEST_F(UnstashWalTest, PurgeKeepsStashWhenUnstashDidNotConverge)
{
    const fs::path stash = test_root / ".lpkg_bak_pkgx_1";
    const fs::path bak1 = stash / "a.lpkg_bak_pkgx_1";  // 同根下的另一个备份（不参与）
    const fs::path bak2 = stash / "b.lpkg_bak_pkgx_2";  // 被 UNSTASH 引用的那份
    const fs::path orig = test_root / "etc" / "b.conf";
    write_file(bak2, "PRECIOUS\n");
    // orig 也存在（前向已做）⇒ 逆操作把它搬回 bak2，于是 reverse 之后 bak2 **仍在**
    // = "没收敛"：唯一一份数据还躺在 stash 里，整目录 remove_all 会把它删掉。
    write_file(orig, "PRECIOUS\n");

    std::vector<wal::WALOp> ops;
    ops.push_back(wal::parse_op("BACKUP /x/a" + std::string(ARROW) + bak1.string()));
    ops.push_back(wal::parse_op("UNSTASH " + bak2.string() + ARROW + orig.string()));
    wal::reverse_execute(ops, false);
    ASSERT_TRUE(fs::exists(bak2)) << "逆操作应把原位那份搬回 stash（本用例的前置）";

    wal::purge_consumed_stashes(ops);

    EXPECT_TRUE(fs::exists(bak2)) << "未收敛的 bak 是**唯一一份数据**，purge 不得 remove_all";
    EXPECT_EQ(read_file(bak2), "PRECIOUS\n");
    EXPECT_TRUE(fs::exists(stash)) << "整个 stash 根都必须留着（宁留残留，不删未收敛的数据）";
}

// ============================================================================
// A6) purge 的正常路径不受影响：收敛好的 stash 照旧整目录清掉
// ============================================================================

TEST_F(UnstashWalTest, PurgeStillRemovesConvergedStashRoots)
{
    const fs::path stash = test_root / ".lpkg_bak_pkgx_1";
    const fs::path bak = stash / "a.lpkg_bak_pkgx_1";
    const fs::path orig = test_root / "usr" / "share" / "a";
    write_file(bak, "OLD\n");

    std::vector<wal::WALOp> ops;
    ops.push_back(wal::parse_op("BACKUP " + orig.string() + ARROW + bak.string()));
    ops.push_back(wal::parse_op("UNSTASH " + bak.string() + ARROW + orig.string()));
    wal::reverse_execute(ops, false);  // 收敛：bak → 原位
    ASSERT_FALSE(fs::exists(bak));
    ASSERT_TRUE(fs::exists(orig));

    wal::purge_consumed_stashes(ops);
    EXPECT_FALSE(fs::exists(stash)) << "收敛好的 stash 照旧整目录清掉（残留为零）";
    EXPECT_TRUE(fs::exists(orig)) << "原位那份不能被 purge 碰";
}

// ============================================================================
// B. 端到端：三个新补的 write-ahead 断点各注入一次失败，断言回滚逐项保真
// ============================================================================

/**
 * 三个断点钉的都是"**WAL 行已写、物理动作未做**"这个窗口：
 *   · `symlink_after_wal_<pkg>` —— `NEW <dest>` 行已落、`create_symlink` 未做；
 *   · `newdir_after_wal_<pkg>`  —— `NEW_DIR <path>` 行已落、`create_directories` 未做；
 *   · `unstash_after_wal_<pkg>` —— `UNSTASH <bak> → <orig>` 行已落、`rename` 未做。
 * 前两个此前**根本无法注入**（符号链接/目录分支没有断点，lpkg/CLAUDE.md §2 记着这个
 * 空档）；第三个是第③步新增的窗口（`/etc` 配置"搬回来"的那一刻）。
 *
 * 每个用例的取证都是三件套：断点**真的命中**（否则断言恒真）+ 失败后**整批回滚**
 * （抛 LpkgException）+ 盘面**逐项**回到批次前（形态 + 内容 + 无 stash/.lpkgtmp 残留）。
 */
class UnstashBreakpointTest : public IntegrationTestBase
{
protected:
    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        setup_local_mirror();  // 空镜像：repo.load_index 快速且不联网
    }

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

    static std::string shape_of(const fs::path& p)
    {
        std::error_code ec;
        if (fs::is_symlink(p, ec)) return "symlink";
        if (fs::is_directory(p, ec)) return "dir";
        if (fs::is_regular_file(p, ec)) return "file";
        return "absent";
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

    /// 未清理的 stash 残留数（`.lpkg_bak_*`）与 `.lpkgtmp` 残留数
    int residue(const std::string& needle) const
    {
        int n = 0;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(test_root, ec)) {
            if (ec) break;
            if (e.path().filename().string().find(needle) != std::string::npos) ++n;
        }
        return n;
    }

    /**
     * stash 里此刻是否躺着某份数据（按备份文件名的**前缀**认）。
     *
     * 与 `residue()` 的区别正是这里要考的东西：`un_stash` 只把文件 rename 回原位、
     * **不删 stash 目录**（消费判断在提交时由 `purge_consumed_stashes` 做），所以"stash
     * 目录还在"对"这份数据此刻在途还是已归位"毫无区分力 —— 只有"文件还在不在"能区分。
     *
     * 为什么要按前缀点名、而不是数个数：让开趟会 stash **新版本会触碰的每个**已存在路径
     * （本例里 `/etc/ub_vanish.conf` 与 `/usr/bin/ub_vanish` 都在内），所以个数是 2 而不是 1
     * —— 写死个数会把"和本条防御无关的那份"也算进来。
     *
     * 备份文件名 = `<原名>` + `SUFFIX_LPKG_BAK`（`.lpkg_bak_`）+ `<包名>_<随机后缀>`
     * （`stash_bak_target`），所以**它自己也含 needle**；本函数因此只下探**目录**
     * （stash 目录 = `.lpkg_bak_<包名>_<pid>`），不靠"对普通文件迭代会报错"这种巧合。
     */
    bool stash_holds(const std::string& bak_name_prefix) const
    {
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(test_root, ec)) {
            if (ec) break;
            if (e.path().filename().string().find(".lpkg_bak_") == std::string::npos) continue;
            std::error_code ec_dir;
            if (!e.is_directory(ec_dir)) continue;
            std::error_code ec2;
            for (const auto& inner : fs::recursive_directory_iterator(e.path(), ec2)) {
                if (!inner.is_regular_file(ec2)) continue;
                if (inner.path().filename().string().starts_with(bak_name_prefix)) return true;
            }
        }
        return false;
    }
};

// ── B1) 符号链接落位窗口（此前没有断点）─────────────────────────────────────

TEST_F(UnstashBreakpointTest, SymlinkPlacementWindowInjectsFailureAndRollsBack)
{
    const std::string pkg = "ub_sym";
    ASSERT_NO_THROW(install_packages({pack(
        pkg, "1.0", [](const fs::path& c) { write_file(c / "usr/share/ub_sym", "v1 file\n"); })}));
    Cache::instance().load();
    ASSERT_EQ(shape_of(test_root / "usr/share/ub_sym"), "file");

    // 用户改动：这份 v1 文件被改过，回滚必须逐字节还原它
    write_file(test_root / "usr/share/ub_sym", "v1 user\n");

    bool bp_hit = false;
    BreakpointManager::instance().set("symlink_after_wal_" + pkg, [&bp_hit] {
        bp_hit = true;
        throw LpkgException("injected: link WAL written, create_symlink not done");
    });
    EXPECT_THROW(install_packages({pack(pkg, "2.0",
                                        [](const fs::path& c) {
                                            fs::create_directories(c / "usr/share");
                                            fs::create_symlink("ub_sym_target",
                                                               c / "usr/share/ub_sym");
                                        })}),
                 LpkgException);
    BreakpointManager::instance().clear_all();
    EXPECT_TRUE(bp_hit) << "断点没命中：本用例没考到「链接已写 WAL、尚未 create_symlink」";

    EXPECT_EQ(shape_of(test_root / "usr/share/ub_sym"), "file")
        << "回滚后必须是 v1 的普通文件（新链接不得留下）";
    EXPECT_EQ(read_file(test_root / "usr/share/ub_sym"), "v1 user\n")
        << "用户改过的那份必须逐字节回来（`ARCH.md` §5.4 不变量 3/4）";
    EXPECT_EQ(residue(".lpkg_bak_"), 0) << "回滚后仍残留 stash";
    EXPECT_EQ(residue(".lpkgtmp"), 0) << "回滚后仍残留 .lpkgtmp";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0") << "DB 必须回到 v1";
}

// ── B2) 目录落位窗口（此前没有断点）─────────────────────────────────────────

TEST_F(UnstashBreakpointTest, DirCreationWindowInjectsFailureAndRollsBack)
{
    const std::string pkg = "ub_dir";
    // v1 在目标路径上什么都没有；v2 新增一个**目录**条目（走 MakeDir + NEW_DIR）
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", [](const fs::path& c) {
        write_file(c / "usr/share/ub_dir_keep.txt", "keep\n");
    })}));
    Cache::instance().load();
    ASSERT_EQ(shape_of(test_root / "usr/share/ub_dir"), "absent");

    bool bp_hit = false;
    BreakpointManager::instance().set("newdir_after_wal_" + pkg, [&bp_hit] {
        bp_hit = true;
        throw LpkgException("injected: NEW_DIR WAL written, mkdir not done");
    });
    EXPECT_THROW(install_packages({pack(pkg, "2.0",
                                        [](const fs::path& c) {
                                            write_file(c / "usr/share/ub_dir/inner.txt", "v2\n");
                                        })}),
                 LpkgException);
    BreakpointManager::instance().clear_all();
    EXPECT_TRUE(bp_hit) << "断点没命中：本用例没考到「NEW_DIR 行已写、目录未建」";

    EXPECT_EQ(shape_of(test_root / "usr/share/ub_dir"), "absent")
        << "回滚后那个目录不该存在（NEW_DIR 的逆操作 = 删掉本批次新建的空目录）";
    EXPECT_EQ(read_file(test_root / "usr/share/ub_dir_keep.txt"), "keep\n");
    EXPECT_EQ(residue(".lpkg_bak_"), 0);
    EXPECT_EQ(residue(".lpkgtmp"), 0);
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");
}

// ── B3) un_stash 窗口：/etc 配置判为"保留"、搬回的那一刻崩掉 ────────────────

TEST_F(UnstashBreakpointTest, UnstashWindowInjectsFailureAndRollsBackKeepLocal)
{
    const std::string pkg = "ub_etc";
    // v1/v2 的该配置**逐字节相同** ⇒ hash_orig == hash_pkg ⇒ KeepLocal（保留用户那份）
    auto fill = [](const fs::path& c) {
        write_file(c / "etc/ub_etc.conf", "SAME\n");
        write_file(c / "usr/bin/ub_etc", "#!/bin/sh\n");
    };
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", fill)}));
    Cache::instance().load();
    ASSERT_EQ(Cache::instance().get_installed_version(pkg), "1.0");
    write_file(test_root / "etc/ub_etc.conf", "USER-CHANGED\n");  // 用户改动留痕

    bool bp_hit = false;
    BreakpointManager::instance().set("unstash_after_wal_" + pkg, [&bp_hit] {
        bp_hit = true;
        throw LpkgException("injected: UNSTASH WAL written, rename not done");
    });
    EXPECT_THROW(install_packages({pack(pkg, "2.0", fill)}), LpkgException);
    BreakpointManager::instance().clear_all();
    EXPECT_TRUE(bp_hit) << "断点没命中：本用例没考到「配置已写 UNSTASH 行、尚未搬回原位」";

    EXPECT_EQ(read_file(test_root / "etc/ub_etc.conf"), "USER-CHANGED\n")
        << "回滚后用户那份必须逐字节回来（先撤 UNSTASH、再撤让开趟的 BACKUP）";
    EXPECT_EQ(shape_of(test_root / "etc/ub_etc.conf.lpkgnew"), "absent")
        << "KeepLocal 连 .lpkgnew 都不该产生";
    EXPECT_EQ(residue(".lpkg_bak_"), 0) << "回滚后仍残留 stash";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");
}

// ── B4) un_stash 窗口：判为"落 .lpkgnew"、搬回之后崩掉 ──────────────────────

TEST_F(UnstashBreakpointTest, UnstashWindowInjectsFailureAndRollsBackSaveLpkgnew)
{
    const std::string pkg = "ub_etc2";
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", [](const fs::path& c) {
        write_file(c / "etc/ub_etc2.conf", "V1\n");
        write_file(c / "usr/bin/ub_etc2", "#!/bin/sh\n");
    })}));
    Cache::instance().load();
    write_file(test_root / "etc/ub_etc2.conf", "USER-CHANGED\n");  // 三者互异 → SaveLpkgnew

    bool bp_hit = false;
    BreakpointManager::instance().set("unstash_after_wal_" + pkg, [&bp_hit] {
        bp_hit = true;
        throw LpkgException("injected: UNSTASH WAL written, rename not done");
    });
    EXPECT_THROW(install_packages({pack(pkg, "2.0",
                                        [](const fs::path& c) {
                                            write_file(c / "etc/ub_etc2.conf", "V2\n");
                                            write_file(c / "usr/bin/ub_etc2", "#!/bin/sh\n");
                                        })}),
                 LpkgException);
    BreakpointManager::instance().clear_all();
    EXPECT_TRUE(bp_hit);

    EXPECT_EQ(read_file(test_root / "etc/ub_etc2.conf"), "USER-CHANGED\n")
        << "用户改过的配置**永不**被静默覆盖（`ARCH.md` §5.4 不变量 4）";
    EXPECT_EQ(shape_of(test_root / "etc/ub_etc2.conf.lpkgnew"), "absent")
        << "这批的「请审阅」副本必须随回滚一起撤掉";
    EXPECT_EQ(residue(".lpkg_bak_"), 0);
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");
}

// ── B5) 第③步的结构性断言：写入趟开始时，本包的 `/etc` 配置**已经不在盘上** ────────

/**
 * 第③步的目标语义是"先移除旧版本的全部触碰面，再安装新版本"，`/etc` 配置家族也不例外：
 * 让开趟把它搬进 stash（为了从 stash 副本读 `hash_local`）、判定之后再搬回来。所以在**写入
 * 趟的中间态**（`copy_after_wal_<pkg>`，WAL 行已落、rename 未做）该配置**不在盘上**，
 * 而 stash 里躺着一份 —— 这条断言把"先搬空再写入"钉成可执行的形态（不是靠注释描述）。
 *
 * 同一条断言的另一半（回滚保真）也在这里：注入失败后配置必须逐字节回到原位。
 */
TEST_F(UnstashBreakpointTest, ConfigIsStashedAwayWhenTheWritePassRuns)
{
    const std::string pkg = "ub_order";
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", [](const fs::path& c) {
        write_file(c / "etc/ub_order.conf", "V1\n");
        write_file(c / "usr/bin/ub_order", "#!/bin/sh\n");
    })}));
    Cache::instance().load();
    write_file(test_root / "etc/ub_order.conf", "USER\n");  // 用户改过

    bool conf_on_disk_during_write = true;
    int stash_baks = 0;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&] {
        conf_on_disk_during_write = fs::exists(test_root / "etc/ub_order.conf");
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(test_root, ec)) {
            if (ec) break;
            if (e.path().filename().string().find(".lpkg_bak_") != std::string::npos) ++stash_baks;
        }
        throw LpkgException("injected: 停在写入趟中间态取证");
    });
    EXPECT_THROW(install_packages({pack(pkg, "2.0",
                                        [](const fs::path& c) {
                                            write_file(c / "etc/ub_order.conf", "V2\n");
                                            write_file(c / "usr/bin/ub_order", "#!/bin/sh\n");
                                        })}),
                 LpkgException);
    BreakpointManager::instance().clear_all();

    EXPECT_FALSE(conf_on_disk_during_write)
        << "写入趟开始时 `/etc` 配置还在盘上 —— 说明让开趟没有先把它搬走（第③步的语义没生效）";
    EXPECT_GE(stash_baks, 1) << "配置被搬走了，但 stash 里找不到那份备份（搬哪去了？）";
    // 回滚：判定没做完就失败 → 配置必须逐字节回到批次前（先撤 UNSTASH、再撤 BACKUP）
    EXPECT_EQ(read_file(test_root / "etc/ub_order.conf"), "USER\n");
    EXPECT_EQ(residue(".lpkg_bak_"), 0);
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");
}

// ── B6) 纵深防御：包内那份在让开趟中途消失 → 配置必须被放回原位（绝不静默丢失）────

/**
 * 让开趟把 `/etc` 配置搬进 stash 之后，写入趟**按新的一次 `content/` 扫描**遍历条目 ——
 * 那个条目如果不在扫描结果里，写入趟连碰都不会碰它，于是搬进 stash 的那份会在提交后随
 * stash 一起被清理掉：**静默丢一份配置**。这条防线（判据 = "包内那份读不到"）就是为它准备的。
 *
 * 今天不可达（两趟扫描的是同一个目录，中间没有东西动它），所以这里用断点**人为造出**那个
 * 状态：`conf_replace_after_wal_<pkg>` 正好落在"BACKUP 行已写、rename 已做"之后、记录写出
 * 之前，回调里把包内那份删掉 —— 正好复刻"搬走了但读不到"。
 */
// 名字里的 "LeavesConfigInPlace" 是改前的落点；2026-09-26 起那条路径被判成废弃条目、
// 改名成 `.lpkgsave`（见断言处的说明），所以改名以反映**不变量**而不是落点。
TEST_F(UnstashBreakpointTest, VanishedPackageContentDoesNotLoseTheConfig)
{
    const std::string pkg = "ub_vanish";
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", [](const fs::path& c) {
        write_file(c / "etc/ub_vanish.conf", "V1\n");
        write_file(c / "usr/bin/ub_vanish", "#!/bin/sh\n");
    })}));
    Cache::instance().load();
    write_file(test_root / "etc/ub_vanish.conf", "USER\n");

    bool bp_hit = false;
    BreakpointManager::instance().set("conf_replace_after_wal_" + pkg, [&bp_hit, &pkg] {
        bp_hit = true;
        // 把**包内**那份（解压临时目录里的）删掉：模拟"写入趟扫描不到它"
        std::error_code ec;
        fs::remove(Config::instance().get_tmp_dir() / pkg / "content" / "etc" / "ub_vanish.conf",
                   ec);
    });
    // ② 放回那一刻（`UNSTASH` 行已落、rename 未做）—— 这个断点此前**没接**在这个调用点上
    // （`record_let_go_facts()` 是唯一不传 `after_wal_breakpoint` 的 un_stash 调用点），
    // 于是"搬回去但没搬成"这个窗口**注入不进去**。接上之后这里必须命中，且命中时刻的
    // 盘面应当**≠ 基线**：原位没有那份、stash 里躺着那份（= "在途"）。
    bool unstash_bp_hit = false;
    std::string orig_shape_inflight;
    bool config_in_stash_inflight = false;
    BreakpointManager::instance().set("unstash_after_wal_" + pkg, [&] {
        unstash_bp_hit = true;
        orig_shape_inflight = shape_of(test_root / "etc/ub_vanish.conf");
        config_in_stash_inflight = stash_holds("ub_vanish.conf.lpkg_bak_");
    });
    const auto v2 = pack(pkg, "2.0", [](const fs::path& c) {
        write_file(c / "etc/ub_vanish.conf", "V2\n");
        write_file(c / "usr/bin/ub_vanish", "#!/bin/sh\n");
    });
    // 这一批**会成功**：少了一个归档条目不构成失败（它只是不在本批次的处置范围里），
    // 关键是"用户那份配置一个字节都没丢"。
    EXPECT_NO_THROW(install_packages({v2}));
    BreakpointManager::instance().clear_all();
    EXPECT_TRUE(bp_hit) << "断点没命中：没造出「配置已搬走、包内那份读不到」那一刻";
    EXPECT_TRUE(unstash_bp_hit) << "unstash_after_wal 断点没命中 ⇒ 该调用点没把断点传下去"
                                   "（这条防御的窗口仍然注入不进去）";
    EXPECT_EQ(orig_shape_inflight, "absent")
        << "断点时刻原位应当**没有**那份（让开趟已把它搬进 stash、rename 还没做）";
    EXPECT_TRUE(config_in_stash_inflight)
        << "断点时刻 stash 里应当躺着这份配置 —— 否则上面那条只是恒真废话"
           "（「从未搬过」同样是 absent）";

    // **配置永不静默丢失**：用户那份必须能在**原位**或 `<路径>.lpkgsave` 里逐字节找回。
    // 本用例的现场落点是后者（2026-09-26 起）：断点把**包内**那份删掉之后，这条路径在
    // `remove_obsolete_files()` 眼里就是"新版本不再提供"的废弃条目，于是按新规则改名成
    // `.lpkgsave`（改前它"保持原位不动"，所以这条断言当时写的是原位）。
    // 断言的是**内容还在**这条不变量，不是它落在哪个名字上 —— 两个落点都逐字节比。
    {
        const std::string in_place = read_file(test_root / "etc/ub_vanish.conf");
        const std::string at_save = read_file(test_root / "etc/ub_vanish.conf.lpkgsave");
        EXPECT_TRUE(in_place == "USER\n" || at_save == "USER\n")
            << "用户那份配置既不在原位也不在 .lpkgsave 里 —— 配置被静默丢了（原位=\"" << in_place
            << "\"，.lpkgsave=\"" << at_save << "\"）";
    }
    EXPECT_EQ(residue(".lpkg_bak_"), 0) << "stash 已被 un_stash 消费干净，不该有残留";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "2.0");
    EXPECT_FALSE(Cache::instance().is_file_owned_by("/etc/ub_vanish.conf", pkg))
        << "本批次没处置这个条目 ⇒ 归属也不该留下（v2 的清单里没有它）";
}
