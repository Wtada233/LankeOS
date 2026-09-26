#pragma once

#include <gtest/gtest.h>

#include <atomic>

#include "../main/src/trigger/trigger.hpp"

/**
 * `sigint_graceful` 的**定义**在 `main/src/main_cli.cpp`（2026-09-26 从 `main.cpp` 下沉）：
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
 * 只有当"负责把状态复位的那一个用例"恰好也在本次过滤范围内时才会被清掉。实测两种踩法：
 *
 *   1. `TriggerManager`（触发器规则表 + 粘性的 `config_loaded`）：一个端到端用例因它
 *      "单跑绿、全量红"，最后只能改成不依赖任何触发器来自保；
 *   2. `sigint_graceful`（SIGINT 优雅退出标志，**定义在 `main/src/main_cli.cpp`** —— 它随
 *      `LPKG_OBJS` 进测试二进制，两个二进制共用一份）：`SigIntTest.AtomicRollbackOnSigInt`
 *      跑完把它留在 `true`，而复位写在同一个套件的另一个用例里。于是
 *      `--gtest_filter="SigIntTest.AtomicRollbackOnSigInt:TypeTransitionMatrixTest.FileToFile"`
 *      里**后一条必然红** —— 报 `Installation aborted by user (SIGINT)`，看起来像被测代码坏了。
 *      （2026-09-26 实测复现：`3 tests ran / 1 PASSED / 2 FAILED`。这不是某个改动引入的，
 *       改前改后一样。）
 *
 * 所以复位放在**与 fixture 无关**的地方：一个 listener，在每个用例结束时无条件清一遍。
 * **新增全局状态时请在这里一并复位**（并在上面两段里补一行说明，写清"单跑绿全量红"的那种症状）。
 */
class TestHygieneListener : public ::testing::EmptyTestEventListener
{
public:
    void OnTestEnd(const ::testing::TestInfo&) override
    {
        sigint_graceful.store(false);                 // 见上文第 2 条
        TriggerManager::instance().reset_for_test();  // 见上文第 1 条
    }
};

/// 在 `main()` 里 `InitGoogleTest` **之后**调用一次（`RUN_ALL_TESTS` 之前）。
inline void install_test_hygiene_listener()
{
    ::testing::UnitTest::GetInstance()->listeners().Append(new TestHygieneListener);
}
