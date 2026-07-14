#pragma once

#include <string>
#include <vector>

#include "../repo/repository.hpp"
#include "version.hpp"

namespace detail {

/**
 * 解析依赖字符串列表为 DependencyInfo 结构体
 *
 * 输入格式（每个字符串一个依赖项）：
 *   "glibc >= 2.0.0 < 3.0.0"
 *
 * 支持复合约束表达区间，解析出所有 (op, version) 对。
 */
std::vector<DependencyInfo>
parse_dep_strings(const std::vector<std::string> &dep_strs);

} // namespace detail
