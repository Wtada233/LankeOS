#ifndef LOCALIZATION_HPP
#define LOCALIZATION_HPP

#include <string>
#include <vector>

void init_localization();
const std::string& get_string(const std::string& key);

// Variadic template for string formatting
template<typename... Args>
std::string string_format(const std::string& format_key, Args... args) {
    const std::string& format = get_string(format_key);
    size_t size = snprintf(nullptr, 0, format.c_str(), args...) + 1; // Extra space for '\0'
    if (size <= 1) { return format; }
    std::vector<char> buf(size);
    snprintf(buf.data(), size, format.c_str(), args...);
    return std::string(buf.data(), buf.data() + size - 1); // We don't want the '\0' inside
}

#endif // LOCALIZATION_HPP
