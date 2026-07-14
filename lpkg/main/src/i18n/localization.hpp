#pragma once

#include <format>
#include <string>
#include <string_view>

/** 初始化本地化系统（加载翻译文件） */
void init_localization();

/** 根据键名获取本地化字符串 */
const std::string &get_string(const std::string &key);

/**
 * 格式化本地化字符串（C++20 std::format 变参模板）
 * 配合本地化键值使用，支持参数替换
 */
template <typename... Args>
std::string string_format(const std::string &key, Args &&...args) {
  try {
    return std::vformat(get_string(key), std::make_format_args(args...));
  } catch (const std::format_error &e) {
    return "Lpkg Formatting Error [key: " + key + "]: " + e.what();
  }
}
