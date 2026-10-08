#pragma once
#include <string>
#include <vector>

/// `source_dir` 须是 .lpkg 的目录布局（含 `content/`、`metadata.json` 等）。
void pack_package(const std::string& output_filename, const std::string& source_dir,
                  const std::string& pkg_name = "package", const std::string& pkg_version = "0.0.0",
                  const std::vector<std::string>& deps = {},
                  const std::vector<std::string>& provides = {},
                  const std::vector<std::string>& provides_soname = {},
                  const std::string& man_content = "",
                  const std::vector<std::string>& needed_so = {});