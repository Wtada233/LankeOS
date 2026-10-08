#pragma once

#include <filesystem>
#include <string>

/// 无法打开文件时抛 LpkgException。
std::string calculate_sha256(const std::filesystem::path& file_path);
