#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "base/build_defaults.hpp"

/**
 * @brief 下载 sources 和 work_sources，并自动解压存档到 work_root
 * @param sources      源码包下载地址列表
 * @param work_sources 工作区源码路径列表
 * @param build_dir    构建目录
 * @param work_root    工作区根目录
 * @return 本次实际下载的文件路径列表
 */
std::vector<std::filesystem::path> download_and_prepare_sources(
    const std::vector<std::string>& sources, const std::vector<std::string>& work_sources,
    const std::filesystem::path& build_dir, const std::filesystem::path& work_root);

/**
 * @brief 检测 work_root 中是否仅包含单个子目录（常见于 tarball 解压结果）
 * @param work_root 工作区根目录
 * @return 若仅有一个顶层子目录则返回该子目录，否则返回 work_root 自身
 */
std::filesystem::path detect_source_tree(const std::filesystem::path& work_root);

/**
 * @brief 替换 LankeBUILD 脚本中的 {PKG_NAME}、{PKG_VER} 等占位符
 * @param script_path 脚本文件路径
 * @param vars        变量映射表
 * @return 替换后的脚本文本
 */
std::string process_build_script(const std::filesystem::path& script_path,
                                 const std::map<std::string, std::string>& vars);

/**
 * @brief 通过 shell 执行指定的构建阶段函数（如 lankebuild_build）
 *
 * 执行前把 CFLAGS/CXXFLAGS/LDFLAGS/MAKEFLAGS export 到构建环境，
 * 使 configure/make/cmake 自动继承（默认 x86-64 generic 标志，见 build_defaults.hpp）。
 * flags 中空字段回退到 build_defaults 默认值。
 *
 * @param phase_name            阶段函数名
 * @param work_dir              工作目录
 * @param processed_script_path 经占位符替换后的脚本路径（先 source）
 * @param flags                 构建标志（默认 = build_defaults 默认值）
 */
void execute_build_phase(const std::string& phase_name, const std::filesystem::path& work_dir,
                         const std::filesystem::path& processed_script_path,
                         const build_defaults::BuildFlags& flags = {});
