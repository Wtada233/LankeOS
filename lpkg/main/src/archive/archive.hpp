#pragma once

#include <filesystem>
#include <string>

/**
 * @brief 解压 .tar.zst 存档到指定目录
 * @param archive_path 存档文件路径
 * @param output_dir   输出目录
 * @param label        被解压对象的**标签**，用于进度/完成日志点名（安装时是包名，
 *                     构建时是源码归档文件名）。多包批次里解压日志一个包接一个包地刷，
 *                     不带标签就分不清在解压谁。标签由调用方给（调用方知道自己在处理谁），
 *                     不从这里回头去猜。
 */
void extract_tar_zst(const std::filesystem::path& archive_path,
                     const std::filesystem::path& output_dir, const std::string& label);

/**
 * @brief 从存档中提取单个文件的内容（不完整解压）
 * @param archive_path  存档文件路径
 * @param internal_path 存档内的文件路径
 * @return 文件内容的字符串
 */
std::string extract_file_from_archive(const std::filesystem::path& archive_path,
                                      const std::string& internal_path);
