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

/// 点分隔符正则，用于 split
static const std::regex re_dot("[.]");

/** 安全地将字符串转为 int，失败时抛出 LpkgException */
int parse_int(const std::string& s, const std::string& ctx) {
    try {
        return std::stoi(s);
    } catch (const std::exception& e) {
        throw LpkgException(string_format("error.invalid_version_format", ctx) + ": " + e.what());
    }
}

/**
 * 逐字符校验版本号格式
 *
 * 格式: 主版本号[补丁后缀][-预发布][+发行修订号]
 * 主版本号 : (\d+)(\.\d+)*
 * 补丁后缀 : [a-zA-Z]\d*           (可选)
 * 预发布   : -[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*  (可选)
 * 修订号   : \+[0-9A-Za-z]+(\.[0-9A-Za-z]+)*   (可选，不含连字符)
 */
void validate_version_format(const std::string& v) {
    if (v.empty())
        throw LpkgException(string_format("error.invalid_version_format", v));

    auto is_digit = [](unsigned char c) { return std::isdigit(c); };
    auto is_letter = [](unsigned char c) { return std::isalpha(c); };
    auto is_alnum = [](unsigned char c) { return std::isalnum(c); };
    auto is_pre_char = [&](unsigned char c) { return is_alnum(c) || c == '-'; };
    auto fail = [&]{ throw LpkgException(string_format("error.invalid_version_format", v)); };

    size_t i = 0;

    // --- 1. 主版本号: (\d+)(\.\d+)* ---
    if (!is_digit(v[i])) fail();
    while (i < v.size() && is_digit(v[i])) i++;
    while (i < v.size() && v[i] == '.') {
        i++; // consume '.'
        if (i >= v.size() || !is_digit(v[i])) fail();
        while (i < v.size() && is_digit(v[i])) i++;
    }

    // --- 2. 补丁后缀: [a-zA-Z]\d* (可选) ---
    if (i < v.size() && is_letter(v[i])) {
        i++; // consume letter
        while (i < v.size() && is_digit(v[i])) i++;
    }

    // --- 3. 预发布: -[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)* (可选) ---
    if (i < v.size() && v[i] == '-') {
        i++; // consume '-'
        if (i >= v.size() || !is_pre_char(v[i])) fail();
        while (i < v.size() && is_pre_char(v[i])) i++;
        while (i < v.size() && v[i] == '.') {
            i++; // consume '.'
            if (i >= v.size() || !is_pre_char(v[i])) fail();
            while (i < v.size() && is_pre_char(v[i])) i++;
        }
    }

    // --- 4. 发行修订号: +[0-9A-Za-z]+(\.[0-9A-Za-z]+)* (可选，不含连字符) ---
    if (i < v.size() && v[i] == '+') {
        i++; // consume '+'
        if (i >= v.size() || !is_alnum(v[i])) fail();
        while (i < v.size() && is_alnum(v[i])) i++;
        while (i < v.size() && v[i] == '.') {
            i++; // consume '.'
            if (i >= v.size() || !is_alnum(v[i])) fail();
            while (i < v.size() && is_alnum(v[i])) i++;
        }
    }

    // 必须完整消耗整个字符串
    if (i != v.size()) fail();
}

struct Version {
    std::vector<int> main_part;
    std::string patch_suffix;                    // pN 补丁后缀，如 "p2"、"p"，空串表示无
    std::vector<std::string> release_part;       // + 发行修订号，如 ["2"]、"["2", "1"]
    std::vector<std::string> pre_release_part;   // - 预发布，如 ["rc", "1"]（不变）

    /**
     * 解析版本号字符串，分离各组成部分
     * 格式: 主版本号[补丁后缀][-预发布][+发行修订号]
     * 主版本号以点分隔的数字序列，允许末尾单字母+数字补丁后缀
     * +后缀为发行修订号（优先级高于正式版），-后缀为预发布（优先级低于正式版）
     */
    Version(const std::string& version_str) {
        std::string_view v_sv(version_str);
        size_t pre_release_pos = v_sv.find('-');
        size_t build_meta_pos = v_sv.find('+');

        size_t main_end = std::min(pre_release_pos, build_meta_pos);
        std::string_view main_sv = v_sv.substr(0, main_end);

        // 解析主版本号（数字段），最后一个段可能携带补丁后缀
        std::string main_str(main_sv);
        std::sregex_token_iterator it(main_str.begin(), main_str.end(), re_dot, -1);
        std::sregex_token_iterator end;
        for (; it != end; ++it) {
            std::string seg = it->str();
            auto next = it;
            ++next;
            if (next == end) {
                // 最后一个段：检查是否有补丁后缀（如 "17p2" → 17 + "p2"）
                size_t pos = 0;
                int num = std::stoi(seg, &pos);
                main_part.push_back(num);
                if (pos < seg.length()) {
                    patch_suffix = seg.substr(pos);
                }
            } else {
                main_part.push_back(parse_int(seg, version_str));
            }
        }

        // 解析预发布部分（-后缀）
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

        // 解析发行修订号（+后缀）
        if (build_meta_pos != std::string::npos) {
            size_t release_end = (pre_release_pos > build_meta_pos) ? pre_release_pos : std::string::npos;
            std::string_view release_sv = v_sv.substr(build_meta_pos + 1, release_end - (build_meta_pos + 1));
            std::string release_str(release_sv);
            std::sregex_token_iterator rel_it(release_str.begin(), release_str.end(), re_dot, -1);
            std::sregex_token_iterator rel_end;
            for (; rel_it != rel_end; ++rel_it) {
                release_part.push_back(rel_it->str());
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
            n1 = parse_int(p1[i], v1_str);
            n2 = parse_int(p2[i], v2_str);
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
 * 比较优先级（主版本号相等时）：
 *   1. 补丁后缀 pN（最大）
 *   2. 发行修订号 +N
 *   3. 基础版本（无后缀）
 *   4. 预发布 -X（最小）
 */
bool version_compare(const std::string& v1_str, const std::string& v2_str) {
    validate_version_format(v1_str);
    validate_version_format(v2_str);

    Version v1(v1_str);
    Version v2(v2_str);

    // 1. 主版本号逐段比较（数字，缺失补 0）
    size_t main_len = std::max(v1.main_part.size(), v2.main_part.size());
    for (size_t i = 0; i < main_len; ++i) {
        int n1 = (i < v1.main_part.size()) ? v1.main_part[i] : 0;
        int n2 = (i < v2.main_part.size()) ? v2.main_part[i] : 0;
        if (n1 < n2) return true;
        if (n1 > n2) return false;
    }

    // 2. 补丁后缀比较（pN）— 优先级最高
    if (!v1.patch_suffix.empty() && v2.patch_suffix.empty())
        return false;  // v1 > v2
    if (v1.patch_suffix.empty() && !v2.patch_suffix.empty())
        return true;   // v1 < v2
    if (!v1.patch_suffix.empty() && !v2.patch_suffix.empty()) {
        char letter1 = v1.patch_suffix[0];
        char letter2 = v2.patch_suffix[0];
        if (letter1 != letter2)
            return letter1 < letter2;
        // 字母相同，比较尾部数字
        int num1 = 0, num2 = 0;
        if (v1.patch_suffix.size() > 1)
            num1 = parse_int(v1.patch_suffix.substr(1), v1_str);
        if (v2.patch_suffix.size() > 1)
            num2 = parse_int(v2.patch_suffix.substr(1), v2_str);
        if (num1 != num2)
            return num1 < num2;
    }

    // 3. 发行修订号比较（+）— 高于基础版和预发布
    bool v1_rel = !v1.release_part.empty();
    bool v2_rel = !v2.release_part.empty();
    if (v1_rel && !v2_rel) return false;  // v1 > v2
    if (!v1_rel && v2_rel) return true;   // v1 < v2
    if (v1_rel && v2_rel) {
        return compare_pre_release_part(v1.release_part, v2.release_part, v1_str, v2_str) < 0;
    }

    // 4. 预发布比较（-）— 低于基础版（原语义）
    if (!v1.pre_release_part.empty() && v2.pre_release_part.empty())
        return true;   // v1 < v2
    if (v1.pre_release_part.empty() && !v2.pre_release_part.empty())
        return false;  // v1 > v2
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

/**
 * 检查版本号是否满足所有指定的复合版本约束
 * 支持将多个约束用 AND 组合（如 >= 2.0.0 且 < 3.0.0）
 * 传入空约束列表时始终返回 true
 */
bool version_satisfies_all(const std::string& current_version, const std::vector<Constraint>& constraints) {
    for (const auto& c : constraints) {
        if (!version_satisfies(current_version, c.op, c.version)) {
            return false;
        }
    }
    return true;
}