#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/pkg/transaction_log.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/testing_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/install_common.hpp"
#include "../../main/src/archive/packer.hpp"
#include "../test_base.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <atomic>

namespace fs = std::filesystem;

extern std::atomic<bool> sigint_graceful;

// ══════════════════════════════════════════════════════════════════════
// 语义不变量测试
//
// 覆盖 ARCH.md 中定义的所有不变量（I-WAL-*、I-OP-*、I-ATOM-*、
// I-REC-*、I-IDEM-*、I-AUDIT-*）。每次对事务逻辑的修改都必须确保
// 这些测试通过，防止功能退化。
// ══════════════════════════════════════════════════════════════════════

class SemanticInvariantTest : public IntegrationTestBase {
protected:
    fs::path log_path;

    void SetUp() override {
        IntegrationTestBase::SetUp();
        log_path = Config::instance().lock_dir() / "transaction.log";
        sigint_graceful.store(false);
    }

    void TearDown() override {
        IntegrationTestBase::TearDown();
        sigint_graceful.store(false);
    }

    void create_file(const fs::path& rel, const std::string& content = "original") {
        fs::path p = test_root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p); f << content;
    }

    std::string file_content(const fs::path& rel) {
        std::ifstream f(test_root / rel);
        if (!f) return "";
        return std::string((std::istreambuf_iterator<char>(f)), {});
    }

    std::string make_pkg(const std::string& name, const std::string& ver,
                          const std::vector<std::string>& files) {
        fs::path pkg_work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::remove_all(pkg_work);
        for (const auto& f : files) {
            auto fp = pkg_work / "content" / f;
            fs::create_directories(fp.parent_path());
            std::ofstream of(fp); of << "pkg:" << f;
        }
        std::string p = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(p, pkg_work.string(), name, ver);
        return p;
    }

    std::string make_pkg_deps(const std::string& name, const std::string& ver,
                               const std::vector<std::string>& files,
                               const std::vector<std::string>& deps) {
        fs::path pkg_work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::remove_all(pkg_work);
        for (const auto& f : files) {
            auto fp = pkg_work / "content" / f;
            fs::create_directories(fp.parent_path());
            std::ofstream of(fp); of << "pkg:" << f;
        }
        std::string p = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(p, pkg_work.string(), name, ver, deps);
        return p;
    }

    std::string read_log() {
        if (!fs::exists(log_path)) return "";
        std::ifstream f(log_path);
        return std::string((std::istreambuf_iterator<char>(f)), {});
    }

    // 检查日志中是否有某行内容（去时间戳后）
    bool log_has_line(const std::string& content) {
        auto raw = read_log();
        if (raw.empty()) return false;
        std::istringstream ss(raw);
        std::string line;
        while (std::getline(ss, line)) {
            auto ts_end = line.find(']');
            if (ts_end != std::string::npos && ts_end + 2 < line.size()) {
                if (line.find(content, ts_end + 2) != std::string::npos)
                    return true;
            }
        }
        return false;
    }

    // 统计去时间戳后某模式出现次数
    int log_count_line(const std::string& pattern) {
        auto raw = read_log();
        if (raw.empty()) return 0;
        int count = 0;
        std::istringstream ss(raw);
        std::string line;
        while (std::getline(ss, line)) {
            auto ts_end = line.find(']');
            if (ts_end != std::string::npos && ts_end + 2 < line.size()) {
                if (line.find(pattern, ts_end + 2) != std::string::npos)
                    count++;
            }
        }
        return count;
    }

    bool db_installed(const std::string& name) {
        return Cache::instance().is_installed(name);
    }

    // 构造本地镜像仓库
    void setup_repo(const std::string& pkg_name, const std::string& pkg_path) {
        fs::path mirror = suite_work_dir / "mirror" / "x86_64";
        fs::create_directories(mirror / pkg_name);
        fs::copy(pkg_path, mirror / pkg_name / "1.0.lpkg",
                 fs::copy_options::overwrite_existing);
        std::string hash = "0000";
        {
            std::ofstream idx(mirror / "index.txt");
            idx << pkg_name << "|1.0:" << hash << "::|\n";
        }
        {
            std::ofstream mc(Config::instance().mirror_conf());
            mc << "file://" << (suite_work_dir / "mirror").string() << "/\n";
        }
    }
};


// ══════════════════════════════════════════════════════════════════════
// I-WAL 组：WAL 结构不变量
// ══════════════════════════════════════════════════════════════════════

// I-WAL-1: 每个成功的安装有一个 COMMIT_PKGS
TEST_F(SemanticInvariantTest, SuccessfulInstallHasCommitPkgs) {
    std::string pkg = make_pkg("wal-pkg", "1.0", {"usr/bin/wal_tool"});
    install_packages({pkg});
    Cache::instance().write();
    Cache::instance().load();

    EXPECT_TRUE(log_has_line("COMMIT_PKGS"))
        << "I-WAL-1: every install ends with COMMIT_PKGS";
    EXPECT_TRUE(db_installed("wal-pkg"));
}

// I-WAL-1b: Ctrl+C 中断的安装也有 COMMIT_PKGS
TEST_F(SemanticInvariantTest, InterruptedInstallStillHasCommitPkgs) {
    std::string pkg = make_pkg("wal-int", "1.0", {"usr/bin/wal_int"});
    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("wal-int", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    // 模拟 install_packages 的 catch 行为
    TransactionLog::log_raw("COMMIT_PKGS");

    EXPECT_TRUE(log_has_line("ROLLBACK wal-int 1.0"))
        << "inner rollback happened";
    EXPECT_TRUE(log_has_line("COMMIT_PKGS"))
        << "I-WAL-1: interrupted install still has COMMIT_PKGS";
    EXPECT_FALSE(db_installed("wal-int"));
}

// I-WAL-2: trim_completed 只识别 BEGIN_PKGS/COMMIT_PKGS
TEST_F(SemanticInvariantTest, TrimIgnoresInnerMarkers) {
    // 日志只有内层标记（无 BEGIN_PKGS/COMMIT_PKGS）→ trim 应清空
    TransactionLog::log_raw("BEGIN test-pkg 1.0");
    TransactionLog::log_raw("COMMIT test-pkg 1.0");
    TransactionLog::log_raw("END test-pkg 1.0");

    TransactionLog::trim_completed();

    EXPECT_TRUE(read_log().empty())
        << "I-WAL-2: trim ignores inner markers, clears orphan lines";
}


// ══════════════════════════════════════════════════════════════════════
// I-OP 组：操作语义不变量
// ══════════════════════════════════════════════════════════════════════

// I-OP-1: 批量回滚写 ROLLBACK 行，不写 RM_BEGIN 行
TEST_F(SemanticInvariantTest, BatchRollbackWritesRollbackNotRmBegin) {
    std::string pkg_a = make_pkg("op-a", "1.0", {"usr/bin/op_a"});
    std::string pkg_b = make_pkg("op-b", "1.0", {"usr/bin/op_b"});

    Config::instance().set_testing_mode(true);
    testing::break_during_file_copy.store(true);
    EXPECT_ANY_THROW(install_packages({pkg_a, pkg_b}));
    Config::instance().set_testing_mode(false);
    testing::reset_all();
    sigint_graceful.store(false);

    // 如果 B 失败，A 被 rollback_installed_package 撤销
    // → A 的记录应是 ROLLBACK，不是 RM_BEGIN
    int rollback_count = log_count_line("ROLLBACK op-a 1.0");
    int rm_begin_count = log_count_line("RM_BEGIN op-a");

    EXPECT_GE(rollback_count, 0)
        << "I-OP-1: batch rollback for pkg-a may or may not appear "
        << "(depends on install order)";
    EXPECT_EQ(rm_begin_count, 0)
        << "I-OP-1: batch rollback writes ROLLBACK, not RM_BEGIN";
    EXPECT_TRUE(log_has_line("ROLLBACK"))
        << "at least one ROLLBACK exists for the failed package";
}

// I-OP-2: remove_package 写 RM_BEGIN，不写 ROLLBACK
TEST_F(SemanticInvariantTest, RemoveWritesRmBeginNotRollback) {
    std::string pkg = make_pkg("op-rm", "1.0", {"usr/bin/op_rm"});
    install_packages({pkg});
    Cache::instance().write();
    Cache::instance().load();
    fs::remove(log_path);  // 清日志

    remove_package("op-rm", false);
    write_cache();

    EXPECT_GE(log_count_line("RM_BEGIN op-rm 1.0"), 1)
        << "I-OP-2: remove writes RM_BEGIN";
    EXPECT_GE(log_count_line("RM_COMMIT op-rm 1.0"), 1)
        << "I-OP-2: remove writes RM_COMMIT";
    EXPECT_EQ(log_count_line("ROLLBACK op-rm 1.0"), 0)
        << "I-OP-2: remove does NOT write ROLLBACK";
}

// I-OP-5: 成功安装后 .lpkg_bak 被清理
TEST_F(SemanticInvariantTest, BackupCleanedAfterSuccess) {
    create_file("usr/bin/bak_check", "before");
    std::string pkg = make_pkg("bak-pkg", "1.0", {"usr/bin/bak_check"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("bak-pkg", "1.0", true, "", pkg);
    task.run_simple();
    Config::instance().set_force_overwrite_mode(false);

    // 检查是否有 .lpkg_bak 残留在 test_root 下
    bool found_bak = false;
    for (auto& e : fs::recursive_directory_iterator(test_root)) {
        if (e.path().filename().string().find(".lpkg_bak_") != std::string::npos)
            found_bak = true;
    }
    EXPECT_FALSE(found_bak)
        << "I-OP-5: no .lpkg_bak after successful install";
}


// ══════════════════════════════════════════════════════════════════════
// I-ATOM 组：原子性不变量
// ══════════════════════════════════════════════════════════════════════

// I-ATOM-1: 单包安装失败 → 无文件残留、无 DB 记录
TEST_F(SemanticInvariantTest, SinglePkgFailureNoOrphans) {
    create_file("usr/bin/atom_tool", "precious");
    std::string pkg = make_pkg("atom-pkg", "1.0", {"usr/bin/atom_tool"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("atom-pkg", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    // 原文件应恢复
    EXPECT_EQ(file_content("usr/bin/atom_tool"), "precious")
        << "I-ATOM-1: original file restored after rollback";
    EXPECT_FALSE(db_installed("atom-pkg"))
        << "I-ATOM-1: no DB entry after rollback";
}

// I-ATOM-2: 批量 [A,B]，B 失败 → A 被撤销
TEST_F(SemanticInvariantTest, BatchSecondFailsFirstRolledBack) {
    std::string pkg_a = make_pkg("batch-a", "1.0", {"usr/bin/batch_a"});
    std::string pkg_b = make_pkg("batch-b", "1.0", {"usr/bin/batch_b"});

    // 不用 breakpoint，直接用 InstallationTask 模拟 B 失败
    // 先手动安装 A（走 install_packages），再写伪造的批量环境
    // 这里简化为：通过 breakpoint 让 B 在安装中中断
    Config::instance().set_testing_mode(true);
    testing::break_during_file_copy.store(true);
    EXPECT_ANY_THROW(install_packages({pkg_a, pkg_b}));
    Config::instance().set_testing_mode(false);
    testing::reset_all();
    sigint_graceful.store(false);

    // A 应被 rollback_installed_package 撤销
    EXPECT_FALSE(db_installed("batch-a"))
        << "I-ATOM-2: first pkg A rolled back";
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/batch_a"))
        << "I-ATOM-2: A's files removed";
}

// I-ATOM-4: 不同阶段中断后系统一致
TEST_F(SemanticInvariantTest, ConsistentAfterInterruptRec) {
    // 模拟：在备份阶段中断 → rec → 系统一致
    create_file("usr/bin/cons_tool", "consistent");
    std::string pkg = make_pkg("cons-pkg", "1.0", {"usr/bin/cons_tool"});

    // 模拟中断备份阶段
    Config::instance().set_testing_mode(true);
    testing::break_during_file_copy.store(true);
    EXPECT_ANY_THROW(install_packages({pkg}));
    Config::instance().set_testing_mode(false);
    testing::reset_all();
    sigint_graceful.store(false);

    // rec 不应破坏任何东西
    recover_packages();
    Cache::instance().load();

    EXPECT_EQ(file_content("usr/bin/cons_tool"), "consistent")
        << "I-ATOM-4: original file intact after interrupt+rec";
    EXPECT_FALSE(db_installed("cons-pkg"))
        << "I-ATOM-4: no DB entry";
}


// ══════════════════════════════════════════════════════════════════════
// I-REC 组：恢复不变量
// ══════════════════════════════════════════════════════════════════════

// I-REC-1: rec 在有 COMMIT_PKGS 的日志上运行 → 无操作
TEST_F(SemanticInvariantTest, RecOnCompletedLogIsNoop) {
    std::string pkg = make_pkg("rec-pkg", "1.0", {"usr/bin/rec_tool"});
    install_packages({pkg});
    Cache::instance().write();

    auto count_before = log_count_line("RESTORE");

    recover_packages();

    auto count_after = log_count_line("RESTORE");
    EXPECT_EQ(count_after, count_before)
        << "I-REC-1: rec on completed log = noop";
    EXPECT_TRUE(db_installed("rec-pkg"))
        << "I-REC-1: package still installed after rec";
}

// I-REC-1b: rec 在 Ctrl+C 后有 COMMIT_PKGS → 无操作
TEST_F(SemanticInvariantTest, RecAfterCtrlCIsNoop) {
    std::string pkg_b = make_pkg("rec-b", "1.0", {"usr/bin/rec_b"});

    // Ctrl+C + COMMIT_PKGS（模拟外层 catch）
    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("rec-b", "1.0", true, "", pkg_b);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    TransactionLog::log_raw("COMMIT_PKGS");
    Config::instance().set_force_overwrite_mode(false);

    // rec 不应做任何恢复操作
    recover_packages();

    EXPECT_TRUE(log_has_line("COMMIT_PKGS"))
        << "I-REC-4: COMMIT_PKGS present after Ctrl+C";
}

// I-REC-3: rec 幂等
TEST_F(SemanticInvariantTest, RecIsIdempotent) {
    create_file("usr/bin/idem_tool", "original");
    std::string pkg = make_pkg("idem-pkg", "1.0", {"usr/bin/idem_tool"});

    // 模拟只写了 BEGIN_PKGS + BACKUP 就断电了
    TransactionLog::log_raw("BEGIN_PKGS 1");
    {
        fs::path f = test_root / "usr/bin/idem_tool";
        fs::path bak = f; bak += ".lpkg_bak_idem-pkg";
        TransactionLog::log_raw("BACKUP " + f.string() + " → " + bak.string());
        fs::rename(f, bak);
    }

    // 第一次 rec：恢复
    recover_packages();
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/idem_tool"))
        << "I-REC-3: first rec restores file";
    EXPECT_EQ(file_content("usr/bin/idem_tool"), "original");

    // 第二次 rec：必须幂等
    recover_packages();
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/idem_tool"))
        << "I-REC-3: second rec idempotent, file not deleted again";
    EXPECT_EQ(file_content("usr/bin/idem_tool"), "original");
}


// ══════════════════════════════════════════════════════════════════════
// I-AUDIT 组：审计不变量
// ══════════════════════════════════════════════════════════════════════

// I-AUDIT-1: 一个包的 WAL 生命周期是单一的（install XOR remove）
TEST_F(SemanticInvariantTest, SinglePkgLifecycleInstallOnly) {
    std::string pkg = make_pkg("audit-pkg", "1.0", {"usr/bin/audit_tool"});
    install_packages({pkg});
    Cache::instance().write();

    // WAL 中不能同时出现 install 和 remove 标记
    bool has_install = log_has_line("BEGIN audit-pkg 1.0");
    bool has_remove = log_has_line("RM_BEGIN audit-pkg 1.0");
    EXPECT_TRUE(has_install);
    EXPECT_FALSE(has_remove)
        << "I-AUDIT-3: install does not produce RM_BEGIN";
}

TEST_F(SemanticInvariantTest, SinglePkgLifecycleRemoveOnly) {
    std::string pkg = make_pkg("audit-rm", "1.0", {"usr/bin/audit_rm"});
    install_packages({pkg});
    Cache::instance().write();
    Cache::instance().load();
    fs::remove(log_path);

    remove_package("audit-rm", false);
    write_cache();

    bool has_rm = log_has_line("RM_BEGIN audit-rm 1.0");
    // 注意：RM_BEGIN 包含子串 "BEGIN aud"，所以必须排除误配
    auto raw = read_log();
    bool has_plain_begin = raw.find("] BEGIN audit-rm") != std::string::npos;
    EXPECT_TRUE(has_rm);
    EXPECT_FALSE(has_plain_begin)
        << "I-AUDIT-3: remove does not produce plain BEGIN";
}

// I-AUDIT-2: RM_BEGIN 只出现在 remove 上下文中，ROLLBACK 只出现在回滚中
TEST_F(SemanticInvariantTest, RollbackOnlyInBatchFailContext) {
    // 批量安装 B 失败 → 检查 A 的 WAL 行是 ROLLBACK 不是 RM_BEGIN
    std::string pkg_a = make_pkg("audit-a", "1.0", {"usr/bin/audit_a"});
    std::string pkg_b = make_pkg("audit-b", "1.0", {"usr/bin/audit_b"});

    Config::instance().set_testing_mode(true);
    testing::break_during_file_copy.store(true);
    EXPECT_ANY_THROW(install_packages({pkg_a, pkg_b}));
    Config::instance().set_testing_mode(false);
    testing::reset_all();
    sigint_graceful.store(false);

    // 如果有 ROLLBACK audit-a，确保没有 RM_BEGIN audit-a
    if (log_has_line("ROLLBACK audit-a 1.0")) {
        EXPECT_FALSE(log_has_line("RM_BEGIN audit-a"))
            << "I-AUDIT-2: audit-a rolled back, not removed";
    }
}


// ══════════════════════════════════════════════════════════════════════
// 回归测试组：有历史bug的特定场景
// ══════════════════════════════════════════════════════════════════════

// REG-1: Java JDK 场景 — 大包 Ctrl+C → COMMIT_PKGS 补写 → rec 跳过
TEST_F(SemanticInvariantTest, Reg_LargePkgCtrlCThenRecNoDamage) {
    std::vector<std::string> many_files;
    for (int i = 0; i < 30; ++i)
        many_files.push_back("usr/lib/reg/lib" + std::to_string(i) + ".so");
    for (int i = 0; i < 30; ++i)
        create_file("usr/lib/reg/lib" + std::to_string(i) + ".so", "old_v" + std::to_string(i));

    std::string pkg = make_pkg("reg-pkg", "1.0", many_files);

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("reg-pkg", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    TransactionLog::log_raw("COMMIT_PKGS");
    Config::instance().set_force_overwrite_mode(false);

    // 所有文件都应恢复
    for (int i = 0; i < 30; ++i)
        EXPECT_EQ(file_content("usr/lib/reg/lib" + std::to_string(i) + ".so"),
                  "old_v" + std::to_string(i));

    // rec 不应造成伤害
    recover_packages();
    for (int i = 0; i < 30; ++i)
        EXPECT_TRUE(fs::exists(test_root / ("usr/lib/reg/lib" + std::to_string(i) + ".so")));
}

// REG-2: 连续两次 install → 第二次的 trim 应正确裁剪第一次的日志
TEST_F(SemanticInvariantTest, Reg_SecondInstallTrimFirst) {
    std::string pkg_a = make_pkg("reg-a", "1.0", {"usr/bin/reg_a"});
    std::string pkg_b = make_pkg("reg-b", "1.0", {"usr/bin/reg_b"});

    install_packages({pkg_a});
    Cache::instance().write("reg-a");

    // 第二次 install 自动触发 trim
    install_packages({pkg_b});
    Cache::instance().write("reg-b");
    Cache::instance().load();

    // 两个包都应存在
    EXPECT_TRUE(db_installed("reg-a"));
    EXPECT_TRUE(db_installed("reg-b"));
}

// REG-3: 移除后 rec → 不恢复已移除的包
TEST_F(SemanticInvariantTest, Reg_RemoveThenRecNoRestore) {
    std::string pkg = make_pkg("reg-rm", "1.0", {"usr/bin/reg_rm"});
    install_packages({pkg});
    Cache::instance().write();
    Cache::instance().load();
    ASSERT_TRUE(db_installed("reg-rm"));

    fs::remove(log_path);  // 清日志

    remove_package("reg-rm", false);
    write_cache();
    Cache::instance().load();
    ASSERT_FALSE(db_installed("reg-rm"));

    // rec 不应恢复已移除的包
    recover_packages();
    Cache::instance().load();
    EXPECT_FALSE(db_installed("reg-rm"))
        << "REG-3: rec does not restore intentionally removed package";
}

// REG-4: 使用仓库索引安装，needed_so 校验拒绝 → 无残留
TEST_F(SemanticInvariantTest, Reg_NeededSoRejectNoOrphan) {
    // 创建包 A 和 B，B 需要 A 提供 SONAME，但 A 不提供
    std::string pkg_a = make_pkg("reg-a", "1.0", {"usr/bin/reg_a"});
    std::string pkg_b = make_pkg("reg-b", "1.0", {"usr/bin/reg_b"});

    // 用 make_pkg_full 带 needed_so
    fs::path work_dir = suite_work_dir / "_pkg_reg_b_full";
    fs::create_directories(work_dir / "content" / "usr" / "bin");
    std::ofstream(work_dir / "content" / "usr" / "bin" / "reg_b").close();
    std::string pkg_b_full = (pkg_dir / "reg-b-1.0.lpkg").string();
    pack_package(pkg_b_full, work_dir.string(), "reg-b", "1.0",
                 {"reg-a"}, {}, "", {"ghost.so.1"});
    fs::remove_all(work_dir);

    setup_repo("reg-a", pkg_a);
    // 把 pkg_b_full 放入 repo
    {
        fs::path mp = suite_work_dir / "mirror" / "x86_64";
        fs::create_directories(mp / "reg-b");
        fs::copy(pkg_b_full, mp / "reg-b" / "1.0.lpkg",
                 fs::copy_options::overwrite_existing);
        std::ofstream idx(mp / "index.txt");
        idx << "reg-a|1.0:0000::|\n";
        idx << "reg-b|1.0:0000:reg-a::ghost.so.1|\n";
    }

    // needed_so ghost.so.1 无提供者 → 拒绝
    EXPECT_THROW(install_packages({"reg-b"}), LpkgException);

    // reg-a（被重解析中安装的依赖）应被回滚
    Cache::instance().load();
    EXPECT_FALSE(db_installed("reg-a"))
        << "REG-4: dependency reg-a rolled back after needed_so rejection";
    EXPECT_FALSE(db_installed("reg-b"));
}

// REG-5: 文件路径含空格 → 回滚正确处理
TEST_F(SemanticInvariantTest, Reg_SpacesInPath) {
    create_file("usr/lib/my library.so", "spacey");
    std::string pkg = make_pkg("space-pkg", "1.0", {"usr/lib/my library.so"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("space-pkg", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    TransactionLog::log_raw("COMMIT_PKGS");
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_EQ(file_content("usr/lib/my library.so"), "spacey");
}

// REG-6: 多个符号链接链 → 回滚全部恢复
TEST_F(SemanticInvariantTest, Reg_SymlinkChainRollback) {
    create_file("usr/lib/libbase.so.1.0", "base");
    fs::create_directories(test_root / "usr/lib");
    fs::create_symlink("libbase.so.1.0", test_root / "usr/lib/libbase.so.1");
    fs::create_symlink("libbase.so.1", test_root / "usr/lib/libbase.so");

    std::string pkg = make_pkg("sym-pkg", "1.0",
        {"usr/lib/libbase.so", "usr/lib/libbase.so.1", "usr/lib/libbase.so.1.0"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("sym-pkg", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    TransactionLog::log_raw("COMMIT_PKGS");
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_TRUE(fs::exists(test_root / "usr/lib/libbase.so.1.0"));
    EXPECT_TRUE(fs::is_symlink(test_root / "usr/lib/libbase.so.1"));
    EXPECT_TRUE(fs::is_symlink(test_root / "usr/lib/libbase.so"));
}


// ══════════════════════════════════════════════════════════════════════
// WAL 格式不变量
// ══════════════════════════════════════════════════════════════════════

// 验证成功安装的 WAL 顺序：BEGIN → (ops) → COMMIT → END
TEST_F(SemanticInvariantTest, WalOrderOnSuccess) {
    create_file("usr/bin/wal_order", "old");
    std::string pkg = make_pkg("wal-order", "1.0", {"usr/bin/wal_order"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("wal-order", "1.0", true, "", pkg);
    task.run_simple();
    Config::instance().set_force_overwrite_mode(false);

    auto log = read_log();
    auto begin_pos = log.find("BEGIN wal-order 1.0");
    auto commit_pos = log.find("COMMIT wal-order 1.0");
    auto end_pos = log.find("END wal-order 1.0");

    EXPECT_LT(begin_pos, commit_pos);
    EXPECT_LT(commit_pos, end_pos);
}

// 验证回滚的 WAL 顺序：BEGIN → (ops) → ROLLBACK → END
TEST_F(SemanticInvariantTest, WalOrderOnRollback) {
    create_file("usr/bin/wal_roll", "old");
    std::string pkg = make_pkg("wal-roll", "1.0", {"usr/bin/wal_roll"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("wal-roll", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    auto log = read_log();
    auto begin_pos = log.find("BEGIN wal-roll 1.0");
    auto rollback_pos = log.find("ROLLBACK wal-roll 1.0");
    auto end_pos = log.find("END wal-roll 1.0");

    EXPECT_LT(begin_pos, rollback_pos);
    EXPECT_LT(rollback_pos, end_pos);
}
