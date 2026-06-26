#include "version.hpp"

#include "config.hpp"
#include "archive/downloader.hpp"
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
/**
 * 校验版本号字符串是否符合语义化版本规范
 * 格式要求: 主版本号.次版本号.修订号（可选预发布和构建元数据）
 */
void validate_version_format(const std::string& version_str) {
    if (!std::regex_match(version_str, version_regex)) {
        throw LpkgException(string_format("error.invalid_version_format", version_str));
    }
}

struct Version {
    std::vector<int> main_part;
    std::vector<std::string> pre_release_part;

    /**
     * 解析版本号字符串，分离主版本号部分和预发布部分
     * 主版本号以点分隔的数字序列，预发布部分以点分隔的字母数字序列
     */
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
                pre_release_part.push_back(pre_it->str());
            }
        }
    }
};

/**
 * 比较两个版本的预发布部分（pre-release）
 * 按语义化版本规范：数字标识符按数值比较，字母标识符按字典序比较，
 * 数字标识符优先级低于字母标识符，更多分段者优先级更高
 */
int compare_pre_release_part(const std::vector<std::string>& p1, const std::vector<std::string>& p2, const std::string& v1_str, const std::string& v2_str) {
    size_t min_len = std::min(p1.size(), p2.size());
    for (size_t i = 0; i < min_len; ++i) {
        int res;
        int n1, n2;
        bool is_num1 = !p1[i].empty() && std::all_of(p1[i].begin(), p1[i].end(), [](unsigned char c) { return std::isdigit(c); });
        bool is_num2 = !p2[i].empty() && std::all_of(p2[i].begin(), p2[i].end(), [](unsigned char c) { return std::isdigit(c); });

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
            return -1; // 数字标识符优先级低于字母/字符串标识符
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

/**
 * 比较两个语义化版本号：v1 < v2 返回 true，否则返回 false
 * 先比较主版本号（数字逐段比较），再考虑预发布版本前缀
 */
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
        return true; // 有预发布版本的版本号优先级低于正式版本
    }
    if (v1.pre_release_part.empty() && !v2.pre_release_part.empty()) {
        return false;
    }
    if (!v1.pre_release_part.empty() && !v2.pre_release_part.empty()) {
        return compare_pre_release_part(v1.pre_release_part, v2.pre_release_part, v1_str, v2_str) < 0;
    }

    return false; // 两个版本相等
}

/**
 * 判断当前版本是否满足给定运算符和所需版本的条件
 * 支持的运算符: =, ==, !=, <, <=, >, >=
 */
bool version_satisfies(const std::string& current_version, const std::string& op, const std::string& required_version) {
    if (op == "=" || op == "==") {
        return current_version == required_version;
    }
    if (op == "!=") {
        return current_version != required_version;
    }

    bool less = version_compare(current_version, required_version);
    bool greater = version_compare(required_version, current_version);
    bool equal = !less && !greater;

    if (op == "<") return less;
    if (op == "<=") return less || equal;
    if (op == ">") return greater;
    if (op == ">=") return greater || equal;

    throw LpkgException(string_format("error.invalid_version_format", op));
}