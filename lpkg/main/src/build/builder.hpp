#pragma once
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

/// `build_dir` 须含 `LankeBUILD`（脚本）与 `LankeBUILD.json`（元数据）。
void run_build(const fs::path& build_dir);
