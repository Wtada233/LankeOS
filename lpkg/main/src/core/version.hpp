#pragma once

#include <string>
#include <vector>

/**
 * 比较两个版本号字符串
 * v1 > v2 返回 true, 否则返回 false
 */
bool version_compare(const std::string& v1_str, const std::string& v2_str);

/**
 * 检查版本号是否满足指定的版本约束
 * 例如 version_satisfies("1.2.0", ">=", "1.0.0") 返回 true
 */
bool version_satisfies(const std::string& current_version, const std::string& op, const std::string& required_version);
