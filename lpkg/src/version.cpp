#include "version.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "downloader.hpp"
#include <vector>
#include <regex>
#include <algorithm>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

bool version_compare(const std::string& v1_str, const std::string& v2_str) {
    // Split into main version and pre-release part
    std::string v1_main, v1_pre, v2_main, v2_pre;
    size_t v1_hyphen = v1_str.find('-');
    if (v1_hyphen != std::string::npos) {
        v1_main = v1_str.substr(0, v1_hyphen);
        v1_pre = v1_str.substr(v1_hyphen + 1);
    } else {
        v1_main = v1_str;
    }

    size_t v2_hyphen = v2_str.find('-');
    if (v2_hyphen != std::string::npos) {
        v2_main = v2_str.substr(0, v2_hyphen);
        v2_pre = v2_str.substr(v2_hyphen + 1);
    } else {
        v2_main = v2_str;
    }

    // Compare main versions
    std::regex re_dot("[.]");
    std::vector<std::string> p1_main{std::sregex_token_iterator(v1_main.begin(), v1_main.end(), re_dot, -1), std::sregex_token_iterator()};
    std::vector<std::string> p2_main{std::sregex_token_iterator(v2_main.begin(), v2_main.end(), re_dot, -1), std::sregex_token_iterator()};
    
    size_t main_len = std::max(p1_main.size(), p2_main.size());
    for (size_t i = 0; i < main_len; ++i) {
        int n1 = (i < p1_main.size() && !p1_main[i].empty()) ? std::stoi(p1_main[i]) : 0;
        int n2 = (i < p2_main.size() && !p2_main[i].empty()) ? std::stoi(p2_main[i]) : 0;
        if (n1 < n2) return true;
        if (n1 > n2) return false;
    }

    // Main versions are equal, compare pre-release
    if (v1_pre.empty() && !v2_pre.empty()) return false; // 1.0.0 > 1.0.0-alpha
    if (!v1_pre.empty() && v2_pre.empty()) return true;  // 1.0.0-alpha < 1.0.0
    if (v1_pre.empty() && v2_pre.empty()) return false; // equal

    // Both have pre-release tags, compare them
    std::regex re_pre("[.-]");
    std::vector<std::string> p1_pre{std::sregex_token_iterator(v1_pre.begin(), v1_pre.end(), re_pre, -1), std::sregex_token_iterator()};
    std::vector<std::string> p2_pre{std::sregex_token_iterator(v2_pre.begin(), v2_pre.end(), re_pre, -1), std::sregex_token_iterator()};

    size_t pre_len = std::max(p1_pre.size(), p2_pre.size());
    for (size_t i = 0; i < pre_len; ++i) {
        if (i >= p1_pre.size()) return true; // 1.0.0-alpha < 1.0.0-alpha.1
        if (i >= p2_pre.size()) return false;

        const std::string& part1 = p1_pre[i];
        const std::string& part2 = p2_pre[i];

        bool is_num1 = !part1.empty() && std::all_of(part1.begin(), part1.end(), ::isdigit);
        bool is_num2 = !part2.empty() && std::all_of(part2.begin(), part2.end(), ::isdigit);

        if (is_num1 && is_num2) {
            int n1 = std::stoi(part1);
            int n2 = std::stoi(part2);
            if (n1 < n2) return true;
            if (n1 > n2) return false;
        } else {
            if (is_num1 && !is_num2) return true;
            if (!is_num1 && is_num2) return false;
            if (part1 < part2) return true;
            if (part1 > part2) return false;
        }
    }

    return false; // equal
}

std::string get_latest_version(const std::string& pkg_name) {
    std::string mirror_url = get_mirror_url();
    std::string arch = get_architecture();
    std::string versions_url = mirror_url + arch + "/" + pkg_name + "/";
    std::string tmp_file = TMP_DIR + "versions.html";

    if (!download_file(versions_url, tmp_file)) {
        exit_with_error("无法获取版本列表: " + versions_url);
    }

    std::ifstream file(tmp_file);
    std::string line;
    std::vector<std::string> versions;
    std::regex version_link_regex(R"(<a href="([^"]+)/">)");

    while (std::getline(file, line)) {
        std::smatch match;
        if (std::regex_search(line, match, version_link_regex)) {
            std::string version = match[1].str();
            if (version != "..") {
                versions.push_back(version);
            }
        }
    }

    fs::remove(tmp_file);

    if (versions.empty()) {
        exit_with_error("没有找到可用版本");
    }

    std::sort(versions.begin(), versions.end(), [](const std::string& a, const std::string& b) {
        return version_compare(a, b);
    });

    return versions.back();
}
