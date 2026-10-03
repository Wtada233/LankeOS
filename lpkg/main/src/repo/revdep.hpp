#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

class Repository;

/**
 * 反向依赖图收哪些边 —— **调用方按语义选**，不是"哪种更全就用哪种"。
 *
 * 两种边的含义不同，混成一张图会给出**错误答案**（不是"更全的答案"）：
 *   · `SonameOnly`：`B` 的 needed_so 命中了 `A` 的 provides —— 即 **B 链接了 A 的 .so**。
 *     这是 ABI 断裂（`depend abibreak`）要问的问题：**谁会因为 .so 变了而需要重构建**。
 *   · `DepsAndSoname`：再加上 `B` 的显式 `deps` 里有 `A` —— 即 **B 声明依赖 A**。
 *     这是 `depend remove` 要问的问题：**删掉 A 会波及谁**。
 *
 * 为什么必须有这个区分：只声明包依赖、并不链接那个 .so 的包（本仓库里就是那几类手写 deps
 * 的例外：纯 Python 包、`xwayland`、**dlopen** 加载的依赖如 `kf-networkmanager-qt` →
 * `networkmanager`）在 ABI 断裂时**不需要**重构建。把 deps 边并进 abibreak 的图，就是让
 * REBUILD 清单虚增。
 */
enum class RevdepEdges {
    SonameOnly,
    DepsAndSoname,
};

/**
 * 由仓库包表构建反向依赖图：`被依赖者 -> {依赖它/链接它的包}`。
 *
 * **唯一实现**：此前有两份 —— 生产侧 `pkg/depend_scanner.cpp` 的 `build_repo_revdep_map()`
 * （读索引文件、**只看 SONAME 边**）与 `pkg/solver.cpp` 的 `repo_revrequires()`
 * （吃内存 `Repository`、两种边都看），而后者**生产侧零调用**、只被一个单测吊着。
 * 两份问的是同一个问题、且必然漂移 —— 收敛到这一处（沿 `parse_repo_index_line()` 那个
 * "两个消费者共用一份"的既有范式）。
 *
 * 语义（与收敛前的生产实现逐字一致，仅"多收 deps 边"这一点是本轮新增的行为）：
 *   · **每个包只看最新版**（`Repository` 的版本列表已按版本号升序，取 `back()`）；
 *     旧版本声明过的边不参与 —— 否则 `depend` 会报出"最新版早已不依赖它"的包，清单虚增。
 *   · **自环一律排除**（构建期就排除，不靠查询期 skip）：包不可能是自己的反向依赖。
 *     `A` 的 deps 里写 `A`、或 `A` 的 needed_so 由自己提供，都不产生 `A -> A`。
 *   · needed_so 有**多个**提供者时**全部连边**（不是只连第一个）。
 */
std::unordered_map<std::string, std::unordered_set<std::string>> build_reverse_dependency_map(
    const Repository& repo, RevdepEdges edges);
