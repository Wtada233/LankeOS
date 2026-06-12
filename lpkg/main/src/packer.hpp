#pragma once
#include <string>

void pack_package(const std::string& output_filename, const std::string& source_dir,
                  const std::string& pkg_name = "package", const std::string& pkg_version = "0.0.0");