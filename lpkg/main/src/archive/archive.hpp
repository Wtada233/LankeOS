#pragma once

#include <filesystem>
#include <string>

/**
 * @brief 解压 .tar.zst 存档到指定目录
 * @param archive_path 存档文件路径
 * @param output_dir   输出目录
 */
void extract_tar_zst(const std::filesystem::path &archive_path,
                     const std::filesystem::path &output_dir);

/**
 * @brief 从存档中提取单个文件的内容（不完整解压）
 * @param archive_path  存档文件路径
 * @param internal_path 存档内的文件路径
 * @return 文件内容的字符串
 */
std::string extract_file_from_archive(const std::filesystem::path &archive_path,
                                      const std::string &internal_path);
