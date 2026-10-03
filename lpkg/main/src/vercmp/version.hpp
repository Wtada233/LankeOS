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

/**
 * 比较两个版本号字符串（libsolv EVRCMP/rpm 语义 + `-`→`~` 归一化）。
 * v1 < v2 返回 true，否则返回 false。
 *
 * 语义（与 rpm 一致，lpkg 不再自研补段）：
 *   - 段按数值比较（6.16.1 > 6.6.1）；
 *   - `1.0` 与 `1.0.0` 视为**不同版本**（段数不同即不等，不做缺失段补 0）；
 *   - 预发布 `1.0-rc1` 归一化为 `1.0~rc1` 后，**旧于** `1.0`（rpm 的 `~` = 预发布）；
 *   - `+N` 是发行修订号，**先拆出版本比较、版本相同再比 release**（261.2+3 > 261+3；
 *     若整串丢给 rpm 段比较，release 会与版本段混比导致错排）。
 */
bool version_compare(const std::string& v1_str, const std::string& v2_str);

/**
 * 检查版本号是否满足指定的版本约束。
 * op: = == != < <= > >=
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

/**
 * lpkg 版本 → libsolv EVR 字符串（**桥接的唯一入口**）。
 *
 * **要求：libsolv 的依赖匹配必须与 lpkg 的 `version_satisfies()` 逐条一致** —— 求解器按前者
 * 出方案、安装期按后者验收，两者不一致就会出现"求出来的方案被自己拒掉"（假不满足 ⇒ 事务
 * 无解）或"装出来才发现不满足"（假满足 ⇒ 坏系统）。
 *
 * 编码规则（`norm` = `-`→`~`，即把 lpkg 的预发布标记换成 rpm 的 tilde）：
 *   `[V, R]` = 在第一个 `+` 处切；`enc = V`（R 空）或 `V + "^^" + R`（R 非空）。
 *
 * **为什么不是"把 `+` 换成 `-` 交给 libsolv 当 release"**（2026-10-03 之前就是这么做的）：
 * libsolv 的依赖匹配走 `EVRCMP_MATCH_RELEASE`（`src/pooldep.c` 的 `pool_intersect_evrs`），
 * 它把"有一侧没有 release"当**通配**（`pool_evrcmp` 返回 ±2 的两个特例分支）⇒ 实测
 * `foo = 1.0` 会匹配 `1.0+1`/`1.0+2`，而 `foo > 1.0+1` 反过来会匹配 `1.0`，
 * `foo >= 1.0` 又不匹配 `1.0+1`。在一份 4032 组合的 (候选版本 × 约束) 矩阵上，它与
 * `version_satisfies` 差 **78 处**，**两个方向都有**。详见
 * `tests/unit/test_vercmp_libsolv_bridge.cpp`（同一矩阵，是这道桥的回归闸门）。
 *
 * **为什么 `^` 对**：libsolv 的 rpm 比较器把 caret 定义为"**比基础版新、比任何真实下一段
 * 旧**"（`1.0^post > 1.0` 且 `1.0^post < 1.0.1`）—— 正是发行修订号的语义；而且全串**不含
 * `-`** ⇒ libsolv 切出的 release 永远为空 ⇒ 上面那些 ±2 特例分支**不可能触发**。同一矩阵
 * 上不一致数 = 0。
 *
 * **因此 `^` 是版本域里的保留字符**：lpkg 版本/包名里不允许出现（`is_safe_path_component`
 * 拒、本函数也抛）。实测真实索引 678 个版本里 0 个含 `^`。
 *
 * @throws LpkgException 版本含保留字符 `^`
 */
std::string to_libsolv_evr(const std::string& v);

/**
 * libsolv EVR 字符串 → lpkg 版本（`to_libsolv_evr` 的**无损**逆）。
 *
 * `~` 还原为 `-`；分隔符是**唯一的** `^^`（版本部分不许含 `^`，所以第一个 `^^` 一定是
 * 分隔符）→ 还原为 `+`。旧实现按"最后一个 `-`"切分，在版本/release 含 `~` 时会还原错。
 */
std::string from_libsolv_evr(const std::string& v);
