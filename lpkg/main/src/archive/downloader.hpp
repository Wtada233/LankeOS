#pragma once

#include <filesystem>
#include <string>

/**
 * @brief 下载文件到本地路径
 * @param url           下载地址
 * @param output_path   保存路径
 * @param show_progress 是否显示进度条（默认显示）
 */
void download_file(const std::string &url,
                   const std::filesystem::path &output_path,
                   bool show_progress = true);

/**
 * @brief 带重试机制的下载
 * @param url           下载地址
 * @param output_path   保存路径
 * @param max_retries   最大重试次数（默认 5 次）
 * @param show_progress 是否显示进度条（默认显示）
 */
void download_with_retries(const std::string &url,
                           const std::filesystem::path &output_path,
                           int max_retries = 5, bool show_progress = true);
