/**
 * test_config_save_on_remove.cpp — 移除时配置文件变 `.lpkgsave`（除非 --purge-config）
 *
 * 语义（已拍板，勿改）：
 *   任何移除路径（`remove` / `remove -r` / `autoremove` / `force-solve-conflict`，
 *   **无论给不给 --force**）碰到包内配置文件（判据仍是路径前缀 `/etc/`，
 *   `constants::DIR_ETC_PREFIX`）时：
 *     - 默认把原文件**改名**成 `<路径>.lpkgsave`：既不原地保留（旧非 force 行为）、
 *       也不静默删除（旧 force / `remove -r` / autoremove 的行为）；
 *     - 只有显式 `--purge-config` 才真删；
 *     - `<路径>.lpkgsave` 已存在时**不覆盖**，把旧的移位到 `.lpkgsave.1`、`.lpkgsave.2`…
 *       （pacman 的 shift_pacsave）。
 *   非配置文件的普通文件行为不变（真删）；升级侧的 /etc 语义（保留 + `.lpkgnew`）不变。
 *
 * 本文件覆盖三类断言，改前全部判红：
 *   1. 三条移除路径都产出 `.lpkgsave` 且内容 == 原配置（旧实现：非 force 原地不动、
 *      force 搬进 stash 后真删、`-r`/autoremove 内部硬编 force 也真删）
 *   2. `--purge-config` 才真删；已有 `.lpkgsave` 时移位不丢旧内容
 *   3. 崩溃/回滚保真：改名是 write-ahead 的，回滚把 `.lpkgsave` 改名回原地且不留半成品
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
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

class ConfigSaveOnRemoveTest : public IntegrationTestBase
{
protected:
    fs::path mirror_dir;

    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        Config::instance().set_no_hooks_mode(true);
        mirror_dir = setup_local_mirror();
        BreakpointManager::instance().clear_all();
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        IntegrationTestBase::TearDown();
    }

    /** 带任意文件清单的虚拟包（content/<相对路径>，内容由测试给定） */
    std::string create_pkg_files(const std::string& name, const std::string& ver,
                                 const std::vector<std::pair<std::string, std::string>>& files,
                                 const std::vector<std::string>& deps = {})
    {
        fs::path work_dir = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::create_directories(work_dir / "content");
        for (const auto& [rel, content] : files) {
            fs::path p = work_dir / "content" / rel;
            ensure_dir_exists(p.parent_path());
            std::ofstream f(p);
            f << content;
        }
        std::string pkg_path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(pkg_path, work_dir.string(), name, ver, deps, {}, "Man page for " + name, {});
        fs::remove_all(work_dir);
        return pkg_path;
    }

    /** 写仓库索引：name|ver:sha256:deps:provides:needed_so */
    void update_index(const std::vector<std::tuple<std::string, std::string, std::string,
                                                   std::string, std::string>>& entries)
    {
        std::ofstream index(mirror_dir / "index.txt");
        for (const auto& [name, ver, deps, provides, needed_so] : entries) {
            const std::string pkg_path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
            const std::string hash = fs::exists(pkg_path) ? calculate_sha256(pkg_path) : "unknown";
            index << name << "|" << ver << ":" << hash << ":" << deps << ":" << provides << ":"
                  << needed_so << "|\n";
        }
    }

    std::string read_file(const fs::path& p) const
    {
        std::ifstream f(p);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    std::string read_wal() const
    {
        return read_file(wal::wal_log_path());
    }

    /** 覆盖写 WAL（"盘面精确复现"崩溃现场用：把窗口里实测到的真实内容放回去） */
    void write_wal(const std::string& content) const
    {
        const std::string p = wal::wal_log_path();
        fs::create_directories(fs::path(p).parent_path());
        std::ofstream f(p, std::ios::trunc);
        f << content;
        f.flush();
        ASSERT_TRUE(f.good()) << "无法写入 WAL " << p;
    }

    static size_t count_substr(const std::string& hay, const std::string& needle)
    {
        size_t n = 0;
        for (auto pos = hay.find(needle); pos != std::string::npos;
             pos = hay.find(needle, pos + needle.size()))
            ++n;
        return n;
    }
};

// ============================================================================
// 1) 普通 remove：原路径不再有该文件，但 <路径>.lpkgsave 在且内容 == 原配置
// ============================================================================

TEST_F(ConfigSaveOnRemoveTest, NormalRemoveKeepsConfigAsLpkgsave)
{
    const std::string pkg = create_pkg_files(
        "cs_normal", "1.0",
        {{"etc/cs_normal.conf", "CS-NORMAL-CONF\n"}, {"usr/bin/cs_normal", "#!/bin/sh\n"}});
    install_packages({pkg}, "", false);

    const fs::path conf = test_root / "etc/cs_normal.conf";
    const fs::path saved = test_root / "etc/cs_normal.conf.lpkgsave";
    ASSERT_TRUE(fs::exists(conf));
    EXPECT_EQ(read_file(conf), "CS-NORMAL-CONF\n");

    // WAL 取证必须在批次**内**做：批次提交后 finish_committed_batch 会 trim_completed
    // （已提交且无残留 bak → 整个日志清空），事后读只能读到空 WAL。断点
    // `remove_after_package_<pkg>` 正好落在"本包已删完、批次尚未提交"那一刻。
    std::string wal_in_batch;
    BreakpointManager::instance().set("remove_after_package_cs_normal",
                                      [&] { wal_in_batch = read_wal(); });

    remove_package("cs_normal", /*force=*/false);

    EXPECT_FALSE(fs::exists(conf)) << "配置不得原地保留（原地保留 = 用户以为删干净了，其实是残留）";
    ASSERT_TRUE(fs::exists(saved)) << "配置应改名为 .lpkgsave 保留";
    EXPECT_EQ(read_file(saved), "CS-NORMAL-CONF\n") << ".lpkgsave 的内容必须是原配置";
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/cs_normal")) << "普通文件照旧真删";
    EXPECT_TRUE(Cache::instance().get_file_owners("/etc/cs_normal.conf").empty())
        << "配置文件的所有权应从 DB 移除";
    EXPECT_TRUE(Cache::instance().get_installed_version("cs_normal").empty());

    // WAL 必须**看得见**这次改名（崩溃/回滚的恢复依据）：SAVE_CONF <src> → <dst>
    EXPECT_NE(wal_in_batch.find("SAVE_CONF " + conf.string() + " \xe2\x86\x92 " + saved.string()),
              std::string::npos)
        << "改名未进 WAL：崩溃后无法回滚（批次内 WAL:\n"
        << wal_in_batch << ")";

    // .lpkgsave 是**最终产物**，不是待清理的 stash/备份：重启恢复（trim/recover）后必须仍在
    trim_completed();
    recover_packages();
    cleanup_db_backups();
    EXPECT_TRUE(fs::exists(saved)) << ".lpkgsave 被恢复/清理流程当成残留 bak 删掉了";
}

// ============================================================================
// 2) remove --force：同样改名保留（旧实现：真删）
// ============================================================================

TEST_F(ConfigSaveOnRemoveTest, ForceRemoveKeepsConfigAsLpkgsave)
{
    const std::string pkg = create_pkg_files(
        "cs_force", "1.0",
        {{"etc/cs_force.conf", "CS-FORCE-CONF\n"}, {"usr/bin/cs_force", "#!/bin/sh\n"}});
    install_packages({pkg}, "", false);

    const fs::path conf = test_root / "etc/cs_force.conf";
    const fs::path saved = test_root / "etc/cs_force.conf.lpkgsave";
    ASSERT_TRUE(fs::exists(conf));

    remove_package("cs_force", /*force=*/true);

    EXPECT_FALSE(fs::exists(conf));
    ASSERT_TRUE(fs::exists(saved))
        << "--force 不得把配置真删（force 的语义是跳过安全检查，不是丢配置）";
    EXPECT_EQ(read_file(saved), "CS-FORCE-CONF\n");
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/cs_force"));
}

// ============================================================================
// 3) 只有显式 --purge-config 才真删
// ============================================================================

TEST_F(ConfigSaveOnRemoveTest, PurgeConfigDeletesConfigEntirely)
{
    const std::string pkg =
        create_pkg_files("cs_purge", "1.0", {{"etc/cs_purge.conf", "CS-PURGE-CONF\n"}});
    install_packages({pkg}, "", false);

    const fs::path conf = test_root / "etc/cs_purge.conf";
    ASSERT_TRUE(fs::exists(conf));

    // 非 force + purge
    remove_package("cs_purge", /*force=*/false, /*wrap_in_txn=*/true, /*purge_config=*/true);

    EXPECT_FALSE(fs::exists(conf));
    EXPECT_FALSE(fs::exists(test_root / "etc/cs_purge.conf.lpkgsave"))
        << "--purge-config 是真删：连 .lpkgsave 都不该出现";

    // 对照：force + purge（两条路径都不该留下任何东西）
    const std::string pkg2 =
        create_pkg_files("cs_purge2", "1.0", {{"etc/cs_purge2.conf", "CS-PURGE2\n"}});
    install_packages({pkg2}, "", false);
    ASSERT_TRUE(fs::exists(test_root / "etc/cs_purge2.conf"));

    remove_packages({"cs_purge2"}, /*force=*/true, /*purge_config=*/true);

    EXPECT_FALSE(fs::exists(test_root / "etc/cs_purge2.conf"));
    EXPECT_FALSE(fs::exists(test_root / "etc/cs_purge2.conf.lpkgsave"));
}

// ============================================================================
// 4) 已有 .lpkgsave 时移位（.1/.2…）而不是覆盖
// ============================================================================

TEST_F(ConfigSaveOnRemoveTest, ExistingLpkgsaveIsShiftedNotOverwritten)
{
    const std::string pkg =
        create_pkg_files("cs_shift", "1.0", {{"etc/cs_shift.conf", "CS-SHIFT-CONF\n"}});
    install_packages({pkg}, "", false);

    const fs::path conf = test_root / "etc/cs_shift.conf";
    const fs::path saved = test_root / "etc/cs_shift.conf.lpkgsave";
    ASSERT_TRUE(fs::exists(conf));

    // 预置一次"更早的移除"留下的存档，以及再早一次的已移位存档
    std::ofstream(saved) << "OLD-SAVE-1\n";
    std::ofstream(fs::path(saved.string() + ".1")) << "OLD-SAVE-2\n";

    // 批次内取证（提交后 WAL 会被 trim 清空，见上）
    std::string wal_in_batch;
    BreakpointManager::instance().set("remove_after_package_cs_shift",
                                      [&] { wal_in_batch = read_wal(); });

    remove_package("cs_shift", /*force=*/false);

    EXPECT_FALSE(fs::exists(conf));
    ASSERT_TRUE(fs::exists(saved));
    EXPECT_EQ(read_file(saved), "CS-SHIFT-CONF\n") << "本次移除的配置应落在 .lpkgsave";
    ASSERT_TRUE(fs::exists(fs::path(saved.string() + ".1")));
    EXPECT_EQ(read_file(fs::path(saved.string() + ".1")), "OLD-SAVE-2\n")
        << "已有 .1 不得被覆盖（第一个空闲后缀是 .2）";
    ASSERT_TRUE(fs::exists(fs::path(saved.string() + ".2")));
    EXPECT_EQ(read_file(fs::path(saved.string() + ".2")), "OLD-SAVE-1\n")
        << "旧 .lpkgsave 必须被**移位**保存，而不是被新配置覆盖掉";

    // 移位同样要进 WAL（否则崩在移位与本次改名之间时无法回滚到一致状态）
    EXPECT_EQ(count_substr(wal_in_batch, "SAVE_CONF "), 2u)
        << "移位 + 本次改名 = 两条 SAVE_CONF 行（批次内 WAL:\n"
        << wal_in_batch << ")";
}

// ============================================================================
// 5) remove -r（remove_package_recursive）：旧实现完全没有 /etc 覆盖，且硬编 force=true
// ============================================================================

TEST_F(ConfigSaveOnRemoveTest, RecursiveRemoveKeepsConfigAsLpkgsave)
{
    const std::string pkg = create_pkg_files(
        "cs_rec", "1.0", {{"etc/cs_rec.conf", "CS-REC-CONF\n"}, {"usr/bin/cs_rec", "#!/bin/sh\n"}});
    install_packages({pkg}, "", false);

    const fs::path conf = test_root / "etc/cs_rec.conf";
    const fs::path saved = test_root / "etc/cs_rec.conf.lpkgsave";
    ASSERT_TRUE(fs::exists(conf));

    remove_package_recursive("cs_rec", /*force=*/false);

    EXPECT_TRUE(Cache::instance().get_installed_version("cs_rec").empty());
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/cs_rec")) << "普通文件照旧真删";
    EXPECT_FALSE(fs::exists(conf));
    ASSERT_TRUE(fs::exists(saved)) << "remove -r 不得真删配置（它内部硬编 force=true）";
    EXPECT_EQ(read_file(saved), "CS-REC-CONF\n");

    // 同一入口 + purge 才真删
    const std::string pkg2 =
        create_pkg_files("cs_rec2", "1.0", {{"etc/cs_rec2.conf", "CS-REC2\n"}});
    install_packages({pkg2}, "", false);
    ASSERT_TRUE(fs::exists(test_root / "etc/cs_rec2.conf"));

    remove_package_recursive("cs_rec2", /*force=*/false, /*purge_config=*/true);

    EXPECT_FALSE(fs::exists(test_root / "etc/cs_rec2.conf"));
    EXPECT_FALSE(fs::exists(test_root / "etc/cs_rec2.conf.lpkgsave"));
}

// ============================================================================
// 6) autoremove：旧实现同样硬编 force=true，且这条路径此前完全没有 /etc 覆盖
// ============================================================================

TEST_F(ConfigSaveOnRemoveTest, AutoremoveKeepsConfigAsLpkgsave)
{
    const std::string lib = create_pkg_files("cs_lib", "1.0", {{"etc/cs_lib.conf", "CS-LIB\n"}});
    const std::string app =
        create_pkg_files("cs_app", "1.0", {{"usr/bin/cs_app", "#!/bin/sh\n"}}, {"cs_lib"});
    (void)lib;
    add_to_mirror("cs_lib", "1.0");
    add_to_mirror("cs_app", "1.0");
    update_index({{"cs_lib", "1.0", "", "", ""}, {"cs_app", "1.0", "cs_lib", "", ""}});

    install_packages({"cs_app"});
    ASSERT_TRUE(Cache::instance().is_installed("cs_lib")) << "依赖没被拉进来，autoremove 无从谈起";
    ASSERT_FALSE(Cache::instance().is_held("cs_lib")) << "依赖不该被记为显式安装";

    remove_package("cs_app", /*force=*/false);
    autoremove();

    EXPECT_TRUE(Cache::instance().get_installed_version("cs_lib").empty())
        << "cs_lib 已成孤儿，autoremove 应删掉它";
    const fs::path conf = test_root / "etc/cs_lib.conf";
    const fs::path saved = test_root / "etc/cs_lib.conf.lpkgsave";
    EXPECT_FALSE(fs::exists(conf));
    ASSERT_TRUE(fs::exists(saved)) << "autoremove 不得真删配置";
    EXPECT_EQ(read_file(saved), "CS-LIB\n");
}

// ============================================================================
// 7) 回滚保真：批次里后一个包失败 → 整批回滚 → 先前已改名保存的配置回到原位
// ============================================================================

TEST_F(ConfigSaveOnRemoveTest, BatchRollbackRestoresSavedConfigInPlace)
{
    const std::string pa = create_pkg_files(
        "cs_rb_a", "1.0", {{"etc/cs_rb_a.conf", "CS-RB-A\n"}, {"usr/bin/cs_rb_a", "#!/bin/sh\n"}});
    const std::string pb = create_pkg_files("cs_rb_b", "1.0", {{"usr/bin/cs_rb_b", "#!/bin/sh\n"}});
    install_packages({pa}, "", false);
    install_packages({pb}, "", false);

    const fs::path conf = test_root / "etc/cs_rb_a.conf";
    const fs::path saved = test_root / "etc/cs_rb_a.conf.lpkgsave";
    ASSERT_TRUE(fs::exists(conf));

    // A 删完那一刻取证（此刻批次还没提交、WAL 还没被 trim）：证明**前向改名真的发生了** ——
    // 否则下面"配置在原位、没有 .lpkgsave"两条在"改名功能根本不存在"时也会通过（空壳）。
    std::string wal_after_a;
    bool saved_existed_after_a = false;
    bool conf_existed_after_a = true;
    BreakpointManager::instance().set("remove_after_package_cs_rb_a", [&] {
        wal_after_a = read_wal();
        saved_existed_after_a = fs::exists(saved);
        conf_existed_after_a = fs::exists(conf);
    });
    // 断点：B 的普通文件在 "BACKUP WAL 已写、rename 未做" 的窗口里抛异常。
    // 此时 A 的配置**已经**改名成 .lpkgsave 了 —— 整批回滚必须把它改名回原位。
    BreakpointManager::instance().set("rm_backup_after_wal_cs_rb_b", [] {
        throw LpkgException("injected failure: 批次中途失败");
    });

    EXPECT_THROW(remove_packages({"cs_rb_a", "cs_rb_b"}, /*force=*/false), LpkgException);

    EXPECT_TRUE(saved_existed_after_a)
        << "A 删完时 .lpkgsave 不存在 → 前向改名压根没做，本用例的'回滚成功'是空壳";
    EXPECT_FALSE(conf_existed_after_a) << "改名后原路径不该还有文件";
    EXPECT_NE(wal_after_a.find("SAVE_CONF " + conf.string() + " \xe2\x86\x92 " + saved.string()),
              std::string::npos)
        << "A 的配置改名没进 WAL（批次内 WAL:\n"
        << wal_after_a << ")";

    // 回滚后：改名被逆操作抵消（RESTORE 审计行随 trim 一起消失，故这里靠"改名确实发生过
    // + 现在配置回到原位且没有 .lpkgsave 残留"这一对断言钉住，不依赖事后 WAL 文本）
    EXPECT_TRUE(fs::exists(conf)) << "回滚后配置必须回到原位";
    EXPECT_EQ(read_file(conf), "CS-RB-A\n");
    EXPECT_FALSE(fs::exists(saved)) << "回滚后不得留下 .lpkgsave 半成品";
    EXPECT_FALSE(fs::exists(fs::path(saved.string() + ".1")));
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/cs_rb_a")) << "A 的普通文件应回滚";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/cs_rb_b")) << "B 的普通文件应回滚";
    EXPECT_FALSE(Cache::instance().get_installed_version("cs_rb_a").empty());
    EXPECT_FALSE(Cache::instance().get_installed_version("cs_rb_b").empty());
}

// ============================================================================
// 7b) 回滚保真（**移位那一行**）：崩在"移位已做、本次改名未做"→ 回滚先把本次改名的
//     no-op 跳过，再把 `.lpkgsave.<N>` rename 回 `.lpkgsave`，三份内容一个都不能错位
//
// ARCH.md §7.2.1 把恢复路径写成三条收敛分支，其中第三条（"移位已做、本次改名未做"）
// 此前**没有任何用例跑过它的逆向**：既有回滚用例（上面的 BatchRollbackRestoresSavedConfigInPlace
// 与 test_op_sink.cpp 的用例）盘上都没有旧 `.lpkgsave`，于是移位那一行的逆操作一次都没被
// 执行过 —— 它错了也没人知道。本用例把那条逆向真正跑起来。
//
// 两根关键钉子：
//   · 断点命中在 save_config 的 write-ahead 窗口（主线已落、`safe_rename(src,dst)` 未做），
//     而**移位在主线之前** → 那一刻正是"移位已做、本次改名未做"；
//   · 逐份断言内容归属（而不是只断言"文件存在"）：顺序/方向错一点点就会串位，典型错法是
//     把 `.lpkgsave.<N>`（上一批的旧存档）rename 到**配置原位**，把真配置盖掉。
// ============================================================================

TEST_F(ConfigSaveOnRemoveTest, RollbackRestoresShiftedLpkgsaveAndArchives)
{
    const std::string pkg =
        create_pkg_files("cs_shrb", "1.0", {{"etc/cs_shrb.conf", "CS-SHRB-CONF\n"}});
    install_packages({pkg}, "", false);

    const fs::path conf = test_root / "etc/cs_shrb.conf";
    const fs::path saved = test_root / "etc/cs_shrb.conf.lpkgsave";
    const fs::path saved1 = fs::path(saved.string() + ".1");
    const fs::path saved2 = fs::path(saved.string() + ".2");
    ASSERT_TRUE(fs::exists(conf));
    // 两次"更早的移除"各留下一份存档：`.1` 已占用 → 本次移位必须落在 `.2`
    std::ofstream(saved) << "OLD-SAVE-1\n";
    std::ofstream(saved1) << "OLD-SAVE-2\n";

    std::string wal_in_window;
    bool shift_done = false;    // 旧 .lpkgsave 已被移位成 .2（"移位已做"）
    bool conf_in_place = true;  // 本次改名**未**做 → 配置仍在原位
    BreakpointManager::instance().set("rm_save_conf_after_wal_cs_shrb", [&] {
        wal_in_window = read_wal();
        shift_done = fs::exists(saved2) && !fs::exists(saved);
        conf_in_place = fs::exists(conf);
        throw LpkgException("injected failure: 移位已做、本次改名未做");
    });

    EXPECT_THROW(remove_package("cs_shrb", /*force=*/false), LpkgException);
    BreakpointManager::instance().clear_all();

    // 窗口取证：没真的停在这个窗口里的话，下面全部是空壳（"回滚成功"可能只是没回滚过）
    EXPECT_TRUE(shift_done) << "旧的 .lpkgsave 还没被移位成 .2 —— 没造出'移位已做'那一刻";
    EXPECT_TRUE(conf_in_place) << "本次改名已经做了 —— 那是另一个窗口（rename 已做）";
    EXPECT_NE(
        wal_in_window.find("SAVE_CONF " + saved.string() + " \xe2\x86\x92 " + saved2.string()),
        std::string::npos)
        << "移位那一行没进 WAL（回滚侧就不知道要把它 rename 回去）：\n"
        << wal_in_window;
    EXPECT_NE(wal_in_window.find("SAVE_CONF " + conf.string() + " \xe2\x86\x92 " + saved.string()),
              std::string::npos)
        << "本次改名那一行没进 WAL：\n"
        << wal_in_window;

    // 回滚后：三份内容的归属逐份回到批次前
    EXPECT_TRUE(fs::exists(conf)) << "回滚后配置必须回到原位";
    EXPECT_EQ(read_file(conf), "CS-SHRB-CONF\n")
        << "配置原位那份被旧存档盖掉了（数据丢失）—— 移位与本次改名的逆序错了";
    ASSERT_TRUE(fs::exists(saved)) << "移位那一行的逆向没被执行：`.lpkgsave` 没有被 rename 回来";
    EXPECT_EQ(read_file(saved), "OLD-SAVE-1\n") << "`.lpkgsave` 的内容错位";
    ASSERT_TRUE(fs::exists(saved1));
    EXPECT_EQ(read_file(saved1), "OLD-SAVE-2\n") << "`.1` 那份不该被这次回滚碰到";
    EXPECT_FALSE(fs::exists(saved2)) << "移位产物没被收走（回滚后不得留下 .2）";
    EXPECT_FALSE(Cache::instance().get_installed_version("cs_shrb").empty()) << "包应仍在册";
}

// 同一窗口的**崩溃续传**（rec）形态：盘面用"窗口内实测到的状态"精确复现（WAL 就是那一刻
// 的真实内容），再交给 recover_packages()。进程死在窗口里时没有任何人替它回滚，只能靠这条
// 路径 —— 而它与批次内回滚走的是同一个 reverse_execute，所以两条都要钉。
TEST_F(ConfigSaveOnRemoveTest, RecoveryRestoresShiftedLpkgsaveFromCrashWindow)
{
    const std::string pkg =
        create_pkg_files("cs_shrec", "1.0", {{"etc/cs_shrec.conf", "CS-SHREC-CONF\n"}});
    install_packages({pkg}, "", false);

    const fs::path conf = test_root / "etc/cs_shrec.conf";
    const fs::path saved = test_root / "etc/cs_shrec.conf.lpkgsave";
    const fs::path saved1 = fs::path(saved.string() + ".1");
    const fs::path saved2 = fs::path(saved.string() + ".2");
    ASSERT_TRUE(fs::exists(conf));
    std::ofstream(saved) << "OLD-SAVE-1\n";
    std::ofstream(saved1) << "OLD-SAVE-2\n";

    // 1) 在窗口里如实记下"那一刻的 WAL"（批次内回滚随后发生，但我们要的是崩溃现场）
    std::string wal_in_window;
    BreakpointManager::instance().set("rm_save_conf_after_wal_cs_shrec", [&] {
        wal_in_window = read_wal();
        throw LpkgException("injected failure: 移位已做、本次改名未做");
    });
    EXPECT_THROW(remove_package("cs_shrec", /*force=*/false), LpkgException);
    BreakpointManager::instance().clear_all();
    ASSERT_FALSE(wal_in_window.empty()) << "窗口里没读到 WAL，复现不出崩溃现场";

    // 2) 把盘面推回崩溃那一刻：配置在原位（回滚已把它放回原位）、旧 .lpkgsave 已移位成 .2
    ASSERT_TRUE(fs::exists(conf));
    ASSERT_TRUE(fs::exists(saved)) << "前置：回滚后 .lpkgsave 应在";
    fs::rename(saved, saved2);  // "移位已做"
    ASSERT_TRUE(fs::exists(saved2));
    ASSERT_FALSE(fs::exists(saved));

    // 3) 把那一刻的真实 WAL 写回去（未完成的批次：没有 COMMIT_PKGS），交给续传
    write_wal(wal_in_window);

    ASSERT_NO_THROW(recover_packages());

    EXPECT_TRUE(fs::exists(conf)) << "续传后配置必须在原位";
    EXPECT_EQ(read_file(conf), "CS-SHREC-CONF\n") << "配置被旧存档盖掉了";
    ASSERT_TRUE(fs::exists(saved)) << "续传没执行移位那一行的逆向：`.lpkgsave` 没回来";
    EXPECT_EQ(read_file(saved), "OLD-SAVE-1\n");
    ASSERT_TRUE(fs::exists(saved1));
    EXPECT_EQ(read_file(saved1), "OLD-SAVE-2\n");
    EXPECT_FALSE(fs::exists(saved2)) << "续传后不得留下 .2";
    EXPECT_FALSE(Cache::instance().get_installed_version("cs_shrec").empty()) << "包应仍在册";
}

// ============================================================================
// 8) write-ahead 窗口：SAVE_CONF 行已落、rename 未做时崩 → 配置在原位、无 .lpkgsave
// ============================================================================

TEST_F(ConfigSaveOnRemoveTest, WriteAheadWindowFailureLeavesConfigInPlace)
{
    const std::string pkg = create_pkg_files("cs_wa", "1.0", {{"etc/cs_wa.conf", "CS-WA-CONF\n"}});
    install_packages({pkg}, "", false);

    const fs::path conf = test_root / "etc/cs_wa.conf";
    const fs::path saved = test_root / "etc/cs_wa.conf.lpkgsave";
    ASSERT_TRUE(fs::exists(conf));

    // write-ahead 契约的取证点就在断点里：它命中在"行已落、rename 未做"之间，此刻 WAL 里
    // **必须**已有这一行（否则崩在这个窗口时回滚侧根本不知道要还原什么）。在断点里读而不是
    // 事后读：批次回滚后 trim_completed 会把 WAL 清掉。
    std::string wal_in_window;
    BreakpointManager::instance().set("rm_save_conf_after_wal_cs_wa", [&] {
        wal_in_window = read_wal();
        throw LpkgException("injected failure: 改名 WAL 与 rename 之间断电");
    });

    EXPECT_THROW(remove_package("cs_wa", /*force=*/false), LpkgException);

    EXPECT_TRUE(fs::exists(conf)) << "WAL 已写但 rename 未做 → 原文件必须还在原位";
    EXPECT_EQ(read_file(conf), "CS-WA-CONF\n");
    EXPECT_FALSE(fs::exists(saved)) << "反向 rename 找不到 dst 时应是 no-op，不得凭空造文件";
    EXPECT_FALSE(Cache::instance().get_installed_version("cs_wa").empty()) << "包应仍在册";

    EXPECT_NE(wal_in_window.find("SAVE_CONF " + conf.string() + " \xe2\x86\x92 " + saved.string()),
              std::string::npos)
        << "改名行没有先于物理 rename 落盘（窗口内 WAL:\n"
        << wal_in_window << ")";
}
