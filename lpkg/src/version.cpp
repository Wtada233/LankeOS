#include "version.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "downloader.hpp"
#include "localization.hpp"
#include "exception.hpp"
#include <vector>
#include <regex>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <cctype>

namespace fs = std::filesystem;

namespace {
void validate_version_format(const std::string& version_str) {
    for (char c : version_str) {
        if (!std::isdigit(c) && c != '.') {
            throw LpkgException(string_format("error.invalid_version_format", version_str));
        }
    }
}
}

bool version_compare(const std::string& v1_str, const std::string& v2_str) {
    validate_version_format(v1_str);
    validate_version_format(v2_str);

    std::regex re_dot("[.]");
    std::vector<std::string> p1_main{std::sregex_token_iterator(v1_str.begin(), v1_str.end(), re_dot, -1), std::sregex_token_iterator()};
    std::vector<std::string> p2_main{std::sregex_token_iterator(v2_str.begin(), v2_str.end(), re_dot, -1), std::sregex_token_iterator()};
    
    size_t main_len = std::max(p1_main.size(), p2_main.size());
    for (size_t i = 0; i < main_len; ++i) {
        int n1 = (i < p1_main.size() && !p1_main[i].empty()) ? std::stoi(p1_main[i]) : 0;
        int n2 = (i < p2_main.size() && !p2_main[i].empty()) ? std::stoi(p2_main[i]) : 0;
        if (n1 < n2) return true;
        if (n1 > n2) return false;
    }

    return false; // equal
}

std::string get_latest_version(const std::string& pkg_name) {
    std::string mirror_url = get_mirror_url();
    std::string arch = get_architecture();
    std::string latest_txt_url = mirror_url + arch + "/" + pkg_name + "/latest.txt";
    std::string tmp_file_path = TMP_DIR + pkg_name + "_latest.txt";

    if (!download_file(latest_txt_url, tmp_file_path, false)) {
        throw LpkgException(string_format("error.download_latest_txt_failed", latest_txt_url));
    }

    std::ifstream latest_file(tmp_file_path);
    std::string latest_version;
    if (!std::getline(latest_file, latest_version) || latest_version.empty()) {
        fs::remove(tmp_file_path);
        throw LpkgException(string_format("error.read_latest_txt_failed", latest_txt_url));
    }

    fs::remove(tmp_file_path);

    validate_version_format(latest_version);

    return latest_version;
}
