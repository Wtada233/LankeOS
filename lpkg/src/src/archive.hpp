#pragma once

#include <string>
#include <filesystem>

void extract_tar_zst(const std::filesystem::path& archive_path, const std::filesystem::path& output_dir);
