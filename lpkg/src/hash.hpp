#ifndef HASH_HPP
#define HASH_HPP

#include <string>

// Calculates the SHA256 hash of a file.
// Throws LpkgException if the file cannot be opened.
std::string calculate_sha256(const std::string& file_path);

#endif // HASH_HPP
