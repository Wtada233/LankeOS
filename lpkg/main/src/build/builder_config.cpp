#include "builder_config.hpp"

#include <algorithm>
#include <fstream>
#include <thread>

#include "base/constants.hpp"
#include "base/exception.hpp"
#include "config/config.hpp"
#include "i18n/localization.hpp"
#include "nlohmann/json.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

/** 解析 LankeBUILD.json 构建配置文件，返回 BuildConfig 结构体 */
BuildConfig parse_build_config(const fs::path& json_path)
{
    json meta;
    try {
        std::ifstream f(json_path);
        f >> meta;
    } catch (const std::exception& e) {
        throw LpkgException(string_format("error.lankebuild_parse_failed", std::string(e.what())));
    }

    BuildConfig cfg;
    cfg.name = meta.at(std::string(constants::J_NAME)).get<std::string>();
    cfg.version = meta.at(std::string(constants::J_VERSION)).get<std::string>();
    cfg.sources = meta.value(std::string(constants::J_SOURCES), std::vector<std::string>{});
    cfg.work_sources =
        meta.value(std::string(constants::J_WORK_SOURCES), std::vector<std::string>{});
    cfg.no_strip = meta.value(std::string(constants::J_NO_STRIP), false);
    cfg.keep_fs_layout = meta.value(std::string(constants::J_KEEP_FS_LAYOUT), false);
    cfg.deps = meta.value(std::string(constants::J_DEPS), std::vector<std::string>{});
    cfg.build_deps = meta.value(std::string(constants::J_BUILD_DEPS), std::vector<std::string>{});
    cfg.provides = meta.value(std::string(constants::J_PROVIDES), std::vector<std::string>{});
    cfg.needed_so = meta.value(std::string(constants::J_NEEDED_SO), std::vector<std::string>{});
    cfg.man_content = meta.value(std::string(constants::J_MAN), "");
    cfg.release = meta.value(std::string(constants::J_RELEASE), 0);

    // 构建标志覆盖（空串 = 用 build_defaults 默认）
    cfg.cflags = meta.value(std::string(constants::J_CFLAGS), std::string{});
    cfg.cxxflags = meta.value(std::string(constants::J_CXXFLAGS), std::string{});
    cfg.ldflags = meta.value(std::string(constants::J_LDFLAGS), std::string{});
    cfg.makeflags = meta.value(std::string(constants::J_MAKEFLAGS), std::string{});
    cfg.lto = meta.value(std::string(constants::J_LTO), false);
    return cfg;
}

namespace
{
/** 去除首尾空白 */
std::string trim_copy(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return std::string(s);
}

/** 解析 makepkg.conf 风格的 KEY=value 行（去掉包裹引号） */
std::string parse_value(std::string_view raw)
{
    std::string v = trim_copy(raw);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
}
}  // namespace

build_defaults::BuildFlags load_build_defaults()
{
    build_defaults::BuildFlags f;
    f.cflags = std::string(build_defaults::CFLAGS);
    f.cxxflags = std::string(build_defaults::CXXFLAGS);
    f.ldflags = std::string(build_defaults::LDFLAGS);
    f.makeflags = build_defaults::default_makeflags();
    f.ltoflags = std::string(build_defaults::LTOFLAGS);

    // 全局默认配置（/etc/lpkg/build.conf，由 make install 从 main/conf/build.conf 安装）。
    // 缺失时全部使用 build_defaults.hpp 内置默认（fallback）。
    std::ifstream file(Config::instance().build_conf());
    if (!file.is_open()) return f;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string_view sv(line);
        if (sv.empty() || sv.front() == '#') continue;
        const auto eq = sv.find('=');
        if (eq == std::string_view::npos) continue;

        const std::string key = trim_copy(sv.substr(0, eq));
        const std::string value = parse_value(sv.substr(eq + 1));
        if (key == "CFLAGS")
            f.cflags = value;
        else if (key == "CXXFLAGS")
            f.cxxflags = value;
        else if (key == "LDFLAGS")
            f.ldflags = value;
        else if (key == "LTOFLAGS")
            f.ltoflags = value;
        else if (key == "MAKEFLAGS")
            f.makeflags = value.empty() ? build_defaults::default_makeflags()
                                        : value;  // 空值/未配置 → 默认 -j<核心数>
    }

    // 展开 makepkg.conf 风格的 MAKEFLAGS="-j$(nproc)"（make 不识别 $(nproc)）
    if (const auto pos = f.makeflags.find("$(nproc)"); pos != std::string::npos) {
        unsigned n = std::thread::hardware_concurrency();
        if (n == 0) n = 1;
        f.makeflags.replace(pos, 8, std::to_string(n));
    }
    return f;
}

build_defaults::BuildFlags resolve_build_flags(const BuildConfig& cfg)
{
    // 全局默认：build.conf（若存在）覆盖 build_defaults.hpp 内置默认
    const auto dflt = load_build_defaults();

    build_defaults::BuildFlags f;
    f.cflags = cfg.cflags.empty() ? dflt.cflags : cfg.cflags;
    f.cxxflags = cfg.cxxflags.empty() ? dflt.cxxflags : cfg.cxxflags;
    f.ldflags = cfg.ldflags.empty() ? dflt.ldflags : cfg.ldflags;
    f.makeflags = cfg.makeflags.empty() ? dflt.makeflags : cfg.makeflags;
    f.ltoflags = dflt.ltoflags;

    // LTO：把 ltoflags（默认 -flto=auto）追加到编译与链接标志（避免重复追加）
    if (cfg.lto) {
        auto append_lto = [](std::string& s, const std::string& lto) {
            if (s.find("-flto") == std::string::npos) {
                if (!s.empty()) s += ' ';
                s += lto;
            }
        };
        append_lto(f.cflags, f.ltoflags);
        append_lto(f.cxxflags, f.ltoflags);
        append_lto(f.ldflags, f.ltoflags);
    }
    return f;
}
