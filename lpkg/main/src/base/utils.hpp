#pragma once

// ============ lpkg 的通用工具（**聚合头**：本文件只有 include）============
//
// 2026-10-03：本头曾是一个 600 余行、被 90 个文件包含的"什么都装"的头（路径谓词族、
// 索引解析、字符串工具、文件原语全挤在一起），职责过宽。现按职责拆成下列子头，
// **本文件只负责转发包含** —— 于是所有既有 `#include "base/utils.hpp"` 的调用方
// **一个都不用改**。新增工具请加到对应的子头里，不要再往本文件塞声明。
//
// 依赖方向（子头之间无环，聚合头按此顺序 include）：
//   strings / path_predicates / locking / tmpdir / process / repo_index
//     → path_safety / fs_atomic / io_set_file / xattr / stash

// ⚠️ 下面这组 include **必须留在聚合头里**：90 个包含者长期靠 `utils.hpp` **传递**拿到
// `<filesystem>/<optional>/<set>/<string>/<string_view>/<unordered_set>/<vector>` 与
// `constants.hpp` / `exception.hpp`。拆头时删掉它们，会同时打断一批"自己没写 include、
// 全靠这里传递"的文件（本仓库的已知形态：**别用 grep 预判影响面，直接 `make -k`**）。
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "constants.hpp"
#include "exception.hpp"
#include "fs_atomic.hpp"
#include "io_set_file.hpp"
#include "locking.hpp"
#include "path_predicates.hpp"
#include "path_safety.hpp"
#include "process.hpp"
#include "repo_index.hpp"
#include "stash.hpp"
#include "strings.hpp"
#include "tmpdir.hpp"
#include "xattr.hpp"

// ============ 二进制文件处理(ELF) ============
// ============ 二进制文件处理(ELF) ============

/** ELF 文件类型枚举 */
// （2026-09-26 清理：此处原有 `enum class BinaryType` 与 `void strip_binary(...)`。
//   · `BinaryType` **只声明、全仓无人使用**（死代码）→ 删除；
//   · `strip_binary` 的唯一调用者是 `build/builder.cpp`，而它内部调 `elf/strip.cpp` 的
//     `strip_file` —— 它住在 base 层会让**最底层反向依赖 ELF 层**（`base/utils.cpp`
//     为了它 `#include "elf/strip.hpp"`）→ 已挪到 `elf/strip.hpp`，方向正过来了。）
