#include "version.hpp"

#include "config.hpp"
#include "downloader.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <vector>
#include <string_view>

namespace fs = std::filesystem;

namespace {
static const std::regex version_regex(R"(^(\d+)(\.\d+)*(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$)");
static const std::regex re_dot("[.]");
void validate_version_format(const std::string& version_str) {
    if (!std::regex_match(version_str, version_regex)) {
        throw LpkgException(string_format("error.invalid_version_format", version_str));
    }
}

struct Version {
    std::vector<int> main_part;
    std::vector<std::string> pre_release_part;

    Version(const std::string& version_str) {
        std::string_view v_sv(version_str);
        size_t pre_release_pos = v_sv.find('-');
        size_t build_meta_pos = v_sv.find('+');

        size_t main_end = std::min(pre_release_pos, build_meta_pos);
        std::string_view main_sv = v_sv.substr(0, main_end);

        std::string main_str(main_sv);
        std::sregex_token_iterator it(main_str.begin(), main_str.end(), re_dot, -1);
        std::sregex_token_iterator end;
        for (; it != end; ++it) {
            try {
                main_part.push_back(std::stoi(it->str()));
            } catch (const std::exception& e) {
                throw LpkgException(string_format("error.invalid_version_format", version_str) + ": " + e.what());
            }
        }

        if (pre_release_pos != std::string::npos) {
            size_t pre_release_end = (build_meta_pos > pre_release_pos) ? build_meta_pos : std::string::npos;
            std::string_view pre_release_sv = v_sv.substr(pre_release_pos + 1, pre_release_end - (pre_release_pos + 1));
            std::string pre_release_str(pre_release_sv);
            std::sregex_token_iterator pre_it(pre_release_str.begin(), pre_release_str.end(), re_dot, -1);
            std::sregex_token_iterator pre_end;
            for (; pre_it != pre_end; ++pre_it) {
                pre_release_part.push_back(pre_it->str()); // Store as std::string
            }
        }
    }
};

int compare_pre_release_part(const std::vector<std::string>& p1, const std::vector<std::string>& p2, const std::string& v1_str, const std::string& v2_str) {
    size_t min_len = std::min(p1.size(), p2.size());
    for (size_t i = 0; i < min_len; ++i) {
        int res;
        int n1, n2;
        bool is_num1 = !p1[i].empty() && std::all_of(p1[i].begin(), p1[i].end(), ::isdigit);
        bool is_num2 = !p2[i].empty() && std::all_of(p2[i].begin(), p2[i].end(), ::isdigit);

        if (is_num1 && is_num2) {
            try {
                n1 = std::stoi(p1[i]);
            } catch (const std::exception& e) {
                throw LpkgException(string_format("error.invalid_version_format", v1_str) + ": " + e.what());
            }
            try {
                n2 = std::stoi(p2[i]);
            } catch (const std::exception& e) {
                throw LpkgException(string_format("error.invalid_version_format", v2_str) + ": " + e.what());
            }
            if (n1 < n2) return -1;
            if (n1 > n2) return 1;
        } else if (is_num1) {
            return -1; // Numeric identifiers have lower precedence than non-numeric identifiers.
        } else if (is_num2) {
            return 1;
        } else {
            res = p1[i].compare(p2[i]);
            if (res != 0) return res < 0 ? -1 : 1;
        }
    }

    if (p1.size() < p2.size()) return -1;
    if (p1.size() > p2.size()) return 1;
    return 0;
}

}

bool version_compare(const std::string& v1_str, const std::string& v2_str) {
    validate_version_format(v1_str);
    validate_version_format(v2_str);

    Version v1(v1_str);
    Version v2(v2_str);

    size_t main_len = std::max(v1.main_part.size(), v2.main_part.size());
    for (size_t i = 0; i < main_len; ++i) {
        int n1 = (i < v1.main_part.size()) ? v1.main_part[i] : 0;
        int n2 = (i < v2.main_part.size()) ? v2.main_part[i] : 0;
        if (n1 < n2) return true;
        if (n1 > n2) return false;
    }

    if (!v1.pre_release_part.empty() && v2.pre_release_part.empty()) {
        return true; // A pre-release version has lower precedence than a normal version.
    }
    if (v1.pre_release_part.empty() && !v2.pre_release_part.empty()) {
        return false;
    }
    if (!v1.pre_release_part.empty() && !v2.pre_release_part.empty()) {
        return compare_pre_release_part(v1.pre_release_part, v2.pre_release_part, v1_str, v2_str) < 0;
    }

    return false; // equal
}

std::string get_latest_version(const std::string& pkg_name) {
    std::string mirror_url = get_mirror_url();
    std::string arch = get_architecture();
    std::string latest_txt_url = mirror_url + arch + "/" + pkg_name + "/latest.txt";
    fs::path tmp_file_path = get_tmp_dir() / (pkg_name + "_latest.txt");

    try {
        download_file(latest_txt_url, tmp_file_path, false);
    } catch (const LpkgException& e) {
        throw LpkgException(string_format("error.download_latest_txt_failed", latest_txt_url) + ": " + e.what());
    }

    std::ifstream latest_file(tmp_file_path);
    if (!latest_file.is_open()) {
        throw LpkgException(string_format("error.open_file_failed", tmp_file_path.string()));
    }
    std::string latest_version;
    if (!std::getline(latest_file, latest_version) || latest_version.empty()) {
        throw LpkgException(string_format("error.read_latest_txt_failed", latest_txt_url));
    }

    // Trim trailing carriage return if present (from CRLF line endings)
    if (!latest_version.empty() && latest_version.back() == '\r') {
        latest_version.pop_back();
    }

    validate_version_format(latest_version);

    return latest_version;
}