#include "localization.hpp"
#include "config.hpp"
#include "utils.hpp"
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <cstdlib>
#include <string>

namespace {
    std::unordered_map<std::string, std::string> translations;
    std::unordered_map<std::string, std::string> missing_key_placeholders;
}

void load_strings(const std::string& lang) {
    std::string file_path = L10N_DIR + lang + ".txt";
    std::ifstream file(file_path);
    if (!file.is_open()) {
        if (lang != "en") { // Avoid infinite recursion
            log_warning("Could not open localization file for " + lang + ", falling back to English.");
            load_strings("en");
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
    load_strings(lang);
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
