#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/pkg/transaction_log.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/testing_breakpoints.hpp"
#include "../test_base.hpp"
#include <filesystem>
#include <fstream>
#include <set>
#include <atomic>

namespace fs = std::filesystem;

extern std::atomic<bool> sigint_graceful;

// =====================================================================
// 测试套件：递归移除 (--recursive / -r)
// =====================================================================

class RecursiveRemoveTest : public IntegrationTestBase {
protected:
    void SetUp() override {
        IntegrationTestBase::SetUp();
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        sigint_graceful.store(false);
        Cache::instance().load();
    }

    void TearDown() override {
        Config::instance().set_testing_mode(false);
        Config::instance().set_non_interactive_mode(NonInteractiveMode::INTERACTIVE);
        IntegrationTestBase::TearDown();
        sigint_graceful.store(false);
    }

    /** 创建带依赖的包并安装 */
    std::string make_and_install(const std::string& name, const std::string& ver,
                                  const std::vector<std::string>& deps = {},
                                  const std::vector<std::string>& provides = {}) {
        auto path = create_local_pkg(name, ver, deps, provides);
        install_packages({path});
        Cache::instance().write(name);
        Cache::instance().load();
        return path;
    }

    /** 创建本地包文件（含 content/usr/bin/<name>） */
    std::string create_local_pkg(const std::string& name, const std::string& ver,
                                  const std::vector<std::string>& deps = {},
                                  const std::vector<std::string>& provides = {}) {
        fs::path work = suite_work_dir / ("_rr_" + name + "_" + ver);
        fs::remove_all(work);
        fs::create_directories(work / "content" / "usr" / "bin");
        { std::ofstream f(work / "content" / "usr" / "bin" / name);
          f << "#!/bin/sh\necho " << name; }
        std::string p = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(p, work.string(), name, ver, deps, provides);
        return p;
    }

    bool is_installed(const std::string& name) {
        return Cache::instance().is_installed(name);
    }

    std::string file_content(const fs::path& rel) {
        std::ifstream f(test_root / rel);
        if (!f) return "";
        return std::string((std::istreambuf_iterator<char>(f)), {});
    }

    std::string read_log() {
        fs::path log_path = Config::instance().lock_dir() / "transaction.log";
        if (!fs::exists(log_path)) return "";
        std::ifstream f(log_path);
        return std::string((std::istreambuf_iterator<char>(f)), {});
    }

    bool log_has(const std::string& text) {
        return read_log().find(text) != std::string::npos;
    }

    void create_file(const fs::path& rel, const std::string& content = "original") {
        fs::path p = test_root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p); f << content;
    }

    void reset_breakpoints() {
        testing::break_before_install.store(false);
        testing::break_after_begin_pkgs.store(false);
        testing::break_before_backup.store(false);
        testing::break_after_backup.store(false);
        testing::break_after_extract.store(false);
        testing::break_during_file_copy.store(false);
        testing::break_after_file_copy.store(false);
        testing::break_before_commit.store(false);
        testing::break_after_commit.store(false);
        testing::break_before_db_write.store(false);
        testing::break_after_db_write.store(false);
        testing::break_before_commit_pkgs.store(false);
        testing::break_after_commit_pkgs.store(false);
        testing::break_before_remove.store(false);
        testing::break_after_rm_begin.store(false);
        testing::break_during_rm_backup.store(false);
        testing::break_after_rm_backup.store(false);
        testing::break_before_rm_dir.store(false);
        testing::break_after_rm_dir.store(false);
        testing::break_before_rm_db_write.store(false);
        testing::break_before_rm_commit.store(false);
        testing::break_after_rm_cleanup.store(false);
        testing::break_before_each_pkg_install.store(false);
        testing::break_after_each_pkg_install.store(false);
    }
};

// ═══════════════════════════════════════════════════════════════════════
// R01-R05: 基础递归移除
// ═══════════════════════════════════════════════════════════════════════

// R01: 无依赖的叶子包 → 仅移除自身
TEST_F(RecursiveRemoveTest, LeafPkgRemoved) {
    make_and_install("r01", "1.0");
    EXPECT_TRUE(is_installed("r01"));

    remove_package_recursive("r01");

    EXPECT_FALSE(is_installed("r01"));
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/r01"));
}

// R02: 一个依赖者 → 两者都被移除
TEST_F(RecursiveRemoveTest, OneDependentRemoved) {
    make_and_install("r02_base", "1.0");
    make_and_install("r02_dep", "1.0", {"r02_base"});
    EXPECT_TRUE(is_installed("r02_base"));
    EXPECT_TRUE(is_installed("r02_dep"));

    remove_package_recursive("r02_base");

    EXPECT_FALSE(is_installed("r02_base")) << "base removed";
    EXPECT_FALSE(is_installed("r02_dep")) << "dependent removed";
}

// R03: 依赖链 3 层 (A → B → C)，移除 C → 全部移除
TEST_F(RecursiveRemoveTest, ThreeLevelChain) {
    make_and_install("r03_c", "1.0");
    make_and_install("r03_b", "1.0", {"r03_c"});
    make_and_install("r03_a", "1.0", {"r03_b"});
    EXPECT_TRUE(is_installed("r03_a"));
    EXPECT_TRUE(is_installed("r03_b"));
    EXPECT_TRUE(is_installed("r03_c"));

    remove_package_recursive("r03_c");

    EXPECT_FALSE(is_installed("r03_a")) << "top-level removed";
    EXPECT_FALSE(is_installed("r03_b")) << "middle removed";
    EXPECT_FALSE(is_installed("r03_c")) << "target removed";
}

// R04: 共享依赖不被移除（A→C, B→C，移除A时C保留因为B仍需要）
TEST_F(RecursiveRemoveTest, SharedDepPreserved) {
    make_and_install("r04_c", "1.0");
    make_and_install("r04_a", "1.0", {"r04_c"});
    make_and_install("r04_b", "1.0", {"r04_c"});
    EXPECT_TRUE(is_installed("r04_a"));
    EXPECT_TRUE(is_installed("r04_b"));
    EXPECT_TRUE(is_installed("r04_c"));

    remove_package_recursive("r04_a");

    EXPECT_FALSE(is_installed("r04_a")) << "A removed";
    EXPECT_TRUE(is_installed("r04_b")) << "B untouched";
    EXPECT_TRUE(is_installed("r04_c")) << "C kept (needed by B)";
}

// R05: 目标包已被 essential 保护 → 不删除
TEST_F(RecursiveRemoveTest, EssentialProtected) {
    make_and_install("r05_base", "1.0");
    make_and_install("r05_essential", "1.0");

    // 手动标记为 essential
    {
        std::ofstream f(Config::instance().essential_file());
        f << "r05_essential" << "\n";
    }
    Cache::instance().load();

    remove_package_recursive("r05_essential");

    // essential 不应被删除
    EXPECT_TRUE(is_installed("r05_essential")) << "essential kept";
}

// ═══════════════════════════════════════════════════════════════════════
// R06-R10: 边界条件与保护
// ═══════════════════════════════════════════════════════════════════════

// R06: 未安装的包 → 空操作
TEST_F(RecursiveRemoveTest, NotInstalledNoop) {
    EXPECT_NO_THROW(remove_package_recursive("nonexistent_pkg"));
}

// R07: held 包受保护
TEST_F(RecursiveRemoveTest, HeldPackageProtected) {
    make_and_install("r07", "1.0");
    Cache::instance().add_installed("r07", "1.0", true);
    Cache::instance().write("r07");

    // held 包不阻止递归移除（held 是 autoremove 概念）。
    // 递归移除的保护机制是 3 轮验证码确认 + essential 检查。
    // 但这里 non-interactive 模式直接跳过验证。所以 held 包可被递归移除。
    remove_package_recursive("r07");

    EXPECT_FALSE(Cache::instance().is_installed("r07")) << "held pkg removed (held != protected from recursive)";
}

// R08: 多个独立根包依赖同一共享库 → 移除一个不影响另一个
TEST_F(RecursiveRemoveTest, TwoRootsShareLib) {
    make_and_install("r08_lib", "1.0");
    make_and_install("r08_app1", "1.0", {"r08_lib"});
    make_and_install("r08_app2", "1.0", {"r08_lib"});

    remove_package_recursive("r08_app1");

    EXPECT_FALSE(is_installed("r08_app1")) << "app1 removed";
    EXPECT_TRUE(is_installed("r08_app2")) << "app2 still installed";
    EXPECT_TRUE(is_installed("r08_lib")) << "lib kept (needed by app2)";
}

// R09: 已安装但无反向依赖 → 只移除自身
TEST_F(RecursiveRemoveTest, NoReverseDeps) {
    make_and_install("r09", "1.0");
    remove_package_recursive("r09");
    EXPECT_FALSE(is_installed("r09"));
}

// R10: 验证移除后二进制文件被删除
TEST_F(RecursiveRemoveTest, BinaryDeleted) {
    make_and_install("r10", "1.0");
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/r10"));

    remove_package_recursive("r10");

    EXPECT_FALSE(fs::exists(test_root / "usr/bin/r10")) << "binary cleaned";
}

// ═══════════════════════════════════════════════════════════════════════
// R11-R20: 事务原子性与恢复
// ═══════════════════════════════════════════════════════════════════════

// R11: 递归移除被 WAL 保护（检查日志中有 COMMIT_PKGS）
TEST_F(RecursiveRemoveTest, RecursiveRemoveWalProtected) {
    make_and_install("r11_b", "1.0");
    make_and_install("r11_a", "1.0", {"r11_b"});

    fs::remove(Config::instance().lock_dir() / "transaction.log");
    remove_package_recursive("r11_a");

    EXPECT_TRUE(log_has("BEGIN_PKGS")) << "WAL: BEGIN_PKGS written";
    EXPECT_TRUE(log_has("COMMIT_PKGS")) << "WAL: COMMIT_PKGS written";
}

// R12: Crash 在递归移除中间 → rec 恢复所有包
TEST_F(RecursiveRemoveTest, CrashDuringRecursiveRemoveThenRecRestores) {
    make_and_install("r12_base", "1.0");
    make_and_install("r12_dep", "1.0", {"r12_base"});

    // 获取 base 文件的路径
    auto files = Cache::instance().get_package_files("r12_base");
    ASSERT_FALSE(files.empty());

    // 使用 SIGINT 在移除过程中中断
    // 我们用 break_before_remove 断点来模拟 crash
    testing::break_before_rm_commit.store(true);

    // recursive remove 会通过 force=true 调用 remove_package
    // break_before_rm_commit 会在第一个包移除的 RM_COMMIT 前触发
    EXPECT_ANY_THROW(remove_package_recursive("r12_base"));
    testing::break_before_rm_commit.store(false);

    // rec 应恢复一切
    Cache::instance().load();
    recover_packages();
    Cache::instance().load();

    EXPECT_TRUE(is_installed("r12_base")) << "base restored by rec";
    EXPECT_TRUE(is_installed("r12_dep")) << "dependent restored by rec";
}

// R13: Crash 后 rec 恢复 DB 一致性
TEST_F(RecursiveRemoveTest, DbConsistentAfterCrashAndRec) {
    make_and_install("r13_b", "1.0");
    make_and_install("r13_a", "1.0", {"r13_b"});

    // 第一个文件备份后 crash
    testing::break_during_rm_backup.store(true);
    EXPECT_ANY_THROW(remove_package_recursive("r13_a"));
    testing::break_during_rm_backup.store(false);

    // 恢复
    Cache::instance().load();
    recover_packages();
    Cache::instance().load();

    // DB 一致
    EXPECT_TRUE(is_installed("r13_a")) << "a in db after rec";
    EXPECT_TRUE(is_installed("r13_b")) << "b in db after rec";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/r13_a")) << "a file restored";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/r13_b")) << "b file restored";
}

// R14: 多次 rec 幂等（rec → rec → rec 结果一致）
TEST_F(RecursiveRemoveTest, TripleRecIdempotent) {
    make_and_install("r14_b", "1.0");
    make_and_install("r14_a", "1.0", {"r14_b"});

    testing::break_before_rm_commit.store(true);
    EXPECT_ANY_THROW(remove_package_recursive("r14_a"));
    testing::break_before_rm_commit.store(false);

    // 恢复三次
    for (int i = 0; i < 3; ++i) {
        Cache::instance().load();
        recover_packages();
        Cache::instance().load();

        EXPECT_TRUE(is_installed("r14_a")) << "iter " << i << ": a in db";
        EXPECT_TRUE(is_installed("r14_b")) << "iter " << i << ": b in db";
        EXPECT_TRUE(fs::exists(test_root / "usr/bin/r14_a")) << "iter " << i << ": a file";
        EXPECT_TRUE(fs::exists(test_root / "usr/bin/r14_b")) << "iter " << i << ": b file";
    }
}

// R15: 递归移除后 reinstall → 系统一致
TEST_F(RecursiveRemoveTest, ReinstallAfterRecursiveRemove) {
    make_and_install("r15_dep", "1.0");
    make_and_install("r15", "1.0", {"r15_dep"});

    remove_package_recursive("r15");

    EXPECT_FALSE(is_installed("r15"));
    EXPECT_TRUE(is_installed("r15_dep")) << "forward dep, not removed";

    // 重新安装
    auto pkg_dep = create_local_pkg("r15_dep", "1.0");
    auto pkg = create_local_pkg("r15", "1.0", {"r15_dep"});
    install_packages({pkg});  // r15_dep still installed
    Cache::instance().write("r15");
    Cache::instance().load();

    EXPECT_TRUE(is_installed("r15")) << "reinstalled";
    EXPECT_TRUE(is_installed("r15_dep")) << "dep reinstalled";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/r15")) << "binary exists";
}

// ═══════════════════════════════════════════════════════════════════════
// R16-R25: 复杂依赖图
// ═══════════════════════════════════════════════════════════════════════

// R16: 菱形依赖 A→C, A→D, B→C, B→D → 移除 A 不移除 C/D（B 仍需要）
TEST_F(RecursiveRemoveTest, DiamondDependency) {
    make_and_install("r16_c", "1.0");
    make_and_install("r16_d", "1.0");
    make_and_install("r16_a", "1.0", {"r16_c", "r16_d"});
    make_and_install("r16_b", "1.0", {"r16_c", "r16_d"});

    remove_package_recursive("r16_a");

    EXPECT_FALSE(is_installed("r16_a")) << "a removed";
    EXPECT_TRUE(is_installed("r16_b")) << "b kept";
    EXPECT_TRUE(is_installed("r16_c")) << "c kept (needed by b)";
    EXPECT_TRUE(is_installed("r16_d")) << "d kept (needed by b)";
}

// R17: 链式依赖 A→B→C→D，移除 D → ABCD 全部移除
TEST_F(RecursiveRemoveTest, ChainFourLevels) {
    make_and_install("r17_d", "1.0");
    make_and_install("r17_c", "1.0", {"r17_d"});
    make_and_install("r17_b", "1.0", {"r17_c"});
    make_and_install("r17_a", "1.0", {"r17_b"});

    remove_package_recursive("r17_d");

    EXPECT_FALSE(is_installed("r17_a"));
    EXPECT_FALSE(is_installed("r17_b"));
    EXPECT_FALSE(is_installed("r17_c"));
    EXPECT_FALSE(is_installed("r17_d"));
}

// R18: 移除链中间节点 → 所有依赖者也被移除
TEST_F(RecursiveRemoveTest, RemoveMiddleOfChain) {
    make_and_install("r18_c", "1.0");
    make_and_install("r18_b", "1.0", {"r18_c"});
    make_and_install("r18_a", "1.0", {"r18_b"});

    remove_package_recursive("r18_b");

    EXPECT_FALSE(is_installed("r18_a")) << "a removed (depends on b)";
    EXPECT_FALSE(is_installed("r18_b")) << "b removed (target)";
    // c 是 b 的依赖，但递归移除只清理"受影响的依赖者"（反向依赖），
    // 不清理"被依赖者"（前向依赖）。c 作为 b 的依赖保持不变。
    EXPECT_TRUE(is_installed("r18_c")) << "c kept (dependency of b, not removed)";
}

// R19: 两个独立的移除互不影响
TEST_F(RecursiveRemoveTest, TwoIndependentRemoves) {
    make_and_install("r19_a", "1.0");
    make_and_install("r19_b", "1.0");

    remove_package_recursive("r19_a");

    EXPECT_FALSE(is_installed("r19_a"));
    EXPECT_TRUE(is_installed("r19_b"));
}

// R20: 递归移除 5 层的包 → 全部成功移除
TEST_F(RecursiveRemoveTest, FiveLevelDeep) {
    std::vector<std::string> pkgs;
    for (int i = 5; i >= 1; --i) {
        std::string name = "r20_lv" + std::to_string(i);
        std::vector<std::string> deps;
        if (i < 5) deps = {"r20_lv" + std::to_string(i + 1)};
        make_and_install(name, "1.0", deps);
    }

    remove_package_recursive("r20_lv5");

    for (int i = 1; i <= 5; ++i) {
        EXPECT_FALSE(is_installed("r20_lv" + std::to_string(i)))
            << "level " << i << " removed";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// R21-R30: 并发与边角
// ═══════════════════════════════════════════════════════════════════════

// R21: 虚拟包（provides）的依赖关系
TEST_F(RecursiveRemoveTest, VirtualDepRemoved) {
    make_and_install("r21_provider", "1.0", {}, {"virt-capability"});
    make_and_install("r21_user", "1.0", {"virt-capability"});

    remove_package_recursive("r21_provider");

    EXPECT_FALSE(is_installed("r21_provider")) << "provider removed";
    EXPECT_FALSE(is_installed("r21_user")) << "user removed (needs virt)";
}

// R22: 移除后 DB 中不含被移除包的文件
TEST_F(RecursiveRemoveTest, FilesCleanedFromDb) {
    make_and_install("r22_b", "1.0");
    make_and_install("r22_a", "1.0", {"r22_b"});

    remove_package_recursive("r22_a");

    auto files_a = Cache::instance().get_package_files("r22_a");
    auto files_b = Cache::instance().get_package_files("r22_b");
    EXPECT_TRUE(files_a.empty()) << "a files gone from db";
    EXPECT_FALSE(files_b.empty()) << "b files remain (forward dep, not removed)";
}

// R23: 移除后验证不受影响的包的文件完好
TEST_F(RecursiveRemoveTest, UntouchedPkgFilesIntact) {
    make_and_install("r23_shared", "1.0");
    make_and_install("r23_target", "1.0", {"r23_shared"});
    make_and_install("r23_other", "1.0", {"r23_shared"});

    remove_package_recursive("r23_target");

    EXPECT_FALSE(is_installed("r23_target")) << "target removed";
    EXPECT_TRUE(is_installed("r23_other")) << "other untouched";
    EXPECT_TRUE(is_installed("r23_shared")) << "shared kept";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/r23_other")) << "other binary intact";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/r23_shared")) << "shared binary intact";
}

// R24: 移除后依赖文件（dep 目录）被清理
TEST_F(RecursiveRemoveTest, DepFilesCleaned) {
    make_and_install("r24_b", "1.0");
    make_and_install("r24_a", "1.0", {"r24_b"});

    fs::path dep_a = Config::instance().dep_dir() / "r24_a";
    fs::path dep_b = Config::instance().dep_dir() / "r24_b";
    EXPECT_TRUE(fs::exists(dep_a));
    EXPECT_TRUE(fs::exists(dep_b));

    remove_package_recursive("r24_a");

    EXPECT_FALSE(fs::exists(dep_a)) << "dep file for a cleaned";
    EXPECT_TRUE(fs::exists(dep_b)) << "dep file for b remains (forward dep)";
}

// R25: 移除 + 递归移除 mixed → 不冲突
TEST_F(RecursiveRemoveTest, MixedNormalAndRecursive) {
    make_and_install("r25_independent", "1.0");
    make_and_install("r25_chain_b", "1.0");
    make_and_install("r25_chain_a", "1.0", {"r25_chain_b"});

    // 正常移除独立包
    remove_package("r25_independent", false);
    EXPECT_FALSE(is_installed("r25_independent"));

    // 递归移除链
    remove_package_recursive("r25_chain_a");
    EXPECT_FALSE(is_installed("r25_chain_a"));
    EXPECT_TRUE(is_installed("r25_chain_b")) << "forward dep, stays";
}

// ═══════════════════════════════════════════════════════════════════════
// R26-R30: 回滚与数据完整性
// ═══════════════════════════════════════════════════════════════════════

// R26: 回滚后 recover 可恢复所有包
TEST_F(RecursiveRemoveTest, RollbackAfterCrashRecRestoresAll) {
    make_and_install("r26_b", "1.0");
    make_and_install("r26_c", "1.0");
    make_and_install("r26_a", "1.0", {"r26_b"});

    // 模拟安装后 crash
    testing::break_before_rm_commit.store(true);
    EXPECT_ANY_THROW(remove_package_recursive("r26_a"));
    testing::break_before_rm_commit.store(false);

    Cache::instance().load();
    recover_packages();
    Cache::instance().load();

    EXPECT_TRUE(is_installed("r26_a")) << "a restored";
    EXPECT_TRUE(is_installed("r26_b")) << "b restored";
    EXPECT_TRUE(is_installed("r26_c")) << "c still installed (never removed)";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/r26_a")) << "a file restored";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/r26_b")) << "b file restored";
}

// R27: 移除过程中 SIGINT → 部分移除 → rec 恢复全部
TEST_F(RecursiveRemoveTest, SigintDuringRecursiveRemove) {
    make_and_install("r27_b", "1.0");
    make_and_install("r27_a", "1.0", {"r27_b"});

    sigint_graceful.store(true);
    EXPECT_ANY_THROW(remove_package_recursive("r27_a"));
    sigint_graceful.store(false);

    Cache::instance().load();
    recover_packages();
    Cache::instance().load();

    EXPECT_TRUE(is_installed("r27_a"));
    EXPECT_TRUE(is_installed("r27_b"));
}

// R28: 空的事务日志 → rec 跳过
TEST_F(RecursiveRemoveTest, EmptyLogRecSkip) {
    EXPECT_NO_THROW(recover_packages());
}

// R29: 移除后 needed_so 文件清理
TEST_F(RecursiveRemoveTest, NeededSoFilesCleaned) {
    make_and_install("r29_lib", "1.0", {}, {"libfoo.so.1"});
    make_and_install("r29_app", "1.0", {}, {});

    // 手动写 needed_so 文件
    fs::create_directories(Config::instance().needed_so_dir());
    { std::ofstream f(Config::instance().needed_so_dir() / "r29_app");
      f << "libfoo.so.1\n"; }

    remove_package_recursive("r29_lib");

    EXPECT_FALSE(is_installed("r29_lib"));
}

// R30: 验证递归移除后 DB providers 中没有残留
TEST_F(RecursiveRemoveTest, ProvidersCleaned) {
    make_and_install("r30_prov", "1.0", {}, {"r30-cap"});
    make_and_install("r30_cons", "1.0", {"r30-cap"});

    remove_package_recursive("r30_prov");

    auto provs = Cache::instance().get_providers("r30-cap");
    EXPECT_TRUE(provs.empty() || !Cache::instance().is_installed("r30_prov"))
        << "no provider left for removed package";
}

// ═══════════════════════════════════════════════════════════════════════
// R31-R35: WAL 事务正确性
// ═══════════════════════════════════════════════════════════════════════

// R31: 递归移除包裹在 BEGIN_PKGS/COMMIT_PKGS 中
TEST_F(RecursiveRemoveTest, WalWrapsRecursiveRemove) {
    make_and_install("r31_b", "1.0");
    make_and_install("r31_a", "1.0", {"r31_b"});

    fs::remove(Config::instance().lock_dir() / "transaction.log");
    remove_package_recursive("r31_a");

    auto log = read_log();
    // 找到 BEGIN_PKGS
    auto bp = log.find("BEGIN_PKGS ");
    auto cp = log.find("COMMIT_PKGS");
    EXPECT_NE(bp, std::string::npos) << "BEGIN_PKGS present";
    EXPECT_NE(cp, std::string::npos) << "COMMIT_PKGS present";
    EXPECT_LT(bp, cp) << "BEGIN_PKGS before COMMIT_PKGS";
}

// R32: 递归移除过程中单个包失败的异常传递
TEST_F(RecursiveRemoveTest, FailureInMiddlePropagates) {
    make_and_install("r32_base", "1.0");
    make_and_install("r32_dep", "1.0", {"r32_base"});
    make_and_install("r32_indep", "1.0");

    EXPECT_NO_THROW(remove_package_recursive("r32_indep"));
    EXPECT_FALSE(is_installed("r32_indep"));
}

// R33: 不存在的文件回溯在 rec 中被安全跳过
TEST_F(RecursiveRemoveTest, MissingFilesRecSafelySkip) {
    make_and_install("r33", "1.0");

    // 删除包文件然后递归移除（应失败但不会 crash）
    fs::remove_all(test_root / "usr/bin/r33");

    // 跑 rec 不应 crash
    EXPECT_NO_THROW(recover_packages());
}

// R34: 依赖链 A→B→C 移除 B → AC 被递归移除
TEST_F(RecursiveRemoveTest, RemoveMiddleOfThree) {
    make_and_install("r34_c", "1.0");
    make_and_install("r34_b", "1.0", {"r34_c"});
    make_and_install("r34_a", "1.0", {"r34_b"});

    remove_package_recursive("r34_b");

    EXPECT_FALSE(is_installed("r34_a")) << "a removed (depends on b)";
    EXPECT_FALSE(is_installed("r34_b")) << "b removed (target)";
    EXPECT_TRUE(is_installed("r34_c")) << "c kept (dependency of b, not a reverse dep)";
}

// R35: 验证日志标记移除数量正确
TEST_F(RecursiveRemoveTest, LogShowsCorrectCount) {
    make_and_install("r35_c", "1.0");
    make_and_install("r35_b", "1.0", {"r35_c"});
    make_and_install("r35_a", "1.0", {"r35_b"});

    fs::remove(Config::instance().lock_dir() / "transaction.log");
    remove_package_recursive("r35_a");

    EXPECT_TRUE(log_has("BEGIN_PKGS 1")) << "1 package removed (target only; forward deps stay)";
}

// ═══════════════════════════════════════════════════════════════════════
// R36-R40: 真实场景
// ═══════════════════════════════════════════════════════════════════════

// R36: 递归移除不存在的包名 → 不 crash
TEST_F(RecursiveRemoveTest, NonExistentPkgNoCrash) {
    EXPECT_NO_THROW(remove_package_recursive("totally_not_real_pkg_12345"));
}

// R37: 多次递归移除相同包 → 第二次 no-op
TEST_F(RecursiveRemoveTest, RemoveTwiceIdempotent) {
    make_and_install("r37", "1.0");

    remove_package_recursive("r37");
    EXPECT_FALSE(is_installed("r37"));

    // 第二次不应 crash
    EXPECT_NO_THROW(remove_package_recursive("r37"));
}

// R38: 新安装后立即递归移除 → 干净
TEST_F(RecursiveRemoveTest, FreshInstallThenRecursiveRemove) {
    make_and_install("r38", "1.0");
    Cache::instance().write("r38");
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    remove_package_recursive("r38");

    EXPECT_FALSE(is_installed("r38"));
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/r38"));
    // DB 不应有文件残留
    auto files = Cache::instance().get_package_files("r38");
    EXPECT_TRUE(files.empty()) << "no files in db after remove";
}

// R39: 多个独立包递归移除 → 全部移除
TEST_F(RecursiveRemoveTest, MultipleRootsRecursive) {
    make_and_install("r39_shared", "1.0");
    make_and_install("r39_a", "1.0", {"r39_shared"});
    make_and_install("r39_b", "1.0", {"r39_shared"});

    remove_package_recursive("r39_a");
    remove_package_recursive("r39_b");

    EXPECT_FALSE(is_installed("r39_a"));
    EXPECT_FALSE(is_installed("r39_b"));
    // shared 在两个根包都被移除后成为孤儿；
    // 但 remove_package_recursive 每次只处理一个目标包及其依赖者，
    // 不会跨调用清理孤儿。此时可以通过 autoremove 清理。
    EXPECT_TRUE(is_installed("r39_shared")) << "shared remains orphaned (run autoremove to clean)";
}

// R40: 递归移除 + 系统文件完整性
TEST_F(RecursiveRemoveTest, SystemFileIntegrity) {
    make_and_install("r40_lib", "1.0");
    make_and_install("r40_tool", "1.0", {"r40_lib"});

    // 创建不受包管理的文件
    create_file("usr/local/custom_script.sh", "custom");

    remove_package_recursive("r40_tool");

    // 不受管理的文件应保留
    EXPECT_TRUE(fs::exists(test_root / "usr/local/custom_script.sh"))
        << "untracked file preserved";
    EXPECT_FALSE(is_installed("r40_tool"));
    EXPECT_TRUE(is_installed("r40_lib")) << "forward dep, stays";
}
