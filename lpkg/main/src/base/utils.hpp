#pragma once

// ============ lpkg 的通用工具（**聚合头**：本文件只有 include）============
//
// 本头曾是一个 600 余行、被 90 个文件包含的"什么都装"的头（路径谓词族、
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
