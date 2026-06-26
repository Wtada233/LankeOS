#include "localization.hpp"
#include "config.hpp"
#include "utils.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <array>
#include <climits> // 用于 PATH_MAX
#include <unistd.h> // 用于 readlink

namespace fs = std::filesystem;

namespace {
    std::unordered_map<std::string, std::string> translations;
    std::unordered_map<std::string, std::string> missing_key_placeholders;

    /**
     * 获取可执行文件所在的目录路径
     * 通过读取 /proc/self/exe 符号链接确定
     */
    fs::path get_executable_dir() {
        std::array<char, PATH_MAX> result{};
        ssize_t count = readlink("/proc/self/exe", result.data(), result.size() - 1);
        if (count != -1) {
            result[count] = '\0';
            return fs::path(result.data()).parent_path();
        } else {
            log_warning(get_string("warning.exe_path_determination_failed"));
            return fs::current_path(); // 回退方案：使用当前工作目录，虽不够可靠
        }
    }
}

/**
 * 加载指定语言的本地化字符串文件
 * 文件格式为 key=value，每行一个键值对
 * 加载失败时自动回退到英文
 */
void load_strings(const std::string& lang, const fs::path& base_dir) {
    auto file_path = base_dir / (lang + ".txt");
    std::ifstream file(file_path);
    if (!file.is_open()) {
        if (lang != "en") { // 防止无限递归
            log_warning(string_format("warning.l10n_load_failed", lang.c_str()));
            load_strings("en", base_dir); // 回退到英文语言包
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

/**
 * 初始化本地化系统
 * 读取 LANG 环境变量确定语言（支持中文和英文），
 * 按优先级依次搜索多个路径以定位 l10n 目录
 */
void init_localization() {
    const char* lang_env = getenv("LANG");
    std::string lang = "en";
    if (lang_env && std::string(lang_env).find("zh") == 0) {
        lang = "zh";
    }

    fs::path exec_dir = get_executable_dir();
    fs::path relative_l10n_dir = exec_dir / ".." / "l10n"; // l10n 目录在构建目录上一级
    fs::path main_l10n_dir = exec_dir / ".." / "main" / "l10n"; // main/l10n 目录布局
    fs::path src_l10n_dir = exec_dir / ".." / "src" / "l10n"; // 测试目录下的 src/l10n 布局

    if (fs::exists(relative_l10n_dir) && fs::is_directory(relative_l10n_dir)) {
        load_strings(lang, relative_l10n_dir);
    } else if (fs::exists(main_l10n_dir) && fs::is_directory(main_l10n_dir)) {
        load_strings(lang, main_l10n_dir);
    } else if (fs::exists(src_l10n_dir) && fs::is_directory(src_l10n_dir)) {
        load_strings(lang, src_l10n_dir);
    } else {
        load_strings(lang, Config::instance().l10n_dir()); // 回退到安装路径下的配置目录
    }
}

/**
 * 根据键名获取本地化字符串
 * 如果键不存在，返回占位符 [MISSING_STRING: key] 以便调试
 */
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
