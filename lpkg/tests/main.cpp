#include <gtest/gtest.h>

#include "test_hygiene.hpp"

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    // 每个用例结束后复位进程级全局状态（`sigint_graceful` / `TriggerManager`）——
    // 为什么不能靠 fixture 的 TearDown，见 test_hygiene.hpp 顶部。
    install_test_hygiene_listener();
    return RUN_ALL_TESTS();
}
