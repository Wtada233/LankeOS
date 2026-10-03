#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>

/**
 * 读集合文件时"文件不存在"的处理策略。**只有调用点能选**，不设隐式默认值以外的东西：
 *   Throw —— 缺文件即抛 error.open_file_failed（**默认**，正常操作路径）
 *   Empty —— 缺文件返回空集（**只给崩溃恢复路径用**，见 Cache::load 的说明）
 *
 * 两个策略下"文件存在却打不开（权限/IO）"都**一律抛** —— 半损/不可读的库绝不能冒充空库。
 */
enum class MissingSetFilePolicy { Throw, Empty };

/** 从文件读取字符串集合（每行一个元素，自动去除 \r） */
std::unordered_set<std::string> read_set_from_file(
    const std::filesystem::path& path, MissingSetFilePolicy policy = MissingSetFilePolicy::Throw);

// 曾有一个配套的 `write_set_to_file()`（`item\n` 逐行写出）。2026-10-03 删除：
// 生产侧写集合文件**从来不用它** —— `Cache` 的各个 `write_*` 自己走
// `ofstream + fsync_and_rename`（见 `db/cache.cpp`），它唯一的"消费者"是一个与
// `read_set_from_file` 的往返测试。**生产零调用 + 测试自产自销 = 死代码**，
// 读侧的覆盖已由下面那条直接造文件的用例保住。
