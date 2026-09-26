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

/**
 * @brief 对二进制文件执行 strip，**失败只告警、绝不抛**（strip 是尽力而为的构建步骤）。
 *
 * 从 `base/utils.{hpp,cpp}` 挪到这里（2026-09-26）：它唯一被 `build/builder.cpp` 调用，
 * 内部却调本文件的 `strip_file` —— 住在 base 层会让最底层反向依赖 ELF 层。
 */
void strip_binary(const std::filesystem::path& path);
