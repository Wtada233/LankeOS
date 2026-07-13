#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/pkg/transaction_log.hpp"
#include "../test_base.hpp"
#include <filesystem>
#include <fstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

extern std::atomic<bool> sigint_graceful;

class AtomicRollbackTest : public IntegrationTestBase {
protected:
    void SetUp() override {
        IntegrationTestBase::SetUp();
        sigint_graceful.store(false);
    }
    void TearDown() override {
        IntegrationTestBase::TearDown();
        sigint_graceful.store(false);
    }

    // 在 test_root 中创建一个常规文件
    void create_file(const fs::path& rel, const std::string& content = "original") {
        fs::path p = test_root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p); f << content;
    }

    // 在 test_root 中创建一个符号链接
    void create_symlink(const fs::path& link_rel, const fs::path& target_rel) {
        fs::path lnk = test_root / link_rel;
        fs::create_directories(lnk.parent_path());
        fs::path tgt = test_root / target_rel;
        if (!fs::exists(tgt)) {
            fs::create_directories(tgt.parent_path());
            std::ofstream f(tgt); f << "symlink target";
        }
        fs::create_symlink(fs::relative(tgt, lnk.parent_path()), lnk);
    }

    // 构建一个包含指定文件的包
    std::string make_pkg(const std::string& name, const std::string& ver,
                         const std::vector<std::string>& files) {
        fs::path pkg_work = suite_work_dir / ("_pkg_" + name);
        for (const auto& f : files) {
            fs::path fp = pkg_work / "content" / f;
            fs::create_directories(fp.parent_path());
            std::ofstream of(fp); of << "pkg:" << f;
        }
        std::string p = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(p, pkg_work.string(), name, ver);
        return p;
    }
};

// ══════════════════════════════════════════════════════════════════════
// 基础回滚测试
// ══════════════════════════════════════════════════════════════════════

// ── 1. 钩子在复制阶段触发 sigint → rollback 恢复文件 ──
TEST_F(AtomicRollbackTest, HookTriggersRollbackDuringCopy) {
    create_file("usr/lib/test.so.1", "original content");
    std::string pkg_path = make_pkg("test-pkg", "1.0", {"usr/lib/test.so.1"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("test-pkg", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    std::ifstream f(test_root / "usr/lib/test.so.1");
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "original content");

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("test-pkg"));
}

// ── 2. 无 force-overwrite → 预检拒绝 ──
TEST_F(AtomicRollbackTest, NoForceOverwrite_RejectsUntracked) {
    create_file("usr/bin/existing_bin", "host file");
    std::string pkg_path = make_pkg("noforce-pkg", "1.0", {"usr/bin/existing_bin"});

    fs::path mirror = setup_local_mirror();
    fs::create_directories(mirror / "noforce-pkg");
    fs::copy(pkg_path, mirror / "noforce-pkg" / "1.0.lpkg");
    { std::ofstream idx(mirror / "index.txt"); idx << "noforce-pkg|1.0:::|\n"; }

    EXPECT_THROW(install_packages({"noforce-pkg:1.0"}), LpkgException);

    std::ifstream f(test_root / "usr/bin/existing_bin");
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "host file");
}

// ── 3. 回滚后重试 → 第二次安装成功 ──
TEST_F(AtomicRollbackTest, RetryAfterRollback) {
    create_file("usr/lib/data.bin", "original data");
    std::string pkg_path = make_pkg("retry-pkg", "1.0", {"usr/lib/data.bin"});

    // 第一次：sigint → rollback
    {
        Config::instance().set_force_overwrite_mode(true);
        InstallationTask task("retry-pkg", "1.0", true, "", pkg_path);
        task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
        EXPECT_ANY_THROW(task.run_simple());
        sigint_graceful.store(false);
        Config::instance().set_force_overwrite_mode(false);
    }
    {
        std::ifstream f(test_root / "usr/lib/data.bin");
        std::string content; std::getline(f, content);
        EXPECT_EQ(content, "original data");
    }

    // 第二次：正常安装
    {
        Config::instance().set_force_overwrite_mode(true);
        InstallationTask task("retry-pkg", "1.0", true, "", pkg_path);
        task.run_simple();
        Config::instance().set_force_overwrite_mode(false);
    }
    {
        std::ifstream f(test_root / "usr/lib/data.bin");
        std::string content; std::getline(f, content);
        EXPECT_EQ(content, "pkg:usr/lib/data.bin");
    }
}

// ══════════════════════════════════════════════════════════════════════
// 悬空符号链接回滚测试（关键 bug 修复）
// ══════════════════════════════════════════════════════════════════════

// ── 4. 悬空软链接在回滚中恢复 ──
TEST_F(AtomicRollbackTest, DanglingSymlinkRestoredOnRollback) {
    // 创建 libfoo.so → libfoo.so.1.0 的符号链接，并且 libfoo.so.1.0 也存在
    create_file("usr/lib/libfoo.so.1.0", "versioned lib");
    create_symlink("usr/lib/libfoo.so", "libfoo.so.1.0");

    // 包包含这两个文件
    std::string pkg_path = make_pkg("sym-pkg", "1.0", {"usr/lib/libfoo.so", "usr/lib/libfoo.so.1.0"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("sym-pkg", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    // 两个文件都应被恢复
    EXPECT_TRUE(fs::exists(test_root / "usr/lib/libfoo.so.1.0")) << "versioned file restored";
    EXPECT_TRUE(fs::is_symlink(test_root / "usr/lib/libfoo.so")) << "symlink restored";
    // 并且版本化文件内容正确
    std::ifstream f(test_root / "usr/lib/libfoo.so.1.0");
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "versioned lib");
}

// ── 5. 仅悬空软链接（无目标文件） → 恢复 ──
TEST_F(AtomicRollbackTest, OrphanSymlinkRestored) {
    // 只有断链软链接（目标不存在）
    fs::path target = test_root / "usr/lib/libmissing.so";
    fs::create_directories(target.parent_path());
    fs::path missing = test_root / "usr/lib/libmissing.so.1";
    fs::create_symlink("libmissing.so.1", target);
    // 注意：libmissing.so.1 不存在，这是故意构造的

    std::string pkg_path = make_pkg("orphan-pkg", "1.0", {"usr/lib/libmissing.so"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("orphan-pkg", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_TRUE(fs::is_symlink(test_root / "usr/lib/libmissing.so")) << "orphan symlink restored";
}

// ── 6. 多层符号链接链 → 全部恢复 ──
TEST_F(AtomicRollbackTest, SymlinkChainRestored) {
    // liba.so → liba.so.1 → liba.so.1.0.0
    create_file("usr/lib/liba.so.1.0.0", "deep target");
    create_symlink("usr/lib/liba.so.1", "liba.so.1.0.0");
    create_symlink("usr/lib/liba.so", "liba.so.1");

    std::string pkg_path = make_pkg("chain-pkg", "1.0",
        {"usr/lib/liba.so", "usr/lib/liba.so.1", "usr/lib/liba.so.1.0.0"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("chain-pkg", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_TRUE(fs::is_symlink(test_root / "usr/lib/liba.so"));
    EXPECT_TRUE(fs::is_symlink(test_root / "usr/lib/liba.so.1"));
    EXPECT_TRUE(fs::exists(test_root / "usr/lib/liba.so.1.0.0"));
}

// ══════════════════════════════════════════════════════════════════════
// 多文件 + 边界情况
// ══════════════════════════════════════════════════════════════════════

// ── 7. 多文件回滚 → 全部恢复 ──
TEST_F(AtomicRollbackTest, MultipleFilesAllRestored) {
    for (int i = 0; i < 20; i++) {
        create_file("usr/share/data/file." + std::to_string(i), "orig:" + std::to_string(i));
    }

    std::vector<std::string> pkg_files;
    for (int i = 0; i < 20; i++)
        pkg_files.push_back("usr/share/data/file." + std::to_string(i));
    std::string pkg_path = make_pkg("multi-pkg", "1.0", pkg_files);

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("multi-pkg", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    for (int i = 0; i < 20; i++) {
        std::ifstream f(test_root / ("usr/share/data/file." + std::to_string(i)));
        std::string content; std::getline(f, content);
        EXPECT_EQ(content, "orig:" + std::to_string(i));
    }
}

// ── 8. 空备份列表不回滚 ──
TEST_F(AtomicRollbackTest, EmptyBackupNoCrash) {
    // 新文件（磁盘上不存在）
    create_file("usr/share/readme.txt", "readme");
    std::string pkg_path = make_pkg("new-pkg", "1.0", {"usr/share/readme.txt"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("new-pkg", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);
}

// ── 9. 混合 新文件 + 备份文件 → 全部正确处理 ──
TEST_F(AtomicRollbackTest, MixedNewAndBackupFile) {
    create_file("usr/lib/existing.so", "old so");
    // 新文件不在磁盘上
    std::string pkg_path = make_pkg("mixed-pkg", "1.0",
        {"usr/lib/existing.so", "usr/lib/new.so"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("mixed-pkg", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    // 旧文件恢复
    {
        std::ifstream f(test_root / "usr/lib/existing.so");
        std::string content; std::getline(f, content);
        EXPECT_EQ(content, "old so");
    }
    // 新文件不应创建
    EXPECT_FALSE(fs::exists(test_root / "usr/lib/new.so"));
}

// ── 10. 只备份不复制 → 回滚正常 ──
TEST_F(AtomicRollbackTest, BackupOnlyThenSigint) {
    create_file("usr/bin/tool", "original binary");
    std::string pkg_path = make_pkg("backup-only", "1.0", {"usr/bin/tool"});

    Config::instance().set_force_overwrite_mode(true);
    // 在备份阶段触发 sigint（第一个文件备份完成后）
    // backup_existing_files 每文件前检查 sigint
    static int call_count = 0; call_count = 0;
    InstallationTask task("backup-only", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{
        if (++call_count >= 1) sigint_graceful.store(true);
    };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    // 文件应恢复
    std::ifstream f(test_root / "usr/bin/tool");
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "original binary");
}

// ══════════════════════════════════════════════════════════════════════
// .lpkgtmp 临时文件处理
// ══════════════════════════════════════════════════════════════════════

// ── 11. .lpkgtmp 在成功安装后被删除 ──
TEST_F(AtomicRollbackTest, TempFileCleanedOnSuccess) {
    create_file("usr/bin/clean_tool", "old");
    std::string pkg_path = make_pkg("clean-pkg", "1.0", {"usr/bin/clean_tool"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("clean-pkg", "1.0", true, "", pkg_path);
    task.run_simple();
    Config::instance().set_force_overwrite_mode(false);

    // .lpkgtmp 不应残留
    bool tmp_found = false;
    for (auto& e : fs::recursive_directory_iterator(test_root))
        if (e.path().extension() == ".lpkgtmp") tmp_found = true;
    EXPECT_FALSE(tmp_found) << "no .lpkgtmp files remain after successful install";
}

// ── 12. .lpkgtmp 在回滚后被清理 ──
TEST_F(AtomicRollbackTest, TempFileCleanedOnRollback) {
    create_file("usr/bin/rollback_tool", "old");
    std::string pkg_path = make_pkg("rollback-clean", "1.0", {"usr/bin/rollback_tool"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("rollback-clean", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    bool tmp_found = false;
    for (auto& e : fs::recursive_directory_iterator(test_root))
        if (e.path().extension() == ".lpkgtmp") tmp_found = true;
    EXPECT_FALSE(tmp_found) << "no .lpkgtmp files remain after rollback";
}

// ══════════════════════════════════════════════════════════════════════
// 完整安装（无中断）
// ══════════════════════════════════════════════════════════════════════

// ── 13. 正常安装不回滚 ──
TEST_F(AtomicRollbackTest, SuccessfulInstallNoRollback) {
    create_file("usr/bin/stable_tool", "old stable");
    std::string pkg_path = make_pkg("stable-pkg", "1.0", {"usr/bin/stable_tool"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("stable-pkg", "1.0", true, "", pkg_path);
    EXPECT_NO_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    std::ifstream f(test_root / "usr/bin/stable_tool");
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "pkg:usr/bin/stable_tool");

    // register_package 修改的是内存缓存，需写入磁盘后重新读取
    Cache::instance().write();
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("stable-pkg"));
}

// ── 14. 安装后验证新文件 ──
TEST_F(AtomicRollbackTest, NewFileCreatedOnSuccess) {
    std::string pkg_path = make_pkg("new-file-pkg", "1.0", {"usr/share/new_file.txt"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("new-file-pkg", "1.0", true, "", pkg_path);
    EXPECT_NO_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_TRUE(fs::exists(test_root / "usr/share/new_file.txt"));
}

// ══════════════════════════════════════════════════════════════════════
// 事务日志验证
// ══════════════════════════════════════════════════════════════════════

// ── 15. 成功安装后日志有 COMMIT ──
TEST_F(AtomicRollbackTest, LogContainsCommitOnSuccess) {
    create_file("usr/bin/log_test", "old");
    std::string pkg_path = make_pkg("log-pkg", "1.0", {"usr/bin/log_test"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("log-pkg", "1.0", true, "", pkg_path);
    task.run_simple();
    Config::instance().set_force_overwrite_mode(false);

    fs::path log_path = Config::instance().lock_dir() / "transaction.log";
    ASSERT_TRUE(fs::exists(log_path));
    std::ifstream f(log_path);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    EXPECT_TRUE(content.find("COMMIT log-pkg 1.0") != std::string::npos);
    EXPECT_TRUE(content.find("END log-pkg 1.0") != std::string::npos);
}

// ── 16. 回滚后日志有 ROLLBACK ──
TEST_F(AtomicRollbackTest, LogContainsRollbackOnAbort) {
    create_file("usr/bin/roll_log", "old");
    std::string pkg_path = make_pkg("roll-log-pkg", "1.0", {"usr/bin/roll_log"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("roll-log-pkg", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    fs::path log_path = Config::instance().lock_dir() / "transaction.log";
    ASSERT_TRUE(fs::exists(log_path));
    std::ifstream f(log_path);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    EXPECT_TRUE(content.find("ROLLBACK roll-log-pkg 1.0") != std::string::npos);
    EXPECT_TRUE(content.find("END roll-log-pkg 1.0") != std::string::npos);
}

// ── 17. 回滚日志包含 BACKUP 和 RESTORE ──
TEST_F(AtomicRollbackTest, LogHasBackupAndRestore) {
    create_file("usr/lib/libtest.so", "lib");
    std::string pkg_path = make_pkg("bak-log", "1.0", {"usr/lib/libtest.so"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("bak-log", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    fs::path log_path = Config::instance().lock_dir() / "transaction.log";
    std::ifstream f(log_path);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    EXPECT_TRUE(content.find("BACKUP") != std::string::npos) << "log has BACKUP";
    EXPECT_TRUE(content.find("RESTORE") != std::string::npos) << "log has RESTORE";
}

// ══════════════════════════════════════════════════════════════════════
// lpkg rec 恢复测试
// ══════════════════════════════════════════════════════════════════════

// ── 18. rec 恢复孤儿 .lpkgbak ──
TEST_F(AtomicRollbackTest, RecoverRestoresOrphanedBak) {
    // 模拟一个孤儿备份文件（原文件不在了）
    fs::path orig = test_root / "usr" / "bin" / "lost_binary";
    fs::create_directories(orig.parent_path());
    { std::ofstream f(orig); f << "original content"; }

    // 模拟备份阶段（rename original → .lpkg_bak_pkg）
    fs::path bak = orig;
    bak += ".lpkg_bak_mypkg";
    fs::rename(orig, bak);

    // 原文件不在了
    ASSERT_FALSE(fs::exists(orig));
    ASSERT_TRUE(fs::exists(bak));

    // rec 应恢复
    recover_packages();

    EXPECT_TRUE(fs::exists(orig)) << "recover should restore the file";
    std::ifstream f(orig);
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "original content");
    // .lpkgbak 应被消费（rename 后不再存在）
    EXPECT_FALSE(fs::exists(bak));
}

// ── 19. rec 清理 .lpkgtmp ──
TEST_F(AtomicRollbackTest, RecoverCleansOrphanedTmp) {
    fs::path tmp_file = test_root / "tmp" / "stray.lpkgtmp";
    fs::create_directories(tmp_file.parent_path());
    { std::ofstream f(tmp_file); f << "garbage"; }

    recover_packages();

    EXPECT_FALSE(fs::exists(tmp_file)) << "recover should clean .lpkgtmp";
}

// ── 20. rec 恢复多个孤儿备份 ──
TEST_F(AtomicRollbackTest, RecoverMultipleOrphans) {
    std::vector<std::string> files = {"usr/lib/a.so", "usr/lib/b.so", "usr/bin/tool"};
    for (const auto& f : files) {
        auto orig = test_root / f;
        fs::create_directories(orig.parent_path());
        { std::ofstream of(orig); of << "data:" << f; }

        auto bak = fs::path(orig).concat(".lpkg_bak_mypkg");
        fs::rename(orig, bak);
    }

    recover_packages();

    for (const auto& f : files) {
        auto orig = test_root / f;
        EXPECT_TRUE(fs::exists(orig)) << "recover should restore: " << f;
        std::ifstream of(orig);
        std::string content; std::getline(of, content);
        EXPECT_EQ(content, "data:" + f);
    }
}

// ── 21. rec 无孤儿文件不崩溃 ──
TEST_F(AtomicRollbackTest, RecoverNoOrphansNoCrash) {
    EXPECT_NO_THROW(recover_packages());
}

// ══════════════════════════════════════════════════════════════════════
// 文件名含空格
// ══════════════════════════════════════════════════════════════════════

// ── 24. 文件名含空格 → 回滚正常 ──
TEST_F(AtomicRollbackTest, SpaceInFilename_RollbackWorks) {
    create_file("usr/lib/my library.so", "spacey lib");
    std::string pkg_path = make_pkg("space-pkg", "1.0", {"usr/lib/my library.so"});

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("space-pkg", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    std::ifstream f(test_root / "usr/lib/my library.so");
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "spacey lib");
}

// ── 25. 文件名含空格 → rec 恢复 ──
TEST_F(AtomicRollbackTest, SpaceInFilename_RecRestores) {
    fs::path orig = test_root / "usr" / "share" / "my data file.txt";
    fs::create_directories(orig.parent_path());
    { std::ofstream f(orig); f << "important data"; }

    fs::path bak = fs::path(orig).concat(".lpkg_bak_pkg");
    fs::rename(orig, bak);

    recover_packages();

    EXPECT_TRUE(fs::exists(orig));
    std::ifstream f(orig);
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "important data");
}

// ══════════════════════════════════════════════════════════════════════
// 断电模拟
// ══════════════════════════════════════════════════════════════════════

// ── 26. 断电恢复：只有 BEGIN 没有其他日志 → rec 恢复孤儿 .lpkgbak ──
TEST_F(AtomicRollbackTest, PowerLoss_BeginOnly_RecRestores) {
    // 模拟断电场景：只有 BEGIN 在日志中，然后进程崩溃
    TransactionLog::log_raw("BEGIN crashed-pkg 1.0");
    // 孤儿 .lpkgbak（备份已完成但 COPY 未开始就断电）
    fs::path orig = test_root / "usr" / "bin" / "powerlost";
    fs::create_directories(orig.parent_path());
    { std::ofstream f(orig); f << "precious data"; }
    fs::path bak = fs::path(orig).concat(".lpkg_bak_crashed-pkg");
    fs::rename(orig, bak);

    // rec 应恢复（扫描 .lpkgbak，不依赖日志）
    recover_packages();

    EXPECT_TRUE(fs::exists(orig));
    std::ifstream f(orig);
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "precious data");
}

// ── 27. 断电 recovery：部分写入日志行不破坏恢复 ──
TEST_F(AtomicRollbackTest, PowerLoss_PartialLine_RecStillWorks) {
    // 模拟日志文件末尾有部分写入行（断电导致 write() 未完成）
    {
        TransactionLog log;
        log.begin("partial-pkg", "1.0");
        log.backup("/usr/lib/ok.so", "/usr/lib/ok.so.lpkg_bak_partial-pkg");
    }
    // 追加一个部分行（模拟断电中断 write）
    {
        std::ofstream f(Config::instance().lock_dir() / "transaction.log", std::ios::app);
        f << "[2026-07-13 10:00:00] BACKUP /usr/lib/partial";
        // 没有换行 — 模拟部分写入
    }

    // 创建孤儿 .lpkgbak（模拟部分完成的备份）
    fs::path orig = test_root / "usr" / "lib" / "ok.so";
    fs::create_directories(orig.parent_path());
    { std::ofstream f(orig); f << "ok data"; }
    fs::path bak = fs::path(orig).concat(".lpkg_bak_partial-pkg");
    fs::rename(orig, bak);

    // rec 应仍能恢复 ok.so（文件扫描不依赖日志完整性）
    recover_packages();

    EXPECT_TRUE(fs::exists(orig));
    std::ifstream f(orig);
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "ok data");
}

// ── 28. 断电后日志有 BEGIN 无 END → check_pending 检测 ──
TEST_F(AtomicRollbackTest, PowerLoss_CheckPendingDetects) {
    TransactionLog::log_raw("BEGIN lost-pkg 3.0");

    std::string pending = TransactionLog::check_pending();
    EXPECT_EQ(pending, "lost-pkg") << "check_pending should detect the incomplete transaction";
}

// ── 22. rec 恢复时原文件已存在 → 覆盖恢复（.bak 是正确版本） ──
TEST_F(AtomicRollbackTest, RecoverOverwritesExisting) {
    fs::path orig = test_root / "usr" / "bin" / "tool";
    fs::create_directories(orig.parent_path());
    { std::ofstream f(orig); f << "wrong version content"; }

    // 创建 .lpkgbak（正确版本）
    fs::path bak = fs::path(orig).concat(".lpkg_bak_mypkg");
    { std::ofstream f(bak); f << "correct version from backup"; }

    recover_packages();

    // .lpkgbak 应覆盖原文件
    std::ifstream f(orig);
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "correct version from backup");
}

// ── 23. rec 扫描完整 /usr 树 ──
TEST_F(AtomicRollbackTest, RecoverDeeplyNestedBak) {
    fs::path orig = test_root / "usr" / "share" / "deep" / "nested" / "file.dat";
    fs::create_directories(orig.parent_path());
    { std::ofstream f(orig); f << "deep data"; }

    fs::path bak = fs::path(orig).concat(".lpkg_bak_pkg");
    fs::rename(orig, bak);

    recover_packages();

    EXPECT_TRUE(fs::exists(orig));
    std::ifstream f(orig);
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "deep data");
}
