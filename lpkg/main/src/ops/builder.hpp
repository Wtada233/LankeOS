#pragma once
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

/**
 * @brief 执行指定目录的 LankeBUILD 构建流程
 * @param build_dir 包含 LankeBUILD.json 和 LankeBUILD.sh 的目录
 */
void run_build(const fs::path& build_dir);
