#include "lib_utils.hpp"
#include "builder.hpp"
#include "builder_config.hpp"
#include "builder_executor.hpp"
#include "core/utils.hpp"
#include "core/exception.hpp"
#include "packer.hpp"
#include "core/localization.hpp"
#include "core/constants.hpp"

#include <fstream>
#include <array>
#include <map>
#include <ctime>

namespace fs = std::filesystem;

// =====================================================================
// Internal helpers
// =====================================================================

namespace {

// Prepare staging directory layout (UsrMerge symlinks, hooks placeholder).
fs::path setup_build_directories(const fs::path& build_dir) {
    fs::path work_root     = build_dir / constants::DIR_WORK;
    fs::path staging_root  = build_dir / constants::DIR_ROOT;
    fs::path staging_hooks = build_dir / constants::DIR_HOOKS;

    fs::remove_all(work_root);
    fs::remove_all(staging_root);
    fs::remove_all(staging_hooks);

    ensure_dir_exists(work_root);
    ensure_dir_exists(staging_root);

    // Path Normalization (UsrMerge)
    log_info(get_string("info.path_normalization"));
    for (const auto& d : {constants::BIN, constants::LIB,
                          constants::INCLUDE, constants::SHARE_MAN,
                          constants::LOCAL_BIN}) {
        ensure_dir_exists(staging_root / constants::USR / d);
    }
    fs::create_directory_symlink(constants::USR_BIN,
                                 staging_root / constants::BIN);
    fs::create_directory_symlink(constants::USR_BIN,
                                 staging_root / constants::SBIN);
    fs::create_directory_symlink(constants::USR_LIB,
                                 staging_root / constants::LIB);
    fs::create_directory_symlink(constants::USR_LIB,
                                 staging_root / constants::LIB64);
    fs::create_directory_symlink(constants::BIN,
                                 staging_root / constants::USR_SBIN);
    fs::create_directory_symlink(constants::LIB,
                                 staging_root / constants::USR_LIB64);

    ensure_dir_exists(staging_hooks);
    {
        std::ofstream h(staging_hooks / constants::POSTINST_SH);
        h << "#!/bin/sh" << constants::NL;
    }
    fs::permissions(staging_hooks / constants::POSTINST_SH,
                    fs::perms::owner_exec | fs::perms::group_exec
                        | fs::perms::others_exec,
                    fs::perm_options::add);

    return staging_hooks;
}

// Build the template-substitution map.
std::map<std::string, std::string>
build_variable_map(const BuildConfig& cfg,
                   const fs::path& work_root,
                   const fs::path& actual_work_dir,
                   const fs::path& staging_root,
                   const fs::path& staging_hooks) {
    return {
        {"{PKG_NAME}",      cfg.name},
        {"{PKG_VER}",       cfg.version},
        {"{WORK_DIR}",       fs::absolute(work_root).string()},
        {"{SRC_DIR}",        fs::absolute(actual_work_dir).string()},
        {"{STAGING_ROOT}",   fs::absolute(staging_root).string()},
        {"{STAGING_HOOKS}",  fs::absolute(staging_hooks).string()},
        {"{NO_STRIP}",       cfg.no_strip ? "1" : "0"},
    };
}

// Post-processing: libtool cleanup, ELF strip, SONAME symlinks.
void finalize_staging(const fs::path& staging_root, bool no_strip) {
    log_info(get_string("info.finalizing_staging"));

    log_info(get_string("info.cleaning_libtool_files"));
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(staging_root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file()
            && it->path().extension() == constants::EXT_LA) {
            fs::remove(it->path(), ec);
        }
    }

    if (!no_strip) {
        log_info(get_string("info.stripping_binaries"));
        for (const auto& sub : {constants::BIN, constants::LIB,
                                constants::LIBEXEC}) {
            fs::path p = staging_root / constants::USR / sub;
            if (!fs::exists(p) || !fs::is_directory(p)) continue;
            for (const auto& entry : fs::recursive_directory_iterator(p)) {
                if (!entry.is_regular_file() || entry.is_symlink()) continue;

                // Only strip ELF files
                bool is_elf = false;
                std::ifstream f(entry.path(), std::ios::binary);
                if (f) {
                    std::array<char, 4> magic{};
                    f.read(magic.data(), magic.size());
                    if (f.gcount() == 4
                        && magic[0] == 0x7f && magic[1] == 'E'
                        && magic[2] == 'L' && magic[3] == 'F') {
                        is_elf = true;
                    }
                }
                if (!is_elf) continue;

                strip_binary(entry.path());
            }
        }
    }

    fs::remove(staging_root / "usr/share/info/dir", ec);

    if (fs::exists(staging_root / constants::USR / constants::LIB)) {
        log_info(get_string("info.generating_soname_links"));
        apply_soname_links(staging_root / constants::USR / constants::LIB);
    }
}

void cleanup_build([[maybe_unused]] const fs::path& build_dir,
                   const fs::path& work_root,
                   const fs::path& staging_root,
                   const fs::path& staging_hooks,
                   const std::vector<fs::path>& downloaded_files) {
    log_info(get_string("info.cleaning_up_build"));
    fs::remove_all(work_root);
    fs::remove_all(staging_root);
    fs::remove_all(staging_hooks);
    for (const auto& f : downloaded_files) {
        fs::remove(f);
    }
}

} // anonymous namespace

// =====================================================================
// Public API
// =====================================================================
void run_build(const fs::path& build_dir) {
    // 1. Parse metadata
    fs::path json_path   = build_dir / constants::LANK_BUILD_JSON;
    fs::path script_path = build_dir / constants::LANK_BUILD_SCRIPT;

    if (!fs::exists(json_path))
        throw LpkgException(string_format("error.missing_lankebuild_json",
                                          build_dir.string()));
    if (!fs::exists(script_path))
        throw LpkgException(string_format("error.missing_lankebuild",
                                          build_dir.string()));

    log_info(get_string("info.parsing_lankebuild"));
    auto cfg = parse_build_config(json_path);
    log_info(string_format("info.building_package", cfg.name, cfg.version));

    // Auto-generate man page if not specified in LankeBUILD.json
    if (cfg.man_content.empty()) {
        std::time_t t = std::time(nullptr);
        std::tm* tm = std::localtime(&t);
        char date_buf[32];
        std::strftime(date_buf, sizeof(date_buf), "%Y%m%d", tm);
        cfg.man_content =
            get_string("man.info_name") + ": " + cfg.name + "\n"
            + get_string("man.info_version") + ": " + cfg.version + "\n"
            + get_string("man.info_build_date") + ": " + date_buf + "\n";
        log_info(string_format("info.auto_generated_man", cfg.name));
    }

    // 2. Prepare directories
    fs::path work_root    = build_dir / constants::DIR_WORK;
    fs::path staging_root = build_dir / constants::DIR_ROOT;
    auto staging_hooks    = setup_build_directories(build_dir);

    // 3. Download and extract sources
    auto downloaded = download_and_prepare_sources(
        cfg.sources, cfg.work_sources, build_dir, work_root);

    // 4. Detect source tree
    auto actual_work_dir = detect_source_tree(work_root);

    // 5. Process script and execute build phases
    auto vars = build_variable_map(cfg, work_root, actual_work_dir,
                                    staging_root, staging_hooks);
    fs::path processed_script = build_dir / constants::LANK_BUILD_PROCESSED;
    {
        std::string content = process_build_script(script_path, vars);
        std::ofstream f(processed_script);
        f << content;
    }

    try {
        execute_build_phase("lankebuild_prepare",
                            actual_work_dir, processed_script);
        execute_build_phase("lankebuild_build",
                            actual_work_dir, processed_script);
        execute_build_phase("lankebuild_package",
                            actual_work_dir, processed_script);
    } catch (...) {
        fs::remove(processed_script);
        throw;
    }
    fs::remove(processed_script);

    // 6. Post-process
    finalize_staging(staging_root, cfg.no_strip);

    // 7. Pack
    log_info(get_string("info.packing_built_pkg"));
    std::string output_filename =
        cfg.name + "-" + cfg.version + std::string(constants::EXT_LPKG);
    pack_package(output_filename, build_dir.string(), cfg.name, cfg.version,
                 cfg.deps, cfg.provides, cfg.man_content);
    log_info(string_format("info.build_success", output_filename));

    // 8. Cleanup
    cleanup_build(build_dir, work_root, staging_root,
                  staging_hooks, downloaded);
}
