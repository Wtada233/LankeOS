#pragma once
#include <filesystem>
#include <string>

/**
 * @brief 对 ELF 文件执行 strip（去除调试符号等）
 * @param path      目标文件路径
 * @param error_msg 输出：失败时的错误描述
 * @return 成功返回 true，失败返回 false 并填充 error_msg
 */
bool strip_file(const std::filesystem::path& path, std::string& error_msg);
