#pragma once

#include <string>
#include <vector>

#include "version.hpp"

/**
 * 依赖信息：包名 + 可选的多个版本约束
 *
 * 支持复合约束表达区间，格式（索引 / metadata.json）：
 *   "glibc >= 2.0.0 < 3.0.0"
 * 解析时先按包名分隔符（逗号）切割，再在一个 dep 内提取所有 (op, version) 对。
 *
 * **它住在这里（`vercmp/`）而不是 `repo/repository.hpp`**（2026-09-26 移动）：它是
 * `parse_dep_strings()` 的返回类型，而那个函数是**纯语法**解析（只依赖 `Constraint`）。
 * 之前它定义在仓库层，导致本头文件必须 `#include "../repo/repository.hpp"` —— 一个分层倒置：
 * 想用依赖串解析就得把整个仓库层拖进来。现在方向正过来了（`repository.hpp` 反过来包含本文件）。
 */
struct DependencyInfo {
    std::string name;                     // 依赖包名
    std::vector<Constraint> constraints;  // 版本约束列表，为空则无版本要求
};

namespace detail
{

/**
 * 解析依赖字符串列表为 DependencyInfo 结构体
 *
 * 输入格式（每个字符串一个依赖项）：
 *   "glibc >= 2.0.0 < 3.0.0"
 *
 * 支持复合约束表达区间，解析出所有 (op, version) 对。
 */
std::vector<DependencyInfo> parse_dep_strings(const std::vector<std::string>& dep_strs);

}  // namespace detail
