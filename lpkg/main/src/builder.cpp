#include "builder.hpp"
#include "nlohmann/json.hpp"
#include "utils.hpp"
#include "exception.hpp"
#include "downloader.hpp"
#include "archive.hpp"
#include "packer.hpp"
#include "localization.hpp"
#include <fstream>
#include <iostream>
#include <vector>
#include <cstdio>
#include <ctime>

using json = nlohmann::json;

void run_build(const fs::path& build_dir) {
    fs::path json_path = build_dir / "LankeBUILD.json";
    fs::path script_path = build_dir / "LankeBUILD.sh";

    if (!fs::exists(json_path)) throw LpkgException(string_format("error.missing_lankebuild_json", build_dir.string()));
    if (!fs::exists(script_path)) throw LpkgException(string_format("error.missing_lankebuild_sh", build_dir.string()));

    // 1. Parse Metadata
    log_info(get_string("info.parsing_lankebuild"));
    json meta;
    try {
        std::ifstream f(json_path);
        f >> meta;
    } catch (const std::exception& e) {
        throw LpkgException(string_format("error.lankebuild_parse_failed", std::string(e.what())));
    }

    std::string name = meta.at("name").get<std::string>();
    std::string version = meta.at("version").get<std::string>();
    std::vector<std::string> sources = meta.value("sources", std::vector<std::string>{});
    bool no_strip = meta.value("no_strip", false);

    log_info(string_format("info.building_package", name, version));

    // 2. Prepare Directories
    fs::path work_root = build_dir / "work";
    fs::path staging_root = build_dir / "root";
    fs::path staging_hooks = build_dir / "hooks";

    std::vector<fs::path> downloaded_files;
    bool created_man = false;
    bool created_deps = false;

    fs::remove_all(work_root);
    fs::remove_all(staging_root);
    fs::remove_all(staging_hooks);

    ensure_dir_exists(work_root);
    ensure_dir_exists(staging_root);
    
    // Pre-create standard hierarchy
    for (const auto& d : {"bin", "lib", "include", "share/man", "local/bin"}) {
        ensure_dir_exists(staging_root / "usr" / d);
    }
    ensure_dir_exists(staging_hooks);
    {
        std::ofstream h(staging_hooks / "postinst.sh");
        h << "#!/bin/sh\n";
    }
    fs::permissions(staging_hooks / "postinst.sh", fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add);

    // 3. Download and Extract Sources
    for (const auto& url : sources) {
        fs::path filename = fs::path(url).filename();
        fs::path dest = build_dir / filename;
        if (!fs::exists(dest)) {
            download_with_retries(url, dest, 3, true);
            downloaded_files.push_back(dest);
        } else {
            log_info(string_format("info.source_exists", filename.string()));
        }

        // Auto-extract archives to work_root
        std::string ext = dest.extension().string();
        if (ext == ".gz" || ext == ".bz2" || ext == ".xz" || ext == ".zst" || ext == ".tgz" || ext == ".tar" || ext == ".zip") {
            log_info(string_format("info.auto_extracting", filename.string()));
            try {
                extract_tar_zst(dest, work_root);
            } catch (const std::exception& e) {
                log_warning(string_format("warning.auto_extract_failed", filename.string(), e.what()));
            }
        }
    }

    // 4. Source Tree Detection
    // If work_root contains exactly one directory, we assume that's the source tree
    fs::path actual_work_dir = work_root;
    int dir_count = 0;
    fs::path lone_dir;
    for (const auto& entry : fs::directory_iterator(work_root)) {
        if (entry.is_directory()) {
            lone_dir = entry.path();
            dir_count++;
        } else {
            // If there's a file at root, it's not a single-dir archive
            dir_count = -1; 
            break;
        }
    }
    if (dir_count == 1) {
        actual_work_dir = lone_dir;
        log_info(string_format("info.detected_source_tree", actual_work_dir.filename().string()));
    }

    // 5. Execution Environment
    std::string env_setup = 
        "export PKG_NAME=\"" + name + "\"\n" +
        "export PKG_VER=\"" + version + "\"\n" +
        "export WORK_DIR=\"" + fs::absolute(work_root).string() + "\"\n" +
        "export SRC_DIR=\"" + fs::absolute(actual_work_dir).string() + "\"\n" +
        "export STAGING_ROOT=\"" + fs::absolute(staging_root).string() + "\"\n" +
        "export STAGING_HOOKS=\"" + fs::absolute(staging_hooks).string() + "\"\n" +
        "export NO_STRIP=\"" + (no_strip ? "1" : "0") + "\"\n" +
        "cd \"$SRC_DIR\" || exit 1\n";

    auto execute_phase = [&](const std::string& phase_name) {
        log_info(string_format("info.executing_phase", phase_name));
        // We run a subshell that sources the build script and calls the function
        std::string cmd = env_setup + " . " + fs::absolute(script_path).string() + " && " + phase_name;
        int ret = run_shell(cmd);
        if (ret != 0) {
            throw LpkgException(string_format("error.build_phase_failed", phase_name, std::to_string(ret)));
        }
    };

    try {
        execute_phase("lankebuild_prepare");
        execute_phase("lankebuild_build");
        execute_phase("lankebuild_package");
    } catch (...) {
        throw;
    }

    // 5. Post-Processing (Normalization, Strip, Cleanup)
    auto finalize_staging = [&]() {
        log_info(get_string("info.finalizing_staging"));
        
        // Path Normalization
        log_info(get_string("info.path_normalization"));
        auto normalize = [&](const std::string& from, const std::string& to) {
            fs::path from_p = staging_root / "usr" / from;
            fs::path to_p = staging_root / "usr" / to;
            if (fs::exists(from_p) && fs::is_directory(from_p)) {
                ensure_dir_exists(to_p);
                for (const auto& entry : fs::directory_iterator(from_p)) {
                    fs::rename(entry.path(), to_p / entry.path().filename());
                }
                fs::remove_all(from_p);
            }
        };
        normalize("lib64", "lib");
        normalize("sbin", "bin");

        // Clean up libtool files
        log_info(get_string("info.cleaning_libtool_files"));
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(staging_root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file() && it->path().extension() == ".la") {
                fs::remove(it->path(), ec);
            }
        }

        // Strip binaries
        if (!no_strip) {
            log_info(get_string("info.stripping_binaries"));
            for (const auto& sub : {"bin", "lib", "libexec"}) {
                fs::path p = staging_root / "usr" / sub;
                if (!fs::exists(p) || !fs::is_directory(p)) continue;
                for (const auto& entry : fs::recursive_directory_iterator(p)) {
                    if (!entry.is_regular_file() || entry.is_symlink()) continue;
                    
                    // Only strip ELF files
                    bool is_elf = false;
                    std::ifstream f(entry.path(), std::ios::binary);
                    if (f) {
                        char magic[4];
                        f.read(magic, 4);
                        if (f.gcount() == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
                            is_elf = true;
                        }
                    }
                    if (!is_elf) continue;
                    
                    strip_binary(entry.path());
                }
            }
        }

	fs::remove(staging_root / "usr/share/info/dir", ec);

        // ldconfig
        if (fs::exists(staging_root / "usr" / "lib")) {
            log_info(get_string("info.executing_ldconfig"));
            run_shell("ldconfig -n \"" + (staging_root / "usr" / "lib").string() + "\"");
        }

        // Generate basic metadata if missing
        if (!fs::exists(build_dir / "man.txt")) {
            std::ofstream f(build_dir / "man.txt");
            f << "Name: " << name << "\n"
              << "Version: " << version << "\n"
              << "BuildDate: " << []() {
                  char buf[16];
                  time_t now = time(nullptr);
                  strftime(buf, sizeof(buf), "%Y%m%d", localtime(&now));
                  return std::string(buf);
              }() << "\n";
            created_man = true;
        }
        if (!fs::exists(build_dir / "deps.txt")) {
            std::ofstream(build_dir / "deps.txt").close();
            created_deps = true;
        }
    };

    finalize_staging();

    // 6. Pack the result
    log_info(get_string("info.packing_built_pkg"));
    std::string output_filename = name + "-" + version + ".lpkg";
    pack_package(output_filename, build_dir.string());
    
    log_info(string_format("info.build_success", output_filename));

    // 7. Cleanup
    log_info(get_string("info.cleaning_up_build"));
    fs::remove_all(work_root);
    fs::remove_all(staging_root);
    fs::remove_all(staging_hooks);

    if (created_man) fs::remove(build_dir / "man.txt");
    if (created_deps) fs::remove(build_dir / "deps.txt");
    for (const auto& f : downloaded_files) {
        fs::remove(f);
    }
}
