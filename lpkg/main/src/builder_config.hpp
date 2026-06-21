#pragma once

#include <string>
#include <vector>
#include <filesystem>

// =====================================================================
// BuildConfig — metadata extracted from LankeBUILD.json
// =====================================================================
struct BuildConfig {
    std::string name;
    std::string version;
    std::vector<std::string> sources;
    std::vector<std::string> work_sources;
    bool no_strip = false;
    std::vector<std::string> deps;
    std::vector<std::string> provides;
    std::string man_content;
};

// Parse a LankeBUILD.json file.  Throws LpkgException on failure.
BuildConfig parse_build_config(const std::filesystem::path& json_path);
