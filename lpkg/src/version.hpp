#pragma once

#include <string>

bool version_compare(const std::string& v1_str, const std::string& v2_str);
std::string get_latest_version(const std::string& pkg_name);
