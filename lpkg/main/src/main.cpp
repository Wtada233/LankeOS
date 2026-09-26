/**
 * 程序入口 —— **只有**这一件事。
 *
 * 其余全部（选项注册、全局选项落定、root/DB 初始化、13 个 handler、退出码与消息）都在
 * `main_cli.cpp` 的 `run_cli()` 里：`Makefile` 的 `LPKG_OBJS` 排除了本文件（测试二进制
 * 有自己的 `main()`），所以只有把分派路径放进**别的**翻译单元，它才能被打进
 * `build/run_tests` 被用例驱动。详见 `main_cli.hpp` 顶部与
 * tests/unit/test_cli_dispatch.cpp。
 */

#include <string>
#include <vector>

#include "main_cli.hpp"

int main(int argc, char** argv)
{
    // argc/argc+1 区间构造：argv[0] 是程序名，语义与原来 `options.parse(argc, argv)` 一致
    // （argv[argc] == nullptr 不进区间）。
    return run_cli(std::vector<std::string>(argv, argv + argc));
}
