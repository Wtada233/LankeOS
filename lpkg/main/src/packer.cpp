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
    fs::path build_dir = get_tmp_dir() / "build_pkg"; // Staging area

    if (!fs::exists(root_dir)) {
        throw LpkgException(string_format("error.pack_root_not_found", root_dir.string()));
    }

    ensure_dir_exists(build_dir);
    ensure_dir_exists(build_dir / "content");

    std::ofstream files_txt(build_dir / "files.txt");
    
    log_info(get_string("info.pack_scanning"));
    
    for (const auto& entry : fs::recursive_directory_iterator(root_dir)) {
        if (entry.is_directory()) continue;
        
        fs::path rel_path = fs::relative(entry.path(), root_dir);
        fs::path dest_path = build_dir / "content" / rel_path;
        
        ensure_dir_exists(dest_path.parent_path());
        
        if (fs::is_symlink(entry.path())) {
            fs::copy_symlink(entry.path(), dest_path);
        } else {
            fs::copy_file(entry.path(), dest_path, fs::copy_options::overwrite_existing);
        }
        
        files_txt << rel_path.string() << " /" << std::endl;
    }
    files_txt.close();

    // Hooks
    if (fs::exists(hooks_dir)) {
        ensure_dir_exists(build_dir / "hooks");
        for (const auto& entry : fs::directory_iterator(hooks_dir)) {
            if (entry.is_regular_file()) {
                fs::copy_file(entry.path(), build_dir / "hooks" / entry.path().filename(), fs::copy_options::overwrite_existing);
            }
        }
    }

    // Metadata defaults
    if (!fs::exists(build_dir / "deps.txt")) {
        if (fs::exists(base_dir / "deps.txt")) {
            fs::copy_file(base_dir / "deps.txt", build_dir / "deps.txt");
        } else {
            std::ofstream(build_dir / "deps.txt").close();
        }
    }
    
    if (!fs::exists(build_dir / "man.txt")) {
        if (fs::exists(base_dir / "man.txt")) {
            fs::copy_file(base_dir / "man.txt", build_dir / "man.txt");
        } else {
            std::ofstream f(build_dir / "man.txt");
            f << "Auto-generated package" << std::endl;
        }
    }

    // Create archive
    // Note: Assuming 'tar' and 'zstd' are available in environment
    std::string cmd = "tar -I zstd -cf " + output_filename + " -C " + build_dir.string() + " .";
    log_info(string_format("info.pack_creating", output_filename));
    int ret = std::system(cmd.c_str());
    
    fs::remove_all(build_dir);
    
    if (ret != 0) {
        throw LpkgException(string_format("error.pack_failed", ret));
    }
    
    std::string hash = calculate_sha256(output_filename);
    std::cout << get_string("info.pack_success") << " " << output_filename << std::endl;
    std::cout << "SHA256: " << hash << std::endl;
}