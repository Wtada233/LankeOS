#pragma once

#include "cxxopts.hpp"

/**
 * CLI 解析层的**可测试边界**。
 *
 * 为什么单独一个翻译单元（而不是留在 main.cpp）：main.cpp 里有 `main()`，测试二进制
 * 也有自己的 `main()`（tests/main.cpp），把这两步放进 main.cpp 就永远只能"绕过 CLI"
 * 测试 —— 而绕过 CLI 的用例对"选项名/`result.count("…")` 里的字符串 typo"是瞎的
 * （count 对未知键只返回 0，连异常都没有：功能静默死掉而全套绿）。抽到这里之后，测试
 * 可以自己构造 Options、喂 argv 数组、调这两个函数、断言 Config。
 *
 * 两者都只依赖参数（无 main() 局部状态），因此可被任意调用方驱动。
 */

/**
 * 注册 install/remove 组选项 —— 本轮新增的 4 个开关（`--purge-config`、
 * `--force-overwrite`、`--overwrite`、`--fsync`）都在这一组。
 *
 * 其余组（general/query/pack/other/位置参数）仍在 main.cpp 里注册：帮助输出按**注册顺序**
 * 分组排版，整组搬家才能逐字节保持同一份帮助文本。选项名/默认值/帮助文本以 main.cpp
 * 原样搬来，不得改动。
 */
void register_cli_options(cxxopts::Options& options);

/**
 * 把解析结果落到进程级配置：覆盖豁免模式（`--force-overwrite` + `--overwrite` 的组装）与
 * durable fsync 全局开关。不含位置参数、`--root`/`--arch` 之类的路径/模式设置（那些仍在
 * main.cpp，因为它们的取值要留在局部供后续命令分发使用）。
 */
void apply_cli_config(const cxxopts::ParseResult& result);
