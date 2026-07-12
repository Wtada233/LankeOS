#include "builder_config.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "i18n/localization.hpp"
#include "nlohmann/json.hpp"

#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

/** 解析 LankeBUILD.json 构建配置文件，返回 BuildConfig 结构体 */
BuildConfig parse_build_config(const fs::path& json_path) {
    json meta;
    try {
        std::ifstream f(json_path);
        f >> meta;
    } catch (const std::exception& e) {
        throw LpkgException(string_format("error.lankebuild_parse_failed", std::string(e.what())));
    }

    BuildConfig cfg;
    cfg.name        = meta.at(std::string(constants::J_NAME)).get<std::string>();
    cfg.version     = meta.at(std::string(constants::J_VERSION)).get<std::string>();
    cfg.sources     = meta.value(std::string(constants::J_SOURCES), std::vector<std::string>{});
    cfg.work_sources= meta.value(std::string(constants::J_WORK_SOURCES), std::vector<std::string>{});
    cfg.no_strip    = meta.value(std::string(constants::J_NO_STRIP), false);
    cfg.deps        = meta.value(std::string(constants::J_DEPS), std::vector<std::string>{});
    cfg.provides    = meta.value(std::string(constants::J_PROVIDES), std::vector<std::string>{});
    cfg.needed_so   = meta.value(std::string(constants::J_NEEDED_SO), std::vector<std::string>{});
    cfg.man_content = meta.value(std::string(constants::J_MAN), "");
    cfg.release     = meta.value(std::string(constants::J_RELEASE), 0);
    return cfg;
}
