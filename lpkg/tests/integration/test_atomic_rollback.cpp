#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../test_base.hpp"
#include <filesystem>
#include <fstream>

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
};

// ── 1. 钩子在复制阶段触发 sigint → rollback 恢复文件 ──
TEST_F(AtomicRollbackTest, HookTriggersRollbackDuringCopy) {
    fs::path target = test_root / "usr" / "lib" / "test.so.1";
    fs::create_directories(target.parent_path());
    { std::ofstream f(target); f << "original host content"; }

    fs::path pkg_work = suite_work_dir / "_pkg";
    fs::create_directories(pkg_work / "content" / "usr" / "lib");
    { std::ofstream f(pkg_work / "content" / "usr" / "lib" / "test.so.1"); f << "lpkg content"; }
    std::string pkg_path = (pkg_dir / "test-pkg-1.0.lpkg").string();
    pack_package(pkg_path, pkg_work.string(), "test-pkg", "1.0");

    Config::instance().set_force_overwrite_mode(true);
    InstallationTask task("test-pkg", "1.0", true, "", pkg_path);
    task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
    EXPECT_ANY_THROW(task.run_simple());
    Config::instance().set_force_overwrite_mode(false);

    std::ifstream f(target);
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "original host content");

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("test-pkg"));
}

// ── 2. 无 force-overwrite → 预检拒绝 ──
TEST_F(AtomicRollbackTest, NoForceOverwrite_RejectsUntracked) {
    fs::path test_file = test_root / "usr" / "bin" / "existing_bin";
    fs::create_directories(test_file.parent_path());
    { std::ofstream f(test_file); f << "host file"; }

    fs::path pkg_work = suite_work_dir / "_pkg2";
    fs::create_directories(pkg_work / "content" / "usr" / "bin");
    { std::ofstream f(pkg_work / "content" / "usr" / "bin" / "existing_bin"); f << "package file"; }
    std::string pkg_path = (pkg_dir / "noforce-pkg-1.0.lpkg").string();
    pack_package(pkg_path, pkg_work.string(), "noforce-pkg", "1.0");

    fs::path mirror = setup_local_mirror();
    fs::create_directories(mirror / "noforce-pkg");
    fs::copy(pkg_path, mirror / "noforce-pkg" / "1.0.lpkg");
    { std::ofstream idx(mirror / "index.txt"); idx << "noforce-pkg|1.0:::|\n"; }

    EXPECT_THROW(install_packages({"noforce-pkg:1.0"}), LpkgException);

    std::ifstream f(test_file);
    std::string content; std::getline(f, content);
    EXPECT_EQ(content, "host file");
}

// ── 3. 回滚后重试 → 第二次安装成功 ──
TEST_F(AtomicRollbackTest, RetryAfterRollback) {
    fs::path target = test_root / "usr" / "lib" / "data.bin";
    fs::create_directories(target.parent_path());
    { std::ofstream f(target); f << "original data"; }

    fs::path pkg_work = suite_work_dir / "_pkg3";
    fs::create_directories(pkg_work / "content" / "usr" / "lib");
    { std::ofstream f(pkg_work / "content" / "usr" / "lib" / "data.bin"); f << "new data"; }
    std::string pkg_path = (pkg_dir / "retry-pkg-1.0.lpkg").string();
    pack_package(pkg_path, pkg_work.string(), "retry-pkg", "1.0");

    // 第一次：force-overwrite + sigint → rollback
    {
        Config::instance().set_force_overwrite_mode(true);
        InstallationTask task("retry-pkg", "1.0", true, "", pkg_path);
        task.on_before_file_copy = [&]{ sigint_graceful.store(true); };
        EXPECT_ANY_THROW(task.run_simple());
        sigint_graceful.store(false);
        Config::instance().set_force_overwrite_mode(false);
    }

    // 验证原始内容
    {
        std::ifstream f(target);
        std::string content; std::getline(f, content);
        EXPECT_EQ(content, "original data");
    }

    // 第二次：force-overwrite 安装（无钩子）
    {
        Config::instance().set_force_overwrite_mode(true);
        InstallationTask task("retry-pkg", "1.0", true, "", pkg_path);
        task.run_simple();
        Config::instance().set_force_overwrite_mode(false);
    }

    // 新内容应写入
    {
        std::ifstream f(target);
        std::string content; std::getline(f, content);
        EXPECT_EQ(content, "new data");
    }
}
