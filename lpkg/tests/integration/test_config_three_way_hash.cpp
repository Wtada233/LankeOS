/**
 * test_config_three_way_hash.cpp — 升级时配置文件的**三哈希分流**（pacman `add.c` 语义）
 *
 * ── 问题（改前）──────────────────────────────────────────────────────────────
 * `InstallationTask::copy_package_files` 对 `/etc/` 前缀条目一律："目标已存在 → 新内容写成
 * `.lpkgnew`、原文件永不覆盖"。于是**用户从没改过的配置也永远拿不到新版本** —— 每次升级都往
 * /etc 堆一个 `.lpkgnew`，而盘上那份永远是 v1 的内容（`.lpkgnew` 只是"请你自己去合并"的提示，
 * 没人合并就等于配置永不前进）。这与 pacman 差得最远的一处：pacman 用**三份哈希**分流。
 *
 * ── 三份哈希 ────────────────────────────────────────────────────────────────
 *   hash_local 盘上那份文件的内容哈希
 *   hash_orig  上一次**我们装进这个包的这个路径**的内容哈希（本仓库：`confhashes.db`，
 *              旧包在本地 DB 里的记录）
 *   hash_pkg   本次包里那个条目的内容哈希
 *
 * 分流（三份哈希全等时第一条命中，无害）：
 *   ① 盘上 == 旧记录  → 用户**没改过** → 静默换成新版（**不产生** `.lpkgnew`）
 *   ② 旧记录 == 新包  → 包本身没改这个配置 → **保留**用户文件，连 `.lpkgnew` 都不产生
 *   ③ 三者互异/无从判定（盘上是符号链接/读不到）→ 新版本落 `.lpkgnew` + 告警
 *
 * ── 老 DB（无旧记录）的退化路径：只算两份哈希，**记录值仍是包内内容** ──────────────
 * 本特性之前装的包在 `confhashes.db` 里没有记录（该文件当时根本不存在）。此时**只算**：
 *   · 盘上那份 == 新包那份 → 保留原文件（内容一致 = 没有"新东西"要审阅）、
 *     把**包内那份**的哈希写进 DB；
 *   · 不一致（或盘上那份不可读）→ 新内容落 `<路径>.lpkgnew`、把**包内那份（新）**的哈希
 *     写进 DB。
 * 两个子分支都会建记录 ⇒ 退化路径只走一次：下一次升级回到正常三哈希分流。
 * ⚠ 代价（本文件 test ⑫/子分支 B 里写明）：盘上那份**不被追认**，所以它与记录不相等时，
 * 包**每次**改配置都会再落一份可见的 `.lpkgnew`（吵，但绝不静默）；用户把 `.lpkgnew`
 * 合并进盘上那份之后，盘上与记录对齐，此后回到正常分流。
 *
 * **安全底线不许退**：用户改过的配置**永远不被静默覆盖**（①的前提是"逐字节等于我们上次装
 * 进去的内容"，这条由 `UserModifiedConfigIsNeverOverwrittenOnEveryUpgrade…` 死死盯住）。
 * 退化路径**不是**例外 —— 它同样只声明"这个包的这个版本提供过什么"：记录值取自**包内**，
 * 盘上那份（往往是无主文件，`--overwrite` 是它唯一的合法入口）永不被追认。曾经追认过，
 * 后果就是 ⑫ 那两条链：记录有三处会按设计被删（升级丢弃 /etc 条目、remove_conf_hash、
 * 移除路径），"追认"被重新武装后，下一次升级就 ① 成立 → **静默就地覆盖用户文件**。
 *
 * ── 记录写的永远是**包内内容**的哈希，不是"盘上当时的内容"（无例外）─────────────
 * 这条不是实现细节而是安全前提：③（写 `.lpkgnew`）之后盘上仍是**用户**的文件，若在那里把
 * hash_local 记成 hash_orig，**下一次**升级就会满足 ①、"用户改过的配置"被静默换成新版 ——
 * 正好踩中底线。记 hash_pkg 则永远只声明"这个包的这个版本提供过什么"，于是用户文件在每次
 * 升级都继续被判为冲突。本文件 `UserModifiedConfigIsNeverOverwrittenOnEveryUpgrade` 的
 * v2→v3 一段钉住它，`DegenerateRecordNeverAuthorizesSilentOverwrite` 钉住退化路径那一段。
 *
 * ── 存储形态与回滚 ──────────────────────────────────────────────────────────
 * 新增一个 DB 文件 `<state_dir>/confhashes.db`（不塞进 files.db：那里的"值"是**属主包名集合**，
 * 被 `add_file_owner`（单一属主检查）、`get_file_owners` 当属主集合直接读，混入哈希会污染所有权
 * 语义）。它和 pkgs/files.db/provides.db 走**同一族写接口**（`Cache::write(milestone)` 里的
 * `write_db_file_wal`）：WAL 行 + 备份 + `.tmp` + rename + fsync，于是批次回滚时由
 * `reverse_execute` 的 DB 分支自动回到批次前（`BatchRollbackRestoresConfigAndHashDb` 钉住）。
 *
 * 移除侧（`.lpkgsave`）与 `--force-overwrite` 的语义**不在本文件范围内**（另有专门文件）。
 *
 * ── `.lpkgnew` 也在事务内（本文件 ⑩ 文件分支 / ⑪ 符号链接分支）──────────────────
 * 落 `.lpkgnew` 同样是"这个批次改了盘面"（改的不是配置本体而已），因此它走的是与普通文件
 * 分支同一套写入层原语：内容先写 `.lpkgtmp`，再 `commit_copy`（WAL `COPY`）；目标已存在时
 * 先把旧那份 `backup` 进 stash（WAL `BACKUP`）。于是**批次回滚后不会有"包没装上却多出一份
 * 请审阅副本"**；而批次前就存在的那份 `.lpkgnew` 会被**还原回来**（而不是被新的盖掉或删掉）。
 * 改前是裸 `fs::copy`（事务外的副作用），回滚收不走。
 * ⑪ 是它的**同类最后一处**：包内条目是**符号链接**时（走符号链接分支，与三哈希分流无关），
 * 那份 `.lpkgnew` 链接改前同样在事务外（裸 `fs::remove` + `fs::create_symlink`，无 WAL 行）。
 */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/crypto/hash.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class ConfigThreeWayHashTest : public IntegrationTestBase
{
protected:
    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        Config::instance().set_no_hooks_mode(true);
        // force-overwrite 是**进程级**开关：本文件里只有一条用例开它，漏了 reset 会污染
        // 同进程里后面的测试（含别的文件的 EXPECT_THROW）。
        Config::instance().set_force_overwrite_mode(false);
        BreakpointManager::instance().clear_all();
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        Config::instance().set_force_overwrite_mode(false);
        IntegrationTestBase::TearDown();
    }

    /** 带任意文件清单的虚拟包（content/<相对路径>） */
    std::string create_pkg_files(const std::string& name, const std::string& ver,
                                 const std::vector<std::pair<std::string, std::string>>& files)
    {
        fs::path work_dir = suite_work_dir / ("_pkg_" + name + "_" + ver);
        if (fs::exists(work_dir)) fs::remove_all(work_dir);
        for (const auto& [rel, content] : files) {
            fs::path p = work_dir / "content" / rel;
            ensure_dir_exists(p.parent_path());
            std::ofstream f(p);
            f << content;
        }
        std::string pkg_path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(pkg_path, work_dir.string(), name, ver, {}, {}, "Man page for " + name, {});
        fs::remove_all(work_dir);
        return pkg_path;
    }

    /**
     * 带**符号链接条目**的虚拟包：files 是普通文件、links 是 `{相对路径, 链接目标}`。
     * 专供 ⑪（符号链接型 conffile 的冲突路径）—— 那条路径与普通文件的配置分支**不是**
     * 同一段代码，`create_pkg_files` 表达不了。
     */
    std::string create_pkg_symlinks(const std::string& name, const std::string& ver,
                                    const std::vector<std::pair<std::string, std::string>>& files,
                                    const std::vector<std::pair<std::string, std::string>>& links)
    {
        fs::path work_dir = suite_work_dir / ("_pkg_" + name + "_" + ver);
        if (fs::exists(work_dir)) fs::remove_all(work_dir);
        for (const auto& [rel, content] : files) {
            fs::path p = work_dir / "content" / rel;
            ensure_dir_exists(p.parent_path());
            std::ofstream f(p);
            f << content;
        }
        for (const auto& [rel, target] : links) {
            fs::path p = work_dir / "content" / rel;
            ensure_dir_exists(p.parent_path());
            fs::create_symlink(target, p);
        }
        std::string pkg_path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(pkg_path, work_dir.string(), name, ver, {}, {}, "Man page for " + name, {});
        fs::remove_all(work_dir);
        return pkg_path;
    }

    static std::string read_file(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f.is_open()) return "<不存在>";
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    /** 在本包的配置文件里写用户内容（模拟"用户改过"） */
    static void user_edit(const fs::path& p, const std::string& content)
    {
        std::ofstream f(p, std::ios::trunc);
        f << content;
    }

    /** 哈希 DB 的**逐字节**快照（回滚保真的判据之一；文件不存在 → "<不存在>"） */
    std::string conf_db_bytes() const
    {
        return read_file(Config::instance().conf_hashes_db());
    }

    /** lstat 出来的 (mode, uid, gid) 三元组（回滚保真要比"内容之外"的那部分状态） */
    static std::tuple<unsigned, unsigned long long, unsigned long long> stat_of(const fs::path& p)
    {
        struct stat st{};
        if (lstat(p.c_str(), &st) != 0) return {0u, ~0ull, ~0ull};
        return {static_cast<unsigned>(st.st_mode & 07777),
                static_cast<unsigned long long>(st.st_uid),
                static_cast<unsigned long long>(st.st_gid)};
    }

    /** 未清理的 stash / `.lpkgtmp` 残留数（回滚必须把它们全部收走） */
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
};

// ============================================================================
// ① 用户没改过的配置：升级时**静默换成新版**，不产生 .lpkgnew
//    （改前：`.lpkgnew` 出现、盘上仍是 v1 → 本用例红）
// ============================================================================

TEST_F(ConfigThreeWayHashTest, UnmodifiedConfigIsUpgradedSilently)
{
    const std::string v1 = create_pkg_files(
        "c3u", "1.0", {{"etc/c3u.conf", "C3U-V1\n"}, {"usr/bin/c3u", "#!/bin/sh\n"}});
    install_packages({v1}, "", false);
    ASSERT_EQ(read_file(test_root / "etc/c3u.conf"), "C3U-V1\n");

    const std::string v2 = create_pkg_files(
        "c3u", "2.0", {{"etc/c3u.conf", "C3U-V2\n"}, {"usr/bin/c3u", "#!/bin/sh\n"}});
    install_packages({v2}, "", false);

    EXPECT_EQ(read_file(test_root / "etc/c3u.conf"), "C3U-V2\n")
        << "用户从未改过的配置必须随包升级；停在 v1 就等于配置永不前进";
    EXPECT_FALSE(fs::exists(test_root / "etc/c3u.conf.lpkgnew"))
        << "盘上那份 == 我们上次装进去的（用户没改过）→ 不该产生 .lpkgnew";
    EXPECT_EQ(Cache::instance().get_installed_version("c3u"), "2.0");

    // 哈希真的进了 DB，且记的是**这次装进去的内容**的哈希（路径 + sha256 都看得到）
    const std::string db = conf_db_bytes();
    EXPECT_NE(db.find("/etc/c3u.conf"), std::string::npos) << "哈希 DB 里没有该路径的记录：" << db;
    EXPECT_NE(db.find(calculate_sha256(test_root / "etc/c3u.conf")), std::string::npos)
        << "记录的哈希必须等于这次装进去的内容：" << db;
}

// ============================================================================
// ①b 降噪：① 分支的日志**按包聚合一行**，不是逐配置文件一行
//
// 逐配置文件打日志的代价在真实场景里是数量级的：`lpkg upgrade` 一次几百个包，每个包
// 几个 /etc 条目 ⇒ 光这一句就刷几百行（pacman 对静默替换**一行都不打**）。聚合不能把
// 信息丢掉 —— 用户要能查到"到底是哪些配置被静默换了"，所以那一行里给**条数 + 完整
// 路径清单**。
//
// 正向对照一起做：这一行必须**真的打出来**（否则"只有一行"可能只是"一行都没有"，那
// 是把日志整个删掉的假绿）。
// ============================================================================

TEST_F(ConfigThreeWayHashTest, SilentConfigUpdatesAreLoggedOncePerPackage)
{
    // 三个 /etc 条目 + 一个非 /etc 条目：只有前三个走 ① 分支
    const std::vector<std::pair<std::string, std::string>> v1_files = {
        {"etc/c3n_a.conf", "C3N-A-V1\n"},
        {"etc/c3n_b.conf", "C3N-B-V1\n"},
        {"etc/c3n_c.conf", "C3N-C-V1\n"},
        {"usr/bin/c3n", "#!/bin/sh\n"}};
    std::vector<std::pair<std::string, std::string>> v2_files = v1_files;
    v2_files[0].second = "C3N-A-V2\n";
    v2_files[1].second = "C3N-B-V2\n";
    v2_files[2].second = "C3N-C-V2\n";

    const std::string v1 = create_pkg_files("c3n", "1.0", v1_files);
    install_packages({v1}, "", false);
    ASSERT_EQ(read_file(test_root / "etc/c3n_a.conf"), "C3N-A-V1\n");

    const std::string v2 = create_pkg_files("c3n", "2.0", v2_files);
    testing::internal::CaptureStdout();
    install_packages({v2}, "", false);
    const std::string out = testing::internal::GetCapturedStdout();

    // 三份都真的被静默换了新版（不然下面数的行数毫无意义）
    EXPECT_EQ(read_file(test_root / "etc/c3n_a.conf"), "C3N-A-V2\n");
    EXPECT_EQ(read_file(test_root / "etc/c3n_b.conf"), "C3N-B-V2\n");
    EXPECT_EQ(read_file(test_root / "etc/c3n_c.conf"), "C3N-C-V2\n");

    // 计数口径**不依赖文案语言**（别拿 get_string 的文案切片去数：英文文案以 "{}" 开头，
    // 切成空前缀后 `find("")` 会匹配每一个位置）。直接数"输出里有多少**行**提到了本包的
    // 配置路径" —— 逐配置一行时是 3 行，聚合后必然只有 1 行。
    size_t lines_with_paths = 0;
    {
        std::istringstream iss(out);
        for (std::string line; std::getline(iss, line);) {
            if (line.find("/etc/c3n_") != std::string::npos) ++lines_with_paths;
        }
    }
    EXPECT_EQ(lines_with_paths, 1u)
        << "三个配置被静默替换，却有 " << lines_with_paths << " 行提到了它们（应为每包 1 行，"
        << "一次全系统升级光这一句就刷几百行）：\n"
        << out;
    // 降噪不许丢信息：三份路径都要能在那唯一一行里查到
    EXPECT_NE(out.find("/etc/c3n_a.conf"), std::string::npos) << out;
    EXPECT_NE(out.find("/etc/c3n_b.conf"), std::string::npos) << out;
    EXPECT_NE(out.find("/etc/c3n_c.conf"), std::string::npos) << out;
}

// ============================================================================
// ② 用户改过 → **坚决保留**，且每次升级都继续保留（不许静默覆盖）
//    改前：`.lpkgnew` + 保留（已如此）→ 本用例改前**即绿**，改后必须继续绿
// ============================================================================

TEST_F(ConfigThreeWayHashTest, UserModifiedConfigIsNeverOverwrittenOnEveryUpgrade)
{
    const std::string v1 = create_pkg_files(
        "c3m", "1.0", {{"etc/c3m.conf", "C3M-V1\n"}, {"usr/bin/c3m", "#!/bin/sh\n"}});
    install_packages({v1}, "", false);
    user_edit(test_root / "etc/c3m.conf", "USER-EDITED\n");

    const std::string v2 = create_pkg_files(
        "c3m", "2.0", {{"etc/c3m.conf", "C3M-V2\n"}, {"usr/bin/c3m", "#!/bin/sh\n"}});
    install_packages({v2}, "", false);

    EXPECT_EQ(read_file(test_root / "etc/c3m.conf"), "USER-EDITED\n")
        << "用户改过的配置不许被静默覆盖（.lpkgnew 存在的唯一理由）";
    ASSERT_TRUE(fs::exists(test_root / "etc/c3m.conf.lpkgnew"));
    EXPECT_EQ(read_file(test_root / "etc/c3m.conf.lpkgnew"), "C3M-V2\n")
        << "新版内容必须落在 .lpkgnew 里供用户审阅";

    // 再升一次：用户**仍然**没动盘上那份（还是 USER-EDITED），必须**继续**保留 + 产生新版
    // .lpkgnew。这一段钉住"记录的必须是**包内内容**的哈希而不是盘上当时的内容"——若记的是
    // 盘上内容（USER-EDITED），这里就会满足"盘上 == 旧记录"而被静默覆盖。
    const std::string v3 = create_pkg_files(
        "c3m", "3.0", {{"etc/c3m.conf", "C3M-V3\n"}, {"usr/bin/c3m", "#!/bin/sh\n"}});
    install_packages({v3}, "", false);

    EXPECT_EQ(read_file(test_root / "etc/c3m.conf"), "USER-EDITED\n")
        << "第二次升级把用户文件静默覆盖了（记录写成了盘上内容）";
    ASSERT_TRUE(fs::exists(test_root / "etc/c3m.conf.lpkgnew"));
    EXPECT_EQ(read_file(test_root / "etc/c3m.conf.lpkgnew"), "C3M-V3\n");
}

// ============================================================================
// ③ 旧记录 == 新包（v2 的配置与 v1 **逐字节相同**）→ 保留用户文件、**不产生** .lpkgnew
//    改前：`.lpkgnew` 出现 → 本用例红
// ============================================================================

TEST_F(ConfigThreeWayHashTest, UnchangedUpstreamConfigKeepsUserFileWithoutLpkgnew)
{
    const std::string v1 = create_pkg_files(
        "c3s", "1.0", {{"etc/c3s.conf", "SAME-CONF\n"}, {"usr/bin/c3s", "#!/bin/sh\n"}});
    install_packages({v1}, "", false);
    user_edit(test_root / "etc/c3s.conf", "USER-EDITED\n");

    // v2 的配置与 v1 **逐字节相同**（包自己没改这个文件）
    const std::string v2 = create_pkg_files(
        "c3s", "2.0", {{"etc/c3s.conf", "SAME-CONF\n"}, {"usr/bin/c3s", "#!/bin/sh\n"}});
    install_packages({v2}, "", false);

    EXPECT_EQ(read_file(test_root / "etc/c3s.conf"), "USER-EDITED\n") << "用户文件必须保留";
    EXPECT_FALSE(fs::exists(test_root / "etc/c3s.conf.lpkgnew"))
        << "包没改这个配置（旧记录 == 新包）→ 没有「新东西」要给用户审阅，不该产生 .lpkgnew";
    EXPECT_EQ(Cache::instance().get_installed_version("c3s"), "2.0");

    // 对照：同一场景下 v3 **改了**配置 → 必须产生 .lpkgnew（否则上面的"不产生"可能只是
    // 因为功能整体坏了）
    const std::string v3 = create_pkg_files(
        "c3s", "3.0", {{"etc/c3s.conf", "CHANGED-CONF\n"}, {"usr/bin/c3s", "#!/bin/sh\n"}});
    install_packages({v3}, "", false);
    EXPECT_EQ(read_file(test_root / "etc/c3s.conf"), "USER-EDITED\n");
    ASSERT_TRUE(fs::exists(test_root / "etc/c3s.conf.lpkgnew"))
        << "包改了配置 + 用户也改过 → 三者互异，必须产生 .lpkgnew";
    EXPECT_EQ(read_file(test_root / "etc/c3s.conf.lpkgnew"), "CHANGED-CONF\n");
}

// ============================================================================
// ④ 旧记录 == 新包且用户**没**改过：整条链路都保持干净（不产生 .lpkgnew）
// ============================================================================

TEST_F(ConfigThreeWayHashTest, UnchangedUpstreamConfigWithUntouchedFileStaysClean)
{
    const std::string v1 = create_pkg_files(
        "c3k", "1.0", {{"etc/c3k.conf", "SAME-CONF\n"}, {"usr/bin/c3k", "#!/bin/sh\n"}});
    install_packages({v1}, "", false);
    const std::string v2 = create_pkg_files(
        "c3k", "2.0", {{"etc/c3k.conf", "SAME-CONF\n"}, {"usr/bin/c3k", "#!/bin/sh\n"}});
    install_packages({v2}, "", false);

    EXPECT_EQ(read_file(test_root / "etc/c3k.conf"), "SAME-CONF\n");
    EXPECT_FALSE(fs::exists(test_root / "etc/c3k.conf.lpkgnew"));
}

// ============================================================================
// ⑤ 连续升级 v1→v2→v3（中间不动配置）：每次都静默跟上，全程无 .lpkgnew
// ============================================================================

TEST_F(ConfigThreeWayHashTest, ConsecutiveUpgradesTrackSilently)
{
    const std::string v1 = create_pkg_files(
        "c3c", "1.0", {{"etc/c3c.conf", "C3C-V1\n"}, {"usr/bin/c3c", "#!/bin/sh\n"}});
    install_packages({v1}, "", false);
    ASSERT_EQ(read_file(test_root / "etc/c3c.conf"), "C3C-V1\n");

    const std::string v2 = create_pkg_files(
        "c3c", "2.0", {{"etc/c3c.conf", "C3C-V2\n"}, {"usr/bin/c3c", "#!/bin/sh\n"}});
    install_packages({v2}, "", false);
    EXPECT_EQ(read_file(test_root / "etc/c3c.conf"), "C3C-V2\n")
        << "v1→v2 没有静默跟上（每一次升级都必须把记录推进到本次装进去的内容）";
    EXPECT_FALSE(fs::exists(test_root / "etc/c3c.conf.lpkgnew"));

    const std::string v3 = create_pkg_files(
        "c3c", "3.0", {{"etc/c3c.conf", "C3C-V3\n"}, {"usr/bin/c3c", "#!/bin/sh\n"}});
    install_packages({v3}, "", false);
    EXPECT_EQ(read_file(test_root / "etc/c3c.conf"), "C3C-V3\n") << "v2→v3 没有静默跟上";
    EXPECT_FALSE(fs::exists(test_root / "etc/c3c.conf.lpkgnew"));
    EXPECT_EQ(Cache::instance().get_installed_version("c3c"), "3.0");
}

// ============================================================================
// ⑥ 哈希记录**随包走**：包被移除后记录必须一并消失
//    （否则重装回来的包会把"上一次装的哈希"当成旧记录，把无主文件静默换成包内版本）
// ============================================================================

TEST_F(ConfigThreeWayHashTest, HashRecordGoesAwayWithThePackage)
{
    const fs::path conf = test_root / "etc/c3r.conf";
    const fs::path saved = test_root / "etc/c3r.conf.lpkgsave";
    const fs::path conf_new = test_root / "etc/c3r.conf.lpkgnew";

    const std::string v1 = create_pkg_files(
        "c3r", "1.0", {{"etc/c3r.conf", "C3R-CONF\n"}, {"usr/bin/c3r", "#!/bin/sh\n"}});
    install_packages({v1}, "", false);
    ASSERT_EQ(read_file(conf), "C3R-CONF\n");

    // 有记录时的正向对照：用户没改过 → 升级静默跟上
    const std::string v2 = create_pkg_files(
        "c3r", "2.0", {{"etc/c3r.conf", "C3R-CONF-V2\n"}, {"usr/bin/c3r", "#!/bin/sh\n"}});
    install_packages({v2}, "", false);
    ASSERT_EQ(read_file(conf), "C3R-CONF-V2\n");
    ASSERT_FALSE(fs::exists(conf_new));

    // 用户改过这份配置，然后移除包：配置改名成 .lpkgsave 保留（移除侧语义，本文件不改），
    // 哈希记录随之消失。
    user_edit(conf, "USER-KEPT\n");
    remove_package("c3r", /*force=*/false);
    ASSERT_TRUE(fs::exists(saved));
    ASSERT_FALSE(fs::exists(conf));

    // 现场造回一份**无主**文件，内容与包内 v2 不同（= 用户手工放回的"自己那份"）
    fs::remove(saved);
    user_edit(conf, "USER-KEPT\n");

    // ── 阶段 1：重装（包不在册 → 全新安装路径；无旧记录 = 退化路径）──────────────
    // 无主文件撞包内文件是**冲突**，需要 --overwrite 豁免（pacman 同：`/etc` 里那个文件没有
    // 任何包认领时属于 "unknown manual file"）。
    Config::instance().set_force_overwrite_mode(true);
    install_packages({v2}, "", false);
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_EQ(read_file(conf), "USER-KEPT\n") << "用户手工放回的文件不得被静默覆盖";
    ASSERT_TRUE(fs::exists(conf_new))
        << "包被移除后哈希记录必须一并消失：留下的记录会让重装走'旧记录 == 新包'那一支"
           "（静默保留、连 .lpkgnew 都不产生），用户再也看不到自己那份与新版的差异";
    EXPECT_EQ(read_file(conf_new), "C3R-CONF-V2\n");

    // ── 阶段 2：路径干净时重装 = 全新安装 → 正常写入包内配置、不产生 .lpkgnew ──
    remove_package("c3r", /*force=*/false);
    ASSERT_TRUE(fs::exists(saved));
    // 清掉上一阶段留下的审阅文件与本次的存档：只留"路径不存在"的干净现场
    fs::remove(saved);
    fs::remove(conf_new);
    ASSERT_FALSE(fs::exists(conf));

    install_packages({v2}, "", false);

    EXPECT_EQ(read_file(conf), "C3R-CONF-V2\n") << "干净的路径上重装应当把包内新配置正常写入";
    EXPECT_FALSE(fs::exists(conf_new)) << "没有冲突，不该有 .lpkgnew";
    EXPECT_EQ(Cache::instance().get_installed_version("c3r"), "2.0");
    EXPECT_NE(conf_db_bytes().find("/etc/c3r.conf"), std::string::npos);

    // ── 阶段 3：新记录确实"活"着，且说的是**我们装进去的那份内容** ────────────────
    // 这里**故意先不编辑**。改前本阶段一上来就 `user_edit(conf, "USER-EDITED-AGAIN")`，
    // 那次编辑让 hash_local != hash_orig，"记录值是谁的哈希"这个断言于是在**任何**实现下
    // 都成立（阶段 2 的干净重装早就用正常路径的记录盖掉了阶段 1 退化路径留下的那条）——
    // 正好掩盖了"退化路径追认盘上那份"的缺陷。退化路径本身的回归由 ⑫
    // `DegenerateRecordNeverAuthorizesSilentOverwrite` 负责；本阶段考的是正常分流：
    // 记录与盘上那份逐字节一致 → ① 静默跟上。
    const std::string v3 = create_pkg_files(
        "c3r", "3.0", {{"etc/c3r.conf", "C3R-CONF-V3\n"}, {"usr/bin/c3r", "#!/bin/sh\n"}});
    install_packages({v3}, "", false);
    EXPECT_EQ(read_file(conf), "C3R-CONF-V3\n")
        << "重装的记录必须是**我们装进去的 v2 内容**的哈希：否则'用户没改过'判不出来";
    EXPECT_FALSE(fs::exists(conf_new)) << "盘上那份 == 记录 → ① 静默跟上，不该有 .lpkgnew";

    // ── 阶段 3b：同一条记录，用户**这次**改了配置 → 与记录不再一致 → 冲突 ──────────
    user_edit(conf, "USER-EDITED-AGAIN\n");
    const std::string v4 = create_pkg_files(
        "c3r", "4.0", {{"etc/c3r.conf", "C3R-CONF-V4\n"}, {"usr/bin/c3r", "#!/bin/sh\n"}});
    install_packages({v4}, "", false);
    EXPECT_EQ(read_file(conf), "USER-EDITED-AGAIN\n")
        << "重装建起的记录必须是**包内内容**的哈希：那样用户这份改动才判为冲突";
    ASSERT_TRUE(fs::exists(conf_new)) << "用户改过 + 包改了配置 → 必须产生 .lpkgnew";
    EXPECT_EQ(read_file(conf_new), "C3R-CONF-V4\n");
}

// ============================================================================
// ⑦ 老 DB（无哈希记录）→ 退化路径：只算"盘上那份"与"新包那份"，并**一次性**把盘上那份
//    追认进 DB。两个子分支各一条用例，每条的第二阶段是"只发生一次"的护栏。
// ============================================================================

/** 子分支 A：两份**逐字节相同** → 保留原文件（新的直接丢弃）、把包内那份的哈希记进 DB
 *  （此处盘上那份与包内那份逐字节相同，两个值本来就是同一个哈希） */
TEST_F(ConfigThreeWayHashTest, LegacyDbIdenticalContentKeepsFileWithoutLpkgnew)
{
    const fs::path conf = test_root / "etc/c3la.conf";
    const fs::path conf_new = test_root / "etc/c3la.conf.lpkgnew";

    const std::string v1 = create_pkg_files(
        "c3la", "1.0", {{"etc/c3la.conf", "C3LA-SAME\n"}, {"usr/bin/c3la", "#!/bin/sh\n"}});
    install_packages({v1}, "", false);
    ASSERT_EQ(read_file(conf), "C3LA-SAME\n");

    // 模拟"由本特性之前的老 lpkg 装的包"：DB 里根本没有哈希记录
    // （老版本不写 confhashes.db，升级上来时也是这个形态）
    ASSERT_TRUE(fs::exists(Config::instance().conf_hashes_db()));
    fs::remove(Config::instance().conf_hashes_db());
    Cache::instance().load();

    // 第一次升级：新包那份与盘上那份**逐字节相同**
    const std::string v2 = create_pkg_files(
        "c3la", "2.0", {{"etc/c3la.conf", "C3LA-SAME\n"}, {"usr/bin/c3la", "#!/bin/sh\n"}});
    install_packages({v2}, "", false);

    EXPECT_EQ(read_file(conf), "C3LA-SAME\n") << "原文件保留（内容与新包那份一致）";
    EXPECT_FALSE(fs::exists(conf_new)) << "两份一致 = 没有新东西要给用户审阅 → 不该产生 .lpkgnew"
                                          "（改前：无论一不一致都落 .lpkgnew）";
    // 这一次把记录**追认**进 DB（值 = 盘上那份 = 新包那份）
    const std::string db_after = conf_db_bytes();
    EXPECT_NE(db_after.find("/etc/c3la.conf"), std::string::npos) << db_after;
    EXPECT_NE(db_after.find(calculate_sha256(conf)), std::string::npos) << db_after;

    // 第二阶段（"只发生一次"护栏）：紧接着升到 v3（配置换了），用户没动过那份 →
    // 必须**静默换新版**、不产生 .lpkgnew（退化路径不该每次升级都刷 .lpkgnew）
    const std::string v3 = create_pkg_files(
        "c3la", "3.0", {{"etc/c3la.conf", "C3LA-V3\n"}, {"usr/bin/c3la", "#!/bin/sh\n"}});
    install_packages({v3}, "", false);
    EXPECT_EQ(read_file(conf), "C3LA-V3\n")
        << "退化路径只该走一次：下一次升级必须回到正常分流（静默换新版）";
    EXPECT_FALSE(fs::exists(conf_new));
}

/**
 * 子分支 B：两份**不一致** → 新内容落 `.lpkgnew`；DB 里记的仍是**包内那份**的哈希
 * （盘上那份**不**被追认 —— 追认它 = 下次升级满足 ① 静默覆盖用户文件，见 ⑫）。
 * 代价是盘上那份在用户"合并"之前不再前进：包每改一次配置都会再落一份**可见的** .lpkgnew。
 */
TEST_F(ConfigThreeWayHashTest, LegacyDbDifferentContentSavesLpkgnewAndRecordsPackageHash)
{
    const fs::path conf = test_root / "etc/c3lb.conf";
    const fs::path conf_new = test_root / "etc/c3lb.conf.lpkgnew";

    const std::string v1 = create_pkg_files(
        "c3lb", "1.0", {{"etc/c3lb.conf", "C3LB-V1\n"}, {"usr/bin/c3lb", "#!/bin/sh\n"}});
    install_packages({v1}, "", false);
    ASSERT_EQ(read_file(conf), "C3LB-V1\n");

    ASSERT_TRUE(fs::exists(Config::instance().conf_hashes_db()));
    fs::remove(Config::instance().conf_hashes_db());
    Cache::instance().load();

    // 第一次升级：新包那份与盘上那份**不一致**
    const std::string v2 = create_pkg_files(
        "c3lb", "2.0", {{"etc/c3lb.conf", "C3LB-V2\n"}, {"usr/bin/c3lb", "#!/bin/sh\n"}});
    install_packages({v2}, "", false);

    EXPECT_EQ(read_file(conf), "C3LB-V1\n") << "拿不到旧记录时宁可保守：原文件**不**动";
    ASSERT_TRUE(fs::exists(conf_new)) << "两份不一致 → 新版落 .lpkgnew";
    EXPECT_EQ(read_file(conf_new), "C3LB-V2\n");
    // 记录写的是**包内那份（新包 v2 内容）**的哈希 —— 记录永远只声明"这个包的这个版本
    // 提供过什么"，盘上那份（用户/旧版内容）**永不**被追认成我们装进去的东西
    const std::string db_after = conf_db_bytes();
    EXPECT_NE(db_after.find("/etc/c3lb.conf"), std::string::npos) << db_after;
    EXPECT_NE(db_after.find(calculate_sha256(conf_new)), std::string::npos)
        << "记录的必须是**包内那份**（新包 v2 内容）的哈希：" << db_after;
    EXPECT_EQ(db_after.find(calculate_sha256(conf)), std::string::npos)
        << "不许把盘上那份（用户/旧版内容）追认成我们装进去过的东西：" << db_after;

    // 第二阶段：用户审阅后删掉 .lpkgnew（最常见处置）、**盘上那份仍是 C3LB-V1**，再升到 v3
    // （配置又换了）→ 记录说的是"我们装过 C3LB-V2"，与盘上那份**不相等** → 不是 ①：
    // 必须继续保留原文件、**再落一份可见的** .lpkgnew。改前：记录 = 盘上那份 → 满足 ① →
    // 把 V1 静默换成 V3（用户那份若与 V1 不同，就是"用户改过的配置被静默覆盖"，且无 .lpkgnew）。
    fs::remove(conf_new);
    const std::string v3 = create_pkg_files(
        "c3lb", "3.0", {{"etc/c3lb.conf", "C3LB-V3\n"}, {"usr/bin/c3lb", "#!/bin/sh\n"}});
    install_packages({v3}, "", false);
    EXPECT_EQ(read_file(conf), "C3LB-V1\n") << "记录不指向盘上那份 → 不许把用户那份静默换掉";
    ASSERT_TRUE(fs::exists(conf_new)) << "仍然判定为冲突 → 再落一份 .lpkgnew";
    EXPECT_EQ(read_file(conf_new), "C3LB-V3\n");
    // ⚠ 代价（有意为之、写在这里免得以后被当成 bug）：盘上那份从此停在 V1 不再前进，除非
    // 用户**把 .lpkgnew 合并进**它 —— 记录说的是"我们装过 C3LB-V2"，而盘上那份是 C3LB-V1，
    // 二者不相等就不是 ①。于是包此后每改一次配置都会再落一份**可见的** .lpkgnew。
    // 换来的正是**不静默**：改前这里记的是"盘上那份"，下一次升级会满足 ① 把 V1 悄悄换成
    // 新版（用户那份若与 V1 不同 = 用户配置被静默覆盖，且连 .lpkgnew 都不产生）。

    // 第三阶段：用户**合并**了新版（把 .lpkgnew 采用为盘上那份）→ 盘上那份与记录对齐 →
    // 下一次升级回到 ①，静默跟上。这就是退出"吵"的状态的入口。
    user_edit(conf, "C3LB-V3\n");
    fs::remove(conf_new);
    const std::string v4 = create_pkg_files(
        "c3lb", "4.0", {{"etc/c3lb.conf", "C3LB-V4\n"}, {"usr/bin/c3lb", "#!/bin/sh\n"}});
    install_packages({v4}, "", false);
    EXPECT_EQ(read_file(conf), "C3LB-V4\n") << "用户合并之后应当回到正常分流（静默跟上）";
    EXPECT_FALSE(fs::exists(conf_new));
}

// ============================================================================
// ⑫ 退化路径的记录值必须是**包内内容**的哈希 —— 盘上那份（无主文件）永远不被"追认"
//
//    触发链 B 的第二段现场：`remove pkg`（配置 → .lpkgsave，记录被删）→ 用户把自己那份
//    放回原路径 → `install --overwrite` 重装（无主文件撞包内文件，唯一合法入口；无旧记录
//    = 退化路径，落一份 .lpkgnew）→ **此后不再编辑该文件** → 升级到配置又变的版本。
//
//    改前：退化路径把**盘上那份**（USER-KEPT）写进 DB = 追认"这份用户文件就是我们装的"
//    → 下一次升级满足 ①（盘上 == 旧记录）→ **静默就地覆盖**，用户那份没了、**连 `.lpkgnew`
//    都不产生**（用户连"新版长什么样"都看不到）。而这不是一次性的：记录有三处会按设计被
//    删除（升级丢弃 /etc 条目、remove_conf_hash、移除路径），"追认"会被反复重新武装
//    （本文件 ⑬ 就是那条链）。
//
//    判定表（classify_config_update）**不动**：老 DB 无记录时仍是"两份一致 → 保留原文件 /
//    不一致 → 落 .lpkgnew + 告警"，用户可见行为完全不变；变的只有**写进 DB 的记录值**。
// ============================================================================
TEST_F(ConfigThreeWayHashTest, DegenerateRecordNeverAuthorizesSilentOverwrite)
{
    const fs::path conf = test_root / "etc/c3x.conf";
    const fs::path saved = test_root / "etc/c3x.conf.lpkgsave";
    const fs::path conf_new = test_root / "etc/c3x.conf.lpkgnew";

    const std::string v1 = create_pkg_files(
        "c3x", "1.0", {{"etc/c3x.conf", "C3X-V1\n"}, {"usr/bin/c3x", "#!/bin/sh\n"}});
    install_packages({v1}, "", false);
    user_edit(conf, "USER-KEPT\n");

    // 移除：配置改名成 .lpkgsave 保留（移除侧语义）、记录随之消失
    remove_package("c3x", /*force=*/false);
    ASSERT_TRUE(fs::exists(saved));
    ASSERT_FALSE(fs::exists(conf));
    fs::remove(saved);
    user_edit(conf, "USER-KEPT\n");  // 用户手工把自己那份放回原路径（此刻**无主**）

    const std::string v2 = create_pkg_files(
        "c3x", "2.0", {{"etc/c3x.conf", "C3X-V2\n"}, {"usr/bin/c3x", "#!/bin/sh\n"}});
    // 无主文件撞包内文件是**冲突**，需要 --overwrite 豁免（pacman 同：无包认领 = "未知手动文件"）
    Config::instance().set_force_overwrite_mode(true);
    install_packages({v2}, "", false);
    Config::instance().set_force_overwrite_mode(false);

    // 退化路径的**用户可见行为**（判定表不动，与本用例无关，先钉住现场）：原文件不动 +
    // 新版落 .lpkgnew（两份不一致 → 保守）
    ASSERT_EQ(read_file(conf), "USER-KEPT\n") << "用户手工放回的文件不得被静默覆盖";
    ASSERT_TRUE(fs::exists(conf_new));
    ASSERT_EQ(read_file(conf_new), "C3X-V2\n");

    // 记录值：必须是**包内那份**（C3X-V2）的哈希。记盘上那份（USER-KEPT）= 追认"这份用户
    // 文件就是我们装的"，下一次升级必被静默覆盖 —— 下面就是那一次。
    const std::string db = conf_db_bytes();
    EXPECT_NE(db.find(calculate_sha256(conf_new)), std::string::npos)
        << "退化路径的记录值必须是包内内容的哈希：" << db;
    EXPECT_EQ(db.find(calculate_sha256(conf)), std::string::npos)
        << "不许把盘上那份（用户文件）追认成我们装进去过的东西：" << db;

    // 用户审阅后删掉这份 .lpkgnew（最常见处置），此后**不再编辑** /etc/c3x.conf
    fs::remove(conf_new);

    const std::string v3 = create_pkg_files(
        "c3x", "3.0", {{"etc/c3x.conf", "C3X-V3\n"}, {"usr/bin/c3x", "#!/bin/sh\n"}});
    install_packages({v3}, "", false);

    EXPECT_EQ(read_file(conf), "USER-KEPT\n")
        << "记录被追认成盘上那份 → 本次升级满足 ① 把用户文件静默覆盖了（连 .lpkgnew 都没有）";
    ASSERT_TRUE(fs::exists(conf_new))
        << "用户那份与新版本仍有差异 → 必须再落一份**可见的** .lpkgnew（吵，但绝不静默）";
    EXPECT_EQ(read_file(conf_new), "C3X-V3\n");
}

// ============================================================================
// ⑬ 记录被删 + 该路径重新归本包 = "追认"被**重新武装**（触发链 A）
//
//    `v1 装 → v2 丢掉该 /etc 文件（记录被删）→ v3 重新发它`
//    —— **改前**：v2 只撤记录、用户那份仍**占着原路径**，v3 走"无旧记录 + 盘上有那份"的
//    退化路径把盘上那份追认进 DB，v4 升级时 ① 成立 → 用户那份被静默覆盖。
//    **改后（2026-09-26）**：v2 把那份改名 `.lpkgsave`，原路径变空 ⇒ v3 直接就地落位、
//    压根不经过退化路径，追认无从发生（用户那份仍留在 `.lpkgsave` 里）。
//    本用例因此改钉**新终态**：丢弃留副本、重新发布就地处，且记录值永远是包内内容的哈希。
//    "追认只发生一次"这个说法只对"记录不再被删"成立；记录按设计有三处会删（⑫ 的说明）——
//    退化路径本身仍在（无主文件占着路径 + `--overwrite`），那条不变量由本文件前半段钉。
// ============================================================================
TEST_F(ConfigThreeWayHashTest, ReshippedAfterDropLandsInPlaceAndKeepsUserCopy)
{
    const fs::path conf = test_root / "etc/c3y.conf";
    const fs::path conf_new = test_root / "etc/c3y.conf.lpkgnew";

    const std::string v1 = create_pkg_files(
        "c3y", "1.0", {{"etc/c3y.conf", "C3Y-V1\n"}, {"usr/bin/c3y", "#!/bin/sh\n"}});
    install_packages({v1}, "", false);
    user_edit(conf, "USER-KEPT\n");

    // v2 **不再提供**这个配置：/etc 条目被丢弃 → 归属与记录一并撤；盘上那份改名
    // `<路径>.lpkgsave` **保留下来**（2026-09-26 起；改前是"原地不动、只撤所有权"）。
    // 与"类型变化"、移除整包统一到一条规则：/etc 下的东西永远不会被无声丢掉，
    // 也永远不会占着"新版本该用的那个名字"。
    const std::string v2 = create_pkg_files("c3y", "2.0", {{"usr/bin/c3y", "#!/bin/sh\n"}});
    install_packages({v2}, "", false);
    const fs::path conf_save = test_root / "etc/c3y.conf.lpkgsave";
    ASSERT_FALSE(fs::exists(conf)) << "废弃的 /etc 文件不该再占着原路径";
    ASSERT_EQ(read_file(conf_save), "USER-KEPT\n") << "用户那份必须逐字节留在 .lpkgsave 里";
    ASSERT_EQ(conf_db_bytes().find("/etc/c3y.conf"), std::string::npos)
        << "记录必须随归属一起撤：" << conf_db_bytes();

    // ── 本条用例原先要钉的"追认被重新武装"在这个场景下**消失了**（2026-09-26）────────
    // 改前：v2 丢弃条目后用户那份仍**占着 `/etc/c3y.conf`**，于是 v3 重新发它时走的是
    // "无旧记录 + 盘上有那份"的**退化路径** —— 那条路会把盘上那份追认进 DB，v4 升级时
    // ① 成立 → 用户那份被**静默覆盖**。
    // 改后：v2 已经把那份改名到 `.lpkgsave`，`/etc/c3y.conf` 是**空路径**，v3 直接就地落位
    // ⇒ 退化路径压根不经过，追认无从发生。用户那份仍在 `.lpkgsave` 里（可寻回）。
    // 所以下面钉的是**新终态**，不是把旧断言放宽。
    const std::string v3 = create_pkg_files(
        "c3y", "3.0", {{"etc/c3y.conf", "C3Y-V3\n"}, {"usr/bin/c3y", "#!/bin/sh\n"}});
    install_packages({v3}, "", false);
    EXPECT_EQ(read_file(conf), "C3Y-V3\n") << "空路径 → 就地落新版（无需 --overwrite）";
    EXPECT_FALSE(fs::exists(conf_new))
        << "路径是空的、没有要与用户对照的旧内容 ⇒ 不该产生 .lpkgnew";
    EXPECT_EQ(read_file(conf_save), "USER-KEPT\n") << "用户那份全程留在 .lpkgsave 里，不被覆盖";
    // 记录值仍然必须是**包内那份**（不许追认盘上任何东西）
    EXPECT_NE(conf_db_bytes().find(calculate_sha256(conf)), std::string::npos)
        << "记录值必须是包内内容（C3Y-V3）的哈希：" << conf_db_bytes();

    // v4 再升一次：用户那份早已不在原路径上 ⇒ 不存在"被静默覆盖"的窗口
    const std::string v4 = create_pkg_files(
        "c3y", "4.0", {{"etc/c3y.conf", "C3Y-V4\n"}, {"usr/bin/c3y", "#!/bin/sh\n"}});
    install_packages({v4}, "", false);
    EXPECT_EQ(read_file(conf), "C3Y-V4\n") << "用户没改过盘上那份（它本来就是包内的）→ 静默换新版";
    EXPECT_EQ(read_file(conf_save), "USER-KEPT\n") << "用户那份仍在 .lpkgsave 里";
}

// ============================================================================
// ⑧ 回滚保真：升级批次中途失败 → 被静默替换掉的配置**与哈希 DB** 都回到批次前
//    （静默替换现在会真的改盘（BACKUP + COPY），所以回滚必须还原它 —— 这条是本
//      "静默替换"能成立的另一半：改得动，就撤得回）
//
//    还原的判据含三样：配置文件**内容**、它的**属主/权限**（用户在现场留下的那部分状态，
//    靠 BACKUP 的 rename 保存整个 inode）、以及 hash DB 里的**记录**（逐字节 + 值两步）。
// ============================================================================

TEST_F(ConfigThreeWayHashTest, BatchRollbackRestoresConfigAndHashDb)
{
    const std::string a1 = create_pkg_files(
        "c3ra", "1.0", {{"etc/c3ra.conf", "C3RA-V1\n"}, {"usr/bin/c3ra", "#!/bin/sh\n"}});
    const std::string b1 = create_pkg_files("c3rb", "1.0", {{"usr/bin/c3rb", "#!/bin/sh\n"}});
    ASSERT_NO_THROW(install_packages({a1, b1}, "", false));

    const fs::path conf_a = test_root / "etc/c3ra.conf";
    ASSERT_EQ(read_file(conf_a), "C3RA-V1\n");

    // 在配置上留下"用户在现场改过"的**内容之外**的状态：属主/权限。包内那份是 root:root，
    // 这里改成一个可区分的组合 —— 回滚必须把它们一起还原（BACKUP 是 rename，整个 inode
    // 原封不动；任何"重新写出内容"式的还原都会丢掉它们）。
    ASSERT_EQ(::chown(conf_a.c_str(), 12345, 12345), 0) << "沙盒无法 chown 出非 root 属主";
    ASSERT_EQ(::chmod(conf_a.c_str(), 0640), 0);
    const auto stat_before = stat_of(conf_a);
    ASSERT_EQ(std::get<1>(stat_before), 12345ull);

    const std::string db_before = conf_db_bytes();
    ASSERT_NE(db_before, "<不存在>") << "批次开始前哈希 DB 就该在（DB 相等断言才有意义）";
    ASSERT_NE(db_before.find("/etc/c3ra.conf"), std::string::npos);
    // 批次前那条记录的值 = 盘上那份（v1）的哈希 —— 回滚后要**按值**核对，不只比字节
    ASSERT_NE(db_before.find(calculate_sha256(conf_a)), std::string::npos) << db_before;

    const std::string a2 = create_pkg_files(
        "c3ra", "2.0", {{"etc/c3ra.conf", "C3RA-V2\n"}, {"usr/bin/c3ra", "#!/bin/sh\n"}});
    const std::string b2 = create_pkg_files("c3rb", "2.0", {{"usr/bin/c3rb", "#!/bin/sh\n"}});

    // 批次中途取证：a 已 COMMIT（配置已被静默换成 v2、DB 里程碑已落）之后、b 失败之前
    std::map<std::string, std::string> mid;
    std::tuple<unsigned, unsigned long long, unsigned long long> mid_stat{};
    BreakpointManager::instance().set("install_after_begin_c3rb", [&] {
        mid["conf"] = read_file(test_root / "etc/c3ra.conf");
        mid["db"] = conf_db_bytes();
        mid_stat = stat_of(conf_a);
        throw LpkgException("injected failure: 批次中途失败");
    });

    EXPECT_THROW(install_packages({a2, b2}, "", false), LpkgException);
    BreakpointManager::instance().clear_all();

    // 中途取证：a 的配置**真的**被静默换掉了（否则下面的"回滚还原"是空壳），
    // 且属主/权限也确实换成了包内那份（root:root）—— 回滚要还原的正是这个"换过"的状态
    ASSERT_FALSE(mid.empty()) << "断点没命中";
    ASSERT_EQ(mid["conf"], "C3RA-V2\n") << "断点取得太早：a 的配置还没被静默替换";
    EXPECT_NE(mid["db"], db_before) << "断点取得太早：a 的哈希记录还没落盘";
    EXPECT_NE(mid_stat, stat_before)
        << "替换后属主/权限还是用户那份 —— 回滚的「属主/权限」维度没被考到";

    // 回滚后：配置回到 v1（内容 + 属主/权限 + 不残留 .lpkgnew + 不走 .lpkgsave），
    // 哈希 DB 逐字节回到批次前
    EXPECT_EQ(read_file(conf_a), "C3RA-V1\n") << "回滚必须还原被静默替换掉的配置";
    {
        const auto st_after = stat_of(conf_a);
        EXPECT_EQ(st_after, stat_before)
            << "回滚后属主/权限没回来（BACKUP 是 rename，整个 inode 该原样回来）";
    }
    EXPECT_FALSE(fs::exists(test_root / "etc/c3ra.conf.lpkgnew"));
    EXPECT_FALSE(fs::exists(test_root / "etc/c3ra.conf.lpkgsave"));
    EXPECT_EQ(conf_db_bytes(), db_before) << "哈希 DB 必须逐字节回到批次前";
    // 记录**按值**也对得上：那条记录说的是"盘上这份（v1）"
    EXPECT_NE(conf_db_bytes().find(calculate_sha256(conf_a)), std::string::npos)
        << "回滚后记录指向的不是盘上那份：" << conf_db_bytes();
    EXPECT_EQ(Cache::instance().get_installed_version("c3ra"), "1.0");
    EXPECT_EQ(Cache::instance().get_installed_version("c3rb"), "1.0");
    // 静默替换产生的备份（进 stash 的那一份）必须随回滚被消费干净
    EXPECT_EQ(count_residue(), 0) << "回滚后仍有 .lpkg_bak_* / .lpkgtmp 残留";

    // 回滚后的"世界"仍然自洽：用户没改过 → 再升一次仍然静默跟上（且这次属主/权限换成包内那份）
    EXPECT_NO_THROW(install_packages({a2, b2}, "", false));
    EXPECT_EQ(read_file(conf_a), "C3RA-V2\n");
    EXPECT_FALSE(fs::exists(test_root / "etc/c3ra.conf.lpkgnew"));
}

// ============================================================================
// ⑨ 回滚保真（"批次前就没有记录"的那一支）：哈希 DB 里该路径的记录回到**不存在**
//    —— 老 DB 退化路径建起来的那条记录，同样必须随批次回滚一起消失
// ============================================================================

TEST_F(ConfigThreeWayHashTest, BatchRollbackDropsHashRecordThatDidNotExistBefore)
{
    const fs::path conf = test_root / "etc/c3rc.conf";
    const std::string a1 = create_pkg_files(
        "c3rc", "1.0", {{"etc/c3rc.conf", "C3RC-V1\n"}, {"usr/bin/c3rc", "#!/bin/sh\n"}});
    const std::string b1 = create_pkg_files("c3rd", "1.0", {{"usr/bin/c3rd", "#!/bin/sh\n"}});
    ASSERT_NO_THROW(install_packages({a1, b1}, "", false));
    ASSERT_EQ(read_file(conf), "C3RC-V1\n");

    // 模拟老 DB：把哈希 DB 整体删掉（本特性之前的世界）
    fs::remove(Config::instance().conf_hashes_db());
    Cache::instance().load();

    const std::string a2 = create_pkg_files(
        "c3rc", "2.0", {{"etc/c3rc.conf", "C3RC-V2\n"}, {"usr/bin/c3rc", "#!/bin/sh\n"}});
    const std::string b2 = create_pkg_files("c3rd", "2.0", {{"usr/bin/c3rd", "#!/bin/sh\n"}});

    std::string mid_db;
    BreakpointManager::instance().set("install_after_begin_c3rd", [&] {
        mid_db = conf_db_bytes();
        throw LpkgException("injected failure: 批次中途失败");
    });

    EXPECT_THROW(install_packages({a2, b2}, "", false), LpkgException);
    BreakpointManager::instance().clear_all();

    // 中途取证：退化路径确实把"盘上那份"的记录建起来了（否则下面的"回到不存在"是空壳）
    ASSERT_NE(mid_db.find("/etc/c3rc.conf"), std::string::npos)
        << "退化路径没有建记录（批次还没走完？）：" << mid_db;

    // 回滚后：记录回到**不存在**（不是回到某个旧值），盘上那份也原样
    EXPECT_EQ(read_file(conf), "C3RC-V1\n") << "回滚必须还原原文件";
    const std::string db_after = conf_db_bytes();
    EXPECT_EQ(db_after.find("/etc/c3rc.conf"), std::string::npos)
        << "批次前没有这条记录 → 回滚后也必须没有：" << db_after;
    EXPECT_EQ(Cache::instance().get_installed_version("c3rc"), "1.0");
    // 之后升级照旧走退化路径（记录确实没了）
    ASSERT_NO_THROW(install_packages({a2, b2}, "", false));
    EXPECT_EQ(read_file(conf), "C3RC-V1\n") << "无记录 + 两份不同 → 原文件不动（新版进 .lpkgnew）";
    EXPECT_TRUE(fs::exists(test_root / "etc/c3rc.conf.lpkgnew"));
}

// ============================================================================
// ⑩ 回滚不留 .lpkgnew：「请审阅」文件同样是**本批次改动的盘面**，回滚必须收走
//
//    `.lpkgnew` 的落位走的是与普通文件分支同一套写入层原语（先 `.lpkgtmp`，再
//    `commit_copy` 写 COPY 行）；目标已存在时先把旧那份 BACKUP 进 stash。于是：
//      · 批次回滚 → COPY 的逆操作删掉刚落的那份 → 盘面回到批次前（没有多出来的副本）
//      · 批次前就有一份 → BACKUP 的逆操作把它 rename 回来（**保留**，不是删掉）
//    改前：裸 `fs::copy` 写在事务外 → 回滚后那份「请审阅」副本赖在 /etc 上（红）。
// ============================================================================

TEST_F(ConfigThreeWayHashTest, RollbackLeavesNoLpkgnewFromConflictedUpgrade)
{
    const fs::path conf = test_root / "etc/c3n.conf";
    const fs::path conf_new = test_root / "etc/c3n.conf.lpkgnew";

    const std::string a1 = create_pkg_files(
        "c3n", "1.0", {{"etc/c3n.conf", "C3N-V1\n"}, {"usr/bin/c3n", "#!/bin/sh\n"}});
    const std::string b1 = create_pkg_files("c3nb", "1.0", {{"usr/bin/c3nb", "#!/bin/sh\n"}});
    ASSERT_NO_THROW(install_packages({a1, b1}, "", false));
    ASSERT_EQ(read_file(conf), "C3N-V1\n");

    // 用户改过盘上那份 → 升级到 v2 必然走 ③（三者互异 → 新版落 .lpkgnew）
    user_edit(conf, "USER-EDITED\n");

    const std::string a2 = create_pkg_files(
        "c3n", "2.0", {{"etc/c3n.conf", "C3N-V2\n"}, {"usr/bin/c3n", "#!/bin/sh\n"}});
    const std::string b2 = create_pkg_files("c3nb", "2.0", {{"usr/bin/c3nb", "#!/bin/sh\n"}});

    // 批次中途取证：a 已办理完（.lpkgnew 已落）之后、b 失败之前
    bool mid_has_new = false;
    std::string mid_new_content;
    BreakpointManager::instance().set("install_after_begin_c3nb", [&] {
        mid_has_new = fs::exists(conf_new);
        mid_new_content = read_file(conf_new);
        throw LpkgException("injected failure: 批次中途失败");
    });

    EXPECT_THROW(install_packages({a2, b2}, "", false), LpkgException);
    BreakpointManager::instance().clear_all();

    // 断点取证：.lpkgnew 在批次里**真的**被写了出来（否则下面的"回滚不留"是空壳）
    ASSERT_TRUE(mid_has_new) << "断点取得太早：a 还没写出 .lpkgnew";
    ASSERT_EQ(mid_new_content, "C3N-V2\n") << "落进 .lpkgnew 的应当是包内新版内容";

    EXPECT_EQ(read_file(conf), "USER-EDITED\n") << "回滚必须把用户那份配置原样留下";
    EXPECT_FALSE(fs::exists(conf_new))
        << "批次被回滚 → 批次里落下的「请审阅」副本也必须一并收走：包根本没装上，"
           "却在 /etc 上多出一份配置副本 = 事务外的副作用（裸 fs::copy 的旧行为）";
    EXPECT_EQ(Cache::instance().get_installed_version("c3n"), "1.0");
    EXPECT_EQ(count_residue(), 0) << "回滚后仍有 .lpkg_bak_* / .lpkgtmp 残留";

    // 回滚后的世界自洽：再升一次（这次成功）→ 该产生的 .lpkgnew 照样产生
    EXPECT_NO_THROW(install_packages({a2, b2}, "", false));
    EXPECT_EQ(read_file(conf), "USER-EDITED\n");
    ASSERT_TRUE(fs::exists(conf_new));
    EXPECT_EQ(read_file(conf_new), "C3N-V2\n");
}

TEST_F(ConfigThreeWayHashTest, RollbackRestoresPreviousLpkgnewFile)
{
    const fs::path conf = test_root / "etc/c3p.conf";
    const fs::path conf_new = test_root / "etc/c3p.conf.lpkgnew";

    const std::string a1 = create_pkg_files(
        "c3p", "1.0", {{"etc/c3p.conf", "C3P-V1\n"}, {"usr/bin/c3p", "#!/bin/sh\n"}});
    const std::string b1 = create_pkg_files("c3pb", "1.0", {{"usr/bin/c3pb", "#!/bin/sh\n"}});
    ASSERT_NO_THROW(install_packages({a1, b1}, "", false));
    user_edit(conf, "USER-EDITED\n");

    // 批次前就有一份**上一次升级留下、用户还没处理**的 .lpkgnew
    user_edit(conf_new, "PREVIOUS-REVIEW\n");
    ASSERT_EQ(read_file(conf_new), "PREVIOUS-REVIEW\n");

    const std::string a2 = create_pkg_files(
        "c3p", "2.0", {{"etc/c3p.conf", "C3P-V2\n"}, {"usr/bin/c3p", "#!/bin/sh\n"}});
    const std::string b2 = create_pkg_files("c3pb", "2.0", {{"usr/bin/c3pb", "#!/bin/sh\n"}});

    std::string mid_new_content;
    BreakpointManager::instance().set("install_after_begin_c3pb", [&] {
        mid_new_content = read_file(conf_new);
        throw LpkgException("injected failure: 批次中途失败");
    });

    EXPECT_THROW(install_packages({a2, b2}, "", false), LpkgException);
    BreakpointManager::instance().clear_all();

    // 断点取证：新那份**确实盖在旧的上面**了（否则"还原旧的"没被考到）
    ASSERT_EQ(mid_new_content, "C3P-V2\n") << "断点取得太早：批次里还没覆盖旧的 .lpkgnew";

    EXPECT_EQ(read_file(conf), "USER-EDITED\n") << "回滚必须把用户那份配置原样留下";
    ASSERT_TRUE(fs::exists(conf_new))
        << "批次前就存在的 .lpkgnew 必须被 BACKUP 进 stash 后**还原**回来 —— "
           "回滚不是「删掉旧的那份」，用户还没审阅过它";
    EXPECT_EQ(read_file(conf_new), "PREVIOUS-REVIEW\n")
        << "还原的必须是**批次前那份**（不是被新的盖住、也不是消失）";
    EXPECT_EQ(Cache::instance().get_installed_version("c3p"), "1.0");
    EXPECT_EQ(count_residue(), 0) << "回滚后仍有 .lpkg_bak_* / .lpkgtmp 残留";
}

// 正向对照：成功批次里 .lpkgnew 必须**存在且内容 == 新版本配置**
// （防止"为了不留残留而干脆不写"这种把功能做没了的"修复"）
TEST_F(ConfigThreeWayHashTest, SuccessfulUpgradeStillWritesLpkgnew)
{
    const fs::path conf = test_root / "etc/c3q.conf";
    const fs::path conf_new = test_root / "etc/c3q.conf.lpkgnew";

    const std::string v1 = create_pkg_files(
        "c3q", "1.0", {{"etc/c3q.conf", "C3Q-V1\n"}, {"usr/bin/c3q", "#!/bin/sh\n"}});
    ASSERT_NO_THROW(install_packages({v1}, "", false));
    user_edit(conf, "USER-EDITED\n");

    const std::string v2 = create_pkg_files(
        "c3q", "2.0", {{"etc/c3q.conf", "C3Q-V2\n"}, {"usr/bin/c3q", "#!/bin/sh\n"}});
    ASSERT_NO_THROW(install_packages({v2}, "", false));

    EXPECT_EQ(read_file(conf), "USER-EDITED\n") << "用户改过的配置不许被静默覆盖";
    ASSERT_TRUE(fs::exists(conf_new))
        << "成功批次里冲突的新版配置必须落到 .lpkgnew（否则用户永远看不到新版本）";
    EXPECT_EQ(read_file(conf_new), "C3Q-V2\n");
    EXPECT_FALSE(fs::is_symlink(conf_new)) << "包内那份是普通文件 → .lpkgnew 也该是普通文件";
    EXPECT_EQ(count_residue(), 0) << "成功批次后不该留下 .lpkgtmp / .lpkg_bak_*";
    EXPECT_EQ(Cache::instance().get_installed_version("c3q"), "2.0");
}

// ============================================================================
// ⑪ 符号链接型 conffile 的 `.lpkgnew` 同样在事务内（⑩ 的同类最后一处）
//
//   包内 `/etc/<x>` 是**符号链接**、而盘上该路径已被占用时，走的是 `copy_package_files` 的
//   **符号链接分支**（与三哈希分流无关的另一段代码）：dest 改写成 `<路径>.lpkgnew`，
//   那份链接照样是"这个批次改动了盘面"。改前它是裸的 `fs::remove(dest)` +
//   `fs::create_symlink(...)`、**没有任何 WAL 行** —— 于是：
//     · 批次回滚后包根本没装上，/etc 上却平白多出一份「请审阅」链接（盘面与 WAL 脱节）；
//     · 批次前已有的那份 `.lpkgnew` 被无条件 `fs::remove` 掉，回滚也回不来。
//   改后与文件分支（⑩）走同一套写入层原语：目标不存在 → 先写 `NEW` 行再建链接（单次
//   原子操作）；目标已存在 → 先 `BACKUP` 进 stash 让开、再写 `NEW` 行、再建链接
//   （行序不可反，逆序回滚才会"先撤新链接、再还原旧链接"）。
// ============================================================================

// 回滚不留 .lpkgnew 符号链接：批次里建出的那份必须被收走，原链接与它的目标都完好
TEST_F(ConfigThreeWayHashTest, RollbackLeavesNoLpkgnewSymlinkFromConflictedUpgrade)
{
    const fs::path conf = test_root / "etc/c4s.conf";
    const fs::path conf_new = test_root / "etc/c4s.conf.lpkgnew";
    const fs::path v1_target = test_root / "usr/share/c4s/c4s.conf";
    const fs::path v2_target = test_root / "usr/share/c4s/c4s-v2.conf";

    // v1：包内 /etc/c4s.conf 是符号链接（首发 → 落原位，盘上成为链接）
    const std::string v1 = create_pkg_symlinks(
        "c4s", "1.0", {{"usr/share/c4s/c4s.conf", "C4S-V1\n"}, {"usr/bin/c4s", "#!/bin/sh\n"}},
        {{"etc/c4s.conf", "/usr/share/c4s/c4s.conf"}});
    const std::string b1 = create_pkg_files("c4sb", "1.0", {{"usr/bin/c4sb", "#!/bin/sh\n"}});
    ASSERT_NO_THROW(install_packages({v1, b1}, "", false));
    ASSERT_TRUE(fs::is_symlink(conf)) << "包内符号链接应当落到 /etc 原位";
    ASSERT_EQ(fs::read_symlink(conf), "/usr/share/c4s/c4s.conf");

    // v2：包内那份仍是符号链接、但换了目标 → 盘上已被占用 → 冲突 → 落 .lpkgnew
    const std::string v2 = create_pkg_symlinks(
        "c4s", "2.0", {{"usr/share/c4s/c4s-v2.conf", "C4S-V2\n"}, {"usr/bin/c4s", "#!/bin/sh\n"}},
        {{"etc/c4s.conf", "/usr/share/c4s/c4s-v2.conf"}});
    const std::string b2 = create_pkg_files("c4sb", "2.0", {{"usr/bin/c4sb", "#!/bin/sh\n"}});

    // 批次中途取证：c4s 已办理完（.lpkgnew 链接已建）之后、c4sb 失败之前
    bool mid_is_link = false;
    std::string mid_target;
    BreakpointManager::instance().set("install_after_begin_c4sb", [&] {
        mid_is_link = fs::is_symlink(conf_new);
        if (mid_is_link) mid_target = fs::read_symlink(conf_new);
        throw LpkgException("injected failure: 批次中途失败");
    });

    EXPECT_THROW(install_packages({v2, b2}, "", false), LpkgException);
    BreakpointManager::instance().clear_all();

    // 断点取证：那份「请审阅」链接在批次里**真的**被建了出来（否则下面的"回滚不留"是空壳）
    ASSERT_TRUE(mid_is_link) << "断点取得太早：批次里还没建出 .lpkgnew 符号链接";
    ASSERT_EQ(mid_target, "/usr/share/c4s/c4s-v2.conf") << "落进 .lpkgnew 的应当是包内那份的目标";

    // dangling 符号链接 `fs::exists` 看不见（此时它的目标已被回滚收走）→ 必须并判 is_symlink
    EXPECT_FALSE(fs::exists(conf_new) || fs::is_symlink(conf_new))
        << "批次被回滚 → 批次里建出的「请审阅」链接也必须一并收走：包根本没装上，"
           "却在 /etc 上多出一份链接 = 事务外的副作用（裸 fs::remove+create_symlink 的旧行为）";

    // 原链接与它的目标都完好（冲突路径本来就不该碰它们）
    ASSERT_TRUE(fs::is_symlink(conf)) << "原 /etc/c4s.conf 链接被动了";
    EXPECT_EQ(fs::read_symlink(conf), "/usr/share/c4s/c4s.conf") << "原链接的目标被改写";
    EXPECT_EQ(read_file(v1_target), "C4S-V1\n") << "原链接的目标文件内容被动了";
    EXPECT_EQ(Cache::instance().get_installed_version("c4s"), "1.0");
    EXPECT_EQ(Cache::instance().get_installed_version("c4sb"), "1.0") << "批次成员也要退回批次前";
    EXPECT_EQ(count_residue(), 0) << "回滚后仍有 .lpkg_bak_* / .lpkgtmp 残留";

    // 回滚后的世界自洽：同一个批次再来一次（这次成功）→ 该产生的 .lpkgnew 照样产生
    EXPECT_NO_THROW(install_packages({v2, b2}, "", false));
    ASSERT_TRUE(fs::is_symlink(conf_new));
    EXPECT_EQ(fs::read_symlink(conf_new), "/usr/share/c4s/c4s-v2.conf");
    EXPECT_EQ(read_file(v2_target), "C4S-V2\n");
    EXPECT_EQ(Cache::instance().get_installed_version("c4s"), "2.0");
    EXPECT_EQ(count_residue(), 0);
}

// 还原批次前已有的 .lpkgnew 符号链接：回滚是"把它 rename 回来"，不是"删掉旧的那份"
TEST_F(ConfigThreeWayHashTest, RollbackRestoresPreviousLpkgnewSymlink)
{
    const fs::path conf = test_root / "etc/c4p.conf";
    const fs::path conf_new = test_root / "etc/c4p.conf.lpkgnew";
    // 上一次留下的「请审阅」链接的目标（用户还没处理的那份）
    const fs::path prev_target = test_root / "usr/share/c4p-prev/c4p.conf";

    const std::string v1 = create_pkg_symlinks(
        "c4p", "1.0", {{"usr/share/c4p/c4p.conf", "C4P-V1\n"}, {"usr/bin/c4p", "#!/bin/sh\n"}},
        {{"etc/c4p.conf", "/usr/share/c4p/c4p.conf"}});
    const std::string b1 = create_pkg_files("c4pb", "1.0", {{"usr/bin/c4pb", "#!/bin/sh\n"}});
    ASSERT_NO_THROW(install_packages({v1, b1}, "", false));
    ASSERT_TRUE(fs::is_symlink(conf));

    // 批次前就有一份 .lpkgnew **符号链接**（上一次冲突留下的）
    ensure_dir_exists(prev_target.parent_path());  // user_edit 不建父目录，必须先建
    user_edit(prev_target, "PREVIOUS-REVIEW\n");
    fs::create_symlink("/usr/share/c4p-prev/c4p.conf", conf_new);
    ASSERT_TRUE(fs::is_symlink(conf_new));

    const std::string v2 = create_pkg_symlinks(
        "c4p", "2.0", {{"usr/share/c4p/c4p-v2.conf", "C4P-V2\n"}, {"usr/bin/c4p", "#!/bin/sh\n"}},
        {{"etc/c4p.conf", "/usr/share/c4p/c4p-v2.conf"}});
    const std::string b2 = create_pkg_files("c4pb", "2.0", {{"usr/bin/c4pb", "#!/bin/sh\n"}});

    std::string mid_target;
    BreakpointManager::instance().set("install_after_begin_c4pb", [&] {
        if (fs::is_symlink(conf_new)) mid_target = fs::read_symlink(conf_new);
        throw LpkgException("injected failure: 批次中途失败");
    });

    EXPECT_THROW(install_packages({v2, b2}, "", false), LpkgException);
    BreakpointManager::instance().clear_all();

    // 断点取证：新的那份**确实接管了**旧的（否则"还原旧的"没被考到）
    ASSERT_EQ(mid_target, "/usr/share/c4p/c4p-v2.conf")
        << "断点取得太早：批次里还没用新的 .lpkgnew 链接盖住旧的";

    ASSERT_TRUE(fs::is_symlink(conf_new))
        << "批次前就存在的 .lpkgnew 链接必须被 BACKUP 进 stash 后**还原**回来 ——"
           "回滚不是「删掉旧的那份」，用户还没审阅过它";
    EXPECT_EQ(fs::read_symlink(conf_new), "/usr/share/c4p-prev/c4p.conf")
        << "还原的必须是**批次前那份**的链接目标（不是被新的盖住、也不是消失）";
    EXPECT_EQ(read_file(prev_target), "PREVIOUS-REVIEW\n") << "旧链接的目标文件不该被动";
    ASSERT_TRUE(fs::is_symlink(conf));
    EXPECT_EQ(fs::read_symlink(conf), "/usr/share/c4p/c4p.conf") << "原配置链接不该被动";
    EXPECT_EQ(Cache::instance().get_installed_version("c4p"), "1.0");
    EXPECT_EQ(count_residue(), 0) << "回滚后仍有 .lpkg_bak_* / .lpkgtmp 残留";
}

// 正向对照：成功批次里 .lpkgnew **符号链接**照常出现且指向包内那份的目标
// （防止"为了不留残留而干脆不建链接"这种把功能做没了的"修复"）
TEST_F(ConfigThreeWayHashTest, SuccessfulUpgradeStillWritesLpkgnewSymlink)
{
    const fs::path conf = test_root / "etc/c4q.conf";
    const fs::path conf_new = test_root / "etc/c4q.conf.lpkgnew";

    const std::string v1 = create_pkg_symlinks(
        "c4q", "1.0", {{"usr/share/c4q/c4q.conf", "C4Q-V1\n"}, {"usr/bin/c4q", "#!/bin/sh\n"}},
        {{"etc/c4q.conf", "/usr/share/c4q/c4q.conf"}});
    ASSERT_NO_THROW(install_packages({v1}, "", false));
    ASSERT_TRUE(fs::is_symlink(conf));

    const std::string v2 = create_pkg_symlinks(
        "c4q", "2.0", {{"usr/share/c4q/c4q-v2.conf", "C4Q-V2\n"}, {"usr/bin/c4q", "#!/bin/sh\n"}},
        {{"etc/c4q.conf", "/usr/share/c4q/c4q-v2.conf"}});
    ASSERT_NO_THROW(install_packages({v2}, "", false));

    ASSERT_TRUE(fs::is_symlink(conf_new))
        << "成功批次里冲突的新版配置必须落到 .lpkgnew（否则用户永远看不到新版本）";
    EXPECT_EQ(fs::read_symlink(conf_new), "/usr/share/c4q/c4q-v2.conf")
        << "落下的必须是**包内那份链接的目标**";
    ASSERT_TRUE(fs::is_symlink(conf)) << "盘上那份配置仍是用户的链接，不许被覆盖";
    EXPECT_EQ(fs::read_symlink(conf), "/usr/share/c4q/c4q.conf");
    EXPECT_EQ(Cache::instance().get_installed_version("c4q"), "2.0");
    EXPECT_EQ(count_residue(), 0) << "成功批次后不该留下 .lpkgtmp / .lpkg_bak_*";
}

// ============================================================================
// ⑭ `--overwrite` 接管 = "归属被摘"：被接管包的哈希记录必须**随归属一起走**
//
//    记录声明的是"**这个包**往这个路径装过什么"（见文件头与 ARCH §6.3）。包 A 的
//    `/etc/x` 归属在**安装 B 的那一刻**就被摘走（collect_content_conflicts 的
//    `--overwrite` 豁免分支 → ConflictView::drop_owners → remove_file_owner），而记录原先
//    只在"移除时**该包仍持有**该路径"（remove_package_files）才清 —— 于是被接管之后，
//    A 的记录会永久残留在 `confhashes.db` 里：
//      · `记录随包走` 在接管场景不成立（A 已不在册，记录却还在）；
//      · 重新装回来的 A 会把这条它**并不拥有**的路径上的旧记录当成 hash_orig，
//        于是"用户没改过"的判定建立在一份 A 从未在这个路径上装过的东西之上。
//    处置选**删除**（而不是把记录改名转手给新持有者）：转手会让 B 在**它自己的这次安装**
//    里读到一条它从未装过的 hash_orig —— 判定表可能就地从 ②/③ 落到 ①（盘上 == 那条外来
//    记录 → 静默就地替换），正是本轮刚修掉的那类静默覆盖；而且它随即会被 B 自己的
//    `set_conf_hash`（先删同包前缀）抹掉，纯粹是"投毒换零收益"。下面的第二个用例
//    （TakeoverDoesNotInheritTheOldOwnersHashRecord）把这条选择钉住。
// ============================================================================

TEST_F(ConfigThreeWayHashTest, OverwriteTakeoverDropsPreviousOwnerHashRecord)
{
    const fs::path conf = test_root / "etc/c3z.conf";
    const fs::path conf_new = test_root / "etc/c3z.conf.lpkgnew";

    // A 装 `/etc/c3z.conf` → 记录 `c3z_a:<A 装进去的那份的哈希>`
    const std::string a1 = create_pkg_files(
        "c3z_a", "1.0", {{"etc/c3z.conf", "C3Z-A\n"}, {"usr/bin/c3z_a", "#!/bin/sh\n"}});
    ASSERT_NO_THROW(install_packages({a1}, "", false));
    ASSERT_EQ(read_file(conf), "C3Z-A\n");
    const std::string hash_a = calculate_sha256(conf);
    ASSERT_NE(conf_db_bytes().find("c3z_a:" + hash_a), std::string::npos)
        << "fixture 自检：A 的记录应已在库里：" << conf_db_bytes();

    // 用户改过这份配置 —— 下面"接管不得让用户内容被静默覆盖"考的就是它
    user_edit(conf, "USER-KEPT\n");

    // B 发**同一个** `/etc/` 路径 → 有真实持有者（A，且 A 不在本批次里）→ 冲突；
    // `--overwrite` 豁免它 → 所有权当场转给 B。这条路径就是"归属被摘"。
    const std::string b1 = create_pkg_files(
        "c3z_b", "1.0", {{"etc/c3z.conf", "C3Z-B\n"}, {"usr/bin/c3z_b", "#!/bin/sh\n"}});
    Config::instance().set_force_overwrite_mode(true);
    ASSERT_NO_THROW(install_packages({b1}, "", false));
    Config::instance().set_force_overwrite_mode(false);

    // 接管现场：路径归 B、A 不再持有；盘上仍是用户那份（B 无旧记录 → 退化路径 → .lpkgnew）
    EXPECT_TRUE(Cache::instance().get_file_owners("/etc/c3z.conf").contains("c3z_b"));
    EXPECT_FALSE(Cache::instance().get_file_owners("/etc/c3z.conf").contains("c3z_a"))
        << "fixture 自检：--overwrite 必须**真的**把归属摘走（否则本用例考的不是那条路径）";
    ASSERT_EQ(read_file(conf), "USER-KEPT\n") << "用户那份不许被静默覆盖";
    ASSERT_TRUE(fs::exists(conf_new));
    ASSERT_EQ(read_file(conf_new), "C3Z-B\n");

    // ── 本用例的断言：remove A 之后，A 在该路径上的记录必须消失 ──────────────
    remove_package("c3z_a", /*force=*/false);
    EXPECT_EQ(conf_db_bytes().find("c3z_a:"), std::string::npos)
        << "被接管包的记录必须随归属一起走（A 已不在册，残留 = 记录不再等价于'装过什么'）："
        << conf_db_bytes();

    // B 的记录仍在，且说的是 **B 自己装过的那份**（不是从 A 那里继承来的哈希）
    const std::string db_after = conf_db_bytes();
    EXPECT_NE(db_after.find("/etc/c3z.conf"), std::string::npos) << db_after;
    EXPECT_NE(db_after.find("c3z_b:" + calculate_sha256(conf_new)), std::string::npos)
        << "B 的记录必须是**它自己装进去的那份内容**的哈希：" << db_after;

    // 接管之后的升级：用户那份（USER-KEPT）≠ B 的记录（C3Z-B）→ 不是 ① ——
    // 必须保留用户文件 + 落一份**可见的** .lpkgnew
    fs::remove(conf_new);  // 用户审阅后删掉那份提示（最常见处置），此后不再编辑该配置
    const std::string b2 = create_pkg_files(
        "c3z_b", "2.0", {{"etc/c3z.conf", "C3Z-B2\n"}, {"usr/bin/c3z_b", "#!/bin/sh\n"}});
    ASSERT_NO_THROW(install_packages({b2}, "", false));
    EXPECT_EQ(read_file(conf), "USER-KEPT\n")
        << "接管的包不得因为一条'继承'来的记录就把用户改过的配置静默覆盖";
    ASSERT_TRUE(fs::exists(conf_new)) << "用户那份与新版本仍有差异 → 必须落 .lpkgnew（绝不静默）";
    EXPECT_EQ(read_file(conf_new), "C3Z-B2\n");
    EXPECT_EQ(Cache::instance().get_installed_version("c3z_b"), "2.0");
}

// ============================================================================
// ⑭' 接管**不继承**被接管包那条记录（上面选"删除"而不是"转手"的那一半）
//
//    现场与 ⑭ 只差一处：用户**没有**改过 A 装的那份配置。于是盘上那份逐字节等于 A 的
//    记录 —— 若把记录改名转手给 B（`c3z_a:<h>` → `c3z_b:<h>`），B 在**自己的这次安装**里
//    读到的 hash_orig 就是那条外来记录，而 hash_local 恰好等于它 → 判定表落到 ①
//    **静默就地替换**（B 的内容直接盖掉盘上那份，连 .lpkgnew 都不产生）。删除则没有这个
//    入口：B 无记录 → 退化路径（盘上那份 ≠ 包内那份 → 保留原文件 + .lpkgnew）。
//    本用例把"不许静默替换"这条底线钉在接管场景上。
// ============================================================================

TEST_F(ConfigThreeWayHashTest, TakeoverDoesNotInheritTheOldOwnersHashRecord)
{
    const fs::path conf = test_root / "etc/c3w.conf";
    const fs::path conf_new = test_root / "etc/c3w.conf.lpkgnew";

    const std::string a1 = create_pkg_files(
        "c3w_a", "1.0", {{"etc/c3w.conf", "C3W-A\n"}, {"usr/bin/c3w_a", "#!/bin/sh\n"}});
    ASSERT_NO_THROW(install_packages({a1}, "", false));
    ASSERT_EQ(read_file(conf), "C3W-A\n");
    // 用户**没有**改过：盘上那份逐字节等于 A 的记录（转手设计下就会满足 ①）
    ASSERT_NE(conf_db_bytes().find("c3w_a:" + calculate_sha256(conf)), std::string::npos)
        << conf_db_bytes();

    const std::string b1 = create_pkg_files(
        "c3w_b", "1.0", {{"etc/c3w.conf", "C3W-B\n"}, {"usr/bin/c3w_b", "#!/bin/sh\n"}});
    Config::instance().set_force_overwrite_mode(true);
    ASSERT_NO_THROW(install_packages({b1}, "", false));
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_EQ(read_file(conf), "C3W-A\n")
        << "接管的包不得把'上一任持有者装过的那份'当成自己的旧记录去 ① 静默覆盖";
    ASSERT_TRUE(fs::exists(conf_new))
        << "B 无旧记录 + 盘上那份与包内那份不同 → 必须走退化路径（保留原文件 + .lpkgnew）";
    EXPECT_EQ(read_file(conf_new), "C3W-B\n");
    EXPECT_EQ(conf_db_bytes().find("c3w_a:"), std::string::npos)
        << "归属已摘 → 旧记录必须消失（不许改名转手）：" << conf_db_bytes();
    EXPECT_NE(conf_db_bytes().find("c3w_b:"), std::string::npos);
}

// ============================================================================
// ①c **只改权限、内容一字未动** 的 `/etc` 配置：升级会纠正权限 —— 此前**完全静默**
//
// 三哈希判的是**内容哈希**（`hash_local` / `hash_orig` / `hash_pkg`），所以"用户只 chmod 过
// 这份配置"在它眼里与"用户没动过"**不可区分** ⇒ 走 ① 静默换新版那条分支。而落位时
// `stage_regular_file` 的 `lchown`/`chmod` 取自**包内条目**，于是用户改的权限被一并改回
// 包内值。实测（2026-09-26）：盘上 0600 → 升级后 0644，**且没有任何输出**。
// 目录那边至少有 `warning.dir_perm_mismatch`（目录元数据的改前值是 write-ahead 的，顺手能比），
// 文件这边此前连告警都没有。
//
// 本用例钉的是 **route (b)：先告警、再纠正** —— 只把"静默"变"可见"，**不改语义**。
// **策略已拍板（2026-09-26）：不保留用户改的 mode，包内值胜出**，要求只是「不静默」。
// 所以本用例的断言就是这条决定的体现：① 告警**必须出现**（否则退回静默）；② 权限**照样被**
// **纠正**。将来若改成「保留用户 mode」，第 ② 条会红 —— 那时这条注释与断言一起改。
// ============================================================================
TEST_F(ConfigThreeWayHashTest, ModeOnlyUserEditIsReportedNotSilentlyReverted)
{
    const std::string pkg = "c3mode";
    const fs::path target = test_root / "etc/c3mode.conf";
    const auto files = [](const char* body) {
        return std::vector<std::pair<std::string, std::string>>{{"etc/c3mode.conf", body},
                                                                {"usr/bin/c3mode", "#!/bin/sh\n"}};
    };
    ASSERT_NO_THROW(install_packages({create_pkg_files(pkg, "1.0", files("SAME\n"))}, "", false));

    // 包内 mode 是多少取决于 umask ⇒ 先读"装完之后"的值（那就是包内值），再改成一个**确定不同**的
    struct stat st{};
    ASSERT_EQ(::lstat(target.c_str(), &st), 0);
    const mode_t pkg_mode = st.st_mode & 07777;
    const mode_t user_mode = (pkg_mode == 0600u) ? 0640u : 0600u;
    ASSERT_EQ(::chmod(target.c_str(), user_mode), 0);
    ASSERT_NE(user_mode, pkg_mode);

    // 内容逐字节不变 ⇒ 三哈希判"用户没动过" ⇒ 走静默换新版那条路（本用例要考的正是它）
    std::ostringstream cap;
    auto* old = std::cerr.rdbuf(cap.rdbuf());
    ASSERT_NO_THROW(install_packages({create_pkg_files(pkg, "2.0", files("SAME\n"))}, "", false));
    std::cerr.rdbuf(old);

    // ① 告警必须**真的出现**（否则仍是静默改用户的盘）。锚取模板里**最长的字面片段** ——
    //    与语言无关，且比"取第一个 `{}` 之前"稳：本键那个前缀只有两个字，太泛，等于没断言。
    const std::string tmpl = get_string("warning.file_perm_mismatch");
    std::string needle;
    for (size_t b = 0; b <= tmpl.size();) {
        const size_t e = tmpl.find("{}", b);
        const size_t end = (e == std::string::npos) ? tmpl.size() : e;
        if (end - b > needle.size()) needle = tmpl.substr(b, end - b);
        if (e == std::string::npos) break;
        b = e + 2;
    }
    ASSERT_GE(needle.size(), 8u) << "l10n 模板里没有足够长的字面片段可作锚：" << tmpl;
    EXPECT_NE(cap.str().find(needle), std::string::npos)
        << "用户只改过权限、内容没变 ⇒ 升级纠正权限时**必须留下告警**，否则就是静默改用户的盘。"
           "要找的锚：\n"
        << needle << "\n捕获到的 stderr：\n"
        << cap.str();

    // ② 当前**语义**：权限照样被纠正（route (b) 只让它可见）。将来若改成保留用户 mode，这条会红。
    ASSERT_EQ(::lstat(target.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, pkg_mode)
        << "已拍板：**不保留**用户改的 mode（包内值胜出），只保证「不静默」（上面那条告警）—— "
           "若这里红了，说明策略被改成"
           "保留用户 mode，请连带更新本用例的注释与上面那条告警断言";
    EXPECT_EQ(read_file(target), "SAME\n") << "内容不该被动（v1/v2 逐字节相同）";
}
