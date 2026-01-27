#pragma once

#include <string>
#include <vector> // Added for std::vector

bool version_compare(const std::string& v1_str, const std::string& v2_str);
bool version_satisfies(const std::string& current_version, const std::string& op, const std::string& required_version);
std::string get_latest_version(const std::string& pkg_name);
