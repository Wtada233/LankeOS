#include <gtest/gtest.h>
#include "localization.hpp"
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>

namespace fs = std::filesystem;

class L10nIntegrityTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_localization();
    }

    std::set<std::string> extract_keys_from_source(const fs::path& src_dir) {
        std::set<std::string> keys;
        // Search for string literals used in localization functions
        // Using a more conservative regex string
        std::string pattern = "(?:get_string|log_info|log_error|log_warning|string_format)\\s*\\(\\s*\"([^\"]+)\"";
        std::regex key_regex(pattern);

        for (auto const& dir_entry : fs::recursive_directory_iterator(src_dir)) {
            if (dir_entry.is_regular_file() && (dir_entry.path().extension() == ".cpp" || dir_entry.path().extension() == ".hpp")) {
                std::ifstream f(dir_entry.path());
                std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                auto words_begin = std::sregex_iterator(content.begin(), content.end(), key_regex);
                auto words_end = std::sregex_iterator();
                for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
                    keys.insert((*i)[1].str());
                }
            }
        }
        return keys;
    }
};

TEST_F(L10nIntegrityTest, AllSourceKeysExistInTranslations) {
    fs::path project_root = fs::current_path();
    // Support running from build directory
    if (!fs::exists(project_root / "main/src")) {
        project_root = project_root.parent_path();
    }
    
    auto source_keys = extract_keys_from_source(project_root / "main/src");
    
    // Explicitly add keys that might be missed by simple regex (e.g. multi-line or macros)
    std::vector<std::string> manual_keys = {
        "info.non_interactive_option_desc", "help.output_file", "help.pack_source", 
        "help.force", "help.force_overwrite", "help.no_hooks", "help.no_deps", 
        "help.testing", "help.root_dir", "help.target_arch", "help.hash", "help.pkg_query",
        "cxxopts.default", "cxxopts.usage", "cxxopts.option_help", "cxxopts.arg", "cxxopts.positional_help"
    };
    for(const auto& k : manual_keys) source_keys.insert(k);

    std::vector<std::string> missing_keys;
    for (const auto& key : source_keys) {
        // Skip common false positives if any
        if (key == "command" || key == "packages") continue;

        std::string val = get_string(key);
        if (val.find("[MISSING_STRING:") != std::string::npos) {
            missing_keys.push_back(key);
        }
    }

    std::string error_msg = "The following keys are missing in localization files: ";
    for(const auto& k : missing_keys) error_msg += k + ", ";

    EXPECT_TRUE(missing_keys.empty()) << error_msg;
}