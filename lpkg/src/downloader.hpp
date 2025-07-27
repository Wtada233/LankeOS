#pragma once

#include <string>
#include <filesystem>

namespace fs = std::filesystem;

void download_file(const std::string& url, const fs::path& output_path, bool show_progress = true);
void download_with_retries(const std::string& url, const fs::path& output_path, int max_retries = 5, bool show_progress = true);
