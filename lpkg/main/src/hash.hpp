#pragma once

#include <string>
#include <filesystem>

// Calculates the SHA256 hash of a file.
// Throws LpkgException if the file cannot be opened.
std::string calculate_sha256(const std::filesystem::path& file_path);
