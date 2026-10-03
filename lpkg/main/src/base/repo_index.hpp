#pragma once

#include <string>
#include <string_view>
#include <vector>

// ============ 仓库索引行解析（**唯一**实现） ============

/**
 * 仓库索引（`constants::REPO_INDEX_FILE`）里一个**版本块**的原始字段。
 *
 * 行格式：`包名|版本:哈希:依赖:提供:needed_so;版本2:...|包级提供`
 * （这套格式的常量——`PIPE_CHAR`/`COLON_CHAR`/`COMMA_CHAR`/`SEMICOLON_CHAR`——就住在
 *   `base/constants.hpp`，故解析器也放在这一层，与格式常量同址。）
 *
 * **两个消费者必须共用这一个解析器**：`repo/repository.cpp`（建包表）与
 * `pkg/depend_scanner.cpp`（建 needed_so 反图）。它们曾各写一份，而 depend_scanner 那份
 * 要求版本块 **≥5 字段**、repository.cpp 那份**有意**容忍 4 字段 —— 4 字段的索引行
 * （旧/部分写入器把 provides 写在 `vh[3]`、不写 needed_so）在 `depend remove` /
 * `depend abibreak` 里被**整行丢掉** → 反向依赖图缺一整类边 → 静默报"无受影响包"
 * （给出错误的答案，而不是报错）。
 */
struct RepoIndexVersionBlock {
    std::string name;       ///< 行首的包名
    std::string version;    ///< vh[0]
    std::string hash;       ///< vh[1]，可为空（LankeBUILD 直出的索引不带哈希）
    std::string deps;       ///< vh[2] 原始串：逗号连接，复合约束需调用方再合并（split_dep_field）
    std::string provides;   ///< vh[3]；为空时**回退包级**（行内第 3 段）——旧格式兼容
    std::string needed_so;  ///< vh[4]；4 字段版本块下为空
};

/**
 * 解析一行索引，返回该行的**全部**版本块（空行/`#` 注释行 → 空表）。
 *
 * "取哪个版本"由调用方决定（`repository.cpp` 按 version_compare 排序后取最后；
 * `depend_scanner.cpp` 取版本号最大者）——解析器只负责字段，不替调用方选版本。
 * 版本号为空（`名|:哈希:...`）的畸形块跳过（与 farm 的 `graph.rs` 同判据）。
 */
std::vector<RepoIndexVersionBlock> parse_repo_index_line(std::string_view line);
