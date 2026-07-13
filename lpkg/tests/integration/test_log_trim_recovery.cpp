#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/pkg/transaction_log.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/archive/packer.hpp"
#include "../test_base.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <atomic>

namespace fs = std::filesystem;

extern std::atomic<bool> sigint_graceful;

/**
 * 测试套件：日志压缩 + 真实安装/移除/恢复的集成测试
 *
 * 验证 trim 在实际工作流中不破坏事务完整性：
 *   - 安装成功后 trim → 日志清空
 *   - 安装中途中断后 trim → 日志保留，rec 能恢复
 *   - 恢复后 trim → 日志可安全清空
 */
class LogTrimIntegrationTest : public IntegrationTestBase {
protected:
    fs::path log_path;

    void SetUp() override {
        IntegrationTestBase::SetUp();
        log_path = Config::instance().lock_dir() / "transaction.log";
    }

    void TearDown() override {
        IntegrationTestBase::TearDown();
    }

    std::vector<std::string> read_log() {
        if (!fs::exists(log_path)) return {};
        std::ifstream f(log_path);
        std::vector<std::string> result;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty()) {
                auto ts_end = line.find(']');
                if (ts_end != std::string::npos && ts_end + 2 < line.size())
                    result.push_back(line.substr(ts_end + 2));
                else
                    result.push_back(line);
            }
        }
        return result;
    }

    bool log_has(const std::string& text) {
        auto c = read_log();
        for (const auto& l : c)
            if (l.find(text) != std::string::npos) return true;
        return false;
    }

    int log_count() { return static_cast<int>(read_log().size()); }

    std::string make_test_pkg(const std::string& name, const std::string& ver,
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

    // ── 辅助工具（同 AtomicityBoundaryTest） ─────────────────────────
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
};

// ── T01: install → trim → log empty ────────────────────────────────────

TEST_F(LogTrimIntegrationTest, InstallThenTrimLogEmpty) {
    std::string pkg = make_test_pkg("t01", "1.0", {"usr/bin/t01"});
    install_packages({pkg});
    Cache::instance().write("t01");
    Cache::instance().load();

    // 安装完成后日志有内容（COMMIT_PKGS 等）
    ASSERT_TRUE(log_has("COMMIT_PKGS")) << "install wrote to log";

    // trim 应在下一任务开始时自动被调用
    // 手动验证 trim 会清除已完结事务
    TransactionLog::trim_completed();

    auto c = read_log();
    EXPECT_TRUE(c.empty()) << "trim after successful install clears log";

    EXPECT_TRUE(Cache::instance().is_installed("t01")) << "pkg still installed";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/t01")) << "files still present";
}

// ── T02: remove → trim → log empty ────────────────────────────────────

TEST_F(LogTrimIntegrationTest, RemoveThenTrimLogEmpty) {
    std::string pkg = make_test_pkg("t02", "1.0", {"usr/bin/t02"});
    install_packages({pkg});
    Cache::instance().write("t02");
    Cache::instance().load();
    fs::remove(log_path);

    remove_package("t02", false);
    write_cache();

    EXPECT_TRUE(log_has("RM_END t02 1.0")) << "remove completed in log";

    TransactionLog::trim_completed();
    EXPECT_TRUE(read_log().empty()) << "trim after remove clears log";
    EXPECT_FALSE(Cache::instance().is_installed("t02"));
}

// ── T03: crash 后（无 ROLLBACK/END）→ trim 保留 BEGIN → rec 能恢复 ──
// 模拟真正的"断电式"crash——ROLLBACK+END 未写出，只有 BEGIN+BACKUP。
// InstallationTask::run() 在异常时会写 ROLLBACK+END（非真正 crash），
// 因此这里直接用 log_raw 模拟"写了一半就没电了"的场景。

TEST_F(LogTrimIntegrationTest, CrashWithoutRollbackThenTrimPreservesLog) {
    create_file("usr/bin/t03", "original");
    std::string pkg = make_test_pkg("t03", "1.0", {"usr/bin/t03"});

    // 模拟：备份完成但 crash，未写 ROLLBACK/END
    fs::remove(log_path);
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN t03 1.0");
    {
        fs::path f = test_root / "usr/bin/t03";
        fs::path bak = f; bak += ".lpkg_bak_t03";
        TransactionLog::log_raw("BACKUP " + f.string() + " → " + bak.string());
        fs::rename(f, bak);
    }
    ASSERT_FALSE(fs::exists(test_root / "usr/bin/t03")) << "file was renamed";

    // trim 保留未完成事务
    TransactionLog::trim_completed();
    EXPECT_TRUE(log_has("BACKUP")) << "backup line preserved after trim";
    EXPECT_TRUE(log_has("BEGIN t03")) << "BEGIN preserved after trim";

    // rec 能恢复
    recover_packages();

    EXPECT_EQ(file_content("usr/bin/t03"), "original") << "recovery restored file";
    EXPECT_FALSE(Cache::instance().is_installed("t03")) << "not in db after rollback";

    // 恢复后 ROLLBACK+END (recovery) 标记 → trim 清空
    TransactionLog::trim_completed();
    EXPECT_TRUE(read_log().empty()) << "trim after recovery clears markers";
}

// ── T04: 批量安装中断 → trim 保留整个 batch → rec 恢复全部 → trim 清空 ──

TEST_F(LogTrimIntegrationTest, BatchCrashTrimPreservesFullBatch) {
    create_file("usr/bin/t04a", "orig_a");
    create_file("usr/bin/t04b", "orig_b");
    std::string pkg_a = make_test_pkg("t04a", "1.0", {"usr/bin/t04a"});
    std::string pkg_b = make_test_pkg("t04b", "1.0", {"usr/bin/t04b"});

    // 手动模拟批量事务：pkg-a 完成，pkg-b 未完成，无 COMMIT_PKGS
    fs::remove(log_path);
    TransactionLog::log_raw("BEGIN_PKGS 2");
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN t04a 1.0");
    {
        fs::path f = test_root / "usr/bin/t04a";
        fs::path b = f; b += ".lpkg_bak_t04a";
        TransactionLog::log_raw("BACKUP " + f.string() + " → " + b.string());
        fs::rename(f, b);
        { std::ofstream of(f); of << "new_a"; }
        TransactionLog::log_raw("COMMIT t04a 1.0");
        TransactionLog::log_raw("END t04a 1.0");
    }
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("BEGIN t04b 1.0");
    {
        fs::path f = test_root / "usr/bin/t04b";
        fs::path b = f; b += ".lpkg_bak_t04b";
        TransactionLog::log_raw("BACKUP " + f.string() + " → " + b.string());
        fs::rename(f, b);
    }
    // 无 COMMIT_PKGS

    // trim 应该保留全部（整个批量未提交）
    TransactionLog::trim_completed();
    EXPECT_TRUE(log_has("BEGIN_PKGS 2"))
        << "trim preserves entire uncommitted batch";
    // 路径名包含 t04b（而非 "/usr/bin/t04b"），因为 log_raw 用的绝对路径
    EXPECT_TRUE(log_has(".lpkg_bak_t04b"))
        << "backup line preserved";
    EXPECT_FALSE(log_has("COMMIT_PKGS"))
        << "no COMMIT_PKGS in the pending batch";

    // rec 应该能恢复全部
    recover_packages();

    EXPECT_TRUE(fs::exists(test_root / "usr/bin/t04a")) << "batch-a restored";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/t04b")) << "batch-b restored";
}

// ── T05: trim 后 rec 的幂等性 ────────────────────────────────────────

TEST_F(LogTrimIntegrationTest, TrimAfterRecoveryIsClean) {
    sigint_graceful.store(false);
    create_file("usr/bin/t05", "v1");
    std::string pkg = make_test_pkg("t05", "1.0", {"usr/bin/t05"});

    // 安装成功
    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg});
    Cache::instance().write("t05");
    Config::instance().set_force_overwrite_mode(false);
    fs::remove(log_path);

    // 模拟移除中断（写一半没电了）
    TransactionLog::log_raw("BEGIN_PKGS 1");
TransactionLog::log_raw("RM_BEGIN t05 1.0");
    fs::path f = test_root / "usr/bin/t05";
    fs::path bak = f; bak += ".lpkg_bak_t05";
    TransactionLog::log_raw("BACKUP " + f.string() + " → " + bak.string());
    fs::rename(f, bak);
    ASSERT_FALSE(fs::exists(f));
    // 无 RM_COMMIT, 无 RM_END

    // trim 保留未完成事务
    TransactionLog::trim_completed();
    EXPECT_TRUE(log_has("RM_BEGIN t05")) << "incomplete remove preserved";

    // rec 恢复
    recover_packages();
    EXPECT_TRUE(fs::exists(f)) << "file restored";
    EXPECT_TRUE(Cache::instance().is_installed("t05")) << "pkg still installed";

    // 恢复后的日志应被 trim 清空
    TransactionLog::trim_completed();
    EXPECT_TRUE(read_log().empty()) << "recovery complete → trim clears log";
}

// ── T06: 多个 install → trim → 多次 trim 幂等 ──────────────────────

TEST_F(LogTrimIntegrationTest, MultipleInstallsAndTrims) {
    sigint_graceful.store(false);
    auto pkg_a = make_test_pkg("t06a", "1.0", {"usr/bin/t06a"});

    // 第一次安装
    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg_a});
    Cache::instance().write("t06a");
    Config::instance().set_force_overwrite_mode(false);

    // trim 清空
    TransactionLog::trim_completed();
    EXPECT_TRUE(read_log().empty()) << "first install cleared by trim";

    // 第二次安装新的包
    auto pkg_b = make_test_pkg("t06b", "1.0", {"usr/bin/t06b"});
    install_packages({pkg_b});
    Cache::instance().write("t06b");

    // trim 应该只清空第二次安装的日志
    TransactionLog::trim_completed();
    EXPECT_TRUE(read_log().empty()) << "second install cleared by trim";
}
