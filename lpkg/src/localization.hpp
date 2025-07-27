#pragma once

#include <string>
#include <format>

void init_localization();
const std::string& get_string(const std::string& key);

// Variadic template for string formatting
template<typename... Args>
std::string string_format(const std::string& key, Args&&... args) {
    try {
        return std::vformat(get_string(key), std::make_format_args(args...));
    } catch (const std::format_error& e) {
        // In case of a format error (e.g., mismatch between format string and arguments),
        // return a descriptive error message. This is much safer than the C-style snprintf.
        return "Formatting error for key '" + key + "': " + e.what();
    }
}
