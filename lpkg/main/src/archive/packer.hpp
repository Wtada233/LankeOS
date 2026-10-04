#pragma once
#include <string>
#include <vector>

/**
 * @brief 打包目录为 .lpkg 包文件
 * @param output_filename 输出文件名
 * @param source_dir      源目录（包含 content/ metadata.json 等）
 * @param pkg_name        包名（默认 "package"）
 * @param pkg_version     包版本（默认 "0.0.0"）
 * @param deps            依赖列表
 * @param provides        虚拟 provider 列表
 * @param provides_soname 本包导出的 SONAME 列表
 * @param man_content     帮助文档内容
 * @param needed_so       本包需要的 SONAME 列表
 */
void pack_package(const std::string& output_filename, const std::string& source_dir,
                  const std::string& pkg_name = "package", const std::string& pkg_version = "0.0.0",
                  const std::vector<std::string>& deps = {},
                  const std::vector<std::string>& provides = {},
                  const std::vector<std::string>& provides_soname = {},
                  const std::string& man_content = "",
                  const std::vector<std::string>& needed_so = {});