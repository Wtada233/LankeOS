#pragma once

#include <string>
#include <vector>
#include <filesystem>

/**
 * @brief BuildConfig — 从 LankeBUILD.json 中提取的构建元数据
 */
struct BuildConfig {
    std::string name;                     ///< 包名
    std::string version;                  ///< 版本号
    std::vector<std::string> sources;     ///< 源码包下载地址列表
    std::vector<std::string> work_sources;///< 工作区源码路径列表
    bool no_strip = false;                ///< 是否禁用 strip
    std::vector<std::string> deps;        ///< 构建依赖
    std::vector<std::string> provides;    ///< 提供的虚拟包
    std::vector<std::string> needed_so;   ///< 运行时 SO 依赖
    std::string man_content;              ///< 帮助文档内容
};

/**
 * @brief 解析 LankeBUILD.json 文件
 * @param json_path JSON 文件路径
 * @return 解析后的 BuildConfig 结构体
 * @throws LpkgException 解析失败时抛出
 */
BuildConfig parse_build_config(const std::filesystem::path& json_path);
