#pragma once

#include <filesystem>
#include <set>
#include <string_view>

/**
 * `name` 是不是一个 stash 目录名（`.lpkg_bak_<pkg>_<pid>`）。
 *
 * **判据的唯一实现**：此前同一个前缀判据在三处各写了一遍，只有一处用了常量
 * （`db/wal_op.cpp` 与 `base/utils.cpp` 是裸字面量、`db/recover.cpp` 用 `constants::`）。
 * 三处问的是同一个问题——"这个名字是不是 lpkg 的 stash 根"——所以必须同源：
 *   · `cleanup_orphan_stashes()` 用它挑出待回收的孤儿；
 *   · `stash_root_of_bak()` 用它决定备份的父目录算不算 stash 根（判错 → 返回值被
 *     `fs::remove_all` 用错地方）；
 *   · post-commit 清理用它拒绝删除"名字不像 stash 的目标"。
 * 传**文件名**不传路径（调用方各自取 `filename()`）。
 */
bool is_stash_dir_name(std::string_view name);

/**
 * 回收孤儿备份 stash（历史 TODO.md §5）：删除各文件系统根下、pid 已死的
 * `.lpkg_bak_<pkg>_<pid>` 目录（崩溃/续传未覆盖的残留）。范围：root_dir 顶层 +
 * root_dir 内每个挂载点（= stash 的落点集合，见 mount_points）；/proc 不可用时降级为
 * root 顶层 + 顶层子挂载点。只认"存活进程已消失"（kill(pid,0) 返回 ESRCH）的，
 * 绝不碰正在运行/自 pid 的 stash。
 */
void cleanup_orphan_stashes(const std::set<std::filesystem::path>& keep = {});
