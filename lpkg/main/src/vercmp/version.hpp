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

    bool operator==(const Constraint& other) const
    {
        return op == other.op && version == other.version;
    }
};

// ============================================================================
// 版本语义：**就是 rpm 的 EVR**（`[epoch:]version[-release]`），8.0.0 起
// ============================================================================
//
// 判据只有一份：libsolv 的 EVR 比较（`vercmp/version.cpp` 的 `evr_cmp`），版本串**原样**进。
// 求解器内部的比较用的也是它，所以"求解器按 X 判、安装期按 Y 判"这类不一致**从根上不存在**。
//
// 段按数值比（`6.16.1 > 6.6.1`）；`1.0` 与 `1.0.0` 是**不同**版本（不补 0）；
// `~` 是**预发布**（`1.0~rc1 < 1.0`）；`-` 之后是 **release**（`1.0-1 > 1.0`，revision 语义）。
//
// ⚠️ **订正 2026-10-04（8.0.0，破坏性）—— 这条"桥"被拆掉了，别再照着旧设计写代码：**
// 此前 lpkg 有**自己的一套**版本语义（`-` = 预发布、`+N` = 发行修订号），并用
// `to_libsolv_evr()` / `from_libsolv_evr()` 在两者之间**编解码**（`-`→`~`、
// `+release`→`^^release`），因为 libsolv 只认 rpm 语义。那座桥的代价是：
// **两套判据必须逐条一致，而它们不一致过** —— 2026-10-03 在 4032 组合的
// (候选 × 约束) 矩阵上差 **78 处、两个方向都有**（`= 1.0` 匹配 `1.0+1` 是假满足 ⇒ 装出坏
// 系统；`> 1.0` 不匹配 `1.0+1` 是假不满足 ⇒ 事务无解）。当时的修法是换一个**更冷门的
// 编码**（`^` caret 当 release 分隔符）并配一份保留字符黑名单 —— 治标不治本：只要还存在
// 两套语义，就得靠"手动保持一致"活着。
//
// **8.0.0 的做法是取消第二套语义**：lpkg 直接采用 rpm 的 EVR，
// `to_libsolv_evr`/`from_libsolv_evr`/`EVR_RELEASE_SEP`/`EVR_RESERVED_CHARS` **全部删除**，
// 版本串原样进池。于是桥、黑名单、等价性矩阵这一整套**都不需要了**。
// 连带的行为变化（**有意为之**，不是回归）：
//   · build 的 `release:` 从"版本串后接 `+N`"改成 rpm 的 `-N`（`builder.cpp`）；
//   · 约束与候选的**release 是否参与匹配**改由 rpm 的规则定 —— 例如 `= 1.0` 会匹配
//     `1.0-5`（rpm 里"没写 release"= 任何 release）。这正是 rpm 的语义，且**两边同一份
//     实现**，所以不再有"求解器与安装期打架"这回事；
//   · `is_safe_path_component` 不再拒 `^`/`~`（它们已经不是保留字符）；`:` 仍然拒，
//     但理由换成了**分帧**（`pkgs` 是 `name:version`、索引版本块是 `<ver>:<hash>:…`），
//     与版本语义无关。

/**
 * 比较两个版本号字符串（libsolv EVR / rpm 语义）。`v1 < v2` 返回 true。
 *
 * 实现见 `vercmp/version.cpp` 的 `evr_cmp`（唯一判据）。"最新版"的判定也用它
 * （`repository.cpp` 拿它当 `ranges::sort` 的比较器 ⇒ 升序 ⇒ 最后一版最新）。
 */
bool version_compare(const std::string& v1_str, const std::string& v2_str);

/**
 * 约束算子 → libsolv 的 REL 标志位（`REL_GT`=1 / `REL_EQ`=2 / `REL_LT`=4）。
 *
 * **唯一实现**：`solver.cpp` 往池里灌依赖时用的就是它 —— 于是"求解器怎么理解 `>=`"与
 * "安装期怎么理解 `>=`"在**算子这一层**也不可能有第二份判据。未知算子三者全置（宽松
 * 兜底：宁可判满足也不误报冲突）。
 */
int version_op_flags(const std::string& op);

/**
 * 检查版本号是否满足指定的版本约束。
 * op: = == != < <= > >=（其它值抛 `error.invalid_version_format`）
 *
 * ⚠️ **用的是 libsolv 的"依赖匹配"语义**（`EVRCMP_MATCH_RELEASE`），不是排序语义 ——
 * 两者对"候选有 release、约束没写 release"的处置不同：匹配语义把**缺 release 当通配**
 * （rpm 的规则，与 `pooldep.c` 的 `pool_match_nevr_rel()` 逐条一致）。所以
 * `version_satisfies("1.0-5", "=", "1.0") == true`，而 `> 1.0` 对它**不**成立。
 * 详情见 `version.cpp` 里 `version_satisfies` 的注释。
 */
bool version_satisfies(const std::string& current_version, const std::string& op,
                       const std::string& required_version);

/**
 * 检查版本号是否满足所有指定的复合版本约束
 * 例如 version_satisfies_all("2.1.0", [">= 2.0.0", "< 3.0.0"]) 返回 true
 * 传入空约束列表时始终返回 true（无约束即任意版本均可）
 */
bool version_satisfies_all(const std::string& current_version,
                           const std::vector<Constraint>& constraints);
