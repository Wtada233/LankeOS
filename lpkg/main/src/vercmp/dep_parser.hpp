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

/**
 * 一行 `deps/` 元数据 → 依赖**包名**（去掉版本约束）。空串输入 ⇒ 空串。
 *
 * 存在的理由是**消灭第二份语法**：`deps/` 文件里存的是**原样的**元数据串
 * （`register_package` 直接写 `deps_`），而"从这一行取出包名"曾在四处各写一遍、
 * 用了三种规则（运算符感知 / 纯空白 / `ss >>`）。规则不一的后果是**同一个包在不同路径上
 * 算出不同的键** —— 最要命的一条在 `Cache::ensure_reverse_deps`：用纯空白切名字时，
 * `provb>=2.0` 这种**约束紧贴包名**的写法会让整串成为键，于是"按包名查反向依赖"
 * 永远查不到（`autoremove` 正是这么查的，见集成用例
 * `tests/integration/test_reverse_dep_key_consistency.cpp`）。
 *
 * 判据与 `parse_dep_strings` **同一套**（事实上它就在用本函数取名字）：包名 =
 * **最早出现的**合法运算符**之前**的那一段，两侧去空白；没有运算符则整串就是包名。
 */
std::string dependency_name_of(std::string_view line);

}  // namespace detail
