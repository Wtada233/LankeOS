#include "localization.hpp"
#include "config.hpp"
#include "utils.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <limits.h> // For PATH_MAX
#include <unistd.h> // For readlink

namespace fs = std::filesystem;

namespace {
    std::unordered_map<std::string, std::string> translations;
    std::unordered_map<std::string, std::string> missing_key_placeholders;

    fs::path get_executable_dir() {
        char result[PATH_MAX];
        ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
        if (count != -1) {
            return fs::path(result).parent_path();
        } else {
            log_warning("Could not determine executable path via /proc/self/exe. Falling back to current working directory. Localization might be incorrect if not run from install location.");
            return fs::current_path(); // Fallback, though less reliable
        }
    }
}

void load_strings(const std::string& lang, const fs::path& base_dir) {
    auto file_path = base_dir / (lang + ".txt");
    std::ifstream file(file_path);
    if (!file.is_open()) {
        if (lang != "en") { // Avoid infinite recursion
            log_warning("Could not open localization file for " + lang + ", falling back to English.");
            load_strings("en", base_dir); // Try English with the same base_dir
        }
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            translations[key] = value;
        }
    }
}

void init_localization() {
    const char* lang_env = getenv("LANG");
    std::string lang = "en";
    if (lang_env && std::string(lang_env).find("zh") == 0) {
        lang = "zh";
    }

    fs::path exec_dir = get_executable_dir();
    fs::path relative_l10n_dir = exec_dir / ".." / "l10n"; // Assuming l10n is one level up from build/

    if (fs::exists(relative_l10n_dir) && fs::is_directory(relative_l10n_dir)) {
        load_strings(lang, relative_l10n_dir);
    } else {
        load_strings(lang, LPKG_L10N_DIR); // Fallback to installed path
    }
}

const std::string& get_string(const std::string& key) {
    auto it = translations.find(key);
    if (it != translations.end()) {
        return it->second;
    }
    auto missing_it = missing_key_placeholders.find(key);
    if (missing_it == missing_key_placeholders.end()) {
        missing_it = missing_key_placeholders.emplace(key, "[MISSING_STRING: " + key + "]").first;
    }
    return missing_it->second;
}
