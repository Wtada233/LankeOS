#pragma once

#include <string>
#include <string_view>
#include <vector>

// ============ 仓库索引行解析（**唯一**实现） ============

/**
 * 仓库索引（`constants::REPO_INDEX_FILE`）里一个**版本块**的原始字段。
 *
 * 行格式：`包名|版本:哈希:依赖:provides:provides_soname:needed_so;版本2:...|`
 * （这套格式的常量——`PIPE_CHAR`/`COLON_CHAR`/`COMMA_CHAR`/`SEMICOLON_CHAR`——就住在
 *   `base/constants.hpp`，故解析器也放在这一层，与格式常量同址。）
 *
 * **两个消费者必须共用这一个解析器**：`repo/repository.cpp`（建包表）与
 * `pkg/depend_scanner.cpp`（建 needed_so 反图）。
 *
 * ⚠️ **订正 2026-10-04（8.0.0，破坏性）**：这里曾有两处兼容分叉 —— ① 行内第 3 段是
 * **包级 provides**，版本级为空时回退到它；② 版本块**容忍 4/5 字段**（旧写入器把 provides
 * 写在 `vh[3]`、不写 needed_so）。两者都已**删除**：字段现在是**恰好 6 个**，第 3 段没有了。
 * 保留这段历史的理由：当年正是"两份解析器字段数不一致"制造过一整类静默错误答案
 * （4 字段的行在 `depend remove` 侧被整行丢掉 → 反图缺边 → 报"无受影响包"）。
 * **不对旧格式做任何兼容读取**（维护者会 repack 全部包）——旧行会因为块字段数不是 6 而被
 * **拒绝**（不是被误读），索引因此解析出 0 个包、落 `warning.repo_index_empty`，**响亮地失败**。
 */
struct RepoIndexVersionBlock {
    std::string name;             ///< 行首的包名
    std::string version;          ///< vh[0]
    std::string hash;             ///< vh[1]，可为空（LankeBUILD 直出的索引不带哈希）
    std::string deps;             ///< vh[2] 原始串：逗号连接，复合约束需调用方再合并
    std::string provides;         ///< vh[3] **虚拟 provider**（与 .so 无关）
    std::string provides_soname;  ///< vh[4] 本包**导出**的 SONAME
    std::string needed_so;        ///< vh[5] 本包**需要**的 SONAME
};

/**
 * 解析一行索引，返回该行的**全部**版本块（空行/`#` 注释行 → 空表）。
 *
 * "取哪个版本"由调用方决定（`repository.cpp` 按 version_compare 排序后取最后；
 * `depend_scanner.cpp` 取版本号最大者）——解析器只负责字段，不替调用方选版本。
 * **字段数不是 6 的版本块整块跳过**（畸形/旧格式），版本号为空同理
 * （与 farm 的 `graph.rs` 同判据）。
 */
std::vector<RepoIndexVersionBlock> parse_repo_index_line(std::string_view line);
