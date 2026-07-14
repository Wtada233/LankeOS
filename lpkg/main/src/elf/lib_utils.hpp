#pragma once
#include <filesystem>
#include <string>

/**
 * @brief 为给定目录中的共享库生成 SONAME 符号链接
 * @param lib_dir 共享库所在目录
 */
void apply_soname_links(const std::filesystem::path &lib_dir);

/**
 * @brief 从 ELF 文件中提取 SONAME
 * @param path ELF 文件路径
 * @return SONAME 字符串，未找到则返回空字符串
 */
std::string get_elf_soname(const std::filesystem::path &path);
