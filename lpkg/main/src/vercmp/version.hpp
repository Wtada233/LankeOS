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
 *   - 预发布 `1.0-rc1` 归一化为 `1.0~rc1` 后，**旧于** `1.0`（rpm 的 `~` = 预发布）。
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
 * lpkg 版本 → libsolv EVR 字符串（供 pool 边界使用）。
 *
 * 为什么需要归一化：`pool_evrcmp`（libsolv）会把版本串**最后一个 `-` 之后当 release**，
 * release 非空即更大 → `1.0-rc1` 被判成 `> 1.0`；而 lpkg 语义里 `-预发布` 旧于基础版。
 * rpm 恰好用 `~` 表达"预发布（小于一切含基础版）"。lpkg 格式里 `-` 只出现在预发布位置，
 * 所以把 `-` 全部替换为 `~` 即可让 libsolv 的比较与 lpkg 语义一致。
 */
std::string to_libsolv_evr(const std::string& v);

/** libsolv EVR 字符串 → lpkg 版本（`~` 还原为 `-`）。lpkg 版本不含 `~`，可安全往返。 */
std::string from_libsolv_evr(const std::string& v);
