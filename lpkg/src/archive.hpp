#pragma once

#include <string>
#include <filesystem>

namespace fs = std::filesystem;

void extract_tar_zst(const fs::path& archive_path, const fs::path& output_dir);
