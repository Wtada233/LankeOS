#pragma once

#include <string>
#include <filesystem>

/**
 * 计算文件的 SHA256 哈希值
 * 无法打开文件时抛出 LpkgException
 */
std::string calculate_sha256(const std::filesystem::path& file_path);
