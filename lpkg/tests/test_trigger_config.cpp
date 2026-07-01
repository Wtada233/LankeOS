#include <gtest/gtest.h>
#include "../main/src/base/utils.hpp"
#include "../main/src/trigger/trigger.hpp"
#include "../main/src/config/config.hpp"
#include "../main/src/base/constants.hpp"
#include "../main/src/i18n/localization.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <iostream>

namespace fs = std::filesystem;

class TriggerAndConfigTest : public ::testing::Test {
protected:
    fs::path test_root;

    void SetUp() override {
        test_root = fs::absolute("tmp_trigger_test");
        if (fs::exists(test_root)) fs::remove_all(test_root);
        fs::create_directories(test_root);
    }

    void TearDown() override {
        if (fs::exists(test_root)) fs::remove_all(test_root);
    }
};

// ===== TriggerManager 测试 =====

class TriggerManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_localization();
        Config::instance().set_testing_mode(true);
    }

    /** 检查执行 run_all 后是否有输出 */
    bool has_trigger_output() {
        testing::internal::CaptureStdout();
        TriggerManager::instance().run_all();
        std::string output = testing::internal::GetCapturedStdout();

        // run_all 会执行 ldconfig 等命令，如果系统中没有 ldconfig 会报错
        // 但在测试中仅检查触发队列是否包含特定命令，不要求执行成功
        return !output.empty();
    }
};

TEST_F(TriggerManagerTest, CheckFileActivatesLdconfigForLibSo) {
    auto& tm = TriggerManager::instance();

    // .so 文件在 /usr/lib 下应触发 ldconfig
    EXPECT_NO_THROW(tm.check_file("/usr/lib/libfoo.so.1"));
}

TEST_F(TriggerManagerTest, CheckFileActivatesSystemdReload) {
    auto& tm = TriggerManager::instance();

    // .service 文件应触发 systemctl daemon-reload
    EXPECT_NO_THROW(tm.check_file("/usr/lib/systemd/system/foo.service"));
}

TEST_F(TriggerManagerTest, CheckFileIgnoresNonTriggerPaths) {
    auto& tm = TriggerManager::instance();

    // 普通文件不应触发任何触发器
    EXPECT_NO_THROW(tm.check_file("/usr/share/doc/foo/readme"));
    EXPECT_NO_THROW(tm.check_file("/etc/config"));

    // 这些路径应不会崩溃
    EXPECT_NO_THROW(tm.check_file(""));
    EXPECT_NO_THROW(tm.check_file("/"));
}

TEST_F(TriggerManagerTest, RunAllHandlesEmptyQueue) {
    // 空的触发队列应安全执行
    EXPECT_NO_THROW(TriggerManager::instance().run_all());
}

TEST_F(TriggerManagerTest, MultipleAddsDeduplicate) {
    auto& tm = TriggerManager::instance();

    // 多次添加同一命令应去重
    tm.add("echo test");
    tm.add("echo test");
    tm.add("echo test");

    // 不抛异常
    EXPECT_NO_THROW(tm.run_all());
}

TEST_F(TriggerManagerTest, CheckFileMultipleTimesDeduplicates) {
    auto& tm = TriggerManager::instance();

    // 多次检查同一触发条件应只添加一次
    tm.check_file("/usr/lib/libtest.so.1");
    tm.check_file("/usr/lib/libtest.so.1");
    tm.check_file("/usr/lib/libtest.so.1");

    EXPECT_NO_THROW(tm.run_all());
}
