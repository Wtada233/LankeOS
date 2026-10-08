#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "base/build_defaults.hpp"

/** 从 LankeBUILD.json 提取的构建元数据 */
struct BuildConfig {
    std::string name;
    std::string version;
    std::vector<std::string> sources;
    std::vector<std::string> work_sources;
    bool no_strip = false;
    bool keep_fs_layout = false;               ///< 保留 usr-merge 兼容符号链接（不打包前删除）
    std::vector<std::string> deps;             ///< 运行时依赖（`needed_so` 覆盖不到的那些）
    std::vector<std::string> build_deps;       ///< 构建依赖（build-time only）
    std::vector<std::string> provides;         ///< 提供的虚拟 provider
    std::vector<std::string> provides_soname;  ///< 本包导出的 SONAME
    std::vector<std::string> needed_so;        ///< 本包运行时需要的 SONAME（DT_NEEDED）
    std::string man_content;
    int release = 0;  ///< 发行修订号（构建时按 rpm 语义附加 `-N`）

    // ── 编译/链接标志覆盖（空字符串 = 使用 build_defaults 默认值）──────
    std::string cflags;
    std::string cxxflags;
    std::string ldflags;
    std::string makeflags;
    bool lto = false;  ///< 启用 LTO（追加 -flto=auto 到编译与链接标志）
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

/// 解析失败抛 LpkgException。
BuildConfig parse_build_config(const std::filesystem::path& json_path);
