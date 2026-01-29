#include "packer.hpp"
#include "utils.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "hash.hpp"
#include "config.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <iostream>
#include <algorithm>

namespace fs = std::filesystem;

void pack_package(const std::string& output_filename, const std::string& source_dir) {
    fs::path base_dir = source_dir;
    fs::path root_dir = base_dir / "root";
    fs::path hooks_dir = base_dir / "hooks";
    fs::path meta_dir = get_tmp_dir() / "lpkg_meta"; 

    if (!fs::exists(root_dir)) {
        throw LpkgException(get_string("error.pack_root_not_found") + ": " + root_dir.string());
    }

    if (fs::exists(meta_dir)) fs::remove_all(meta_dir);
    ensure_dir_exists(meta_dir);

    // 1. Generate files.txt
    std::ofstream files_txt(meta_dir / "files.txt");
    log_info(get_string("info.pack_scanning"));
    for (const auto& entry : fs::recursive_directory_iterator(root_dir, fs::directory_options::none)) {
        fs::path rel_path = fs::relative(entry.path(), root_dir);
        files_txt << rel_path.string() << " /" << std::endl;
    }
    files_txt.close();

    // 2. Prepare Metadata via system cp to avoid fs::copy loops
    if (fs::exists(hooks_dir)) {
        std::string cmd = "cp -a " + hooks_dir.string() + " " + meta_dir.string() + "/hooks";
        if (std::system(cmd.c_str()) != 0) throw LpkgException("Failed to copy hooks");
    } else {
        ensure_dir_exists(meta_dir / "hooks");
    }

    if (fs::exists(base_dir / "deps.txt")) fs::copy_file(base_dir / "deps.txt", meta_dir / "deps.txt");
    else std::ofstream(meta_dir / "deps.txt").close();

    if (fs::exists(base_dir / "man.txt")) fs::copy_file(base_dir / "man.txt", meta_dir / "man.txt");
    else {
        std::ofstream f(meta_dir / "man.txt");
        f << "LankeOS Package" << std::endl;
    }

    // 3. Robust Tar Packing
    fs::path abs_output = fs::absolute(output_filename);
    std::string tar_cmd = "tar -I zstd -cf " + abs_output.string() + 
                          " -C " + meta_dir.string() + " files.txt deps.txt man.txt hooks" +
                          " -C " + root_dir.parent_path().string() + " --transform='s|^root|content|' root";
    
    log_info(string_format("info.pack_creating", output_filename));
    int ret = std::system(tar_cmd.c_str());
    
    fs::remove_all(meta_dir);
    if (ret != 0) throw LpkgException(string_format("error.pack_failed", ret));
    
    std::string hash = calculate_sha256(output_filename);
    std::cout << get_string("info.pack_success") << " " << output_filename << std::endl;
    std::cout << "SHA256: " << hash << std::endl;
}
