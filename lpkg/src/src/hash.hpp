#pragma once

#include <string>
#include <filesystem>

namespace fs = std::filesystem;

// Calculates the SHA256 hash of a file.
// Throws LpkgException if the file cannot be opened.
std::string calculate_sha256(const fs::path& file_path);
