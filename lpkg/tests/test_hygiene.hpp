#pragma once

#include <gtest/gtest.h>

#include <atomic>

#include "../main/src/base/utils.hpp"  // set_durable_fsync_enabled
#include "../main/src/config/config.hpp"
#include "../main/src/db/test_breakpoints.hpp"
#include "../main/src/trigger/trigger.hpp"

/**
 * `sigint_graceful` 的**定义**在 `main/src/main_cli.cpp`（从 `main.cpp` 下沉）：
 * 该翻译单元在 `LPKG_OBJS` 里 ⇒ 测试二进制与 `build/lpkg` **共用同一份**定义
 * （此前测试二进制不链接 `main.o`，只能由 `tests/integration/test_sigint.cpp` 自己造一份，
 * 与生产那份互相独立 —— 于是测试置位它并不能真的影响生产的 CLI 路径）。这里只声明，
 * 供下面的 listener 复位用。
 */
extern std::atomic<bool> sigint_graceful;

/**
 * **测试卫生：每个用例结束后无条件复位「进程级全局状态」**（与 fixture 无关）。
 *
 * ## 为什么不能靠 fixture 的 TearDown
 * gtest 的 `TearDown` 只在**用例所属的那个 fixture** 里生效。本仓库有几处状态是**进程级**的
 * （跨 fixture、跨套件存活），于是一个"污染者"用例会把状态留给**它后面每一个**用例 ——
 * 只有当"负责把状态复位的那一个用例"恰好也在本次过滤范围内时才会被清掉。三种踩法：
 *
 *   1. `TriggerManager`（触发器规则表 + 粘性的 `config_loaded`）：一个端到端用例因它
 *      "单跑绿、全量红"，最后只能改成不依赖任何触发器来自保；
 *   2. `sigint_graceful`（SIGINT 优雅退出标志，**定义在 `main/src/main_cli.cpp`** —— 它随
 *      `LPKG_OBJS` 进测试二进制，两个二进制共用一份）：`SigIntTest.AtomicRollbackOnSigInt`
 *      跑完把它留在 `true`，而复位写在同一个套件的另一个用例里。于是
 *      `--gtest_filter="SigIntTest.AtomicRollbackOnSigInt:TypeTransitionMatrixTest.FileToFile"`
 *      里**后一条必然红** —— 报 `Installation aborted by user (SIGINT)`，看起来像被测代码坏了。
 *      （复现输出：`3 tests ran / 1 PASSED / 2 FAILED`。这不是某个改动引入的，
 *       改前改后一样。）
 *   3. `Config` 的各模式开关：`test_hook_log_message` 在自己的 TearDown 里
 *      `set_no_hooks_mode(true)`，于是**后面每个用例都跑在"钩子禁用"下**。症状有两种，第二种更阴：
 *      ① `PhaseSectionTest` 里"运行安装后钩子"那一节整段消失；
 *      ② 它那条"**没有**钩子的包不该出现该阶段"的断言因此变成**空转的绿**（两种情形都给绿）。
 *      `overwrite_patterns_` / `use_system_soname_mode_` / `durable_fsync_enabled` 同理 ——
 *      一个用例打开的开关会改变后面用例的**语义**，而断言常常照样绿。
 *
 * 所以复位放在**与 fixture 无关**的地方：一个 listener，在每个用例结束时无条件清一遍。
 * **新增全局状态时请在这里一并复位**（并在上面几段里补一行说明，写清"单跑绿全量红"的那种症状）。
 *
 * ⚠️ 复位的是**配置类**状态；`Cache`（内存 DB）不在其列 —— 它由各 fixture 的 SetUp 自己
 * `load()`，而 listener 跑在 fixture 的 TearDown **之后**，此时代码已经不需要那份状态了。
 */
class TestHygieneListener : public ::testing::EmptyTestEventListener
{
public:
    void OnTestEnd(const ::testing::TestInfo&) override
    {
        sigint_graceful.store(false);                 // 见上文第 2 条
        TriggerManager::instance().reset_for_test();  // 见上文第 1 条
        Config::instance().reset_for_test();          // 见上文第 3 条（各模式开关 + 路径 + 架构）
        set_durable_fsync_enabled(false);             // 见上文第 3 条（CLI 用例会打开它）
        // 断点也是进程级状态：设了断点却在跑之前就抛异常的用例，会把断点留给下一个用例
        // （"命中即清除"只对命中过的生效）。清一遍是零成本的保险。
        BreakpointManager::instance().clear_all();
    }
};

/// 在 `main()` 里 `InitGoogleTest` **之后**调用一次（`RUN_ALL_TESTS` 之前）。
inline void install_test_hygiene_listener()
{
    ::testing::UnitTest::GetInstance()->listeners().Append(new TestHygieneListener);
}
