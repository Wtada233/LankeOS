#pragma once
#include <filesystem>
#include <string>

// Generates SONAME symbolic links for shared libraries in the given directory
void apply_soname_links(const std::filesystem::path& lib_dir);

// Extracts the SONAME from an ELF file. Returns empty string if none found.
std::string get_elf_soname(const std::filesystem::path& path);
