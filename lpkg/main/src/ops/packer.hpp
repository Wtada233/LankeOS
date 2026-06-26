#pragma once
#include <string>

#include <vector>

void pack_package(const std::string& output_filename, const std::string& source_dir,
                  const std::string& pkg_name = "package", const std::string& pkg_version = "0.0.0",
                  const std::vector<std::string>& deps = {},
                  const std::vector<std::string>& provides = {},
                  const std::string& man_content = "");