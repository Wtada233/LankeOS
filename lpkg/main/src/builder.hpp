#pragma once
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

/**
 * Executes the LankeBUILD process for a directory.
 * @param build_dir The directory containing LankeBUILD.json and LankeBUILD.sh
 */
void run_build(const fs::path& build_dir);
