#pragma once

#include <string>
#include <vector>

/**
 * 版本约束结构体：运算符 + 目标版本
 * 用于支持同一包的复合区间约束（如 >= 2.0.0 且 < 3.0.0）
 */
struct Constraint {
  std::string op;
  std::string version;

  bool operator==(const Constraint &other) const {
    return op == other.op && version == other.version;
  }
};

/**
 * 比较两个版本号字符串
 * v1 > v2 返回 true, 否则返回 false
 */
bool version_compare(const std::string &v1_str, const std::string &v2_str);

/**
 * 检查版本号是否满足指定的版本约束
 * 例如 version_satisfies("1.2.0", ">=", "1.0.0") 返回 true
 */
bool version_satisfies(const std::string &current_version,
                       const std::string &op,
                       const std::string &required_version);

/**
 * 检查版本号是否满足所有指定的复合版本约束
 * 例如 version_satisfies_all("2.1.0", [">= 2.0.0", "< 3.0.0"]) 返回 true
 * 传入空列表时始终返回 true（无约束即任意版本均可）
 */
bool version_satisfies_all(const std::string &current_version,
                           const std::vector<Constraint> &constraints);
