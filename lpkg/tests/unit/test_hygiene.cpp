/**
 * test_hygiene.cpp — **钉住"跨用例复位"这个机制本身**
 *
 * `tests/test_hygiene.hpp` 的全局 listener 在每个用例结束时无条件复位进程级状态
 * （`sigint_graceful` / `TriggerManager` / `Config` 的各模式开关与路径 / `BreakpointManager`）。
 * 那套机制的存在理由是"单跑绿、全量红"，但它**自己没有守卫** —— 哪天有人把它删了/改漏一项，
 * 复现出来的是**别的**套件里随机的红，而不是这里。
 *
 * 所以本文件故意做一对**顺序耦合**的用例：第一个把进程级开关拨到非默认值，第二个**什么都不设**，
 * 直接断言自己看到的是**默认值**。中间隔着 listener ⇒ 第二个必须绿。
 *   · 把 listener 摘掉 → 第二个红（且报错会直接点名是哪个开关被污染）；
 *   · 单跑第二个（`--gtest_filter`）→ 没有污染者，照样绿（不会假红）。
 *
 * 覆盖三类代表（不同宿主，够用了）：`Config` 的模式开关、`Config` 的路径、`BreakpointManager`。
 * 想加新状态时照着这里的第一个用例加一条"设脏"，第二个用例里加一条"断言干净"。
 */

#include "../test_hygiene.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../../main/src/config/config.hpp"
#include "../../main/src/db/test_breakpoints.hpp"

namespace fs = std::filesystem;

class TestHygieneMechanism : public ::testing::Test
{
protected:
    /// 第 1 步：把三类进程级状态**全部拨脏**
    static void pollute_everything()
    {
        Config::instance().set_no_hooks_mode(true);
        Config::instance().set_no_deps_mode(true);
        Config::instance().set_missing_so_no_error_mode(true);
        Config::instance().set_use_system_soname_mode(true);
        Config::instance().set_root_path("/definitely/not/the/default");
        BreakpointManager::instance().set("hygiene_pollution_probe", [] {});
    }

    /// 第 2 步：什么都不设，断言看到的都是默认值
    static void expect_clean_slate()
    {
        EXPECT_FALSE(Config::instance().no_hooks_mode()) << "no_hooks_mode 未被复位";
        EXPECT_FALSE(Config::instance().no_deps_mode()) << "no_deps_mode 未被复位";
        EXPECT_FALSE(Config::instance().missing_so_no_error_mode()) << "缺失-so-容忍 未被复位";
        EXPECT_FALSE(Config::instance().use_system_soname_mode()) << "use_system_soname 未被复位";
        EXPECT_EQ(Config::instance().root_dir(), fs::path("/")) << "root_dir 未被复位";
        // 断点是"命中即清除"的，设了却没命中就会留给下一个用例 —— 复位必须是**无条件**清的
        EXPECT_FALSE(BreakpointManager::instance().hit("hygiene_pollution_probe"))
            << "上一个用例设的断点没被清掉";
    }
};

TEST_F(TestHygieneMechanism, PolluteGlobalState)
{
    pollute_everything();
    // 断言"确实拨脏了"——否则下面那条会变成恒真废话
    ASSERT_TRUE(Config::instance().no_hooks_mode());
    ASSERT_EQ(Config::instance().root_dir(), fs::path("/definitely/not/the/default"));
}

TEST_F(TestHygieneMechanism, NextTestStartsWithACleanSlate)
{
    expect_clean_slate();
}
