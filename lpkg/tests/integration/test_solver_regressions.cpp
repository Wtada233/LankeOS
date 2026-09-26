/**
 * test_solver_regressions.cpp — 求解 / 包管理行为的回归（TODO.md D3/E1/E2/E3/F1/F2）
 *
 * 每条都对应一个"命令照打就中、且结果是静默错误"的缺陷：
 *   D3 目标没进计划 → 曾打印"所有包都已安装"并 exit 0（实际什么都没装）
 *   E1 `install --force A B`（A 已当前版本）→ 曾静默漏掉 A 的强制重装
 *   E2 按能力/SONAME 安装 → 曾被记成"依赖"不 hold → 紧接着 autoremove 就删掉
 *   E3 autoremove 走 force → 曾绕过核心包保护，删掉 /etc/lpkg/essential 里的包
 *   F1 upgrade 不 flush 触发器 → 升级后 ldconfig/glib-compile-schemas/daemon-reload 全不跑
 *   F2 libelf 未初始化版本 → 安装期 ldconfig 触发器空转（一个 SONAME 链接都不建）
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <tuple>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/crypto/hash.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/db/wal_op.hpp"  // wal::wal_log_path()（取证用）
#include "../../main/src/elf/lib_utils.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/trigger/trigger.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

/** 定义在 main/src/main_cli.cpp（随 LPKG_OBJS 进测试二进制）；此处声明以便模拟 Ctrl+C */
extern std::atomic<bool> sigint_graceful;

class SolverRegressionTest : public IntegrationTestBase
{
protected:
    fs::path mirror_dir;

    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        Config::instance().set_no_hooks_mode(true);
        mirror_dir = setup_local_mirror();
        // 触发器规则是**文件驱动**的（/etc/lpkg/triggers.conf，缺失则所有触发器静默
        // 失效）——沙盒 root 里必须显式提供，否则 F1/F2 的断言测不到东西
        {
            std::ofstream(Config::instance().triggers_conf()) << "^/usr/lib/.*\\.so.*\tldconfig\n";
        }
    }

    void TearDown() override
    {
        Config::instance().set_missing_so_no_error_mode(false);
        sigint_graceful.store(false);  // 防止把"中断"状态泄漏给后续用例
        IntegrationTestBase::TearDown();
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

    /** 打一个带真实 DT_SONAME 的库包（content/usr/lib/<name>.so.<ver>）；gcc 不可用返回 false */
    bool create_lib_pkg(const std::string& name, const std::string& ver)
    {
        const fs::path work = suite_work_dir / ("_lib_" + name + "_" + ver);
        fs::create_directories(work / "content" / "usr" / "lib");
        const fs::path src = work / "lib.c";
        std::ofstream(src) << "int " << name << "_v" << ver[0] << "(void){return " << ver[0]
                           << ";}\n";
        const fs::path out = work / "content" / "usr" / "lib" / (name + ".so." + ver);
        const std::string cmd = "gcc -shared -fPIC -Wl,-soname," + name + ".so.1 -o " +
                                out.string() + " " + src.string() + " 2>/dev/null";
        if (std::system(cmd.c_str()) != 0 || !fs::exists(out)) return false;
        pack_package((pkg_dir / (name + "-" + ver + ".lpkg")).string(), work.string(), name, ver,
                     {}, {}, "", {});
        return true;
    }

    /** 打一个带 hooks/ 的包（用于验证升级后陈旧 hook 的剪枝） */
    std::string create_pkg_with_hooks(const std::string& name, const std::string& version,
                                      const std::vector<std::string>& hooks)
    {
        const fs::path work = suite_work_dir / ("_hk_" + name + "_" + version);
        fs::create_directories(work / "content/usr/bin");
        std::ofstream(work / "content/usr/bin" / name) << "#!/bin/sh\n";
        fs::create_directories(work / "hooks");
        for (const auto& h : hooks) std::ofstream(work / "hooks" / h) << "#!/bin/sh\nexit 0\n";
        const std::string path = (pkg_dir / (name + "-" + version + ".lpkg")).string();
        pack_package(path, work.string(), name, version, {}, {}, "", {});
        return path;
    }

    /** 按显式路径清单打包（目录条目以 '/' 结尾），用于"文件 ↔ 目录"这类布局变更 */
    std::string create_layout_pkg(const std::string& name, const std::string& version,
                                  const std::vector<std::string>& paths)
    {
        const fs::path work = suite_work_dir / ("_ly_" + name + "_" + version);
        for (const auto& p : paths) {
            const fs::path full = work / "content" / p;
            if (p.ends_with('/')) {
                fs::create_directories(full);
            } else {
                fs::create_directories(full.parent_path());
                std::ofstream(full) << "v" << version << "\n";
            }
        }
        const std::string path = (pkg_dir / (name + "-" + version + ".lpkg")).string();
        pack_package(path, work.string(), name, version, {}, {}, "", {});
        return path;
    }

    static bool gcc_available()
    {
        return std::system("command -v gcc >/dev/null 2>&1") == 0;
    }
};

// ============================================================================
// D3：目标没进计划必须是错误，不能报"所有包都已安装"
// ============================================================================

TEST_F(SolverRegressionTest, UnreachableTargetIsAnErrorNotSilentSuccess)
{
    // 顶层请求一个"看起来像 SONAME"、仓库里却没有的名字，并开启 bootstrap 容忍开关：
    // 旧实现把它归入可容忍的 missing_so → 求解"成功"但事务为空 → 打印
    // "所有请求的包都已安装" 并 exit 0（什么都没装）。现在必须显式报错。
    Config::instance().set_missing_so_no_error_mode(true);
    EXPECT_THROW(install_packages({"libghost.so.1"}), LpkgException);
}

TEST_F(SolverRegressionTest, AlreadyInstalledTargetStillReportsUpToDate)
{
    // 正向对照：目标确实已装时，必须仍然走"所有包都已安装"（不能误报错误）
    create_pkg("wa", "1.0");
    add_to_mirror("wa", "1.0");
    update_index({{"wa", "1.0", "", "", ""}});

    install_packages({"wa"});
    ASSERT_TRUE(Cache::instance().is_installed("wa"));
    EXPECT_NO_THROW(install_packages({"wa"}));  // 重复安装：什么都不做，但不是错误
}

// ============================================================================
// E1：--force 在混合目标下也必须重装"已是最新版本"的包
// ============================================================================

TEST_F(SolverRegressionTest, ForceReinstallMixedTargetsReinstallsCurrentPackage)
{
    create_pkg("wa", "1.0");
    create_pkg("wb", "1.0");
    add_to_mirror("wa", "1.0");
    add_to_mirror("wb", "1.0");
    update_index({{"wa", "1.0", "", "", ""}, {"wb", "1.0", "", "", ""}});

    install_packages({"wa"});
    ASSERT_TRUE(Cache::instance().is_installed("wa"));

    // 篡改已装文件：只有"真的重装过"才会被覆盖回包内容
    const fs::path wa_bin = test_root / "usr/bin/wa";
    std::ofstream(wa_bin) << "TAMPERED\n";
    ASSERT_EQ(read_file(wa_bin), "TAMPERED\n");

    install_packages({"wa", "wb"}, "", /*force_reinstall=*/true);

    EXPECT_TRUE(Cache::instance().is_installed("wb"));
    EXPECT_EQ(read_file(wa_bin), "#!/bin/sh\necho wa\n")
        << "混合目标下 --force 静默漏掉了已是最新版本的 wa";
}

// ============================================================================
// E2：按能力/SONAME 安装 = 用户显式请求（要 hold，否则被 autoremove 删）
// ============================================================================

TEST_F(SolverRegressionTest, CapabilityTargetIsExplicitAndSurvivesAutoremove)
{
    create_pkg("prov", "1.0", {}, {"libcap.so.1"});
    add_to_mirror("prov", "1.0");
    update_index({{"prov", "1.0", "", "libcap.so.1", ""}});

    install_packages({"libcap.so.1"});  // 按能力名安装
    ASSERT_TRUE(Cache::instance().is_installed("prov")) << "能力目标没有解析到提供者";

    EXPECT_TRUE(Cache::instance().is_held("prov"))
        << "按能力安装被记成依赖（不 hold）→ autoremove 会删掉用户刚装的包";

    autoremove();  // 端到端复核
    EXPECT_TRUE(Cache::instance().is_installed("prov")) << "autoremove 删掉了显式安装的包";
}

// ============================================================================
// E3：autoremove 永不删核心包
// ============================================================================

TEST_F(SolverRegressionTest, AutoremoveNeverRemovesEssentialPackages)
{
    create_pkg("corelib", "1.0");
    create_pkg("app", "1.0", {"corelib"});
    add_to_mirror("corelib", "1.0");
    add_to_mirror("app", "1.0");
    update_index({{"corelib", "1.0", "", "", ""}, {"app", "1.0", "corelib", "", ""}});

    install_packages({"app"});  // corelib 作为依赖被拉入 → 不 hold
    ASSERT_TRUE(Cache::instance().is_installed("corelib"));
    ASSERT_FALSE(Cache::instance().is_held("corelib"));

    {
        std::ofstream(Config::instance().essential_file()) << "corelib\n";
    }
    Cache::instance().load();  // 重新读 essential + 已装列表

    remove_package("app", false);
    autoremove();  // 旧实现走 remove_package(force=true)，绕过 is_essential

    EXPECT_TRUE(Cache::instance().is_installed("corelib")) << "autoremove 删掉了核心包";
}

// ============================================================================
// F1 + F2：触发器接线（安装与升级都要 flush；ldconfig 分支要真的建链接）
// ============================================================================

TEST_F(SolverRegressionTest, UpgradeFlushesTriggersAndRegeneratesSonameLinks)
{
    if (!gcc_available()) GTEST_SKIP() << "gcc 不可用，跳过";
    if (!create_lib_pkg("libtrig", "1.0")) GTEST_SKIP() << "无法编译测试用共享库";

    add_to_mirror("libtrig", "1.0");
    update_index({{"libtrig", "1.0", "", "", ""}});
    install_packages({"libtrig"});

    const fs::path link = test_root / "usr/lib/libtrig.so.1";
    ASSERT_TRUE(fs::exists(link))
        << "安装后没有生成 SONAME 链接：ldconfig 触发器空转（libelf 未 elf_version？）";

    fs::remove(link);  // 清掉，用来观察升级是否重新生成

    // 直击接线本身：显式登记一条待执行触发器（不依赖 check_file 的累积路径），
    // 升级若 flush，它就会被执行 → SONAME 链接重建
    TriggerManager::instance().add("ldconfig");

    ASSERT_TRUE(create_lib_pkg("libtrig", "2.0"));
    add_to_mirror("libtrig", "2.0");
    update_index({{"libtrig", "2.0", "", "", ""}});
    upgrade_packages();

    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("libtrig"), "2.0") << "升级没有落地新版本";
    EXPECT_TRUE(fs::exists(test_root / "usr/lib/libtrig.so.2.0")) << "新版本文件不存在";
    EXPECT_TRUE(fs::exists(link))
        << "升级没有 flush 触发器（ldconfig/glib-compile-schemas/daemon-reload 全都不跑）";
}

TEST_F(SolverRegressionTest, GetElfSonameReadsRealSharedObject)
{
    // F2 的直接断言：不依赖"本进程先 strip 过"也能读出 SONAME
    if (!gcc_available()) GTEST_SKIP() << "gcc 不可用，跳过";
    ASSERT_TRUE(create_lib_pkg("libsoname", "1.0"));
    const fs::path so = test_root / "nope.so";  // 尚未安装，先解包目录里的原件更直接：
    fs::path built;
    for (const auto& e : fs::recursive_directory_iterator(suite_work_dir / "_lib_libsoname_1.0"))
        if (e.path().extension() == ".0") built = e.path();
    ASSERT_FALSE(built.empty());
    EXPECT_EQ(get_elf_soname(built), "libsoname.so.1");
    (void)so;
}

TEST_F(SolverRegressionTest, SonameEscapingLibDirIsNotLinked)
{
    // X3 回归：SONAME 取自被扫描的库文件（不可信）。绝对路径的 SONAME 曾让
    // `lib_dir / soname` 逃出 lib_dir，以 root 在任意位置建符号链接（如
    // /etc/ld.so.preload → 任意 .so，等于给下次进程启动注入代码）。
    if (!gcc_available()) GTEST_SKIP() << "gcc 不可用";
    const fs::path libdir = test_root / "usr/lib/sonametest";
    fs::create_directories(libdir);
    const fs::path src = suite_work_dir / "soname_probe.c";
    std::ofstream(src) << "int probe(void){return 1;}\n";
    const fs::path evil_target = suite_work_dir / "EVIL.so";  // 在 lib_dir 之外
    const fs::path evil_so = libdir / "libevil.so.1.0";
    const std::string cmd = "gcc -shared -fPIC -Wl,-soname," + evil_target.string() + " -o " +
                            evil_so.string() + " " + src.string() + " 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) GTEST_SKIP() << "无法编译测试用共享库";

    apply_soname_links(libdir);
    EXPECT_FALSE(fs::exists(evil_target))
        << "SONAME 逃出 lib_dir 并在外面建了符号链接：" << evil_target;

    // 正向对照：普通 SONAME 仍必须在 lib_dir 内建链接
    const fs::path good_so = libdir / "libgood.so.1.0";
    const std::string cmd2 = "gcc -shared -fPIC -Wl,-soname,libgood.so.1 -o " + good_so.string() +
                             " " + src.string() + " 2>/dev/null";
    ASSERT_EQ(std::system(cmd2.c_str()), 0);
    apply_soname_links(libdir);
    EXPECT_TRUE(fs::is_symlink(libdir / "libgood.so.1")) << "合法 SONAME 链接没建";
}

// ============================================================================
// X4：包名/版本号来自不可信元数据，绝不能当路径分量用
// ============================================================================

TEST_F(SolverRegressionTest, TraversingPackageNameIsRejected)
{
    // 本地 .lpkg 的 metadata.json 是攻击者可控的：name = "../../../<x>" 曾被直接当成
    // 路径分量（tmp_pkg_dir()、dep_dir()/name、docs_dir()/(name+".man")、hooks_dir()/name）
    // → 以 root 写到这些目录之外。
    const fs::path work = suite_work_dir / "_evil_name";
    fs::create_directories(work / "content/usr/bin");
    std::ofstream(work / "content/usr/bin/evil") << "x";
    const std::string evil = (pkg_dir / "evil-name.lpkg").string();
    pack_package(evil, work.string(), "../../../evilpkg", "1.0", {}, {}, "", {});

    EXPECT_THROW(install_packages({evil}), LpkgException);
    // 逃逸落点必须没有被创建（dep_dir() 是 <root>/var/lib/lpkg/deps → 上一级是 <root>/var/lib）
    EXPECT_FALSE(fs::exists(test_root / "var/lib/evilpkg")) << "包名穿越写出了状态目录之外的东西";
    EXPECT_FALSE(fs::exists(test_root / "var/lib/lpkg/deps/../../../evilpkg"));
}

TEST_F(SolverRegressionTest, TraversingVersionFromRepoIndexIsRejected)
{
    // 版本号会被拼进下载落点与 `tmp_pkg_dir_ / (版本 + ".lpkg")`。
    // 注意 CLI 这条向量是关着的：含 '/' 的参数会被当成"本地包路径"（install_packages
    // 的解析分支），根本走不到版本校验。真实向量是**远端索引**里的版本号。
    create_pkg("wa", "1.0");
    add_to_mirror("wa", "1.0");
    // 索引里把版本写成穿越串（模拟被污染的镜像索引）
    std::ofstream index(mirror_dir / "index.txt");
    index << "wa|../../../../tmp/evilver:deadbeef::|\n";
    index.close();

    EXPECT_THROW(install_packages({"wa"}), LpkgException);
    EXPECT_FALSE(fs::exists("/tmp/evilver.lpkg")) << "版本穿越写出了临时目录之外";
}

// ============================================================================
// 多包移除的跨包原子性（用户实测：autoremove 中途 Ctrl+C 只留下"删了几个包"的状态）
//
// 旧实现：`remove a b c`（main.cpp 逐包调用）与 autoremove（逐包 remove_package）都是
// **每包一个批次** → 中途中断只回滚当前包那个批次，之前已提交的删除保持删除。
// 修法：多包路径统一走 remove_packages_checked()（一个批次，全部删完才 CLEANUP）。
// ============================================================================

TEST_F(SolverRegressionTest, MultiPackageRemoveRollsBackAllWhenInterrupted)
{
    create_pkg("ma", "1.0");
    create_pkg("mb", "1.0");
    add_to_mirror("ma", "1.0");
    add_to_mirror("mb", "1.0");
    update_index({{"ma", "1.0", "", "", ""}, {"mb", "1.0", "", "", ""}});
    install_packages({"ma", "mb"});
    ASSERT_TRUE(Cache::instance().is_installed("ma"));

    // ma 已删完、mb 尚未开始 → 模拟 Ctrl+C（循环下一次迭代会看到 gracefully 标志并抛出）
    BreakpointManager::instance().set("remove_after_package_ma",
                                      [] { sigint_graceful.store(true); });
    EXPECT_THROW(remove_packages({"ma", "mb"}, false), LpkgException);
    BreakpointManager::instance().clear_all();
    sigint_graceful.store(false);

    // 整批回滚：两个包都必须回来（修复前 ma 已在自己的批次里提交 → 只剩 mb 被回滚）
    EXPECT_TRUE(Cache::instance().is_installed("ma")) << "多包移除中途中断没有整批回滚（ma 丢了）";
    EXPECT_TRUE(Cache::instance().is_installed("mb"));
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/ma"));
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/mb"));
}

// ============================================================================
// `remove a b c` 的**全或无**：列表里任一包被安全检查拒绝 → 一个包都不删
//
// 旧实现：逐个跑 removal_allowed()，被拒的那几个 `continue` 掉、**其余照删**，删完才因
// refused 抛错 —— 落点是"部分包已删 + 非零退出码"。而非零退出码对脚本/farm 的含义是
// "什么都没发生"（TODO G4 就是为这个语义加的），盘面却已经少了几个包。
// pacman 的 `-R a b` 是整个列表先检完，任一不通过即中止、一个都不删。
// ============================================================================

TEST_F(SolverRegressionTest, MultiPackageRemoveRefusedRemovesNothing)
{
    create_pkg("good", "1.0");
    create_pkg("needed", "1.0");
    create_pkg("dependent", "1.0", {"needed"});
    add_to_mirror("good", "1.0");
    add_to_mirror("needed", "1.0");
    add_to_mirror("dependent", "1.0");
    update_index({{"good", "1.0", "", "", ""},
                  {"needed", "1.0", "", "", ""},
                  {"dependent", "1.0", "needed", "", ""}});
    install_packages({"good", "dependent"});  // dependent 把 needed 作为依赖拉进来
    ASSERT_TRUE(Cache::instance().is_installed("good"));
    ASSERT_TRUE(Cache::instance().is_installed("needed"));
    ASSERT_FALSE(Cache::instance().get_reverse_deps("needed").empty())
        << "fixture 自检：needed 必须真的被 dependent 依赖，否则下面的'拒绝'退化成恒真";

    const std::string wal_before = read_file(wal::wal_log_path());

    // 判别力在**批次内**：`rm_before_file_removal_<pkg>` 写在 do_remove_package 内部
    // （good 的文件已搬进 stash、即将真删那一刻），命中 ⟺ good 真的被删过。拒绝必须发生在
    // 任何文件操作之前 → 这条断点必然不命中；旧实现（检查在批次内逐包做）里 good 会走完
    // 整条删除路径 → 它必然命中。
    bool good_removal_began = false;
    BreakpointManager::instance().set("rm_before_file_removal_good",
                                      [&] { good_removal_began = true; });

    // needed 被 dependent 依赖 → 拒绝；good 排在列表**前面**，旧实现会先把它删掉再报错
    std::string msg;
    try {
        remove_packages({"good", "needed"}, /*force=*/false);
        FAIL() << "needed 被 dependent 依赖，整批移除必须被拒绝";
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    BreakpointManager::instance().clear_all();

    // 拒绝的理由：CLI 边界（remove_packages）在"被安全检查拒绝"时抛这条（TODO G4）
    EXPECT_NE(msg.find(get_string("error.removal_refused")), std::string::npos)
        << "拒绝信息不是'移除被拒'：" << msg;
    EXPECT_FALSE(good_removal_began)
        << "good 的删除已经开始（批次内断点命中）—— 拒绝没有前移到任何文件操作之前";
    // 全或无：被拒绝时不得动任何包
    EXPECT_TRUE(Cache::instance().is_installed("good")) << "被拒绝的移除仍把 good 从 DB 里删掉了";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/good")) << "被拒绝的移除仍删掉了 good 的文件";
    // ⚠ **本断言不具判别力**（注释此前把因果写反了）：`run_batch_transaction` 的异常路径
    // 回滚成功后同样会 `trim_completed()` —— 它把**已提交且无残留 bak**的整个 WAL 清空
    // （而进批次前那次 trim 与成功批次收尾的 trim 也会清）。于是"预检/入口拒绝"与旧实现
    // 的"批次内拒绝 → 回滚"两条路径终点都是空 WAL：`RM_BEGIN` 在两边都不存在。
    // 它钉得住的只是"拒绝后没有留下未提交批次"，判别力来自上面那条
    // `rm_before_file_removal_good` 断点（+ 拒绝信息点名理由）。
    const std::string wal_after = read_file(wal::wal_log_path());
    EXPECT_EQ(wal_after.find("RM_BEGIN"), std::string::npos)
        << "被拒绝的移除留下了事务痕迹（WAL 里出现 RM_BEGIN）：拒绝必须前移到任何文件操作"
           "之前，且事后不得残留未提交批次。WAL 变化：\n"
        << wal_before << "\n----→\n"
        << wal_after;

    // ── 正向对照：同一个断点名字必须是活的（否则上面那条"没命中"只是名字打错） ──
    // 真删一次（force 绕过反依赖检查）→ `rm_before_file_removal_good` 必须命中。
    BreakpointManager::instance().set("rm_before_file_removal_good",
                                      [&] { good_removal_began = true; });
    ASSERT_NO_THROW(remove_packages({"good"}, /*force=*/true)) << "force 移除应当成功";
    BreakpointManager::instance().clear_all();
    EXPECT_TRUE(good_removal_began)
        << "正向对照：真的删 good 时该断点必须命中（名字写错的话上面的'没命中'是恒真的）";
    EXPECT_FALSE(Cache::instance().is_installed("good"));
}

// ============================================================================
// `reinstall a b` / `remove -r a b` 也必须**整组一个批次**
//
// 旧实现：main.cpp 里这两个命令**逐参数各调一次**库函数，等于每参数一个批次、跨参数
// 不原子 —— 后面那个失败时前面那个已经装完/删完并提交，而退出码非零又让脚本/farm
// 以为"什么都没发生"。与 `remove a b c` 曾经踩的是同一个坑（那条已修），install/remove
// 也早已是整批，只有这两条漏了。
// ============================================================================

TEST_F(SolverRegressionTest, ReinstallGroupIsOneBatch)
{
    create_pkg("rna", "1.0");
    create_pkg("rnb", "1.0");
    add_to_mirror("rna", "1.0");
    add_to_mirror("rnb", "1.0");
    update_index({{"rna", "1.0", "", "", ""}, {"rnb", "1.0", "", "", ""}});
    install_packages({"rna", "rnb"});

    // 模拟 rna 装坏了（文件丢失）：重装的目的正是把它补回来。旧实现下"补回来"这个
    // 副作用会在 rnb 失败**之前**随 rna 自己的批次提交，因此这条断言能区分两种实现。
    const fs::path rna_bin = test_root / "usr/bin/rna";
    ASSERT_TRUE(fs::exists(rna_bin));
    fs::remove(rna_bin);

    // ── 顺序必须**钉住**：rna 先于 rnb ─────────────────────────────────────────
    // 本用例的全部判别力都建立在"rna 已经跑过、rnb 才失败"之上。顺序反过来（rnb 先）
    // 时，"rnb 失败前 rna 已经提交"这件事**根本没发生过** —— 下面那条 EXPECT_FALSE
    // 在旧实现下也会通过，判别力静默消失。所以顺序不能只靠"碰巧这么排"。
    //
    // 这两个成员都已是最新版本，libsolv 不为它们产生事务步骤，顺序由 solver 的
    // `force_reinstall` 补回兜底给出：它按 **targets（CLI 参数序）** 逐个补
    // （solver.cpp 的 force_reinstall 段），所以 rna 在 rnb 之前来自**参数序**。
    // 断言它即可把顺序钉死：一旦求解器的排法改回拓扑序、或有人调换了传参，
    // 这里**响亮地**失败，而不是悄悄失去判别力。
    std::vector<std::string> order_log;
    BreakpointManager::instance().set("install_after_begin_rna",
                                      [&] { order_log.push_back("rna"); });
    // 第二个成员在事务中途失败（BEGIN 之后、任何文件操作之前）
    BreakpointManager::instance().set("install_after_begin_rnb", [&] {
        order_log.push_back("rnb");
        throw LpkgException("injected failure: 重装批次中途失败");
    });
    EXPECT_THROW(reinstall_packages({"rna", "rnb"}), LpkgException);
    BreakpointManager::instance().clear_all();
    Cache::instance().load();

    EXPECT_EQ(order_log, (std::vector<std::string>{"rna", "rnb"}))
        << "批次内处理顺序不是 rna → rnb：顺序反过来时下面那条断言恒真（rna 压根没轮到），"
           "本用例失去判别力";
    EXPECT_FALSE(fs::exists(rna_bin))
        << "重装是逐参数各一批：rnb 失败前 rna 已经提交，'整组一个批次'不成立";
    EXPECT_TRUE(Cache::instance().is_installed("rna")) << "批次回滚后 rna 应仍是已安装";
    EXPECT_TRUE(Cache::instance().is_installed("rnb"));
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/rnb"));
}

TEST_F(SolverRegressionTest, RecursiveRemoveGroupIsOneBatch)
{
    // ── 顺序必须**钉住**：rra 先于 rrb ─────────────────────────────────────────
    // rra **依赖** rrb → 反向依赖数 rra=0、rrb=1，而移除序按"反向依赖数升序（叶子先删）"
    // 排（`remove_packages_recursive`）。于是 rra 必定排在 rrb 之前。
    //
    // 为什么非要依赖不可：本用例的判别力全在"rrb 失败前 rra 已经删完并提交"上。无依赖
    // 时两者的反向依赖数都是 0，顺序只剩 `std::ranges::sort`（**不稳定**排序）对等键的
    // 实现细节 —— 一旦反过来，下面那条断言在旧实现下也会通过，判别力静默消失。
    create_pkg("rra", "1.0", {"rrb"});
    create_pkg("rrb", "1.0");
    add_to_mirror("rra", "1.0");
    add_to_mirror("rrb", "1.0");
    update_index({{"rra", "1.0", "rrb", "", ""}, {"rrb", "1.0", "", "", ""}});
    install_packages({"rra", "rrb"});
    ASSERT_TRUE(fs::exists(test_root / "usr/bin/rra"));
    ASSERT_TRUE(fs::exists(test_root / "usr/bin/rrb"));

    // 断点按**每个文件**的 BACKUP 命中，同一个包会产生多条 → 只记首次出现的转移
    std::vector<std::string> order_log;
    auto mark = [&](const char* who) {
        if (order_log.empty() || order_log.back() != who) order_log.push_back(who);
    };
    BreakpointManager::instance().set("rm_backup_after_wal_rra", [&] { mark("rra"); });
    // 第二个成员在删除中途失败（BACKUP 的 write-ahead 窗口里注入）
    BreakpointManager::instance().set("rm_backup_after_wal_rrb", [&] {
        mark("rrb");
        throw LpkgException("injected failure: 递归移除批次中途失败");
    });
    EXPECT_THROW(remove_packages_recursive({"rra", "rrb"}), LpkgException);
    BreakpointManager::instance().clear_all();
    Cache::instance().load();

    EXPECT_EQ(order_log, (std::vector<std::string>{"rra", "rrb"}))
        << "批次内处理顺序不是 rra → rrb：顺序反过来时下面那条断言恒真（rra 压根没轮到），"
           "本用例失去判别力";
    EXPECT_TRUE(Cache::instance().is_installed("rra"))
        << "`remove -r a b` 逐参数各一批：rrb 失败前 rra 已经删完并提交";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/rra")) << "rra 的文件必须被整批回滚还原";
    EXPECT_TRUE(Cache::instance().is_installed("rrb"));
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/rrb"));
}

TEST_F(SolverRegressionTest, AutoremoveRollsBackAllPackagesWhenInterrupted)
{
    // d1/d2 作为 app 的**依赖**被拉入（非 hold）→ 删掉 app 后它们成为 autoremove 候选
    create_pkg("d1", "1.0");
    create_pkg("d2", "1.0");
    create_pkg("app", "1.0", {"d1", "d2"});
    add_to_mirror("d1", "1.0");
    add_to_mirror("d2", "1.0");
    add_to_mirror("app", "1.0");
    update_index(
        {{"d1", "1.0", "", "", ""}, {"d2", "1.0", "", "", ""}, {"app", "1.0", "d1,d2", "", ""}});

    install_packages({"app"});
    ASSERT_TRUE(Cache::instance().is_installed("d1"));
    ASSERT_FALSE(Cache::instance().is_held("d1"));  // 是依赖而非显式目标
    remove_package("app", false);
    ASSERT_TRUE(Cache::instance().is_installed("d1"));

    // 第一个候选包删完后中断 → autoremove 必须整批回滚（旧实现逐包各一批，d1 会永久丢失）
    BreakpointManager::instance().set("remove_after_package_d1",
                                      [] { sigint_graceful.store(true); });
    autoremove();  // 内部 catch 异常并告警
    BreakpointManager::instance().clear_all();
    sigint_graceful.store(false);

    EXPECT_TRUE(Cache::instance().is_installed("d1")) << "autoremove 中途中断后 d1 没被恢复";
    EXPECT_TRUE(Cache::instance().is_installed("d2")) << "autoremove 中途中断后 d2 没被恢复";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/d1"));
}

TEST_F(SolverRegressionTest, UpgradeFromFileToDirectorySucceeds)
{
    // E4：路径从**文件**变成**目录**（python 包 foo.py → foo/__init__.py 是真实场景）。
    // 此前 backup_existing_files 的目录分支只判 exists → 跳过（既不备份也不删除），
    // 随后 copy_package_files 的 ensure_dir_exists 抛 error.path_not_dir → **整批中止**，
    // 用户完全无法升级。现在按文件冲突处理：搬进 stash（可回滚）+ 删掉，让目录得以创建。
    const std::string v1 = create_layout_pkg("fd", "1.0", {"usr/share/foo"});
    install_packages({v1});
    ASSERT_TRUE(fs::is_regular_file(test_root / "usr/share/foo"));

    create_layout_pkg("fd", "2.0", {"usr/share/foo/", "usr/share/foo/inner.txt"});
    add_to_mirror("fd", "2.0");
    update_index({{"fd", "2.0", "", "", ""}});

    try {
        upgrade_packages();
    } catch (const std::exception& e) {
        FAIL() << "文件→目录的升级不该整批中止: " << e.what();
    }
    EXPECT_TRUE(fs::is_directory(test_root / "usr/share/foo"));
    EXPECT_TRUE(fs::exists(test_root / "usr/share/foo/inner.txt"));
}

TEST_F(SolverRegressionTest, UpgradePrunesHooksDroppedByNewVersion)
{
    // v1 带 postinst.sh + prerm.sh，v2 只带 postinst.sh → 升级后 prerm.sh 必须消失。
    // 陈旧的 prerm.sh 会被后续 remove/upgrade 继续执行（静默跑旧版本逻辑），
    // 而剪枝必须发生在**提交后**：批次回滚时旧 hook 要完好（与 Z7 同一理由）。
    const std::string v1 = create_pkg_with_hooks("hkpkg", "1.0", {"postinst.sh", "prerm.sh"});
    install_packages({v1});

    const fs::path hd = Config::instance().hooks_dir() / "hkpkg";
    ASSERT_TRUE(fs::exists(hd / "postinst.sh"));
    ASSERT_TRUE(fs::exists(hd / "prerm.sh")) << "v1 的两个 hook 都应被装上";

    create_pkg_with_hooks("hkpkg", "2.0", {"postinst.sh"});
    add_to_mirror("hkpkg", "2.0");
    update_index({{"hkpkg", "2.0", "", "", ""}});
    upgrade_packages();

    EXPECT_TRUE(fs::exists(hd / "postinst.sh")) << "新版本仍提供的 hook 不该被删";
    EXPECT_FALSE(fs::exists(hd / "prerm.sh"))
        << "新版本已删掉的 hook 仍在磁盘上，会被后续 remove/upgrade 执行";
}

TEST_F(SolverRegressionTest, RemoveInterruptedBeforeCommitRollsBackEverything)
{
    // 用户要求的核心语义：**只要批次未提交，中途中断一律整批恢复**。
    // 旧设计里 remove 的 cleanup 在批次内 → "全部包都删完、尚未提交"这个窗口一旦写了
    // CLEANUP 就不可回滚（Ctrl+C 后包保持已删）。新设计 cleanup 挪到提交后，
    // 这个窗口的中断必须把两个包都恢复。
    create_pkg("ma", "1.0");
    create_pkg("mb", "1.0");
    add_to_mirror("ma", "1.0");
    add_to_mirror("mb", "1.0");
    update_index({{"ma", "1.0", "", "", ""}, {"mb", "1.0", "", "", ""}});
    install_packages({"ma", "mb"});
    ASSERT_TRUE(Cache::instance().is_installed("ma"));

    BreakpointManager::instance().set("remove_batch_before_commit", [] {
        throw LpkgException("interrupt after all removals, before COMMIT_PKGS");
    });
    EXPECT_THROW(remove_packages({"ma", "mb"}, false), LpkgException);
    BreakpointManager::instance().clear_all();

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("ma")) << "提交前中断没有整批回滚（ma 丢了）";
    EXPECT_TRUE(Cache::instance().is_installed("mb")) << "提交前中断没有整批回滚（mb 丢了）";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/ma"));
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/mb"));
}
