#pragma once

#include <string>
#include <format>
#include <string_view>

void init_localization();
const std::string& get_string(const std::string& key);

// Variadic template for string formatting using modern C++20 std::format
template<typename... Args>
std::string string_format(const std::string& key, Args&&... args) {
    try {
        return std::vformat(get_string(key), std::make_format_args(args...));
    } catch (const std::format_error& e) {
        return "Lpkg Formatting Error [key: " + key + "]: " + e.what();
    }
}
