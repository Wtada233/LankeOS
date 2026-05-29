#pragma once
#include <filesystem>
#include <string>

bool strip_file(const std::filesystem::path& path, std::string& error_msg);
