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
