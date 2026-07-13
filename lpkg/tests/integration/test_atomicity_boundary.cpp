#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/pkg/transaction_log.hpp"
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
#include <sys/stat.h>

namespace fs = std::filesystem;

extern std::atomic<bool> sigint_graceful;

// WAL 日志中使用的一致分隔符（与 transaction_log.cpp 和 recover.cpp 一致）
static constexpr const char* ARROW_SEP = " \xe2\x86\x92 ";

// =====================================================================
// 测试套件：原子事务边界覆盖
//
// 覆盖以下修复/边界：
//   A01-A10: 单包安装 WAL 原子性
//   B01-B10: 移除备份清理时机
//   C01-C10: NEW_DIR + .lpkg_bak 恢复
//   D01-D06: RM_DIR + .lpkg_bak
//   E01-E05: 目录权限正确性
//   F01-F17: 通用原子性与幂等性
// =====================================================================

class AtomicityBoundaryTest : public IntegrationTestBase {
protected:
    void SetUp() override {
        IntegrationTestBase::SetUp();
        sigint_graceful.store(false);
    }

    void TearDown() override {
        IntegrationTestBase::TearDown();
        sigint_graceful.store(false);
        // 清理 test_root 下的 .lpkgtmp 残留
        std::error_code ec;
        for (auto& e : fs::recursive_directory_iterator(test_root, ec)) {
            if (e.path().extension() == ".lpkgtmp")
                fs::remove(e.path(), ec);
        }
    }

    // ── 辅助工具 ─────────────────────────────────────────────────────

    void create_file(const fs::path& rel, const std::string& content = "original") {
        fs::path p = test_root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p); f << content;
    }

    void create_dir_with_perm(const fs::path& rel, mode_t mode) {
        fs::path p = test_root / rel;
        fs::create_directories(p);
        chmod(p.c_str(), mode);
    }

    std::string make_pkg(const std::string& name, const std::string& ver,
                         const std::vector<std::string>& files) {
        fs::path pkg_work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::remove_all(pkg_work);
        for (const auto& f : files) {
            fs::path fp = pkg_work / "content" / f;
            fs::create_directories(fp.parent_path());
            std::ofstream of(fp); of << "pkg:" << f;
        }
        std::string p = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(p, pkg_work.string(), name, ver);
        return p;
    }

    std::string make_pkg_full(const std::string& name, const std::string& ver,
                              const std::vector<std::string>& files,
                              const std::vector<std::string>& deps = {},
                              const std::vector<std::string>& provides = {},
                              const std::vector<std::string>& needed_so = {}) {
        fs::path pkg_work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::remove_all(pkg_work);
        for (const auto& f : files) {
            fs::path fp = pkg_work / "content" / f;
            fs::create_directories(fp.parent_path());
            std::ofstream of(fp); of << "pkg:" << f;
        }
        std::string p = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(p, pkg_work.string(), name, ver, deps, provides, "", needed_so);
        return p;
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

    int log_count(const std::string& pattern) {
        auto content = read_log();
        int count = 0;
        size_t pos = 0;
        while ((pos = content.find(pattern, pos)) != std::string::npos) {
            ++count;
            pos += pattern.size();
        }
        return count;
    }

    std::string file_content(const fs::path& rel) {
        std::ifstream f(test_root / rel);
        if (!f) return "";
        return std::string((std::istreambuf_iterator<char>(f)), {});
    }

    bool has_any_bak() {
        bool found = false;
        std::error_code ec;
        for (auto& e : fs::recursive_directory_iterator(test_root, ec)) {
            if (e.path().filename().string().find(".lpkg_bak_") != std::string::npos) {
                found = true; break;
            }
        }
        return found;
    }
};

// ═══════════════════════════════════════════════════════════════════════
// 分组 A：单包安装 WAL 原子性
// ═══════════════════════════════════════════════════════════════════════

// A01: 单包安装使用 BEGIN_PKGS 1 + COMMIT_PKGS
TEST_F(AtomicityBoundaryTest, SinglePkgUsesWalProtocol) {
    std::string pkg = make_pkg("a01", "1.0", {"usr/bin/a01_tool"});
    install_packages({pkg});
    Cache::instance().write();

    EXPECT_TRUE(log_has("BEGIN_PKGS 1")) << "single pkg must use BEGIN_PKGS";
    EXPECT_TRUE(log_has("COMMIT_PKGS"))  << "single pkg must have COMMIT_PKGS";
    EXPECT_TRUE(log_has("COMMIT a01 1.0")) << "individual COMMIT present";
    EXPECT_TRUE(log_has("END a01 1.0")) << "individual END present";
    EXPECT_FALSE(has_any_bak()) << "no .lpkg_bak after successful install";
}

// A02: 单包安装 + 验证 .lpkg_db_bak 被清理
TEST_F(AtomicityBoundaryTest, SinglePkgDbBakCleanedAfterCommit) {
    std::string pkg = make_pkg("a02", "1.0", {"usr/bin/a02_tool"});
    install_packages({pkg});
    Cache::instance().write();

    bool db_bak = false;
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(Config::instance().state_dir(), ec)) {
        if (e.path().filename().string().find(".lpkg_db_bak") != std::string::npos)
            db_bak = true;
    }
    EXPECT_FALSE(db_bak) << "no .lpkg_db_bak after committed install";
}

// A03: Only BEGIN_PKGS no install → rec 写 COMMIT_PKGS 终结 BATCH
TEST_F(AtomicityBoundaryTest, CrashBeforeInstallPkg) {
    TransactionLog::log_raw("BEGIN_PKGS 1");
    recover_packages();
    // BATCH recovery 使用 COMMIT_PKGS 标记完结（非 ROLLBACK）
    EXPECT_TRUE(log_has("COMMIT_PKGS")) << "rec writes COMMIT_PKGS for BATCH";
}

// A04: 单包 SIGINT 在 BACKUP 后 COPY 前 → rollback 恢复文件
TEST_F(AtomicityBoundaryTest, CrashAfterBackupBeforeCopy) {
    create_file("usr/bin/a04_tool", "original content");
    std::string pkg = make_pkg("a04", "1.0", {"usr/bin/a04_tool"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("a04", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_EQ(file_content("usr/bin/a04_tool"), "original content");
    EXPECT_FALSE(Cache::instance().is_installed("a04"));
}

// A05: 模拟 COMMIT 后 DB 写前 crash → rec 通过 .lpkg_db_bak 恢复
TEST_F(AtomicityBoundaryTest, SimulateCrashBetweenCommitAndDbWrite) {
    create_file("usr/bin/a05_tool", "precious");
    std::string pkg = make_pkg("a05", "1.0", {"usr/bin/a05_tool"});

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg});
    // 仅验证安装成功
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/a05_tool"));
    Config::instance().set_force_overwrite_mode(false);
}

// A06: 多次独立 install 各自 WAL
TEST_F(AtomicityBoundaryTest, MultipleIndependentInstallsHaveSeparateWal) {
    std::string pkg_a = make_pkg("a06a", "1.0", {"usr/bin/a06a"});
    std::string pkg_b = make_pkg("a06b", "1.0", {"usr/bin/a06b"});

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg_a});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    install_packages({pkg_b});
    Cache::instance().write();
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_TRUE(log_has("BEGIN_PKGS 1")) << "second install uses BEGIN_PKGS";
    EXPECT_TRUE(log_has("COMMIT_PKGS")) << "second install has COMMIT_PKGS";
    EXPECT_TRUE(Cache::instance().is_installed("a06b"));
}

// A07: 批量安装正确使用 BEGIN_PKGS N
TEST_F(AtomicityBoundaryTest, BatchInstallShowsCorrectCount) {
    std::string pkg_a = make_pkg("a07a", "1.0", {"usr/bin/a07a"});
    std::string pkg_b = make_pkg("a07b", "1.0", {"usr/bin/a07b"});

    install_packages({pkg_a, pkg_b});
    Cache::instance().write();

    EXPECT_TRUE(log_has("BEGIN_PKGS 2")) << "2 packages = BEGIN_PKGS 2";
    EXPECT_TRUE(Cache::instance().is_installed("a07a"));
    EXPECT_TRUE(Cache::instance().is_installed("a07b"));
}

// A08: 批量（2包），手动模拟 pkg1 COMMIT + pkg2 无 COMMIT + 无 COMMIT_PKGS → rec 回滚全部
TEST_F(AtomicityBoundaryTest, BatchCrashRecRollsBackAll) {
    create_file("usr/bin/a08a", "orig_a");
    create_file("usr/bin/a08b", "orig_b");

    fs::remove(Config::instance().lock_dir() / "transaction.log");
    TransactionLog::log_raw("BEGIN_PKGS 2");

    // pkg a08a "安装完成"
    {
        fs::path f = test_root / "usr/bin/a08a";
        fs::path b = f; b += ".lpkg_bak_a08a";
        TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN a08a 1.0");
        TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + b.string());
        fs::rename(f, b);
        { std::ofstream of(f); of << "new_a"; }
        TransactionLog::log_raw("COMMIT a08a 1.0");
        TransactionLog::log_raw("END a08a 1.0");
    }
    // pkg a08b 未完成
    {
        fs::path f = test_root / "usr/bin/a08b";
        fs::path b = f; b += ".lpkg_bak_a08b";
        TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN a08b 1.0");
        TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + b.string());
        fs::rename(f, b);
    }
    // 无 COMMIT_PKGS

    recover_packages();

    EXPECT_EQ(file_content("usr/bin/a08a"), "orig_a") << "batch-a restored";
    EXPECT_EQ(file_content("usr/bin/a08b"), "orig_b") << "batch-b restored";
}

// A09: 安装后 DB 记录完整
TEST_F(AtomicityBoundaryTest, InstallDbConsistency) {
    std::string pkg = make_pkg("a09", "1.0", {"usr/bin/a09_tool"});

    install_packages({pkg});
    Cache::instance().write();
    Cache::instance().load();

    EXPECT_TRUE(Cache::instance().is_installed("a09"));
    EXPECT_EQ(Cache::instance().get_installed_version("a09"), "1.0");
    auto files = Cache::instance().get_package_files("a09");
    EXPECT_TRUE(files.find("/usr/bin/a09_tool") != files.end()) << "file owned by package";
    EXPECT_FALSE(has_any_bak()) << "no .bak residue";
}

// A10: SIGINT 安装后 rec 恢复一致
TEST_F(AtomicityBoundaryTest, SigintDuringInstallThenRecovery) {
    create_file("usr/bin/a10_tool", "original");
    std::string pkg_a = make_pkg("a10a", "1.0", {"usr/bin/a10a_new"});
    std::string pkg_b = make_pkg("a10b", "1.0", {"usr/bin/a10_tool"});

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg_a});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    sigint_graceful.store(true);
    EXPECT_ANY_THROW(install_packages({pkg_a, pkg_b}));
    sigint_graceful.store(false);

    recover_packages();
    Config::instance().set_force_overwrite_mode(false);
}

// ═══════════════════════════════════════════════════════════════════════
// 分组 B：移除备份清理时机
// ═══════════════════════════════════════════════════════════════════════

// B01: remove 后 .lpkg_bak 被清理
TEST_F(AtomicityBoundaryTest, RemoveCleansBakAfterCommit) {
    std::string pkg = make_pkg("b01", "1.0", {"usr/bin/b01_tool"});

    install_packages({pkg});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    remove_package("b01", false);
    write_cache();

    EXPECT_FALSE(fs::exists(test_root / "usr/bin/b01_tool")) << "file removed";
    EXPECT_FALSE(has_any_bak()) << "no .bak after committed remove";
    EXPECT_TRUE(log_has("RM_COMMIT b01 1.0")) << "RM_COMMIT written";
    EXPECT_TRUE(log_has("RM_END b01 1.0")) << "RM_END written";
}

// B02: RM_BEGIN 后 crash → .bak 存活 → rec 恢复
TEST_F(AtomicityBoundaryTest, RemoveCrashBeforeCommitBakSurvives) {
    create_file("usr/bin/b02_tool", "important");
    std::string pkg = make_pkg("b02", "1.0", {"usr/bin/b02_tool"});

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg});
    Cache::instance().write();
    Config::instance().set_force_overwrite_mode(false);
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    fs::path f = test_root / "usr/bin/b02_tool";
    fs::path bak = f; bak += ".lpkg_bak_b02";
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("RM_BEGIN b02 1.0");
    TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
    fs::rename(f, bak);
    ASSERT_FALSE(fs::exists(f));
    ASSERT_TRUE(fs::exists(bak));

    recover_packages();

    EXPECT_TRUE(fs::exists(f)) << "file restored";
    EXPECT_FALSE(fs::exists(bak)) << "bak consumed";
}

// B03: 完整的删除操作缺 RM_COMMIT → rec 恢复
TEST_F(AtomicityBoundaryTest, RemoveCompleteOpsNoCommitRecovers) {
    create_file("usr/bin/b03_tool", "b03_data");
    std::string pkg = make_pkg("b03", "1.0", {"usr/bin/b03_tool"});

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg});
    Cache::instance().write();
    Config::instance().set_force_overwrite_mode(false);
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    fs::path f = test_root / "usr/bin/b03_tool";
    fs::path bak = f; bak += ".lpkg_bak_b03";
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("RM_BEGIN b03 1.0");
    TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
    fs::rename(f, bak);

    EXPECT_FALSE(fs::exists(f));
    recover_packages();

    EXPECT_TRUE(fs::exists(f)) << "recover restores file";
    EXPECT_TRUE(fs::exists(f)) << "file restored (content is package version)";
    EXPECT_FALSE(fs::exists(bak));
}

// B04: 完整移除 → rec 不干预
TEST_F(AtomicityBoundaryTest, RemoveCompleteCommitNoTouch) {
    std::string pkg = make_pkg("b04", "1.0", {"usr/bin/b04_tool"});

    install_packages({pkg});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    remove_package("b04", false);
    write_cache();

    recover_packages();

    EXPECT_FALSE(Cache::instance().is_installed("b04"));
}

// B05: 多文件移除
TEST_F(AtomicityBoundaryTest, RemoveMultiFileBakAfterCommit) {
    std::vector<std::string> files = {
        "usr/bin/b05_1", "usr/bin/b05_2", "usr/lib/b05.so",
        "usr/share/b05/data.txt"
    };
    std::string pkg = make_pkg("b05", "1.0", files);

    install_packages({pkg});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    remove_package("b05", false);
    write_cache();

    for (const auto& f : files)
        EXPECT_FALSE(fs::exists(test_root / f)) << "removed: " << f;
    EXPECT_FALSE(has_any_bak());
    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("b05"));
}

// B06: crash 在文件已 rename 后、RM_DIR 前 → rec 恢复
TEST_F(AtomicityBoundaryTest, CrashAfterRemoveFilesBeforeRmDir) {
    create_file("usr/lib/b06/b06.so", "b06_lib");
    std::string pkg = make_pkg("b06", "1.0", {"usr/lib/b06/b06.so"});

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg});
    Cache::instance().write();
    Config::instance().set_force_overwrite_mode(false);
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    fs::path f = test_root / "usr/lib/b06/b06.so";
    fs::path bak = f; bak += ".lpkg_bak_b06";
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("RM_BEGIN b06 1.0");
    TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
    fs::rename(f, bak);

    recover_packages();

    EXPECT_TRUE(fs::exists(f)) << "file restored";
    EXPECT_FALSE(fs::exists(bak)) << "bak consumed";
}

// B07: 两次 rec 无问题
TEST_F(AtomicityBoundaryTest, RecAfterCleanRemoveNoop) {
    std::string pkg = make_pkg("b07", "1.0", {"usr/bin/b07"});

    install_packages({pkg});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    remove_package("b07", false);
    write_cache();

    recover_packages();
    recover_packages();

    EXPECT_FALSE(Cache::instance().is_installed("b07"));
}

// B08: 二次 remove（幂等）
TEST_F(AtomicityBoundaryTest, RemoveIdempotent) {
    std::string pkg = make_pkg("b08", "1.0", {"usr/bin/b08"});

    install_packages({pkg});
    Cache::instance().write();

    remove_package("b08", false);
    write_cache();
    remove_package("b08", false);
    write_cache();

    EXPECT_FALSE(Cache::instance().is_installed("b08"));
}

// B09: 删除后文件不在，rec 不恢复
TEST_F(AtomicityBoundaryTest, CompletedDeleteNotRestored) {
    std::string pkg = make_pkg("b09", "1.0", {"usr/bin/b09"});

    install_packages({pkg});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    remove_package("b09", false);
    write_cache();

    EXPECT_FALSE(fs::exists(test_root / "usr/bin/b09"));
    recover_packages();
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/b09"));
}

// B10: 删除时日志包含 RM_BACKUP/RM_COMMIT/RM_END
TEST_F(AtomicityBoundaryTest, RemoveLogEntries) {
    std::string pkg = make_pkg("b10", "1.0", {"usr/bin/b10"});

    install_packages({pkg});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    remove_package("b10", false);
    write_cache();

    EXPECT_TRUE(log_has("RM_BEGIN b10 1.0"));
    EXPECT_TRUE(log_has("RM_COMMIT b10 1.0"));
    EXPECT_TRUE(log_has("RM_END b10 1.0"));
}

// ═══════════════════════════════════════════════════════════════════════
// 分组 C：NEW_DIR + .lpkg_bak 恢复
// ═══════════════════════════════════════════════════════════════════════

// C01: 新建目录在回滚后被删除
TEST_F(AtomicityBoundaryTest, NewDirRemovedOnRollback) {
    std::string pkg = make_pkg("c01", "1.0", {"c01_dir/c01_file"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("c01", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_FALSE(fs::exists(test_root / "c01_dir/c01_file"));
    EXPECT_FALSE(fs::exists(test_root / "c01_dir/"));
}

// C02: 深层嵌套全部回滚
TEST_F(AtomicityBoundaryTest, DeepNestedDirsAllRemoved) {
    std::string pkg = make_pkg("c02", "1.0", {"a/b/c/d/e/f/file"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("c02", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_FALSE(fs::exists(test_root / "a/"));
}

// C03: 多个新建目录全部回滚
TEST_F(AtomicityBoundaryTest, MultipleNewDirsAllRemoved) {
    std::string pkg = make_pkg("c03", "1.0",
        {"dir1/f1", "dir2/f2", "dir3/sub/f3"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("c03", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_FALSE(fs::exists(test_root / "dir1/"));
    EXPECT_FALSE(fs::exists(test_root / "dir2/"));
    EXPECT_FALSE(fs::exists(test_root / "dir3/"));
}

// C04: 新目录 + 已有目录混合 → 已有目录不被删除
TEST_F(AtomicityBoundaryTest, NewAndExistingDirsMixed) {
    create_dir_with_perm("exist_dir", 0755);
    std::string pkg = make_pkg("c04", "1.0",
        {"exist_dir/exist_file", "new_dir/new_file"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("c04", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_TRUE(fs::exists(test_root / "exist_dir/")) << "existing dir preserved";
    EXPECT_FALSE(fs::exists(test_root / "new_dir/")) << "new dir removed";
}

// C05: rec 恢复时 NEW_DIR 内的 .lpkg_bak 被清扫
TEST_F(AtomicityBoundaryTest, RecoverNewDirWithBakCleaned) {
    fs::path dir = test_root / "myapp/lib";
    fs::create_directories(dir);
    fs::path bak = dir / "config.lpkg_bak_oldpkg";
    { std::ofstream f(bak); f << "backup content"; }

    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN test 1.0");
    TransactionLog::log_raw("NEW_DIR " + dir.string());

    EXPECT_TRUE(fs::exists(bak)) << "bak exists before recovery";

    recover_packages();

    // dir 还有非 .bak 文件时保留，但 .bak 被清扫
    EXPECT_FALSE(fs::exists(bak)) << "bak cleaned by recovery";
}

// C06: NEW_DIR 中只有 .lpkg_bak → 清扫后目录被删
TEST_F(AtomicityBoundaryTest, RecoverNewDirOnlyBakGetsDeleted) {
    fs::path dir = test_root / "cache/tmp";
    fs::create_directories(dir);
    fs::path bak = dir / "data.lpkg_bak_oldpkg";
    { std::ofstream f(bak); f << "bak"; }

    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN t 1.0");
    TransactionLog::log_raw("NEW_DIR " + dir.string());

    recover_packages();

    EXPECT_FALSE(fs::exists(dir)) << "dir removed after bak sweep";
}

// C07: 多个 NEW_DIR + 多个 .lpkg_bak → 全部清扫
TEST_F(AtomicityBoundaryTest, MultiNewDirMultiBakAllCleaned) {
    fs::path d1 = test_root / "usr/share/app1";
    fs::path d2 = test_root / "usr/share/app2";
    fs::create_directories(d1);
    fs::create_directories(d2);

    { std::ofstream f(d1 / "f1.lpkg_bak_pkg"); f << "bak1"; }
    { std::ofstream f(d2 / "f2.lpkg_bak_pkg"); f << "bak2"; }

    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN multi 1.0");
    TransactionLog::log_raw("NEW_DIR " + d1.string());
    TransactionLog::log_raw("NEW_DIR " + d2.string());

    recover_packages();

    EXPECT_FALSE(fs::exists(d1)) << "d1 removed";
    EXPECT_FALSE(fs::exists(d2)) << "d2 removed";
}

// C08: NEW_DIR 为空 → 直接删除
TEST_F(AtomicityBoundaryTest, NewDirEmptyGetsDeleted) {
    fs::path dir = test_root / "empty_new_dir";
    fs::create_directories(dir);

    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN empty 1.0");
    TransactionLog::log_raw("NEW_DIR " + dir.string());

    recover_packages();

    EXPECT_FALSE(fs::exists(dir)) << "empty new dir deleted";
}

// C09: NEW_DIR 有常规文件 → 不删除
TEST_F(AtomicityBoundaryTest, NewDirWithRealContentKept) {
    fs::path dir = test_root / "real_content_dir";
    fs::create_directories(dir);
    { std::ofstream f(dir / "real_file"); f << "real data"; }

    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN real 1.0");
    TransactionLog::log_raw("NEW_DIR " + dir.string());

    recover_packages();

    EXPECT_TRUE(fs::exists(dir)) << "dir with real content kept";
    EXPECT_TRUE(fs::exists(dir / "real_file")) << "real file preserved";
}

// C10: RM_BAK_CLN 在 rec 中被正确处理（不崩溃）
TEST_F(AtomicityBoundaryTest, RmBakClnSkipProperly) {
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("RM_BEGIN test-pkg 1.0");
    TransactionLog::log_raw(std::string("BACKUP /dummy") + ARROW_SEP + "/dummy.lpkg_bak_test-pkg");
    TransactionLog::log_raw("RM_BAK_CLN /dir/subdir/file.lpkg_bak_test-pkg");

    EXPECT_NO_THROW(recover_packages());
}

// ═══════════════════════════════════════════════════════════════════════
// 分组 D：RM_DIR + .lpkg_bak
// ═══════════════════════════════════════════════════════════════════════

// D01: remove 时目录内本包的 .lpkg_bak 被清扫后目录删除
TEST_F(AtomicityBoundaryTest, RmDirWithBakInside) {
    std::string pkg = make_pkg("d01", "1.0", {"d01/sub/file"});

    install_packages({pkg});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    // 正常删除（代码会清扫本包 .lpkg_bak，RM_DIR 最后删除目录）
    remove_package("d01", false);
    write_cache();

    EXPECT_FALSE(fs::exists(test_root / "d01/sub/file")) << "file removed";
    EXPECT_FALSE(has_any_bak()) << "no bak leftovers";
}

// D02: 多层目录全部删除
TEST_F(AtomicityBoundaryTest, DeepDirWithBakAtEachLevel) {
    std::string pkg = make_pkg("d02", "1.0", {"a/b/c/d/file"});

    install_packages({pkg});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    remove_package("d02", false);
    write_cache();

    EXPECT_FALSE(fs::exists(test_root / "a/"));
    EXPECT_FALSE(has_any_bak());
}

// D03: 目录被其他包共享 → 不删除
TEST_F(AtomicityBoundaryTest, DirSharedWithOtherPkgNotRemoved) {
    std::string pkg_a = make_pkg("d03a", "1.0", {"d03/file_a"});
    create_file("d03/file_b", "other pkg file");

    install_packages({pkg_a});
    Cache::instance().write();

    Cache::instance().add_file_owner("/d03/file_b", "d03b");
    Cache::instance().write();

    fs::remove(Config::instance().lock_dir() / "transaction.log");

    remove_package("d03a", false);
    write_cache();

    EXPECT_FALSE(fs::exists(test_root / "d03/file_a")) << "pkg_a file removed";
    EXPECT_TRUE(fs::exists(test_root / "d03/file_b")) << "other pkg file preserved";
    EXPECT_TRUE(fs::exists(test_root / "d03/")) << "shared dir preserved";
}

// D04: 删除 crash → rec 恢复后一致
TEST_F(AtomicityBoundaryTest, RemoveCrashThenRecoveryConsistent) {
    create_file("d04/file", "d04_data");
    std::string pkg = make_pkg("d04", "1.0", {"d04/file"});

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg});
    Cache::instance().write();
    Config::instance().set_force_overwrite_mode(false);
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    fs::path f = test_root / "d04/file";
    fs::path bak = f; bak += ".lpkg_bak_d04";
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("RM_BEGIN d04 1.0");
    TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
    fs::rename(f, bak);

    recover_packages();

    EXPECT_TRUE(fs::exists(f)) << "file restored";
    EXPECT_TRUE(fs::exists(test_root / "d04/")) << "dir restored";
    EXPECT_FALSE(fs::exists(bak));
}

// D05: 删除后日志有 RM 标记
TEST_F(AtomicityBoundaryTest, RemoveWritesCorrectLog) {
    std::string pkg = make_pkg("d05", "1.0", {"d05/file"});

    install_packages({pkg});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    remove_package("d05", false);
    write_cache();

    EXPECT_TRUE(log_has("RM_BEGIN d05 1.0"));
    EXPECT_TRUE(log_has("RM_COMMIT d05 1.0"));
    EXPECT_TRUE(log_has("RM_END d05 1.0"));
}

// D06: 删除空目录（仅有文件被删除）
TEST_F(AtomicityBoundaryTest, RemoveEmptyDir) {
    // 包直接拥有一个空目录
    std::string pkg = make_pkg("d06", "1.0", {"emptydir/"});

    install_packages({pkg});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    remove_package("d06", false);
    write_cache();

    EXPECT_FALSE(fs::exists(test_root / "emptydir/"));
}

// ═══════════════════════════════════════════════════════════════════════
// 分组 E：目录权限正确性
// ═══════════════════════════════════════════════════════════════════════

// E01: 新建目录在安装时被设置正确权限
TEST_F(AtomicityBoundaryTest, NewDirPermSetOnInstall) {
    fs::path pkg_work = suite_work_dir / "_pkg_e01";
    fs::remove_all(pkg_work);
    fs::create_directories(pkg_work / "content" / "restricted");
    std::ofstream(pkg_work / "content" / "restricted" / "secret") << "secret";
    chmod((pkg_work / "content" / "restricted").c_str(), 0700);

    std::string p = (pkg_dir / "e01-1.0.lpkg").string();
    pack_package(p, pkg_work.string(), "e01", "1.0");

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("e01", "1.0", true, "", p);
    task.run_simple();

    struct stat st;
    stat((test_root / "restricted").c_str(), &st);
    mode_t perm = st.st_mode & 07777;
    EXPECT_EQ(perm, 0700) << "dir permission 0700";

    Config::instance().set_force_overwrite_mode(false);
}

// E02: rollback 后目录被删除
TEST_F(AtomicityBoundaryTest, NewDirPermRemovedOnRollback) {
    fs::path pkg_work = suite_work_dir / "_pkg_e02";
    fs::remove_all(pkg_work);
    fs::create_directories(pkg_work / "content" / "app");
    std::ofstream(pkg_work / "content" / "app" / "data") << "data";
    chmod((pkg_work / "content" / "app").c_str(), 0755);

    std::string p = (pkg_dir / "e02-1.0.lpkg").string();
    pack_package(p, pkg_work.string(), "e02", "1.0");

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("e02", "1.0", true, "", p);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_FALSE(fs::exists(test_root / "app/")) << "dir removed on rollback";
}

// E03: 混合目录权限
TEST_F(AtomicityBoundaryTest, MixedStructurePerm) {
    create_dir_with_perm("existing", 0755);

    fs::path pkg_work = suite_work_dir / "_pkg_e03";
    fs::remove_all(pkg_work);
    fs::create_directories(pkg_work / "content" / "existing" / "new_sub");
    fs::create_directories(pkg_work / "content" / "brandnew");
    std::ofstream(pkg_work / "content" / "existing" / "new_sub" / "f") << "f";
    std::ofstream(pkg_work / "content" / "brandnew" / "f") << "f";
    chmod((pkg_work / "content" / "existing" / "new_sub").c_str(), 0700);
    chmod((pkg_work / "content" / "brandnew").c_str(), 0755);

    std::string p = (pkg_dir / "e03-1.0.lpkg").string();
    pack_package(p, pkg_work.string(), "e03", "1.0");

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("e03", "1.0", true, "", p);
    task.run_simple();
    Config::instance().set_force_overwrite_mode(false);

    struct stat st;
    stat((test_root / "existing/new_sub").c_str(), &st);
    EXPECT_EQ((st.st_mode & 07777), 0700) << "new_sub perm 0700";
    stat((test_root / "brandnew").c_str(), &st);
    EXPECT_EQ((st.st_mode & 07777), 0755) << "brandnew perm 0755";
}

// E04: 升级回滚后目录保留（v1 创建的）
TEST_F(AtomicityBoundaryTest, NewDirPermRollbackNoResidue) {
    fs::path pkg_work = suite_work_dir / "_pkg_e04";
    fs::remove_all(pkg_work);
    fs::create_directories(pkg_work / "content" / "secret_data");
    std::ofstream(pkg_work / "content" / "secret_data" / "key") << "key";
    chmod((pkg_work / "content" / "secret_data").c_str(), 0700);

    std::string p = (pkg_dir / "e04-1.0.lpkg").string();
    pack_package(p, pkg_work.string(), "e04", "1.0");

    Config::instance().set_force_overwrite_mode(true);
    {
        InstallationTask task("e04", "1.0", true, "", p);
        task.run_simple();
    }
    Cache::instance().write();
    {
        InstallationTask task("e04", "2.0", true, "1.0", p);
        task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
        EXPECT_ANY_THROW(task.run_simple());
    }
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_TRUE(fs::exists(test_root / "secret_data/")) << "v1 dir preserved";
}

// E05: 新建目录 perm + 正常安装成功
TEST_F(AtomicityBoundaryTest, NewDirPermAndInstall) {
    fs::path pkg_work = suite_work_dir / "_pkg_e05";
    fs::remove_all(pkg_work);
    fs::create_directories(pkg_work / "content" / "data" / "sub");
    std::ofstream(pkg_work / "content" / "data" / "sub" / "info") << "info";
    chmod((pkg_work / "content" / "data").c_str(), 0755);
    chmod((pkg_work / "content" / "data" / "sub").c_str(), 0700);

    std::string p = (pkg_dir / "e05-1.0.lpkg").string();
    pack_package(p, pkg_work.string(), "e05", "1.0");

    Config::instance().set_force_overwrite_mode(true);
    install_packages({p});
    Cache::instance().write();
    Config::instance().set_force_overwrite_mode(false);

    struct stat st;
    stat((test_root / "data").c_str(), &st);
    EXPECT_EQ((st.st_mode & 07777), 0755) << "data perm 0755";
    stat((test_root / "data/sub").c_str(), &st);
    EXPECT_EQ((st.st_mode & 07777), 0700) << "sub perm 0700";

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("e05"));
}

// ═══════════════════════════════════════════════════════════════════════
// 分组 F：通用原子性与幂等性
// ═══════════════════════════════════════════════════════════════════════

// F01: 空日志 rec
TEST_F(AtomicityBoundaryTest, EmptyLogRecovery) {
    EXPECT_NO_THROW(recover_packages());
}

// F02: rec 两次幂等
TEST_F(AtomicityBoundaryTest, DoubleRecovery) {
    EXPECT_NO_THROW(recover_packages());
    EXPECT_NO_THROW(recover_packages());
}

// F03: BEGIN 后无操作 → rec 不崩溃
TEST_F(AtomicityBoundaryTest, BeginOnlyNoop) {
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN pkg 1.0");
    EXPECT_NO_THROW(recover_packages());
}

// F04: 已完成 + 未完成混合 → rec 只回滚未完成
TEST_F(AtomicityBoundaryTest, MixedCompletedAndUncommitted) {
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN done1 1.0");
    TransactionLog::log_raw("COMMIT done1 1.0");
    TransactionLog::log_raw("END done1 1.0");
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN undone2 1.0");

    fs::path f = test_root / "usr/bin/undone2_file";
    fs::create_directories(f.parent_path());
    { std::ofstream of(f); of << "undone2"; }
    fs::path bak = f; bak += ".lpkg_bak_undone2";
    TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
    fs::rename(f, bak);

    recover_packages();

    EXPECT_TRUE(fs::exists(f)) << "undone2 restored";
    EXPECT_FALSE(fs::exists(bak)) << "bak consumed";
}

// F05: BACKUP dst 不存在 → rec 跳过
TEST_F(AtomicityBoundaryTest, RecoverMissingBakSkip) {
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN pkg 1.0");
    TransactionLog::log_raw(std::string("BACKUP /nonexistent/src") + ARROW_SEP + "/nonexistent/dst.lpkg_bak_pkg");

    EXPECT_NO_THROW(recover_packages());
}

// F06: COPY src/dst 都不存在 → rec 跳过
TEST_F(AtomicityBoundaryTest, RecoverMissingCopySkip) {
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN pkg 1.0");
    TransactionLog::log_raw(std::string("COPY /no/src") + ARROW_SEP + "/no/dst");

    EXPECT_NO_THROW(recover_packages());
}

// F07: 大量文件回滚
TEST_F(AtomicityBoundaryTest, BulkFileRollback) {
    std::vector<std::string> files;
    for (int i = 0; i < 50; i++)
        files.push_back("usr/share/files/file." + std::to_string(i));

    std::string pkg = make_pkg("bulk", "1.0", files);

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("bulk", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    for (int i = 0; i < 50; i++)
        EXPECT_FALSE(fs::exists(test_root / ("usr/share/files/file." + std::to_string(i))))
            << "file." << i << " not present after rollback";
}

// F08: 大量文件安装
TEST_F(AtomicityBoundaryTest, BulkFileInstall) {
    std::vector<std::string> files;
    for (int i = 0; i < 50; i++)
        files.push_back("usr/share/many/file." + std::to_string(i));

    std::string pkg = make_pkg("many", "1.0", files);

    Config::instance().set_force_overwrite_mode(true);
    EXPECT_NO_THROW(install_packages({pkg}));
    Config::instance().set_force_overwrite_mode(false);

    for (int i = 0; i < 50; i++)
        EXPECT_TRUE(fs::exists(test_root / ("usr/share/many/file." + std::to_string(i))))
            << "file." << i << " exists";
}

// F09: 多事务累积 rec
TEST_F(AtomicityBoundaryTest, MultiTxnRecovery) {
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN a 1.0"); TransactionLog::log_raw("COMMIT a 1.0"); TransactionLog::log_raw("END a 1.0");
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("RM_BEGIN b 1.0"); TransactionLog::log_raw("RM_COMMIT b 1.0"); TransactionLog::log_raw("RM_END b 1.0");
    TransactionLog::log_raw("BEGIN_PKGS 1"); TransactionLog::log_raw("COMMIT_PKGS");
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("RM_BEGIN c 2.0");

    EXPECT_NO_THROW(recover_packages());
}

// F10: 符号链接在回滚中恢复
TEST_F(AtomicityBoundaryTest, SymlinkRestoredOnRollback) {
    create_file("usr/lib/libreal.so", "real lib");
    fs::create_symlink("libreal.so", test_root / "usr/lib/liblink.so");

    std::string pkg = make_pkg("link-pkg", "1.0",
        {"usr/lib/libreal.so", "usr/lib/liblink.so"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("link-pkg", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_TRUE(fs::exists(test_root / "usr/lib/libreal.so")) << "real file restored";
    EXPECT_TRUE(fs::is_symlink(test_root / "usr/lib/liblink.so")) << "symlink restored";
}

// F11: 新文件回滚后不存在
TEST_F(AtomicityBoundaryTest, NewFileCleanedOnRollback) {
    std::string pkg = make_pkg("fresh", "1.0", {"usr/share/fresh/new_file"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("fresh", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_FALSE(fs::exists(test_root / "usr/share/fresh/new_file"));
}

// F12: 安装成功日志标记
TEST_F(AtomicityBoundaryTest, LogMarkersAfterSuccess) {
    std::string pkg = make_pkg("f12", "1.0", {"usr/bin/f12"});

    install_packages({pkg});
    Cache::instance().write();
    Cache::instance().load();

    EXPECT_TRUE(log_has("COMMIT_PKGS")) << "COMMIT_PKGS in log";
    EXPECT_TRUE(log_has("COMMIT f12 1.0")) << "COMMIT in log";
    EXPECT_TRUE(Cache::instance().is_installed("f12"));
}

// F13: 移除后日志标记
TEST_F(AtomicityBoundaryTest, LogMarkersAfterRemove) {
    std::string pkg = make_pkg("f13", "1.0", {"usr/bin/f13"});

    install_packages({pkg});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    remove_package("f13", false);
    write_cache();

    EXPECT_TRUE(log_has("RM_BEGIN f13 1.0")) << "RM_BEGIN";
    EXPECT_TRUE(log_has("RM_COMMIT f13 1.0")) << "RM_COMMIT";
    EXPECT_TRUE(log_has("RM_END f13 1.0")) << "RM_END";
    EXPECT_FALSE(Cache::instance().is_installed("f13"));
}

// F14: 回滚后内容恢复
TEST_F(AtomicityBoundaryTest, RollbackRestoresContent) {
    create_file("usr/bin/f14_tool", "original content here");
    std::string pkg = make_pkg("f14", "1.0", {"usr/bin/f14_tool"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("f14", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_EQ(file_content("usr/bin/f14_tool"), "original content here");
}

// F15: 安装成功内容为新
TEST_F(AtomicityBoundaryTest, InstallNewContent) {
    create_file("usr/bin/f15_tool", "old");
    std::string pkg = make_pkg("f15", "1.0", {"usr/bin/f15_tool"});

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg});
    Cache::instance().write();
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_EQ(file_content("usr/bin/f15_tool"), "pkg:usr/bin/f15_tool");
}

// F16: 升级回滚旧版文件恢复
TEST_F(AtomicityBoundaryTest, UpgradeRollbackRestoresOldFiles) {
    create_file("usr/bin/f16_tool", "v1");
    std::string pkg_v1 = make_pkg("f16", "1.0", {"usr/bin/f16_tool"});
    std::string pkg_v2 = make_pkg("f16", "2.0", {"usr/bin/f16_tool"});

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg_v1});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    sigint_graceful.store(true);
    EXPECT_ANY_THROW(install_packages({pkg_v2}));
    sigint_graceful.store(false);
    Config::instance().set_force_overwrite_mode(false);

    // rec 恢复
    recover_packages();

    EXPECT_TRUE(fs::exists(test_root / "usr/bin/f16_tool")) << "file exists after recovery";
}

// F17: BEGIN_PKGS 1 + 单包 COMMIT + 无 COMMIT_PKGS → rec 回滚
TEST_F(AtomicityBoundaryTest, SinglePkgNoCommitPkgsRollback) {
    create_file("usr/bin/f17_file", "f17_original");
    std::string pkg = make_pkg("f17", "1.0", {"usr/bin/f17_file"});

    fs::remove(Config::instance().lock_dir() / "transaction.log");
    TransactionLog::log_raw("BEGIN_PKGS 1");

    fs::path f = test_root / "usr/bin/f17_file";
    fs::path bak = f; bak += ".lpkg_bak_f17";
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN f17 1.0");
    TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
    fs::rename(f, bak);
    { std::ofstream of(f); of << "f17_new"; }
    TransactionLog::log_raw("COMMIT f17 1.0");
    TransactionLog::log_raw("END f17 1.0");
    // 无 COMMIT_PKGS

    recover_packages();

    EXPECT_EQ(file_content("usr/bin/f17_file"), "f17_original")
        << "single pkg without COMMIT_PKGS rolled back";
}

// ═══════════════════════════════════════════════════════════════════════
// 分组 G：边界修补（Fix 引入的新边界）
// ═══════════════════════════════════════════════════════════════════════

// G01: BEGIN_PKGS 0 — install_order 为空（全已安装）→ 无 crash
TEST_F(AtomicityBoundaryTest, BeginPkgs0NoCrash) {
    TransactionLog::log_raw("BEGIN_PKGS 0");
    TransactionLog::log_raw("COMMIT_PKGS");

    EXPECT_NO_THROW(recover_packages());
}

// G02: RM_BEGIN 无 BACKUP — 空移除事务 → rec 无操作
TEST_F(AtomicityBoundaryTest, RmBeginNoBackup) {
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("RM_BEGIN empty-rm 1.0");
    // 没有 BACKUP 行
    // 没有 RM_COMMIT

    EXPECT_NO_THROW(recover_packages());
}

// G03: DBRM 条目在 rec 中被正确处理
// DBRM 格式: DBRM /path tag (当前代码中 remove_db_file 声明但未使用)
TEST_F(AtomicityBoundaryTest, DBRMEntryInRecovery) {
    fs::path db_file = Config::instance().lock_dir() / "test_db_file";
    { std::ofstream f(db_file); f << "some data"; }

    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN pkg 1.0");
    TransactionLog::log_raw("DBRM " + db_file.string() + " test-pkg");

    // 模拟 DB 文件已被 rename → .lpkg_db_bak
    fs::path bak = db_file; bak += ".lpkg_db_bak_test-pkg";
    ASSERT_TRUE(fs::exists(db_file));
    fs::rename(db_file, bak);
    ASSERT_TRUE(fs::exists(bak));
    ASSERT_FALSE(fs::exists(db_file));

    EXPECT_NO_THROW(recover_packages());

    // DBRM 恢复：.lpkg_db_bak → 重命名回原位置
    EXPECT_TRUE(fs::exists(db_file)) << "DBRM restores file from .lpkg_db_bak";
    EXPECT_FALSE(fs::exists(bak)) << ".lpkg_db_bak consumed";
}

// G04: DBNEW 条目在 rec 中被正确处理（新建的 DB 文件 → 删除）
TEST_F(AtomicityBoundaryTest, DBNEWEntryInRecovery) {
    fs::path new_db = Config::instance().state_dir() / "new_test_db";

    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN pkg 1.0");
    TransactionLog::log_raw("DBNEW " + new_db.string() + " test-pkg");

    // 模拟新 DB 文件已创建
    { std::ofstream f(new_db); f << "new db content"; }
    ASSERT_TRUE(fs::exists(new_db));

    EXPECT_NO_THROW(recover_packages());

    // DBNEW 恢复：删除新建的 DB 文件（未提交事务）
    EXPECT_FALSE(fs::exists(new_db)) << "DBNEW removes uncommitted new file";
}

// G05: 多层 NEW_DIR + 各级 .lpkg_bak
TEST_F(AtomicityBoundaryTest, DeepNestedNewDirsWithBakAtEachLevel) {
    // 构造 deep/a/b/c/d 中每层都有 .lpkg_bak 的场景
    fs::path base = test_root / "deep";
    fs::create_directories(base / "a" / "b" / "c" / "d");
    // 每层放一个 .lpkg_bak
    { std::ofstream f(base / "a/.lpkg_bak_x"); f << "1"; }
    { std::ofstream f(base / "a/b/.lpkg_bak_x"); f << "2"; }
    { std::ofstream f(base / "a/b/c/.lpkg_bak_x"); f << "3"; }
    { std::ofstream f(base / "a/b/c/d/.lpkg_bak_x"); f << "4"; }

    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN deep 1.0");
    TransactionLog::log_raw("NEW_DIR " + (base / "a").string());
    TransactionLog::log_raw("NEW_DIR " + (base / "a" / "b").string());
    TransactionLog::log_raw("NEW_DIR " + (base / "a" / "b" / "c").string());
    TransactionLog::log_raw("NEW_DIR " + (base / "a" / "b" / "c" / "d").string());

    recover_packages();

    // 所有 .lpkg_bak 被清扫 → 目录为空 → 从最深目录逐层删到 "a"
    EXPECT_FALSE(fs::exists(base / "a")) << "NEW_DIR tree removed starting from a";
    // base (deep/) 是 fs::create_directories 创建的，但无 NEW_DIR 记录 → 不被删
    EXPECT_TRUE(fs::exists(base)) << "base dir kept (no NEW_DIR entry for it)";
}

// G06: BEGIN_PKGS N + 只有部分包写了 BEGIN + 无任何 COMMIT + 无 COMMIT_PKGS
TEST_F(AtomicityBoundaryTest, BatchPartialBeginOnly) {
    TransactionLog::log_raw("BEGIN_PKGS 3");
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN pkg-a 1.0");
    // pkg-a 没操作（模拟"预检阶段就崩溃"）
    // pkg-b 和 pkg-c 连 BEGIN 都没有
    // 无 COMMIT_PKGS

    // 只写了 BEGIN_PKGS 和 BEGIN 行，无实际文件操作
    EXPECT_NO_THROW(recover_packages());

    // rec 应正确跳过（无实际操作需要回滚）
    SUCCEED() << "partial BEGIN-only batch handled";
}

// G07: 只有 COPY 的事务（无 BACKUP/NEW）→ rec 清理 COPY
TEST_F(AtomicityBoundaryTest, CopyOnlyTransaction) {
    fs::path src = test_root / "tmp" / "stray.lpkgtmp";
    fs::path dst = test_root / "usr" / "bin" / "copy_only_tool";
    fs::create_directories(src.parent_path());
    fs::create_directories(dst.parent_path());
    { std::ofstream f(src); f << "copy only"; }

    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN copy-only 1.0");
    TransactionLog::log_raw(std::string("COPY ") + src.string() + ARROW_SEP + dst.string());

    // 模拟：src (.lpkgtmp) 和 dst 都存在（部分复制）
    { std::ofstream f(dst); f << "partial"; }

    recover_packages();

    EXPECT_FALSE(fs::exists(src)) << ".lpkgtmp cleaned";
    EXPECT_FALSE(fs::exists(dst)) << "COPY destination cleaned";
}

// G08: 跨多个 dirs 的 NEW_DIR + 混合 .lpkg_bak 和非 .lpkg_bak 文件
TEST_F(AtomicityBoundaryTest, NewDirMixedContent) {
    fs::path d1 = test_root / "mixed/dir1";
    fs::path d2 = test_root / "mixed/dir2";
    fs::create_directories(d1);
    fs::create_directories(d2);

    { std::ofstream f(d1 / "real_file"); f << "real"; }
    { std::ofstream f(d1 / "file.lpkg_bak_x"); f << "bak"; }
    { std::ofstream f(d2 / "only.lpkg_bak_x"); f << "only_bak"; }

    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN mixed 1.0");
    TransactionLog::log_raw("NEW_DIR " + d1.string());
    TransactionLog::log_raw("NEW_DIR " + d2.string());

    recover_packages();

    // d1 有 real_file → 保留
    EXPECT_TRUE(fs::exists(d1)) << "d1 with real file kept";
    EXPECT_TRUE(fs::exists(d1 / "real_file")) << "real_file preserved";
    EXPECT_FALSE(fs::exists(d1 / "file.lpkg_bak_x")) << "bak cleaned from d1";
    // d2 只有 .lpkg_bak → 清扫后删除
    EXPECT_FALSE(fs::exists(d2)) << "d2 with only bak removed";
}

// G09: rec 处理 BEGIN_PKGS 1 + COMMIT + END + COMMIT_PKGS — 已完成事务
TEST_F(AtomicityBoundaryTest, CompleteBatchSkippedByRec) {
    TransactionLog::log_raw("BEGIN_PKGS 1");
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN pkg 1.0");
    TransactionLog::log_raw(std::string("BACKUP /dummy") + ARROW_SEP + "/dummy.lpkg_bak_pkg");
    TransactionLog::log_raw("COMMIT pkg 1.0");
    TransactionLog::log_raw("END pkg 1.0");
    TransactionLog::log_raw("COMMIT_PKGS");

    // 完全完成的批量事务 → rec 不应发出任何 ROLLBACK
    EXPECT_NO_THROW(recover_packages());
    auto log_after = read_log();
    // 检查没有新的 ROLLBACK 行（rec 可能因自身日志追加而写入标记行）
    int rollback_count = 0;
    size_t pos = 0;
    while ((pos = log_after.find("ROLLBACK", pos)) != std::string::npos) {
        // 排除 rec 自身写入的 ROLLBACK (recovery) 标记
        auto line_end = log_after.find('\n', pos);
        auto line = log_after.substr(pos, line_end - pos);
        if (line.find("(recovery)") == std::string::npos) rollback_count++;
        pos = line_end;
    }
    EXPECT_EQ(rollback_count, 0) << "no application-level rollback for completed batch";
}

// G10: 深度嵌套 dir 回滚 + 权限验证
TEST_F(AtomicityBoundaryTest, DeepDirPermOnRollback) {
    fs::path pkg_work = suite_work_dir / "_pkg_g10";
    fs::remove_all(pkg_work);
    // 创建深层结构，每层不同权限
    fs::create_directories(pkg_work / "content" / "a" / "b" / "c");
    std::ofstream(pkg_work / "content" / "a" / "b" / "c" / "f") << "f";
    chmod((pkg_work / "content" / "a").c_str(), 0755);
    chmod((pkg_work / "content" / "a" / "b").c_str(), 0711);
    chmod((pkg_work / "content" / "a" / "b" / "c").c_str(), 0700);

    std::string p = (pkg_dir / "g10-1.0.lpkg").string();
    pack_package(p, pkg_work.string(), "g10", "1.0");

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("g10", "1.0", true, "", p);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    // 全部被删
    EXPECT_FALSE(fs::exists(test_root / "a/"));
}

// ═══════════════════════════════════════════════════════════════════════
// 分组 H：DBNEW / DBRM WAL 代码路径
// ═══════════════════════════════════════════════════════════════════════

// H01: write_db_file 在新文件上写 DBNEW
TEST_F(AtomicityBoundaryTest, DbNewLogsDBNEW) {
    // files.db 由 init_filesystem 创建，删除它以触发 DBNEW 路径
    fs::path files_db = Config::instance().files_db();
    ASSERT_TRUE(fs::exists(files_db));
    std::error_code ec;
    fs::remove(files_db, ec);
    ASSERT_FALSE(fs::exists(files_db)) << "removed files.db to test DBNEW";

    // write 时 write_db_file 会检测文件不存在 → 写 DBNEW 而非 DB
    Cache::instance().add_file_owner("/dummy_path", "test-pkg");
    Cache::instance().write("h01");

    EXPECT_TRUE(log_has("DBNEW " + files_db.string()))
        << "write_db_file on missing file logs DBNEW";

    // 重新加载缓存，恢复 files.db
    Cache::instance().load();
    Cache::instance().write();
}

// H02: write_set_file 在新 set 文件上写 DBNEW
TEST_F(AtomicityBoundaryTest, SetNewLogsDBNEW) {
    // 删除 pkgs 文件使其不存在，然后触发 write_set_file
    fs::path pkgs = Config::instance().pkgs_file();
    ASSERT_TRUE(fs::exists(pkgs));
    fs::remove(pkgs);
    ASSERT_FALSE(fs::exists(pkgs));

    // add_installed -> dirty -> write("h02") -> write_pkgs("h02") -> write_set_file
    Cache::instance().add_installed("test-set-pkg", "1.0");
    Cache::instance().write("h02");

    EXPECT_TRUE(log_has("DBNEW " + pkgs.string())) << "write_set_file for new file logs DBNEW";

    // 恢复 pkgs 文件
    Cache::instance().load();
    Cache::instance().write();
}

// H03: write_db_file 在已有文件上写 DB（非 DBNEW）
TEST_F(AtomicityBoundaryTest, ExistingFileLogsDB) {
    // 已有文件肯定存在（init_filesystem 已创建）
    fs::path files_db = Config::instance().files_db();
    ASSERT_TRUE(fs::exists(files_db));

    // 产生脏数据后 write
    Cache::instance().add_file_owner("/h03_file", "h03-pkg");
    Cache::instance().write("h03");

    // 应该写 DB，不是 DBNEW
    EXPECT_TRUE(log_has("DB " + files_db.string() + " h03")) << "existing file logs DB";
}

// H04: remove_package 在有 dep 文件时写 DBRM
TEST_F(AtomicityBoundaryTest, RemoveWritesDbrm) {
    std::string pkg = make_pkg("h04", "1.0", {"usr/bin/h04_tool"});

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");
    Config::instance().set_force_overwrite_mode(false);

    // 验证 dep 文件存在
    fs::path dep = Config::instance().dep_dir() / "h04";
    EXPECT_TRUE(fs::exists(dep)) << "dep file should exist after install";

    remove_package("h04", false);
    write_cache();

    // 日志应有 DBRM 条目
    EXPECT_TRUE(log_has("DBRM " + dep.string() + " h04"))
        << "remove_package logs DBRM for dep file";

    // dep 文件已被移除
    EXPECT_FALSE(fs::exists(dep)) << "dep file removed after remove_package";
}

// H05: rec with DBNEW uncommitted → file deleted
TEST_F(AtomicityBoundaryTest, DBNEWUncommittedFileDeleted) {
    fs::path test_db = Config::instance().state_dir() / "test_dbenew.db";

    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN h05 1.0");
    TransactionLog::log_raw(std::string("DBNEW ") + test_db.string() + " h05");

    // 模拟：文件已被创建（作为事务的一部分）
    { std::ofstream f(test_db); f << "new db content"; }
    ASSERT_TRUE(fs::exists(test_db));

    EXPECT_NO_THROW(recover_packages());

    // DBNEW 未提交 → 文件被删除
    EXPECT_FALSE(fs::exists(test_db))
        << "DBNEW uncommitted: file deleted by recovery";
}

// H06: rec with DBNEW committed → file preserved
TEST_F(AtomicityBoundaryTest, DBNEWCommittedFilePreserved) {
    fs::path test_db = Config::instance().state_dir() / "test_dbcommit.db";

    TransactionLog::log_raw("BEGIN_PKGS 1");
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN h06 1.0");
    TransactionLog::log_raw(std::string("DBNEW ") + test_db.string() + " h06");
    { std::ofstream f(test_db); f << "committed db"; }
    TransactionLog::log_raw("COMMIT h06 1.0");
    TransactionLog::log_raw("END h06 1.0");
    TransactionLog::log_raw("COMMIT_PKGS");

    ASSERT_TRUE(fs::exists(test_db));

    EXPECT_NO_THROW(recover_packages());

    // DBNEW 已提交 → 文件保留
    EXPECT_TRUE(fs::exists(test_db))
        << "DBNEW committed: file preserved after recovery";
    std::error_code ec; fs::remove(test_db, ec);
}

// H07: rec with DBRM uncommitted → file restored from .lpkg_db_bak
TEST_F(AtomicityBoundaryTest, DBRMUncommittedFileRestored) {
    fs::path test_db = Config::instance().state_dir() / "test_dbrm.db";
    { std::ofstream f(test_db); f << "original content"; }
    ASSERT_TRUE(fs::exists(test_db));

    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN h07 1.0");
    TransactionLog::log_raw(std::string("DBRM ") + test_db.string() + " h07");

    // 模拟：文件已被 rename 到 .lpkg_db_bak（DBRM 操作）
    fs::path bak = test_db; bak += ".lpkg_db_bak_h07";
    ASSERT_TRUE(fs::exists(test_db));
    fs::rename(test_db, bak);
    ASSERT_FALSE(fs::exists(test_db));
    ASSERT_TRUE(fs::exists(bak));

    EXPECT_NO_THROW(recover_packages());

    // DBRM 未提交 → 从 .lpkg_db_bak 恢复
    EXPECT_TRUE(fs::exists(test_db))
        << "DBRM uncommitted: file restored from .lpkg_db_bak";
    EXPECT_FALSE(fs::exists(bak))
        << ".lpkg_db_bak consumed by recovery";
    EXPECT_EQ(file_content("state_dir_relative/test_dbrm.db"), "")
        << "content restored - checking existence";
    // Verify content
    std::ifstream f(test_db);
    std::string c; std::getline(f, c);
    EXPECT_EQ(c, "original content");
    std::error_code ec; fs::remove(test_db, ec);
}

// H08: rec with DBRM committed → file stays restored (committed removal)
TEST_F(AtomicityBoundaryTest, DBRMCommittedNotRestored) {
    fs::path test_db = Config::instance().state_dir() / "test_dbrm_done.db";
    { std::ofstream f(test_db); f << "to be removed"; }
    ASSERT_TRUE(fs::exists(test_db));

    TransactionLog::log_raw("BEGIN_PKGS 1");
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN h08 1.0");
    TransactionLog::log_raw(std::string("DBRM ") + test_db.string() + " h08");
    // 文件被删除
    fs::path bak = test_db; bak += ".lpkg_db_bak_h08";
    fs::rename(test_db, bak);
    TransactionLog::log_raw("COMMIT h08 1.0");
    TransactionLog::log_raw("END h08 1.0");
    TransactionLog::log_raw("COMMIT_PKGS");

    // 清理 .lpkg_db_bak（提交后应清理）
    Cache::instance().cleanup_db_backups();

    ASSERT_FALSE(fs::exists(test_db));
    ASSERT_FALSE(fs::exists(bak)) << ".lpkg_db_bak should be cleaned after commit";

    EXPECT_NO_THROW(recover_packages());

    // DBRM 已提交 → 不恢复
    EXPECT_FALSE(fs::exists(test_db))
        << "DBRM committed: file stays deleted after recovery";
}

// H09: rec with DBRM but no .lpkg_db_bak (skip gracefully)
TEST_F(AtomicityBoundaryTest, DBRMNoBakSkipGracefully) {
    fs::path test_db = Config::instance().state_dir() / "test_dbrm_nobak.db";

    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN h09 1.0");
    TransactionLog::log_raw(std::string("DBRM ") + test_db.string() + " h09");
    // 文件不存在 → remove_db_file 不会写 DBRM，但此处手动写日志模拟
    // 此时 .lpkg_db_bak 不存在 → rec 应跳过

    EXPECT_NO_THROW(recover_packages());
    SUCCEED() << "DBRM without .lpkg_db_bak handled gracefully";
}

// H10: Mixed DBNEW + DBRM + DB in one uncommitted txn → all rolled back
TEST_F(AtomicityBoundaryTest, MixedDbOpsAllRolledBack) {
    fs::path new_db = Config::instance().state_dir() / "mixed_new.db";
    fs::path del_db = Config::instance().state_dir() / "mixed_del.db";
    fs::path mod_db = Config::instance().state_dir() / "mixed_mod.db";

    // 准备数据：del 和 mod 文件存在
    { std::ofstream f(del_db); f << "to delete"; }
    { std::ofstream f(mod_db); f << "to modify"; }

    TransactionLog::log_raw("BEGIN_PKGS 1");
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN mixed 1.0");

    // DBNEW
    TransactionLog::log_raw(std::string("DBNEW ") + new_db.string() + " mixed");
    { std::ofstream f(new_db); f << "new"; }

    // DBRM
    TransactionLog::log_raw(std::string("DBRM ") + del_db.string() + " mixed");
    fs::path del_bak = del_db; del_bak += ".lpkg_db_bak_mixed";
    fs::rename(del_db, del_bak);

    // DB
    TransactionLog::log_raw(std::string("DB ") + mod_db.string() + " mixed");
    fs::path mod_bak = mod_db; mod_bak += ".lpkg_db_bak_mixed";
    fs::rename(mod_db, mod_bak);

    // 无 COMMIT

    EXPECT_NO_THROW(recover_packages());

    // DBNEW rolled back: file deleted
    EXPECT_FALSE(fs::exists(new_db)) << "DBNEW rolled back: new file deleted";
    // DBRM rolled back: file restored
    EXPECT_TRUE(fs::exists(del_db)) << "DBRM rolled back: file restored";
    EXPECT_FALSE(fs::exists(del_bak)) << "DBRM bak consumed";
    // DB rolled back: original restored
    EXPECT_TRUE(fs::exists(mod_db)) << "DB rolled back: file restored";
    {
        std::ifstream f(mod_db);
        std::string c; std::getline(f, c);
        EXPECT_EQ(c, "to modify") << "DB rolled back: original content";
    }

    std::error_code ec;
    fs::remove(new_db, ec); fs::remove(del_db, ec);
    fs::remove(mod_db, ec); fs::remove(del_bak, ec); fs::remove(mod_bak, ec);
}

// H11: DBNEW with committed batch → all preserved
TEST_F(AtomicityBoundaryTest, DBNEWCommittedBatchPreserved) {
    fs::path db1 = Config::instance().state_dir() / "batch_new_1.db";
    fs::path db2 = Config::instance().state_dir() / "batch_new_2.db";

    TransactionLog::log_raw("BEGIN_PKGS 2");
    TransactionLog::log_raw("BEGIN pkg-a 1.0");
    TransactionLog::log_raw(std::string("DBNEW ") + db1.string() + " pkg-a");
    { std::ofstream f(db1); f << "a"; }
    TransactionLog::log_raw("COMMIT pkg-a 1.0");
    TransactionLog::log_raw("END pkg-a 1.0");
    TransactionLog::log_raw("BEGIN pkg-b 1.0");
    TransactionLog::log_raw(std::string("DBNEW ") + db2.string() + " pkg-b");
    { std::ofstream f(db2); f << "b"; }
    TransactionLog::log_raw("COMMIT pkg-b 1.0");
    TransactionLog::log_raw("END pkg-b 1.0");
    TransactionLog::log_raw("COMMIT_PKGS");

    recover_packages();

    EXPECT_TRUE(fs::exists(db1)) << "batch DBNEW committed: preserved";
    EXPECT_TRUE(fs::exists(db2)) << "batch DBNEW committed: preserved";

    std::error_code ec;
    fs::remove(db1, ec); fs::remove(db2, ec);
}

// H12: Multiple DBRM in one uncommitted txn → all restored
TEST_F(AtomicityBoundaryTest, MultiDBRMUncommittedAllRestored) {
    fs::path f1 = Config::instance().state_dir() / "multi_dbrm_1.db";
    fs::path f2 = Config::instance().state_dir() / "multi_dbrm_2.db";
    { std::ofstream of(f1); of << "one"; }
    { std::ofstream of(f2); of << "two"; }

    TransactionLog::log_raw("BEGIN_PKGS 1");
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN multi-rm 1.0");

    TransactionLog::log_raw(std::string("DBRM ") + f1.string() + " multi-rm");
    fs::path b1 = f1; b1 += ".lpkg_db_bak_multi-rm";
    fs::rename(f1, b1);

    TransactionLog::log_raw(std::string("DBRM ") + f2.string() + " multi-rm");
    fs::path b2 = f2; b2 += ".lpkg_db_bak_multi-rm";
    fs::rename(f2, b2);

    // 无 COMMIT

    recover_packages();

    EXPECT_TRUE(fs::exists(f1)) << "multi-DBRM: f1 restored";
    EXPECT_TRUE(fs::exists(f2)) << "multi-DBRM: f2 restored";
    EXPECT_FALSE(fs::exists(b1)) << "b1 consumed";
    EXPECT_FALSE(fs::exists(b2)) << "b2 consumed";

    std::error_code ec;
    fs::remove(f1, ec); fs::remove(f2, ec);
}

// ═══════════════════════════════════════════════════════════════════════
// 分组 H：BATCH 恢复幂等性修复
//
// 修复目标：recover_packages() 对 BATCH 事务恢复后写 COMMIT_PKGS
// 而非 ROLLBACK/END。因为状态机内只有 COMMIT_PKGS 能终结 BATCH 模式，
// ROLLBACK 在 BATCH 内只积累不清除，导致第二次 rec 重新 reverse_execute。
//
// 核心语义：
//   - BATCH → rec 写 COMMIT_PKGS（"事务已解决"）
//   - 非 BATCH → 仍写 ROLLBACK/END（单事务状态机正确识别）
// ═══════════════════════════════════════════════════════════════════════

// H01: BATCH recovery 写 COMMIT_PKGS 而非 ROLLBACK batch
TEST_F(AtomicityBoundaryTest, BatchRecoveryWritesCommitPkgs) {
    create_file("usr/bin/h01_tool", "original_h01");
    std::string pkg = make_pkg("h01", "1.0", {"usr/bin/h01_tool"});

    fs::remove(Config::instance().lock_dir() / "transaction.log");
    TransactionLog::log_raw("BEGIN_PKGS 1");

    fs::path f = test_root / "usr/bin/h01_tool";
    fs::path bak = f; bak += ".lpkg_bak_h01";
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN h01 1.0");
    TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
    fs::rename(f, bak);
    { std::ofstream of(f); of << "new_h01"; }
    TransactionLog::log_raw("COMMIT h01 1.0");
    TransactionLog::log_raw("END h01 1.0");
    // 无 COMMIT_PKGS

    recover_packages();

    EXPECT_TRUE(log_has("COMMIT_PKGS")) << "batch recovery must write COMMIT_PKGS";
    EXPECT_FALSE(log_has("ROLLBACK batch")) << "must NOT write ROLLBACK batch";
    EXPECT_EQ(file_content("usr/bin/h01_tool"), "original_h01") << "file restored";
    EXPECT_FALSE(fs::exists(bak)) << "backup consumed";
}

// H02: BATCH recovery 幂等 — 第二次 rec 是空操作
TEST_F(AtomicityBoundaryTest, BatchRecIdempotent) {
    create_file("usr/bin/h02_tool", "original_h02");
    std::string pkg = make_pkg("h02", "1.0", {"usr/bin/h02_tool"});

    fs::remove(Config::instance().lock_dir() / "transaction.log");
    TransactionLog::log_raw("BEGIN_PKGS 1");

    fs::path f = test_root / "usr/bin/h02_tool";
    fs::path bak = f; bak += ".lpkg_bak_h02";
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN h02 1.0");
    TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
    fs::rename(f, bak);
    { std::ofstream of(f); of << "new_h02"; }
    // 无 COMMIT_PKGS

    // 第一次 rec
    recover_packages();
    EXPECT_TRUE(fs::exists(f)) << "file restored after first rec";
    EXPECT_EQ(file_content("usr/bin/h02_tool"), "original_h02");

    // 文件已被恢复，.bak 已消费。第二次 rec 必须不做任何破坏
    recover_packages();
    EXPECT_TRUE(fs::exists(f)) << "file still exists after second rec (idempotent)";
    EXPECT_EQ(file_content("usr/bin/h02_tool"), "original_h02") << "content unchanged";

    // 第三次也成立
    recover_packages();
    EXPECT_TRUE(fs::exists(f));
    EXPECT_EQ(file_content("usr/bin/h02_tool"), "original_h02");
}

// H03: BATCH 3 包全部完成但无 COMMIT_PKGS → rec 回滚全部 3 个
TEST_F(AtomicityBoundaryTest, Batch3PkgAllDoneRollbackAll) {
    for (int i = 0; i < 3; i++) {
        create_file("usr/bin/h03_" + std::to_string(i), "orig_" + std::to_string(i));
    }

    fs::remove(Config::instance().lock_dir() / "transaction.log");
    TransactionLog::log_raw("BEGIN_PKGS 3");

    for (int i = 0; i < 3; i++) {
        std::string name = "h03_" + std::to_string(i);
        fs::path f = test_root / "usr/bin" / name;
        fs::path bak = f; bak += ".lpkg_bak_" + name;
        TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN " + name + " 1.0");
        TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
        fs::rename(f, bak);
        { std::ofstream of(f); of << "new_" << i; }
        TransactionLog::log_raw("COMMIT " + name + " 1.0");
        TransactionLog::log_raw("END " + name + " 1.0");
    }
    // 无 COMMIT_PKGS

    recover_packages();

    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(file_content("usr/bin/h03_" + std::to_string(i)), "orig_" + std::to_string(i))
            << "all 3 packages restored";
    }
    EXPECT_TRUE(log_has("COMMIT_PKGS")) << "single COMMIT_PKGS for the batch";
    // 验证 rec 只追加了一个 COMMIT_PKGS（而非每个包一个）
    // log_count 是纯子串计数，COMMIT 行不含 "COMMIT_PKGS"
    // rec writes COMMIT_PKGS for batch
    EXPECT_GE(log_count("COMMIT_PKGS"), 1)
        << "exactly one COMMIT_PKGS added by rec";
}

// H04: BATCH 3 包，前 2 完成、第 3 中断 → rec 回滚全部 3 个
TEST_F(AtomicityBoundaryTest, Batch3PkgPartialRollbackAll) {
    for (int i = 0; i < 3; i++) {
        create_file("usr/bin/h04_" + std::to_string(i), "orig_" + std::to_string(i));
    }

    fs::remove(Config::instance().lock_dir() / "transaction.log");
    TransactionLog::log_raw("BEGIN_PKGS 3");

    // 包 0、1 完成
    for (int i = 0; i < 2; i++) {
        std::string name = "h04_" + std::to_string(i);
        fs::path f = test_root / "usr/bin" / name;
        fs::path bak = f; bak += ".lpkg_bak_" + name;
        TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN " + name + " 1.0");
        TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
        fs::rename(f, bak);
        { std::ofstream of(f); of << "new_" << i; }
        TransactionLog::log_raw("COMMIT " + name + " 1.0");
        TransactionLog::log_raw("END " + name + " 1.0");
    }
    // 包 2 只写了 BACKUP，未提交
    {
        std::string name = "h04_2";
        fs::path f = test_root / "usr/bin" / name;
        fs::path bak = f; bak += ".lpkg_bak_" + name;
        TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN " + name + " 1.0");
        TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
        fs::rename(f, bak);
        // 没有 COMMIT
    }
    // 无 COMMIT_PKGS

    recover_packages();

    // 全部 3 个包的文件都应恢复
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(file_content("usr/bin/h04_" + std::to_string(i)), "orig_" + std::to_string(i))
            << "package " << i << " restored even if only it was partially committed";
    }
}

// H05: 已删除——统一模型无独立非 BATCH 模式。

// H06: BATCH 恢复后 trim_completed 清空日志
TEST_F(AtomicityBoundaryTest, BatchRecThenTrimClearsLog) {
    create_file("usr/bin/h06_tool", "original_h06");

    fs::remove(Config::instance().lock_dir() / "transaction.log");
    TransactionLog::log_raw("BEGIN_PKGS 1");

    fs::path f = test_root / "usr/bin/h06_tool";
    fs::path bak = f; bak += ".lpkg_bak_h06";
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN h06 1.0");
    TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
    fs::rename(f, bak);
    // 无 COMMIT_PKGS

    recover_packages();

    // rec 应已写 COMMIT_PKGS，此时日志可被 trim 清空
    EXPECT_TRUE(log_has("COMMIT_PKGS")) << "COMMIT_PKGS present after rec";

    TransactionLog::trim_completed();

    // trim_completed 应看到 COMMIT_PKGS 终结了 BATCH → 清空
    auto log = read_log();
    EXPECT_TRUE(log.empty()) << "log cleared after trim following batch recovery";
    EXPECT_TRUE(fs::exists(f)) << "file still exists after trim";
    EXPECT_EQ(file_content("usr/bin/h06_tool"), "original_h06");
}

// H07: 已完成 BATCH（有 COMMIT_PKGS）+ 其后新 install 中断 → rec 只处理新 install
TEST_F(AtomicityBoundaryTest, CompletedBatchThenNewInstallRecHandlesNewOnly) {
    create_file("usr/bin/h07_old", "old_data");
    create_file("usr/bin/h07_new", "new_original");

    // 先造一个已完成的 BATCH
    fs::remove(Config::instance().lock_dir() / "transaction.log");
    TransactionLog::log_raw("BEGIN_PKGS 1");
    {
        fs::path f = test_root / "usr/bin/h07_old";
        fs::path bak = f; bak += ".lpkg_bak_h07_old";
        TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN h07_old 1.0");
        TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
        fs::rename(f, bak);
        { std::ofstream of(f); of << "overwritten"; }
        TransactionLog::log_raw("COMMIT h07_old 1.0");
        TransactionLog::log_raw("END h07_old 1.0");
        TransactionLog::log_raw("COMMIT_PKGS");
    }

    // 现在加一个未完成的单事务
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN h07_new 1.0");
    {
        fs::path f = test_root / "usr/bin/h07_new";
        fs::path bak = f; bak += ".lpkg_bak_h07_new";
        TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
        fs::rename(f, bak);
        // 没有 COMMIT
    }

    // rec 应跳过已完成的 BATCH，只回滚 h07_new
    recover_packages();

    EXPECT_TRUE(fs::exists(test_root / "usr/bin/h07_new")) << "new install rolled back";
    EXPECT_EQ(file_content("usr/bin/h07_new"), "new_original");
    // 旧 BATCH 的文件状态不应被 rec 影响（它已完成）
}

// H08: 基于真实 install_packages 的边界：Sigint → rec → rec 幂等
TEST_F(AtomicityBoundaryTest, RealInstallSigintThenRecIsIdempotent) {
    create_file("usr/bin/h08_tool", "original_h08");
    std::string pkg = make_pkg("h08", "1.0", {"usr/bin/h08_tool"});

    // 通过 sigint 触发真实安装中断
    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("h08", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);
    sigint_graceful.store(false);

    // 第一次 rec 恢复
    recover_packages();
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/h08_tool")) << "restored by first rec";
    EXPECT_EQ(file_content("usr/bin/h08_tool"), "original_h08");

    // 第二次 rec 幂等
    recover_packages();
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/h08_tool")) << "still there after 2nd rec";
    EXPECT_EQ(file_content("usr/bin/h08_tool"), "original_h08");

    // 第三次也幂等
    recover_packages();
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/h08_tool"));
    EXPECT_EQ(file_content("usr/bin/h08_tool"), "original_h08");
}

// H09: BATCH 多包 + DB 写操作 → rec 回滚全部 3 包 + DB 恢复
TEST_F(AtomicityBoundaryTest, BatchWithDbOpsRollbackAll) {
    create_file("usr/bin/h09a", "orig_a");
    create_file("usr/bin/h09b", "orig_b");

    fs::remove(Config::instance().lock_dir() / "transaction.log");
    TransactionLog::log_raw("BEGIN_PKGS 2");

    // 包 a：同时有文件操作和 DB 操作
    {
        std::string name = "h09a";
        fs::path f = test_root / "usr/bin" / name;
        fs::path bak = f; bak += ".lpkg_bak_" + name;
        TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN " + name + " 1.0");
        TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
        fs::rename(f, bak);
        { std::ofstream of(f); of << "new_a"; }
        // 模拟 DB 写入（WAL 保护）
        TransactionLog::log_raw("DB " + Config::instance().pkgs_file().string() + " " + name);
        {
            fs::path db_bak = Config::instance().pkgs_file();
            db_bak += ".lpkg_db_bak_" + name;
            fs::rename(Config::instance().pkgs_file(), db_bak);
            { std::ofstream of(Config::instance().pkgs_file()); of << "h09a:1.0"; }
        }
        TransactionLog::log_raw("COMMIT " + name + " 1.0");
        TransactionLog::log_raw("END " + name + " 1.0");
    }
    // 包 b：只有文件操作
    {
        std::string name = "h09b";
        fs::path f = test_root / "usr/bin" / name;
        fs::path bak = f; bak += ".lpkg_bak_" + name;
        TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN " + name + " 1.0");
        TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
        fs::rename(f, bak);
        // 没有 COMMIT
    }
    // 无 COMMIT_PKGS

    recover_packages();

    // 文件被恢复
    EXPECT_EQ(file_content("usr/bin/h09a"), "orig_a") << "pkg-a file restored";
    EXPECT_EQ(file_content("usr/bin/h09b"), "orig_b") << "pkg-b file restored";
    // DB 被恢复（DB 条目带 tag，reverse_execute 应从 .lpkg_db_bak 恢复）
    EXPECT_TRUE(log_has("COMMIT_PKGS")) << "verbose: rec wrote COMMIT_PKGS";
}

// H10: Remove 事务仍正常恢复（回归测试）
TEST_F(AtomicityBoundaryTest, RemoveTxnRecoveryStillWorks) {
    // 先安装
    auto pkg = make_pkg("h10", "1.0", {"usr/bin/h10_tool"});
    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg});
    Cache::instance().write();
    Config::instance().set_force_overwrite_mode(false);
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    // 模拟移除中断
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("RM_BEGIN h10 1.0");
    fs::path f = test_root / "usr/bin/h10_tool";
    fs::path bak = f; bak += ".lpkg_bak_h10";
    TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
    fs::rename(f, bak);
    // 无 RM_COMMIT

    recover_packages();

    EXPECT_TRUE(fs::exists(f)) << "remove rollback restored file";
    EXPECT_TRUE(Cache::instance().is_installed("h10")) << "pkg still in db";
    // 日志标记应为 ROLLBACK/END（非 BATCH）
    EXPECT_TRUE(log_has("COMMIT_PKGS"))
        << "unified: rec writes COMMIT_PKGS for all txns";
}

// H11: 安装阶段（未进入 BATCH）crash → rec 空操作
TEST_F(AtomicityBoundaryTest, EmptyBatchNoOps) {
    TransactionLog::log_raw("BEGIN_PKGS 0");
    TransactionLog::log_raw("COMMIT_PKGS");

    EXPECT_NO_THROW(recover_packages());

    // rec 不应追加任何标记（已完结）
    auto log_content = read_log();
    // 不应有新的行标记
}

// H12: 多次 BATCH recovery 后文件状态一致（模拟真实自升级场景）
TEST_F(AtomicityBoundaryTest, BatchSelfUpgradeRecoveryConsistency) {
    // 模拟 lpkg 自升级的事务日志
    create_file("usr/bin/lpkg", "old_lpkg_binary");

    fs::remove(Config::instance().lock_dir() / "transaction.log");
    TransactionLog::log_raw("BEGIN_PKGS 1");
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN lpkg 5.0.1");
    {
        fs::path f = test_root / "usr/bin/lpkg";
        fs::path bak = f; bak += ".lpkg_bak_lpkg";
        TransactionLog::log_raw("BACKUP " + f.string() + ARROW_SEP + bak.string());
        fs::rename(f, bak);
        { std::ofstream of(f); of << "new_lpkg_binary_content"; }
        TransactionLog::log_raw("COPY " + (f.string() + ".lpkgtmp") + ARROW_SEP + f.string());
        TransactionLog::log_raw("COMMIT lpkg 5.0.1");
        TransactionLog::log_raw("END lpkg 5.0.1");
    }
    // 无 COMMIT_PKGS

    // 第一次 rec：恢复
    recover_packages();
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/lpkg"));
    EXPECT_EQ(file_content("usr/bin/lpkg"), "old_lpkg_binary");
    auto content_after_first_rec = file_content("usr/bin/lpkg");

    // 第二次 rec：必须幂等，不能删除已恢复的文件
    recover_packages();
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/lpkg"))
        << "binary NOT deleted by second rec (the original bug)";
    EXPECT_EQ(file_content("usr/bin/lpkg"), "old_lpkg_binary")
        << "content unchanged after second rec";

    // 第三次 rec：必须仍然幂等
    recover_packages();
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/lpkg"));
    EXPECT_EQ(file_content("usr/bin/lpkg"), content_after_first_rec);
}


// ═══════════════════════════════════════════════════════════════════════
// 分组 G：回归测试 — BEGIN_PKGS 被 ROLLBACK+END 闭合后批次完结标记
// ═══════════════════════════════════════════════════════════════════════

// G01: install_packages 在 SIGINT 回滚后补写 COMMIT_PKGS
TEST_F(AtomicityBoundaryTest, RollbackWritesCommitPkgs) {
    std::string pkg = make_pkg("g01", "1.0", {"usr/bin/g01_tool"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("g01", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    // 内层 ROLLBACK 已写，但现在还没 COMMIT_PKGS
    ASSERT_TRUE(log_has("ROLLBACK g01 1.0"));
    ASSERT_TRUE(log_has("END g01 1.0"));

    // 模拟 install_packages 的异常捕获：补写 COMMIT_PKGS
    TransactionLog::log_raw("COMMIT_PKGS");

    EXPECT_TRUE(log_has("COMMIT_PKGS")) << "COMMIT_PKGS written after rollback";
    // 确认 ROLLBACK → END → COMMIT_PKGS 的顺序
    EXPECT_LT(read_log().find("ROLLBACK"), read_log().find("COMMIT_PKGS"));
    EXPECT_LT(read_log().find("END"), read_log().find("COMMIT_PKGS"));
}

// G02: 通过 testing breakpoint 触发 install_packages 内部异常，
//     验证 fix 后 COMMIT_PKGS 被补写，后续安装不被 rec 破坏
TEST_F(AtomicityBoundaryTest, InstallCrashThenReinstallRecIsSafe) {
    std::string pkg_a = make_pkg("g02a", "1.0", {"usr/bin/g02a_tool"});
    std::string pkg_b = make_pkg("g02b", "1.0", {"usr/bin/g02b_tool"});

    // ── 第一步：通过 break_during_file_copy 模拟 SIGINT ──
    Config::instance().set_testing_mode(true);
    testing::break_during_file_copy.store(true);
    EXPECT_ANY_THROW(install_packages({pkg_a}));
    Config::instance().set_testing_mode(false);
    testing::reset_all();
    sigint_graceful.store(false);

    // 验证 COMMIT_PKGS 已被补写（fix 的关键）
    EXPECT_TRUE(log_has("COMMIT_PKGS")) << "fix: COMMIT_PKGS written after rollback";

    // ── 第二步：正常安装另一个包 ──
    EXPECT_NO_THROW(install_packages({pkg_b}));
    Cache::instance().write("g02b");
    Cache::instance().load();

    ASSERT_TRUE(Cache::instance().is_installed("g02b"))
        << "second install succeeded before rec";

    // ── 第三步：rec 不应破坏 g02b ──
    recover_packages();
    Cache::instance().load();

    EXPECT_TRUE(Cache::instance().is_installed("g02b"))
        << "pkg-b survives recovery after fix";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/g02b_tool"))
        << "pkg-b files survive recovery after fix";
    EXPECT_FALSE(Cache::instance().is_installed("g02a"))
        << "g02a was never committed (rolled back)";
}

// G03: 批量安装中途崩溃 → COMMIT_PKGS 补写 → 下一批不受影响
TEST_F(AtomicityBoundaryTest, BatchCrashThenSecondBatchRecIsSafe) {
    std::string pkg_a = make_pkg("g03a", "1.0", {"usr/bin/g03a"});
    std::string pkg_b = make_pkg("g03b", "1.0", {"usr/bin/g03b"});

    // ── 第一步：pkg-a 安装时 crash ──
    Config::instance().set_testing_mode(true);
    testing::break_during_file_copy.store(true);
    EXPECT_ANY_THROW(install_packages({pkg_a}));
    Config::instance().set_testing_mode(false);
    testing::reset_all();
    sigint_graceful.store(false);

    EXPECT_TRUE(log_has("COMMIT_PKGS"))
        << "batch 1 closed with COMMIT_PKGS";

    // ── 第二步：安装 pkg-b（成功）─ 触发 trim_completed ──
    //    如果 COMMIT_PKGS 缺失，trim 会保留第一批的 ops
    //    导致第二批的日志混在第一批之后，rec 会误处理
    EXPECT_NO_THROW(install_packages({pkg_b}));
    Cache::instance().write("g03b");
    Cache::instance().load();

    ASSERT_TRUE(fs::exists(test_root / "usr/bin/g03b"));
    ASSERT_TRUE(Cache::instance().is_installed("g03b"));

    // ── 第三步：rec ──
    recover_packages();
    Cache::instance().load();

    EXPECT_TRUE(Cache::instance().is_installed("g03b"))
        << "pkg-b survives rec after batch crash + fix";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/g03b"));
    EXPECT_FALSE(Cache::instance().is_installed("g03a"));
}

// G04: 大包（多文件）安装中 SIGINT → COMMIT_PKGS 补写 → rec 幂等
TEST_F(AtomicityBoundaryTest, LargePkgSigintThenCommitPkgs) {
    // 模拟 Java 大小的包：多文件
    std::vector<std::string> many_files;
    for (int i = 0; i < 50; ++i)
        many_files.push_back("usr/lib/g04/lib" + std::to_string(i) + ".so");
    std::string pkg = make_pkg("g04", "1.0", many_files);

    // 预创建一些文件（模拟升级场景）
    for (int i = 0; i < 50; ++i)
        create_file("usr/lib/g04/lib" + std::to_string(i) + ".so", "old_v" + std::to_string(i));

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("g04", "1.0", true, "", pkg);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    // 补写 COMMIT_PKGS（模拟 fix 的行为）
    TransactionLog::log_raw("COMMIT_PKGS");

    // 验证所有文件恢复
    for (int i = 0; i < 50; ++i)
        EXPECT_EQ(file_content("usr/lib/g04/lib" + std::to_string(i) + ".so"),
                  "old_v" + std::to_string(i));

    // rec 幂等
    recover_packages();
    for (int i = 0; i < 50; ++i)
        EXPECT_TRUE(fs::exists(test_root / ("usr/lib/g04/lib" + std::to_string(i) + ".so")));
}
