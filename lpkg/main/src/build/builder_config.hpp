#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "base/build_defaults.hpp"

/**
 * @brief BuildConfig — 从 LankeBUILD.json 中提取的构建元数据
 */
struct BuildConfig {
    std::string name;                       ///< 包名
    std::string version;                    ///< 版本号
    std::vector<std::string> sources;       ///< 源码包下载地址列表
    std::vector<std::string> work_sources;  ///< 工作区源码路径列表
    bool no_strip = false;                  ///< 是否禁用 strip
    bool keep_fs_layout = false;            ///< 是否保留 usr-merge 兼容符号链接（不打包前删除）
    std::vector<std::string> deps;          ///< 构建依赖
    std::vector<std::string> build_deps;    ///< 构建依赖（build-time only）
    std::vector<std::string> provides;      ///< 提供的虚拟包
    std::vector<std::string> needed_so;     ///< 运行时 SO 依赖
    std::string man_content;                ///< 帮助文档内容
    int release = 0;                        ///< 发行修订号（构建时附加 +N 到版本号）

    // ── 编译/链接标志覆盖（空字符串 = 使用 build_defaults 默认值）──────
    std::string cflags;    ///< 覆盖 CFLAGS（如 "-O3 -march=x86-64-v3"）
    std::string cxxflags;  ///< 覆盖 CXXFLAGS
    std::string ldflags;   ///< 覆盖 LDFLAGS
    std::string makeflags; ///< 覆盖 MAKEFLAGS（如 "-j2"）
    bool lto = false;      ///< 启用 LTO（追加 -flto=auto 到编译与链接标志）
};

/**
 * 读取全局默认构建标志（/etc/lpkg/build.conf，makepkg.conf 风格 KEY=value）。
 * 配置文件缺失或键缺失时回退到 build_defaults.hpp 的内置默认。
 */
build_defaults::BuildFlags load_build_defaults();

/**
 * 解析 LankeBUILD.json 的构建标志：全局默认（build.conf / build_defaults）
 * + 逐包覆盖 + LTO → 完整 BuildFlags。所有字段返回时均已填充（非空）。
 */
build_defaults::BuildFlags resolve_build_flags(const BuildConfig& cfg);

/**
 * @brief 解析 LankeBUILD.json 文件
 * @param json_path JSON 文件路径
 * @return 解析后的 BuildConfig 结构体
 * @throws LpkgException 解析失败时抛出
 */
BuildConfig parse_build_config(const std::filesystem::path& json_path);
