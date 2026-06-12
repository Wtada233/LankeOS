#include "packer.hpp"
#include "utils.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "hash.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "nlohmann/json.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <iostream>
#include <algorithm>
#include <climits>
#include <sstream>

namespace fs = std::filesystem;

using json = nlohmann::json;

namespace {
    void add_to_archive(struct archive* a, const fs::path& path, const std::string& entry_name) {
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, entry_name.c_str());
        
        struct stat st;
        if (lstat(path.c_str(), &st) != 0) {
            archive_entry_free(entry);
            return;
        }
        archive_entry_copy_stat(entry, &st);

        if (S_ISLNK(st.st_mode)) {
            char link_target[PATH_MAX];
            ssize_t len = readlink(path.c_str(), link_target, sizeof(link_target) - 1);
            if (len != -1) {
                link_target[len] = '\0';
                archive_entry_set_symlink(entry, link_target);
            }
        }

        if (archive_write_header(a, entry) != ARCHIVE_OK) {
            archive_entry_free(entry);
            throw LpkgException("Archive write header failed: " + std::string(archive_error_string(a)));
        }

        if (S_ISREG(st.st_mode)) {
            std::ifstream f(path, std::ios::binary);
            char buffer[8192];
            while (f.read(buffer, sizeof(buffer)) || f.gcount() > 0) {
                if (archive_write_data(a, buffer, f.gcount()) < 0) {
                    archive_entry_free(entry);
                    throw LpkgException("Archive write data failed: " + std::string(archive_error_string(a)));
                }
            }
        }
        
        archive_entry_free(entry);
    }

    void add_dir_recursive(struct archive* a, const fs::path& dir, const std::string& archive_prefix) {
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            fs::path rel = entry.path().lexically_relative(dir);
            std::string entry_name = archive_prefix + "/" + rel.string();
            add_to_archive(a, entry.path(), entry_name);
        }
    }
}

void pack_package(const std::string& output_filename, const std::string& source_dir,
                  const std::string& pkg_name, const std::string& pkg_version) {
    fs::path base_dir = source_dir;
    fs::path root_dir = base_dir / constants::DIR_ROOT;
    fs::path hooks_dir = base_dir / constants::DIR_HOOKS;

    if (!fs::exists(root_dir)) {
        throw LpkgException(get_string("error.pack_root_not_found") + ": " + root_dir.string());
    }

    struct archive* a = archive_write_new();
    archive_write_add_filter_zstd(a);
    archive_write_set_format_pax_restricted(a);
    
    if (archive_write_open_filename(a, output_filename.c_str()) != ARCHIVE_OK) {
        throw LpkgException("Failed to open output archive: " + std::string(archive_error_string(a)));
    }

    try {
        log_info(get_string("info.pack_scanning"));
        
        // 1. Generate and add metadata.json
        fs::path tmp_meta = get_tmp_dir() / constants::PKG_METADATA_FILE;
        ensure_dir_exists(tmp_meta.parent_path());
        {
            json meta;
            meta["name"] = pkg_name;
            meta["version"] = pkg_version;

            std::ofstream f(tmp_meta);
            f << meta.dump(2) << std::endl;
        }
        add_to_archive(a, tmp_meta, std::string(constants::PKG_METADATA_FILE));
        fs::remove(tmp_meta);

        // 2. Add metadata files
        if (fs::exists(base_dir / constants::PKG_DEPS_FILE)) add_to_archive(a, base_dir / constants::PKG_DEPS_FILE, std::string(constants::PKG_DEPS_FILE));
        else {
            fs::path empty = get_tmp_dir() / "empty_deps";
            std::ofstream(empty).close();
            add_to_archive(a, empty, std::string(constants::PKG_DEPS_FILE));
            fs::remove(empty);
        }

        if (fs::exists(base_dir / constants::PKG_PROVIDES_FILE)) add_to_archive(a, base_dir / constants::PKG_PROVIDES_FILE, std::string(constants::PKG_PROVIDES_FILE));

        if (fs::exists(base_dir / constants::PKG_MAN_FILE)) add_to_archive(a, base_dir / constants::PKG_MAN_FILE, std::string(constants::PKG_MAN_FILE));
        else {
            fs::path empty_man = get_tmp_dir() / "empty_man";
            std::ofstream f(empty_man);
            f << "LankeOS Package" << std::endl;
            f.close();
            add_to_archive(a, empty_man, std::string(constants::PKG_MAN_FILE));
            fs::remove(empty_man);
        }
        
        if (fs::exists(hooks_dir)) {
            add_dir_recursive(a, hooks_dir, std::string(constants::DIR_HOOKS));
        }

        // 3. Add content (root dir -> content/)
        add_dir_recursive(a, root_dir, std::string(constants::DIR_CONTENT));

        archive_write_close(a);
        archive_write_free(a);
    } catch (...) {
        archive_write_close(a);
        archive_write_free(a);
        throw;
    }

    std::string hash = calculate_sha256(output_filename);
    std::cout << get_string("info.pack_success") << " " << output_filename << std::endl;
    std::cout << "SHA256: " << hash << std::endl;
}
