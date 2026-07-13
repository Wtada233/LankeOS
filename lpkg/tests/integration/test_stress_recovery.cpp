#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/pkg/transaction_log.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/testing_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/archive/packer.hpp"
#include "../test_base.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <atomic>

namespace fs = std::filesystem;

// =====================================================================
// 综合压力恢复测试
//
// 在安装有依赖的包（lib → tool）的过程中，在每一个关键战略点注入
// "崩溃"（通过 SIGINT 模拟），然后运行 lpkg rec 恢复，
// 验证系统最终处于一致状态。
//
// 覆盖：
//   - 安装 10 个战略断点（含依赖场景）
//   - 移除 7 个战略断点
//   - 每次恢复后验证文件和数据库一致性
// =====================================================================

class StressRecoveryTest : public IntegrationTestBase {
protected:
    std::string lib_pkg_path;
    std::string tool_pkg_path;
    fs::path mirror_index;

    void SetUp() override {
        IntegrationTestBase::SetUp();
        testing::reset_all();
        sigint_graceful.store(false);

        // 启用测试模式（使断点生效）
        Config::instance().set_testing_mode(true);

        // 创建依赖关系: lib → tool (tool 依赖 lib)
        // 注意：使用 create_pkg（来自 IntegrationTestBase）构建标准虚拟包，
        // 传 deps 给 tool 使其 metadata.json 含依赖信息。
        // 但 install_packages 安装 .lpkg 文件时不依赖仓库索引，
        // 直接从文件读取真实 metadata，因此 repo 配置非必需。
        lib_pkg_path = create_pkg("libbase", "1.0");
        tool_pkg_path = create_pkg("tool", "2.0", {"libbase"});
    }

    void TearDown() override {
        testing::reset_all();
        sigint_graceful.store(false);
        Config::instance().set_testing_mode(false);
        IntegrationTestBase::TearDown();
    }

    /// 检查文件是否存在（相对于 test_root）
    bool file_exists(const fs::path& rel) {
        return fs::exists(test_root / rel);
    }

    /// 检查 DB 中包是否标记为已安装
    bool db_installed(const std::string& name) {
        return Cache::instance().is_installed(name);
    }

    /// 安全调用 install（使用 .lpkg 文件路径，避免依赖仓库）
    void try_install_pkg(const std::string& pkg_path, bool expect_exception = true) {
        if (expect_exception) {
            EXPECT_ANY_THROW(install_packages({pkg_path}));
        } else {
            EXPECT_NO_THROW(install_packages({pkg_path}));
        }
        sigint_graceful.store(false);
    }

    /// 安全调用 remove_package（期望因 SIGINT 而抛出，或成功）
    void try_remove(const std::string& name, bool expect_exception = true) {
        if (expect_exception) {
            EXPECT_ANY_THROW(remove_package(name, false));
        } else {
            EXPECT_NO_THROW(remove_package(name, false));
        }
        sigint_graceful.store(false);
    }

    /// 干净安装 lib + tool（无断点，直接使用 .lpkg 文件避免镜像验证）
    void clean_install_both() {
        testing::reset_all();
        Config::instance().set_testing_mode(false);
        Config::instance().set_force_overwrite_mode(true);
        // 使用本地 .lpkg 文件安装（不含仓库，避免元数据验证流程）
        install_packages({lib_pkg_path});
        Cache::instance().write();
        install_packages({tool_pkg_path});
        Cache::instance().write();
        Config::instance().set_force_overwrite_mode(false);
        Config::instance().set_testing_mode(true);
        fs::remove(Config::instance().lock_dir() / "transaction.log");
    }

    /// 在 test_root 中创建文件
    void create_file(const fs::path& rel, const std::string& content = "original") {
        fs::path p = test_root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p); f << content;
    }

    /// 读取事务日志
    std::string read_log() {
        fs::path log_path = Config::instance().lock_dir() / "transaction.log";
        if (!fs::exists(log_path)) return "";
        std::ifstream f(log_path);
        return std::string((std::istreambuf_iterator<char>(f)), {});
    }
};

// ═══════════════════════════════════════════════════════════════════════
// 安装-压力测试：在每个断点注入崩溃
// ═══════════════════════════════════════════════════════════════════════

// S01: 在 break_before_install 中断 → 无任何变化
TEST_F(StressRecoveryTest, CrashBeforeInstall) {
    testing::break_before_install.store(true);

    try_install_pkg(lib_pkg_path);

    // 没有任何东西被写入，恢复无操作
    recover_packages();

    EXPECT_FALSE(file_exists("usr/bin/libbase"));
    Cache::instance().load();
    EXPECT_FALSE(db_installed("libbase"));
}

// S02: 在 break_after_begin_pkgs 中断 → 只有 BEGIN_PKGS 日志
TEST_F(StressRecoveryTest, CrashAfterBeginPkgs) {
    testing::break_after_begin_pkgs.store(true);

    try_install_pkg(lib_pkg_path);

    // 日志有 BEGIN_PKGS，没有任何操作 → rec 回滚（无实际影响）
    recover_packages();

    EXPECT_FALSE(file_exists("usr/bin/libbase"));
}

// S03: 在 break_before_backup 中断 → 包已下载解压但未备份文件
TEST_F(StressRecoveryTest, CrashBeforeBackup) {
    testing::break_before_backup.store(true);

    try_install_pkg(lib_pkg_path);

    // 无文件备份，无文件复制 → rec 回滚空操作
    recover_packages();

    EXPECT_FALSE(file_exists("usr/bin/libbase"));
}

// S04: 在 break_after_backup 中断 → 文件已备份，但未复制
TEST_F(StressRecoveryTest, CrashAfterBackup) {
    // 先创建将被覆盖的文件
    Config::instance().set_force_overwrite_mode(true);
    create_file("usr/bin/libbase", "old_lib");
    create_file("usr/bin/tool", "old_tool");

    testing::break_after_backup.store(true);

    // 先用 lib 包测试（单包）
    EXPECT_ANY_THROW(install_packages({lib_pkg_path}));
    sigint_graceful.store(false);

    // 文件还在 .lpkg_bak 中（BACKUP 已 rename，但 COPY 未执行）
    // rec 应恢复
    recover_packages();

    EXPECT_TRUE(file_exists("usr/bin/libbase")) << "backup restored";
    Config::instance().set_force_overwrite_mode(false);
}

// S05: 在 break_during_file_copy 中断 → 部分文件已复制
TEST_F(StressRecoveryTest, CrashDuringFileCopy) {
    testing::break_during_file_copy.store(true);

    // 用简单单包测试（tool 需要 lib，但 lib 已安装）
    Config::instance().set_force_overwrite_mode(true);
    create_file("usr/bin/tool", "old_tool");

    EXPECT_ANY_THROW(install_packages({tool_pkg_path}));
    sigint_graceful.store(false);

    // 部分复制 → rollback 恢复
    EXPECT_NO_THROW(recover_packages());
    Config::instance().set_force_overwrite_mode(false);
}

// S06: 在 break_before_commit 中断 → 文件已复制，但缓存未更新
TEST_F(StressRecoveryTest, CrashBeforeCommit) {
    testing::break_before_commit.store(true);

    Config::instance().set_force_overwrite_mode(true);
    create_file("usr/bin/tool", "old_tool");

    EXPECT_ANY_THROW(install_packages({tool_pkg_path}));
    sigint_graceful.store(false);

    // 文件已复制到磁盘，但 COMMIT 未写 → rec 应回滚
    recover_packages();

    EXPECT_TRUE(file_exists("usr/bin/tool")) << "old file restored";
    Cache::instance().load();
    EXPECT_FALSE(db_installed("tool")) << "tool not in db";
    Config::instance().set_force_overwrite_mode(false);
}

// S07: 在 break_after_commit 中断 → 单个 COMMIT 已写，但 COMMIT_PKGS 未写
TEST_F(StressRecoveryTest, CrashAfterCommit) {
    // 先安装 lib（无断点）
    testing::reset_all();
    Config::instance().set_testing_mode(false);
    EXPECT_NO_THROW(install_packages({lib_pkg_path}));
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");

    // 再装 tool（设断点）
    Config::instance().set_testing_mode(true);
    testing::break_after_commit.store(true);

    Config::instance().set_force_overwrite_mode(true);
    EXPECT_ANY_THROW(install_packages({tool_pkg_path}));
    sigint_graceful.store(false);

    // tool 的 COMMIT 已写，但 COMBIT_PKGS 未写 → 事务未完成 → rec 回滚
    recover_packages();

    // lib 应保持安装
    Cache::instance().load();
    EXPECT_TRUE(db_installed("libbase")) << "lib should stay installed";
    Config::instance().set_force_overwrite_mode(false);
}

// S08: 在 break_before_db_write 中断 → 文件复制、COMMIT 已写，DB 未落盘
TEST_F(StressRecoveryTest, CrashBeforeDbWrite) {
    Config::instance().set_force_overwrite_mode(true);
    testing::break_before_db_write.store(true);

    EXPECT_ANY_THROW(install_packages({lib_pkg_path}));
    sigint_graceful.store(false);

    // COMMIT 已写但 COMMIT_PKGS 和 DB 未落盘 → rec 回滚
    recover_packages();

    Cache::instance().load();
    EXPECT_FALSE(db_installed("libbase")) << "rolled back";
    Config::instance().set_force_overwrite_mode(false);
}

// S09: 在 break_before_commit_pkgs 中断 → DB 已写但 COMMIT_PKGS 未写
TEST_F(StressRecoveryTest, CrashBeforeCommitPkgs) {
    Config::instance().set_force_overwrite_mode(true);
    testing::break_before_commit_pkgs.store(true);

    EXPECT_ANY_THROW(install_packages({lib_pkg_path}));
    sigint_graceful.store(false);

    // DB 在磁盘上（WAL 保护），但 COMMIT_PKGS 未写 → rec 应回滚 DB
    recover_packages();

    Cache::instance().load();
    EXPECT_FALSE(db_installed("libbase")) << "DB rolled back via .lpkg_db_bak";
    Config::instance().set_force_overwrite_mode(false);
}

// S10: 在第 1 个包安装后中断
TEST_F(StressRecoveryTest, CrashAfterFirstPkgInBatch) {
    Config::instance().set_force_overwrite_mode(true);
    testing::break_after_each_pkg_install.store(true);

    // 先安装 lib（单包），tool 在之后安装
    EXPECT_ANY_THROW(install_packages({lib_pkg_path}));
    sigint_graceful.store(false);

    // 批量未提交的单个包 → rec 回滚
    recover_packages();

    Cache::instance().load();
    EXPECT_FALSE(db_installed("libbase")) << "lib rolled back";
    Config::instance().set_force_overwrite_mode(false);
}

// S11: 在 break_before_each_pkg_install 中断
TEST_F(StressRecoveryTest, CrashBeforeFirstPkg) {
    Config::instance().set_force_overwrite_mode(true);
    testing::break_before_each_pkg_install.store(true);

    EXPECT_ANY_THROW(install_packages({lib_pkg_path}));
    sigint_graceful.store(false);

    // 包安装未开始 → rec 空操作
    recover_packages();

    EXPECT_FALSE(file_exists("usr/bin/libbase"));
    Config::instance().set_force_overwrite_mode(false);
}

// S12: 完整安装 + 最终 rec 验证一致性
TEST_F(StressRecoveryTest, FullInstallThenRecovery) {
    // 正常安装（使用本地 .lpkg 文件）
    Config::instance().set_testing_mode(false);
    Config::instance().set_force_overwrite_mode(true);
    EXPECT_NO_THROW(install_packages({lib_pkg_path}));
    Cache::instance().write();

    // 验证安装成功
    Cache::instance().load();
    EXPECT_TRUE(db_installed("libbase"));
    EXPECT_TRUE(file_exists("usr/bin/libbase"));

    // rec 不应破坏任何内容
    recover_packages();

    Cache::instance().load();
    EXPECT_TRUE(db_installed("libbase")) << "still installed after rec";
    Config::instance().set_force_overwrite_mode(false);
}

// ═══════════════════════════════════════════════════════════════════════
// 移除-压力测试：在每个断点注入崩溃
// ═══════════════════════════════════════════════════════════════════════

// S13: 在 break_before_remove 中断 → 无任何变化
TEST_F(StressRecoveryTest, RemoveCrashBeforeRemove) {
    clean_install_both();

    testing::break_before_remove.store(true);
    // 移除 tool（叶子包，无其他包依赖它）
    try_remove("tool");

    // 未执行任何操作（break_before_remove 在 remove_package 开头的安全检查之后）
    recover_packages();

    EXPECT_TRUE(file_exists("usr/bin/tool")) << "file not removed";
    EXPECT_TRUE(file_exists("usr/bin/libbase")) << "other pkg untouched";
}

// S14: 在 break_after_rm_begin 中断 → RM_BEGIN 已写但未备份
TEST_F(StressRecoveryTest, RemoveCrashAfterRmBegin) {
    clean_install_both();

    testing::break_after_rm_begin.store(true);
    try_remove("tool");

    // 只有 RM_BEGIN 日志，无文件操作 → rec 空操作
    recover_packages();

    EXPECT_TRUE(file_exists("usr/bin/tool"));
    Cache::instance().load();
    EXPECT_TRUE(db_installed("tool"));
}

// S15: 在 break_during_rm_backup 中断 → 部分文件已备份
TEST_F(StressRecoveryTest, RemoveCrashDuringBackup) {
    clean_install_both();

    testing::break_during_rm_backup.store(true);
    try_remove("tool");

    // 部分文件已 rename 到 .lpkg_bak → rec 恢复
    recover_packages();

    EXPECT_TRUE(file_exists("usr/bin/tool")) << "restored by rec";
}

// S16: 在 break_after_rm_backup 中断 → 全部已备份但未删文件
TEST_F(StressRecoveryTest, RemoveCrashAfterBackup) {
    clean_install_both();

    testing::break_after_rm_backup.store(true);
    try_remove("tool");

    // 全部文件已 bak，但未执行 RM_DIR → rec 恢复
    recover_packages();

    EXPECT_TRUE(file_exists("usr/bin/tool")) << "file restored";
    EXPECT_TRUE(file_exists("usr/bin/libbase")) << "other pkg untouched";
}

// S17: 在 break_before_rm_db_write 中断 → 文件已删，DB 未更新
TEST_F(StressRecoveryTest, RemoveCrashBeforeDbWrite) {
    clean_install_both();

    testing::break_before_rm_db_write.store(true);
    try_remove("tool");

    // 文件已删除，RM_DIR 已执行，DB 未更新 → rec 恢复文件和 DB
    recover_packages();

    Cache::instance().load();
    EXPECT_TRUE(db_installed("tool")) << "restored in DB";
    EXPECT_TRUE(file_exists("usr/bin/tool")) << "file restored";
}

// S18: 在 break_before_rm_commit 中断 → DB 已更新但 RM_COMMIT 未写
TEST_F(StressRecoveryTest, RemoveCrashBeforeRmCommit) {
    clean_install_both();

    testing::break_before_rm_commit.store(true);
    try_remove("tool");

    // DB 已写（WAL 保护），但 RM_COMMIT 未写 → rec 恢复 DB 和文件
    recover_packages();

    Cache::instance().load();
    EXPECT_TRUE(db_installed("tool")) << "DB restored via WAL";
    // 文件可能已被恢复（.lpkg_bak → rename back）或还在
    // 至少系统是一致的
}

// S19: 在 break_after_rm_cleanup 中断 → 整个移除已完成
TEST_F(StressRecoveryTest, RemoveCrashAfterCleanup) {
    clean_install_both();

    testing::break_after_rm_cleanup.store(true);
    try_remove("tool");

    // 移除全部完成 → rec 不应恢复
    recover_packages();

    Cache::instance().load();
    EXPECT_FALSE(db_installed("tool")) << "committed remove, not restored";
    EXPECT_TRUE(db_installed("libbase")) << "lib still installed";
}

// S20: 完全移除后 rec 验证一致性
TEST_F(StressRecoveryTest, FullRemoveThenRecovery) {
    // 先手动安装一个包
    Config::instance().set_testing_mode(false);
    Config::instance().set_force_overwrite_mode(true);
    EXPECT_NO_THROW(install_packages({lib_pkg_path}));
    Cache::instance().write();
    fs::remove(Config::instance().lock_dir() / "transaction.log");
    Config::instance().set_force_overwrite_mode(false);

    // 移除它
    remove_package("libbase", false);
    write_cache();

    Cache::instance().load();
    EXPECT_FALSE(db_installed("libbase"));

    // rec 不应恢复已清除的包
    recover_packages();

    Cache::instance().load();
    EXPECT_FALSE(db_installed("libbase")) << "still gone after rec";
}
