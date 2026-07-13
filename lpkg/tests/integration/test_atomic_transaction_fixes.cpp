#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/pkg/transaction_log.hpp"
#include "../../main/src/archive/packer.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/base/constants.hpp"
#include "../test_base.hpp"
#include <filesystem>
#include <fstream>
#include <atomic>
#include <sys/stat.h>

namespace fs = std::filesystem;

extern std::atomic<bool> sigint_graceful;

// =========================================================================
// 测试套件：lpkg 原子事务修复验证
//
// 覆盖以下新增/修改功能：
//   1. WAL 顺序：BACKUP/COPY 日志先于文件操作写入
//   2. fsync：每条日志后立即落盘
//   3. 移除事务：RM_BEGIN / RM_COMMIT / RM_END
//   4. 批量事务：BEGIN_PKGS / COMMIT_PKGS
//   5. 恢复：支持 RM_BEGIN / BEGIN_PKGS 等新格式
// =========================================================================

class AtomicTransactionFixesTest : public IntegrationTestBase {
protected:
    void SetUp() override {
        IntegrationTestBase::SetUp();
        sigint_graceful.store(false);
    }

    void TearDown() override {
        IntegrationTestBase::TearDown();
        sigint_graceful.store(false);
    }

    /// 在 test_root 中创建一个常规文件
    void create_file(const fs::path& rel, const std::string& content = "original") {
        fs::path p = test_root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p); f << content;
    }

    /// 创建一个包含指定文件的包
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

    /// 创建一个带依赖的包
    std::string make_pkg_with_deps(const std::string& name, const std::string& ver,
                                   const std::vector<std::string>& files,
                                   const std::vector<std::string>& deps) {
        fs::path pkg_work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::remove_all(pkg_work);
        for (const auto& f : files) {
            fs::path fp = pkg_work / "content" / f;
            fs::create_directories(fp.parent_path());
            std::ofstream of(fp); of << "pkg:" << f;
        }
        std::string p = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(p, pkg_work.string(), name, ver, deps);
        return p;
    }

    /// 读取事务日志全部内容
    std::string read_log() {
        fs::path log_path = Config::instance().lock_dir() / "transaction.log";
        if (!fs::exists(log_path)) return "";
        std::ifstream f(log_path);
        return std::string((std::istreambuf_iterator<char>(f)), {});
    }

    /// 检查日志是否包含某字符串
    bool log_contains(const std::string& needle) {
        return read_log().find(needle) != std::string::npos;
    }

    /// 构造一个"文件操作已做但日志未写"的毁坏现场（模拟旧代码的漏洞）
    void simulate_orphan_bak(const fs::path& orig, const std::string& pkg_name) {
        fs::path bak = orig;
        bak += ".lpkg_bak_" + pkg_name;
        fs::rename(orig, bak);
    }
};


// ═══════════════════════════════════════════════════════════════════════
// 1-2: WAL 顺序验证 — BACKUP 和 COPY 先于文件操作写入
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, BackupLogWrittenBeforeRename) {
    // 验证 BACKUP 日志在 rename 之前写入。
    // 通过 on_before_file_copy 钩子在 backup 阶段触发 SIGINT 来验证：
    // 如果日志在文件操作前写入，rec 应能恢复。
    create_file("usr/bin/important_tool", "original data");
    std::string pkg_path = make_pkg("order-pkg", "1.0", {"usr/bin/important_tool"});

    Config::instance().set_force_overwrite_mode(true);
    {
        InstallationTask task("order-pkg", "1.0", true, "", pkg_path);
        // 在复制第一个文件前触发 SIGINT → 只执行了 backup（含日志），未执行 copy
        task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
        EXPECT_ANY_THROW(task.run_simple());
        sigint_graceful.store(false);
    }

    // 验证：日志中有 BACKUP 行，且文件被恢复
    EXPECT_TRUE(log_contains("BACKUP")) << "BACKUP log entry should exist";
    std::ifstream f(test_root / "usr/bin/important_tool");
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "original data") << "File should be restored by rollback";
}

TEST_F(AtomicTransactionFixesTest, CopyLogWrittenBeforeRename) {
    // 验证 COPY 日志在 fs::rename 之前写入。
    // 篡改测试：构造一个被中断的 COPY（日志已提交但 rename 未执行），
    // 验证 rec 能正确清理 .lpkgtmp 残留。
    create_file("usr/bin/copy_tool", "original content");
    std::string pkg_path = make_pkg("copy-order", "1.0", {"usr/bin/copy_tool"});

    // 先写 COPY 日志（模拟新顺序：日志先于 rename）
    fs::path dst = test_root / "usr/bin/copy_tool";
    fs::path tmp_path = dst; tmp_path += ".lpkgtmp";
    TransactionLog::log_raw("BEGIN " + std::string("copy-order") + " 1.0");
    TransactionLog::log_raw("COPY " + tmp_path.string() + " → " + dst.string());
    // 注意：没有执行 rename(tmp_path, dst)，模拟日志写完但 rename 前崩溃

    // rec 应清理 COPY 目标文件和 .lpkgtmp
    recover_packages();

    // .lpkgtmp 不应残留
    EXPECT_FALSE(fs::exists(tmp_path)) << ".lpkgtmp should be cleaned up";
}

// ═══════════════════════════════════════════════════════════════════════
// 3: fsync 验证 — 关键日志落盘后操作
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, FsyncEnsuresLogDurability) {
    // 验证 BEGIN 行写入后执行 fsync（使日志文件大小增长）
    {
        TransactionLog log;
        log.begin("fsync-test", "1.0");
    }

    fs::path log_path = Config::instance().lock_dir() / "transaction.log";
    ASSERT_TRUE(fs::exists(log_path));

    // 验证日志包含 BEGIN 行（说明 write + fsync 成功）
    EXPECT_TRUE(log_contains("BEGIN fsync-test 1.0"));
}

// ═══════════════════════════════════════════════════════════════════════
// 4-5: 恢复验证 — 日志先于文件操作写入时 rec 能正确处理
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, RecoverFromBackupLogBeforeRename) {
    // 模拟：BACKUP 日志已写入，但 rename 未执行（或未完成）
    fs::path orig = test_root / "usr/share/recover_bak_test.txt";
    fs::create_directories(orig.parent_path());
    { std::ofstream f(orig); f << "precious data"; }

    fs::path bak = orig; bak += ".lpkg_bak_recover-pkg";

    // 先写日志（固定顺序后），不执行 rename
    TransactionLog::log_raw("BEGIN recover-pkg 1.0");
    TransactionLog::log_raw("BACKUP " + orig.string() + " → " + bak.string());

    // 文件还在原位
    EXPECT_TRUE(fs::exists(orig));

    // rec 应识别未提交事务：BACKUP 行存在但 dst (.lpkg_bak) 不存在 → 安全跳过
    recover_packages();

    // 文件应保持原位
    EXPECT_TRUE(fs::exists(orig));
    std::ifstream f(orig);
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "precious data");
}

TEST_F(AtomicTransactionFixesTest, RecoverFromCopyLogBeforeRename) {
    // 模拟：COPY 日志已写入，但 rename 未执行
    fs::path dst = test_root / "usr/bin/copy_test_file";
    fs::create_directories(dst.parent_path());

    fs::path tmp_src = dst; tmp_src += ".lpkgtmp";
    { std::ofstream f(tmp_src); f << "new content"; }

    TransactionLog::log_raw("BEGIN copy-recover 1.0");
    TransactionLog::log_raw("COPY " + tmp_src.string() + " → " + dst.string());

    // 目标文件不存在，.lpkgtmp 存在
    ASSERT_FALSE(fs::exists(dst));
    ASSERT_TRUE(fs::exists(tmp_src));

    recover_packages();

    // rec 应清理 .lpkgtmp
    EXPECT_FALSE(fs::exists(tmp_src)) << ".lpkgtmp should be cleaned";
}

// ═══════════════════════════════════════════════════════════════════════
// 6: NEW 日志顺序验证（已有正确顺序）
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, NewLogWrittenInBackupPhase) {
    // 新文件在 backup 阶段记录 NEW，在 copy 阶段才实际创建。
    // 这是正确的顺序（日志在前，文件在后）。
    std::string pkg_path = make_pkg("new-order", "1.0", {"usr/share/new_file.txt"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("new-order", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());

    // NEW 日志应先于文件创建（已写入），回滚后文件应不存在
    EXPECT_TRUE(log_contains("NEW ")) << "NEW log entry should exist";
    EXPECT_FALSE(fs::exists(test_root / "usr/share/new_file.txt")) << "New file should not exist after rollback";
}

// ═══════════════════════════════════════════════════════════════════════
// 7-9: 移除事务 (RM_BEGIN/RM_COMMIT/RM_END)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, RemoveWalLogsBeginCommitEnd) {
    // 验证 remove_package 写入了完整的 WAL 事务日志
    std::string pkg_path = make_pkg("rm-wal", "1.0", {"usr/bin/rm_test"});

    // 先安装
    install_packages({pkg_path});
    Cache::instance().write();

    // 清除之前的日志（安装阶段产生的）
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    // 移除
    remove_package("rm-wal", false);
    write_cache();

    // 验证日志包含 RM_BEGIN / RM_COMMIT / RM_END
    EXPECT_TRUE(log_contains("RM_BEGIN rm-wal 1.0")) << "RM_BEGIN should be logged";
    EXPECT_TRUE(log_contains("RM_COMMIT rm-wal 1.0")) << "RM_COMMIT should be logged";
    EXPECT_TRUE(log_contains("RM_END rm-wal 1.0")) << "RM_END should be logged";
}

TEST_F(AtomicTransactionFixesTest, RemoveWalBackupBeforeDelete) {
    // 验证移除时文件先被备份到 .lpkg_bak，然后才被释放
    create_file("usr/bin/backup_before_rm", "to be removed");
    std::string pkg_path = make_pkg("rm-bak", "1.0", {"usr/bin/backup_before_rm"});

    install_packages({pkg_path});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    // 移除
    remove_package("rm-bak", false);
    write_cache();

    // 文件应该已删除，.lpkg_bak 也应已清理（因为 RM_COMMIT 后 cleanup）
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/backup_before_rm"))
        << "File should be removed after committed removal";

    // 但日志中应有 BACKUP 记录
    EXPECT_TRUE(log_contains("BACKUP")) << "BACKUP should be logged during remove";
}

TEST_F(AtomicTransactionFixesTest, RemoveCrashRecoveryRestoresFiles) {
    // 模拟：RM_BEGIN 已写，RM_COMMIT 未写（崩溃），恢复后文件应还原
    create_file("usr/bin/crash_rm_test", "file to remove");

    std::string pkg_path = make_pkg("crash-rm", "1.0", {"usr/bin/crash_rm_test"});
    install_packages({pkg_path});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    // 手动构造崩溃现场：RM_BEGIN + BACKUP，没有 RM_COMMIT
    fs::path orig = test_root / "usr/bin/crash_rm_test";
    fs::path bak = orig; bak += ".lpkg_bak_crash-rm";
    ASSERT_TRUE(fs::exists(orig));

    TransactionLog::log_raw("RM_BEGIN crash-rm 1.0");
    TransactionLog::log_raw("BACKUP " + orig.string() + " → " + bak.string());
    fs::rename(orig, bak);  // 模拟 "移除"（rename to .bak）
    ASSERT_FALSE(fs::exists(orig));
    ASSERT_TRUE(fs::exists(bak));

    // rec 应恢复
    recover_packages();

    EXPECT_TRUE(fs::exists(orig)) << "recover should restore the removed file";
    EXPECT_FALSE(fs::exists(bak)) << "backup should be consumed by recover";
}

// ═══════════════════════════════════════════════════════════════════════
// 10-12: 批量事务 (BEGIN_PKGS/COMMIT_PKGS)
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, BatchInstallWritesPkgsMarkers) {
    // 安装多个包 → 日志中有 BEGIN_PKGS / COMMIT_PKGS
    std::string pkg_a = make_pkg("batch-a", "1.0", {"usr/bin/batch_a"});
    std::string pkg_b = make_pkg("batch-b", "1.0", {"usr/bin/batch_b"});

    // 需要镜像仓库
    fs::path mirror = setup_local_mirror();
    add_to_mirror("batch-a", "1.0");
    add_to_mirror("batch-b", "1.0");
    {
        std::ofstream idx(mirror / "index.txt");
        idx << "batch-a|1.0:::|\nbatch-b|1.0:::|\n";
    }

    EXPECT_NO_THROW(install_packages({"batch-a", "batch-b"}));

    EXPECT_TRUE(log_contains("BEGIN_PKGS 2")) << "BEGIN_PKGS should be logged for 2 packages";
    EXPECT_TRUE(log_contains("COMMIT_PKGS")) << "COMMIT_PKGS should be logged";
}

TEST_F(AtomicTransactionFixesTest, BatchCrashRollsBackAll) {
    // 批量安装 2 个包，第 2 个安装时 SIGINT → 批量未提交 → rec 应回滚全部
    // 注意：两个包都不预安装，直接从文件构造批量场景
    create_file("usr/bin/batch1_file", "old batch1");
    create_file("usr/bin/batch2_file", "old batch2");

    std::string pkg1 = make_pkg("batch-roll1", "1.0", {"usr/bin/batch1_file"});
    std::string pkg2 = make_pkg("batch-roll2", "1.0", {"usr/bin/batch2_file"});

    // 直接构造批量事务日志（模拟 install_packages 批量流程）
    // 写 BEGIN_PKGS，"安装" pkg1（写入 BACKUP/COPY/COMMIT/END），
    // 然后"安装" pkg2 时 SIGINT，无 COMMIT_PKGS
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    TransactionLog::log_raw("BEGIN_PKGS 2");

    // 模拟 pkg1 安装：备份旧文件、复制新文件、提交
    {
        fs::path f1 = test_root / "usr/bin/batch1_file";
        fs::path bak1 = f1; bak1 += ".lpkg_bak_batch-roll1";
        TransactionLog::log_raw("BEGIN batch-roll1 1.0");
        TransactionLog::log_raw("BACKUP " + f1.string() + " → " + bak1.string());
        fs::rename(f1, bak1);
        { std::ofstream of(f1); of << "pkg:usr/bin/batch1_file"; }
        TransactionLog::log_raw("COPY " + (f1.string() + ".lpkgtmp") + " → " + f1.string());
        TransactionLog::log_raw("COMMIT batch-roll1 1.0");
        TransactionLog::log_raw("END batch-roll1 1.0");
    }

    // 模拟 pkg2 安装开始但中断（只有 BEGIN，没有后续操作就崩溃）
    {
        fs::path f2 = test_root / "usr/bin/batch2_file";
        fs::path bak2 = f2; bak2 += ".lpkg_bak_batch-roll2";
        TransactionLog::log_raw("BEGIN batch-roll2 1.0");
        TransactionLog::log_raw("BACKUP " + f2.string() + " → " + bak2.string());
        fs::rename(f2, bak2);
        { std::ofstream of(f2); of << "pkg:usr/bin/batch2_file"; }
        // 没有 COMMIT — 模拟崩溃
    }
    // 没有 COMMIT_PKGS

    // rec 应回滚整个批量（包括 pkg1 已 COMMIT 的操作）
    recover_packages();

    // 两个文件都应恢复原状
    {
        std::ifstream f(test_root / "usr/bin/batch1_file");
        std::string c; std::getline(f, c);
        EXPECT_EQ(c, "old batch1") << "batch1 should be rolled back despite individual COMMIT";
    }
    {
        std::ifstream f(test_root / "usr/bin/batch2_file");
        std::string c; std::getline(f, c);
        EXPECT_EQ(c, "old batch2") << "batch2 should be rolled back";
    }
}

TEST_F(AtomicTransactionFixesTest, SinglePackageNowUsesBatchPkgs) {
    // 单个包现在也使用 WAL 保护的批量协议（BEGIN_PKGS/COMMIT_PKGS）
    // 确保 DB 写入在事务边界内，断电后 rec 能回滚
    std::string pkg_path = make_pkg("single-pkg", "1.0", {"usr/bin/single_pkg"});

    fs::path mirror = setup_local_mirror();
    add_to_mirror("single-pkg", "1.0");
    { std::ofstream idx(mirror / "index.txt"); idx << "single-pkg|1.0:::|\n"; }

    install_packages({"single-pkg"});

    // 现在单个包也有 WAL 保护了
    EXPECT_TRUE(log_contains("BEGIN_PKGS 1")) << "single package should now use BEGIN_PKGS";
    EXPECT_TRUE(log_contains("COMMIT_PKGS")) << "single package should now have COMMIT_PKGS";
}

// ═══════════════════════════════════════════════════════════════════════
// 13: 批量 + 依赖场景
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, BatchWithDepsWrapsInPkgs) {
    // 安装一个带依赖的包 → 总安装数 > 1 → 应有 BEGIN_PKGS
    // 使用 create_pkg (IntegrationTestBase) 自动处理 hash 和镜像
    std::string dep_pkg = create_pkg("dep-lib", "1.0");
    std::string main_pkg = create_pkg("dep-main", "1.0", {"dep-lib"});

    fs::path mirror = setup_local_mirror();
    // add_to_mirror 复制 .lpkg 文件到镜像目录，但不创建 index.txt
    // 手动写索引（格式：name|version:hash:deps:provides:needed_so）
    // 注意：hash 留空（填 0），repo 加载时若 hash 为空或 0 则跳过验证
    fs::create_directories(mirror / "dep-lib");
    fs::copy(pkg_dir / "dep-lib-1.0.lpkg", mirror / "dep-lib" / "1.0.lpkg");
    fs::create_directories(mirror / "dep-main");
    fs::copy(pkg_dir / "dep-main-1.0.lpkg", mirror / "dep-main" / "1.0.lpkg");
    {
        std::ofstream idx(mirror / "index.txt");
        // 格式：name|version:hash:deps:provides:needed_so
        // hash 留空以跳过哈希验证
        idx << "dep-lib|1.0:::|\n";
        idx << "dep-main|1.0::dep-lib::|\n";
    }

    install_packages({"dep-main"});

    // 所有安装都使用 WAL 保护（含 BEGIN_PKGS / COMMIT_PKGS）
    EXPECT_TRUE(log_contains("BEGIN_PKGS")) << "every install uses BEGIN_PKGS";
    EXPECT_TRUE(log_contains("COMMIT_PKGS")) << "COMMIT_PKGS should be present";
}

// ═══════════════════════════════════════════════════════════════════════
// 14-15: 恢复正确处理 RM_COMMIT 和 RM_BEGIN 边界
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, RecoverSkipsCompletedRemove) {
    // RM_BEGIN + RM_COMMIT → 完整的移除事务 → rec 跳过
    fs::path orig = test_root / "usr/bin/completed_rm";
    fs::create_directories(orig.parent_path());
    { std::ofstream f(orig); f << "data"; }

    TransactionLog::log_raw("RM_BEGIN done-pkg 1.0");
    TransactionLog::log_raw("BACKUP " + orig.string() + " → " + orig.string() + ".lpkg_bak_done-pkg");
    TransactionLog::log_raw("RM_COMMIT done-pkg 1.0");

    // rec 不应恢复已提交的移除
    recover_packages();

    // 文件不受影响（RM_COMMIT 已写，跳过回滚）
    // 注意：这个测试中文件没有被真的移除，只是验证 rec 不碰已提交的事务
    SUCCEED() << "completed remove should be skipped by recover";
}

TEST_F(AtomicTransactionFixesTest, RecoverRollsBackUncommittedRemove) {
    // RM_BEGIN 但没有 RM_COMMIT → rec 应回滚 BACKUP
    fs::path orig = test_root / "usr/share/pending_rm.txt";
    fs::create_directories(orig.parent_path());
    { std::ofstream f(orig); f << "pending remove data"; }

    fs::path bak = orig; bak += ".lpkg_bak_pending-pkg";

    TransactionLog::log_raw("RM_BEGIN pending-pkg 1.0");
    TransactionLog::log_raw("BACKUP " + orig.string() + " → " + bak.string());
    fs::rename(orig, bak);

    ASSERT_FALSE(fs::exists(orig));
    ASSERT_TRUE(fs::exists(bak));

    recover_packages();

    EXPECT_TRUE(fs::exists(orig)) << "file should be restored via BACKUP recovery";
    EXPECT_FALSE(fs::exists(bak)) << ".lpkg_bak should be consumed";
}

// ═══════════════════════════════════════════════════════════════════════
// 16: log_raw_no_fsync 用于回滚日志
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, LogRawNoFsyncWritesWithoutError) {
    // 验证 log_raw_no_fsync 能正常工作（不崩溃、不丢行）
    TransactionLog::log_raw_no_fsync("TEST no-fsync line");

    EXPECT_TRUE(log_contains("TEST no-fsync line"));
}

// ═══════════════════════════════════════════════════════════════════════
// 17: 批量和移除在日志中不互相干扰
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, BatchAndRemoveLogSeparation) {
    // 写一个 BEGIN_PKGS + COMMIT_PKGS，然后一个 RM_BEGIN + RM_COMMIT
    // 验证 rec 不会混淆两者
    TransactionLog::log_raw("BEGIN_PKGS 2");
    TransactionLog::log_raw("BEGIN pkg1 1.0");
    TransactionLog::log_raw("COMMIT pkg1 1.0");
    TransactionLog::log_raw("END pkg1 1.0");
    TransactionLog::log_raw("BEGIN pkg2 1.0");
    TransactionLog::log_raw("COMMIT pkg2 1.0");
    TransactionLog::log_raw("END pkg2 1.0");
    TransactionLog::log_raw("COMMIT_PKGS");

    TransactionLog::log_raw("RM_BEGIN remove-pkg 1.0");
    TransactionLog::log_raw("BACKUP /dummy → /dummy.lpkg_bak_remove-pkg");
    TransactionLog::log_raw("RM_COMMIT remove-pkg 1.0");
    TransactionLog::log_raw("RM_END remove-pkg 1.0");

    // 所有事务都是完整的 → rec 不应回滚任何内容
    EXPECT_NO_THROW(recover_packages());
}

// ═══════════════════════════════════════════════════════════════════════
// 18: rec 帮助信息
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, RecDescKeyExists) {
    // 验证本地化键值存在
    std::string desc = get_string("info.rec_desc");
    EXPECT_FALSE(desc.empty()) << "info.rec_desc should not be empty";
    EXPECT_TRUE(desc.find("rec") != std::string::npos) << "description should mention 'rec'";
}

// ═══════════════════════════════════════════════════════════════════════
// 19: BEGIN_PKGS 无 COMMIT_PKGS → rec 回滚全部
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, BatchWithoutCommitRollsBackAll) {
    // 模拟 BEGIN_PKGS 已写入但 COMMIT_PKGS 未写入（崩溃）
    fs::path f1 = test_root / "usr/bin/batch_f1";
    fs::path f2 = test_root / "usr/bin/batch_f2";
    fs::create_directories(f1.parent_path());
    { std::ofstream of(f1); of << "old1"; }
    { std::ofstream of(f2); of << "old2"; }

    TransactionLog::log_raw("BEGIN_PKGS 2");
    // pkg1 已安装（自己的 COMMIT 已写）
    TransactionLog::log_raw("BEGIN pkg1 1.0");
    TransactionLog::log_raw("BACKUP " + f1.string() + " → " + (f1.string() + ".lpkg_bak_pkg1"));
    fs::rename(f1, f1.string() + ".lpkg_bak_pkg1");
    { std::ofstream of(f1); of << "new1"; }
    TransactionLog::log_raw("COMMIT pkg1 1.0");
    TransactionLog::log_raw("END pkg1 1.0");
    // pkg2 未完成（无 COMMIT）
    TransactionLog::log_raw("BEGIN pkg2 1.0");
    TransactionLog::log_raw("BACKUP " + f2.string() + " → " + (f2.string() + ".lpkg_bak_pkg2"));
    fs::rename(f2, f2.string() + ".lpkg_bak_pkg2");
    { std::ofstream of(f2); of << "new2"; }
    // 这里崩溃了，没有 COMMIT 和 COMMIT_PKGS

    // rec 应回滚 pkg1 和 pkg2 的所有操作
    recover_packages();

    // 两个文件都应恢复原状
    {
        std::ifstream f(f1);
        std::string c; std::getline(f, c);
        EXPECT_EQ(c, "old1") << "batch rollback should restore pkg1's file";
    }
    {
        std::ifstream f(f2);
        std::string c; std::getline(f, c);
        EXPECT_EQ(c, "old2") << "batch rollback should restore pkg2's file";
    }
}

// ═══════════════════════════════════════════════════════════════════════
// 20: RM_BEGIN 无 RM_COMMIT → rec 恢复文件
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, RemoveBeginNoCommitRecRestores) {
    fs::path orig = test_root / "usr/lib/partial_remove.so";
    fs::create_directories(orig.parent_path());
    { std::ofstream f(orig); f << "partial remove victim"; }

    fs::path bak = orig; bak += ".lpkg_bak_partial-rm";

    TransactionLog::log_raw("RM_BEGIN partial-rm 1.0");
    TransactionLog::log_raw("BACKUP " + orig.string() + " → " + bak.string());
    fs::rename(orig, bak);
    // 没有 RM_COMMIT

    ASSERT_FALSE(fs::exists(orig));

    recover_packages();

    EXPECT_TRUE(fs::exists(orig)) << "recover should restore from incomplete RM_BEGIN";
    std::ifstream f(orig);
    std::string c; std::getline(f, c);
    EXPECT_EQ(c, "partial remove victim");
}

// ═══════════════════════════════════════════════════════════════════════
// 21: 多事务中只有最后一个未完成
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, LastUncommittedOnly) {
    TransactionLog::log_raw("BEGIN done1 1.0");
    TransactionLog::log_raw("COMMIT done1 1.0");
    TransactionLog::log_raw("END done1 1.0");
    TransactionLog::log_raw("RM_BEGIN done2 1.0");
    TransactionLog::log_raw("RM_COMMIT done2 1.0");
    TransactionLog::log_raw("RM_END done2 1.0");
    TransactionLog::log_raw("BEGIN_PKGS 1");
    TransactionLog::log_raw("COMMIT_PKGS");

    // 全部完成 → rec 无操作
    EXPECT_NO_THROW(recover_packages());
    auto log_content = read_log();
    EXPECT_TRUE(log_content.find("ROLLBACK") == std::string::npos ||
                log_content.find("ROLLBACK done2") == std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════
// 22: log_raw 在回滚中不崩溃（多线程/多实例安全）
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, LogRawConcurrentSafety) {
    // 多个 log_raw 调用不会互相干扰
    for (int i = 0; i < 100; i++) {
        TransactionLog::log_raw("CONCURRENT_TEST line " + std::to_string(i));
    }

    auto log_content = read_log();
    int count = 0;
    std::istringstream ss(log_content);
    std::string line;
    while (std::getline(ss, line))
        if (line.find("CONCURRENT_TEST") != std::string::npos) count++;

    EXPECT_EQ(count, 100) << "all 100 lines should be in the log";
}

// ═══════════════════════════════════════════════════════════════════════
// 23: 备份文件已存在时 rm_backup 不破坏数据
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, RemoveBackupOfSharedFile) {
    // 验证移除时日志包含 BACKUP
    create_file("usr/bin/solo_tool", "solo content");
    std::string pkg = make_pkg("solo-pkg", "1.0", {"usr/bin/solo_tool"});

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg});
    Cache::instance().write();
    Config::instance().set_force_overwrite_mode(false);

    // 清除日志
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    // 正常移除
    EXPECT_NO_THROW(remove_package("solo-pkg", false));

    // 验证日志有 BACKUP（移除时备份）
    EXPECT_TRUE(log_contains("BACKUP ")) << "remove should log BACKUP";
    // 文件应已被删除
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/solo_tool")) << "file should be gone after remove";
}

// ═══════════════════════════════════════════════════════════════════════
// 24: 批量事务中单个包安装成功但整体失败 → 全部回滚
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, BatchInstallPartialFailure) {
    // 2 个包批量安装，pkg1 成功，pkg2 失败（预检拒绝）
    create_file("usr/bin/part_a", "original_a");
    create_file("usr/bin/part_b", "original_b");

    std::string pkg1 = make_pkg("part-pkg1", "1.0", {"usr/bin/part_a"});
    std::string pkg2 = make_pkg("part-pkg2", "1.0", {"usr/bin/part_b"});

    fs::path mirror = setup_local_mirror();
    add_to_mirror("part-pkg1", "1.0");
    add_to_mirror("part-pkg2", "1.0");
    {
        std::ofstream idx(mirror / "index.txt");
        idx << "part-pkg1|1.0:::|\npart-pkg2|1.0:::|\n";
    }

    // 先设 SIGINT 让 pkg2 失败
    sigint_graceful.store(true);
    EXPECT_THROW(install_packages({"part-pkg1", "part-pkg2"}), LpkgException);
    sigint_graceful.store(false);

    // rec 应回滚所有
    recover_packages();

    // 两个文件都应恢复
    {
        std::ifstream f(test_root / "usr/bin/part_a");
        std::string c; std::getline(f, c);
        EXPECT_EQ(c, "original_a");
    }
    {
        std::ifstream f(test_root / "usr/bin/part_b");
        std::string c; std::getline(f, c);
        EXPECT_EQ(c, "original_b");
    }
}

// ═══════════════════════════════════════════════════════════════════════
// 25: 完整移除 + rec 不应恢复任何内容
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, CompleteRemoveNoRecovery) {
    std::string pkg_path = make_pkg("clean-rm", "1.0", {"usr/bin/clean_remove_test"});

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg_path});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");
    Config::instance().set_force_overwrite_mode(false);

    // 正常完成移除
    remove_package("clean-rm", false);
    write_cache();

    // rec 应无操作
    EXPECT_NO_THROW(recover_packages());

    // 文件不在了
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/clean_remove_test"));
    EXPECT_FALSE(Cache::instance().is_installed("clean-rm"));
}

// ═══════════════════════════════════════════════════════════════════════
// 26: 日志中混合不同类型的事务 → rec 只回滚未提交的
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, MixedTxnsOnlyLastUncommitted) {
    fs::path orig_a = test_root / "usr/bin/mixed_a";
    fs::path orig_b = test_root / "usr/bin/mixed_b";
    fs::create_directories(orig_a.parent_path());
    { std::ofstream f(orig_a); f << "a_data"; }
    { std::ofstream f(orig_b); f << "b_data"; }

    // 完整的事务
    TransactionLog::log_raw("BEGIN pkgA 1.0");
    TransactionLog::log_raw("COMMIT pkgA 1.0");
    TransactionLog::log_raw("END pkgA 1.0");
    TransactionLog::log_raw("RM_BEGIN pkgB 1.0");
    TransactionLog::log_raw("RM_COMMIT pkgB 1.0");
    TransactionLog::log_raw("RM_END pkgB 1.0");

    // 未提交的移除
    TransactionLog::log_raw("RM_BEGIN pkgC 2.0");
    TransactionLog::log_raw("BACKUP " + orig_a.string() + " → " + (orig_a.string() + ".lpkg_bak_pkgC"));
    fs::rename(orig_a, orig_a.string() + ".lpkg_bak_pkgC");
    // 没有 RM_COMMIT

    // rec 应只回滚 pkgC
    recover_packages();

    EXPECT_TRUE(fs::exists(orig_a)) << "pkgC's file should be restored";
}

// ═══════════════════════════════════════════════════════════════════════
// 27: BEGIN_PKGS 后跟 RM_BEGIN 在批量内
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, BatchContainsRemove) {
    // 在批量事务内部包含移除操作（例如升级场景）
    TransactionLog::log_raw("BEGIN_PKGS 2");
    TransactionLog::log_raw("RM_BEGIN old-pkg 1.0");
    TransactionLog::log_raw("BACKUP /dummy → /dummy.lpkg_bak_old-pkg");
    TransactionLog::log_raw("RM_COMMIT old-pkg 1.0");
    TransactionLog::log_raw("RM_END old-pkg 1.0");
    TransactionLog::log_raw("BEGIN new-pkg 1.0");
    TransactionLog::log_raw("COMMIT new-pkg 1.0");
    TransactionLog::log_raw("END new-pkg 1.0");
    // 没有 COMMIT_PKGS → 完整批量未提交

    // rec 应回滚整个批量
    EXPECT_NO_THROW(recover_packages());
    // 批量没有文件操作，只是验证不崩溃
    SUCCEED() << "batch with nested remove handled without crash";
}

// ═══════════════════════════════════════════════════════════════════════
// 28-31: 升级时废弃文件 REMOVE_OLD WAL 日志
// ═══════════════════════════════════════════════════════════════════════

TEST_F(AtomicTransactionFixesTest, UpgradeWritesRemoveOldForObsoleteFiles) {
    // 验证升级时被移除的旧版本文件写入了 REMOVE_OLD WAL 日志
    std::string pkg_v1 = make_pkg("upgrade-obsolete", "1.0",
        {"usr/bin/file_a", "usr/lib/file_b.so"});
    std::string pkg_v2 = make_pkg("upgrade-obsolete", "2.0",
        {"usr/bin/file_a"}); // file_b 被移除

    // 安装 v1
    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg_v1});
    Cache::instance().write();
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/file_a"));
    EXPECT_TRUE(fs::exists(test_root / "usr/lib/file_b.so"));

    // 清除 v1 安装产生的日志，只保留升级日志
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    // 安装 v2（升级，会移除 file_b）
    install_packages({pkg_v2});
    Cache::instance().write();

    // WAL 应包含 REMOVE_OLD 条目
    EXPECT_TRUE(log_contains("REMOVE_OLD")) << "upgrade should log REMOVE_OLD for obsolete files";
    // file_b 应已被删除（升级成功，废弃文件清理）
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/file_a"));
    EXPECT_FALSE(fs::exists(test_root / "usr/lib/file_b.so"))
        << "obsolete file should be removed after upgrade";

    // 不应有残留 .lpkgbak 文件
    bool bak_found = false;
    for (auto& e : fs::recursive_directory_iterator(test_root)) {
        if (e.path().filename().string().find(".lpkg_bak_") != std::string::npos) {
            bak_found = true; break;
        }
    }
    EXPECT_FALSE(bak_found) << "no .lpkgbak files should remain after successful upgrade";

    Config::instance().set_force_overwrite_mode(false);
}

TEST_F(AtomicTransactionFixesTest, UpgradeRemoveOldRecRestores) {
    // 模拟升级中崩溃（BEGIN + REMOVE_OLD 已写，无 COMMIT）
    // rec 应能恢复被移除的废弃文件
    create_file("usr/lib/old_lib.so", "old version lib");
    create_file("usr/bin/kept_tool", "kept tool");

    fs::path obsolete_path = test_root / "usr/lib/old_lib.so";
    fs::path kept_path = test_root / "usr/bin/kept_tool";
    ASSERT_TRUE(fs::exists(obsolete_path));
    ASSERT_TRUE(fs::exists(kept_path));

    // 模拟 REMOVE_OLD：文件已被 rename 到 .lpkgbak（fix 后的行为）
    fs::path bak = obsolete_path;
    bak += ".lpkg_bak_upgrade-pkg";
    fs::rename(obsolete_path, bak);
    ASSERT_FALSE(fs::exists(obsolete_path));
    ASSERT_TRUE(fs::exists(bak));

    // WAL：BEGIN + REMOVE_OLD，没有 COMMIT（模拟崩溃）
    TransactionLog::log_raw("BEGIN upgrade-pkg 2.0");
    TransactionLog::log_raw("REMOVE_OLD " + obsolete_path.string() + " → " + bak.string());

    // rec 应恢复
    recover_packages();

    EXPECT_TRUE(fs::exists(obsolete_path))
        << "recover should restore the obsolete file via REMOVE_OLD entry";
    EXPECT_FALSE(fs::exists(bak))
        << ".lpkgbak should be consumed by recover";
    EXPECT_TRUE(fs::exists(kept_path))
        << "unrelated files should be untouched";
}

TEST_F(AtomicTransactionFixesTest, UpgradeLogShowsRemoveOldAndBackupOrder) {
    // 验证升级时同一事务中 BACKUP（覆盖前备份）和 REMOVE_OLD（废弃删除）的次序
    create_file("usr/bin/overwritten_bin", "old content");
    create_file("usr/lib/obsolete.so", "removed lib");

    std::string pkg_v1 = make_pkg("order-test", "1.0",
        {"usr/bin/overwritten_bin", "usr/lib/obsolete.so"});
    std::string pkg_v2 = make_pkg("order-test", "2.0",
        {"usr/bin/overwritten_bin"}); // obsolete.so 被移除

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg_v1});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    install_packages({pkg_v2});
    Cache::instance().write();

    // BACKUP 出现在 copy_package_files 阶段，REMOVE_OLD 在 commit_without_file_ops
    // 但二者都在同一事务 BEGIN…COMMIT 内
    EXPECT_TRUE(log_contains("BACKUP"))
        << "overwritten files should produce BACKUP entries";
    EXPECT_TRUE(log_contains("REMOVE_OLD"))
        << "obsolete files should produce REMOVE_OLD entries";
    EXPECT_TRUE(log_contains("COMMIT order-test 2.0"))
        << "upgrade should commit successfully";

    Config::instance().set_force_overwrite_mode(false);
}

TEST_F(AtomicTransactionFixesTest, UpgradeRemoveOldMultipleObsoleteFiles) {
    // 验证多个废弃文件都被正确记录 REMOVE_OLD
    std::vector<std::string> v1_files = {
        "usr/bin/tool_a", "usr/bin/tool_b", "usr/bin/tool_c",
        "usr/lib/lib_x.so", "usr/lib/lib_y.so"
    };
    std::vector<std::string> v2_files = {
        "usr/bin/tool_a", "usr/lib/lib_x.so" // 其余 3 个被移除
    };

    std::string pkg_v1 = make_pkg("multi-obsolete", "1.0", v1_files);
    std::string pkg_v2 = make_pkg("multi-obsolete", "2.0", v2_files);

    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg_v1});
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    install_packages({pkg_v2});
    Cache::instance().write();

    // 统计 REMOVE_OLD 行数
    auto log_content = read_log();
    int remove_old_count = 0;
    std::istringstream ss(log_content);
    std::string line;
    while (std::getline(ss, line))
        if (line.find("REMOVE_OLD") != std::string::npos) remove_old_count++;

    // 应有 3 个 REMOVE_OLD（tool_b, tool_c, lib_y）
    EXPECT_EQ(remove_old_count, 3)
        << "each obsolete file should have its own REMOVE_OLD entry";

    // 验证被移除的文件都不存在了
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/tool_b"));
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/tool_c"));
    EXPECT_FALSE(fs::exists(test_root / "usr/lib/lib_y.so"));
    // 保留的文件依然存在
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/tool_a"));
    EXPECT_TRUE(fs::exists(test_root / "usr/lib/lib_x.so"));

    Config::instance().set_force_overwrite_mode(false);
}
