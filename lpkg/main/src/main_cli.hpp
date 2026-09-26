#pragma once

#include <string>
#include <vector>

/**
 * CLI 分派层的**可测试边界**（与 `config/cli.hpp` 同一手法，只是这一层更靠外）。
 *
 * 为什么单独一个翻译单元（而不是留在 main.cpp）：`Makefile` 的
 * `LPKG_OBJS = $(filter-out $(BUILD_DIR)/main/main.o, $(MAIN_OBJS))` —— 测试二进制
 * （`build/run_tests`）**不链接 main.o**（它有 tests/main.cpp 自己的 `main()`）。
 * 于是"解析 argv → 应用全局选项 → 校验 → 分派到 13 个 handler"这整条路径此前
 * **一行用例都盖不到**：错误消息、退出码、`--yes/--no` 冲突、位置参数个数校验全是人工冒烟。
 * 搬到这里之后，`main_cli.cpp` 不是 `main.o` ⇒ 自动进 `LPKG_OBJS` ⇒ 自动进测试二进制，
 * 测试可以喂 argv 数组、调 `run_cli`、断言退出码与消息。
 *
 * `extern std::atomic<bool> sigint_graceful`（`sigint_handler` 设、`SigIntGuard` 清、
 * 事务各阶段轮询）的**定义**也随之下沉到 main_cli.cpp —— 它从进程级共享状态变成"两个
 * 二进制里各有一份"的那个例外，所以测试二进制里不再需要自己造一份（见
 * tests/integration/test_sigint.cpp 顶部注释）。
 */

/// 跑一遍完整 CLI：解析 argv → 应用全局选项 → 校验 → 分派。返回**进程退出码**。
/// `main()` 只调它；测试也调它 —— 于是分派路径进了测试二进制（`main.o` 不在其中）。
///
/// `argv[0]` 是程序名（cxxopts 拿它当 `Options` 的名字，也进用法行）；空向量按
/// "没有程序名"处理（仅测试会有这种调用，真实 `main()` 至少有一个元素）。
int run_cli(const std::vector<std::string>& argv);
