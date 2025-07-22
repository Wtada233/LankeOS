#include "package_manager.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "downloader.hpp"
#include "archive.hpp"
#include "version.hpp"

#include <iostream>
#include <vector>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <set>
#include <algorithm>

namespace fs = std::filesystem;

// Forward declarations for internal functions
std::string get_installed_version(const std::string& pkg_name);
bool is_manually_installed(const std::string& pkg_name);
void do_install(const std::string& pkg_name, const std::string& version, bool explicit_install);

// Implementation
std::string get_installed_version(const std::string& pkg_name) {
    auto pkgs = read_set_from_file(PKGS_FILE);
    for (const auto& pkg : pkgs) {
        if (pkg.find(pkg_name + ":") == 0) {
            return pkg.substr(pkg_name.length() + 1);
        }
    }
    return "";
}

bool is_manually_installed(const std::string& pkg_name) {
    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
    return holdpkgs.count(pkg_name) > 0;
}

void install_package(const std::string& pkg_name, const std::string& version) {
    do_install(pkg_name, version, true);
}

void do_install(const std::string& pkg_name, const std::string& version, bool explicit_install) {
    if (!get_installed_version(pkg_name).empty()) {
        log_info("包 " + pkg_name + " 已安装，跳过。");
        return;
    }

    log_info("开始安装 " + pkg_name + " (版本: " + version + ")");

    std::string mirror_url = get_mirror_url();
    std::string arch = get_architecture();

    std::string actual_version = version;
    if (version == "latest") {
        actual_version = get_latest_version(pkg_name);
        log_info("最新版本为: " + actual_version);
    }

    std::string tmp_pkg_dir = TMP_DIR + pkg_name;
    fs::remove_all(tmp_pkg_dir);
    ensure_dir_exists(tmp_pkg_dir);

    std::string download_url = mirror_url + arch + "/" + pkg_name + "/" + actual_version + "/app.tar.zst";
    std::string archive_path = tmp_pkg_dir + "/app.tar.zst";

    log_info("正在从 " + download_url + " 下载...");
    if (!download_file(download_url, archive_path)) {
        fs::remove_all(tmp_pkg_dir);
        exit_with_error("无法下载包: " + download_url);
    }

    log_info("正在解压到临时目录...");
    if (!extract_tar_zst(archive_path, tmp_pkg_dir)) {
        fs::remove_all(tmp_pkg_dir);
        exit_with_error("解压失败: " + archive_path);
    }

    std::vector<std::string> required_files = {"man.txt", "deps.txt", "files.txt", "content/"};
    for (const auto& file : required_files) {
        if (!fs::exists(tmp_pkg_dir + "/" + file)) {
            fs::remove_all(tmp_pkg_dir);
            exit_with_error("包不完整，缺少文件: " + file);
        }
    }

    log_info("正在检查依赖...");
    std::ifstream deps_file(tmp_pkg_dir + "/deps.txt");
    std::string dep;
    while (std::getline(deps_file, dep)) {
        if (dep.empty()) continue;

        log_sync("发现依赖包: " + dep);
        std::string installed_version = get_installed_version(dep);

        if (installed_version.empty()) {
            log_sync("依赖 " + dep + " 未安装，开始安装...");
            do_install(dep, "latest", false);
        } else {
            log_sync("依赖 " + dep + " 已安装，跳过。");
        }
    }

    if (!get_installed_version(pkg_name).empty()) {
        fs::remove_all(tmp_pkg_dir);
        log_info("警告: 跳过安装，包已在依赖安装过程中安装: " + pkg_name);
        return;
    }

    log_info("正在复制文件到系统...");
    std::ifstream files_list(tmp_pkg_dir + "/files.txt");
    std::string src, dest;
    int file_count = 0;
    std::vector<std::string> installed_files;
    while (files_list >> src >> dest) {
        std::string src_path = tmp_pkg_dir + "/content/" + src;
        std::string dest_path = dest + "/" + src;
        installed_files.push_back(dest_path);

        if (!fs::exists(src_path)) {
            fs::remove_all(tmp_pkg_dir);
            exit_with_error("包中缺少文件: " + src);
        }

        fs::path dest_parent = fs::path(dest_path).parent_path();
        if (!dest_parent.empty()) {
            ensure_dir_exists(dest_parent.string());
        }

        try {
            fs::copy(src_path, dest_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            file_count++;
        } catch (const fs::filesystem_error& e) {
            fs::remove_all(tmp_pkg_dir);
            exit_with_error("复制失败: " + std::string(e.what()));
        }
    }
    log_sync("文件复制完成，共复制 " + std::to_string(file_count) + " 个文件");

    std::ofstream pkg_files(FILES_DIR + pkg_name + ".txt");
    for(const auto& file : installed_files) {
        pkg_files << file << "\n";
    }

    std::ofstream pkg_deps(DEP_DIR + pkg_name);
    deps_file.clear();
    deps_file.seekg(0);
    while (std::getline(deps_file, dep)) {
        if (!dep.empty()) pkg_deps << dep << "\n";
    }

    fs::copy(tmp_pkg_dir + "/man.txt", DOCS_DIR + pkg_name + ".man", fs::copy_options::overwrite_existing);

    auto pkgs = read_set_from_file(PKGS_FILE);
    pkgs.insert(pkg_name + ":" + actual_version);
    write_set_to_file(PKGS_FILE, pkgs);

    if (explicit_install) {
        auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
        holdpkgs.insert(pkg_name);
        write_set_to_file(HOLDPKGS_FILE, holdpkgs);
    }

    fs::remove_all(tmp_pkg_dir);
    log_info(pkg_name + " 已成功安装!");
}

void remove_package(const std::string& pkg_name, bool force) {
    if (get_installed_version(pkg_name).empty()) {
        log_info("包 " + pkg_name + " 未安装，无需移除。");
        return;
    }

    if (!force) {
        for (const auto& dep_file : fs::directory_iterator(DEP_DIR)) {
            std::string current_pkg_name = dep_file.path().stem().string();
            if (current_pkg_name == pkg_name) continue;

            std::ifstream file(dep_file.path());
            std::string dep;
            while (std::getline(file, dep)) {
                if (dep == pkg_name) {
                    exit_with_error("跳过删除，包 " + pkg_name + " 被 " + current_pkg_name + " 依赖");
                }
            }
        }
    }

    log_info("正在移除 " + pkg_name + "...");

    std::string files_list_path = FILES_DIR + pkg_name + ".txt";
    if (fs::exists(files_list_path)) {
        std::ifstream files_list(files_list_path);
        std::string file_path;
        std::vector<std::string> file_paths;
        while (std::getline(files_list, file_path)) {
            file_paths.push_back(file_path);
        }
        
        std::sort(file_paths.rbegin(), file_paths.rend());

        int removed_count = 0;
        for (const auto& path : file_paths) {
            if (fs::exists(path) || fs::is_symlink(path)) {
                try {
                    fs::remove(path);
                    removed_count++;
                } catch (...) { /* Ignore errors */ }
            }
        }
        log_sync("移除了 " + std::to_string(removed_count) + " 个文件");
        fs::remove(files_list_path);
    }

    fs::remove(DEP_DIR + pkg_name);
    fs::remove(DOCS_DIR + pkg_name + ".man");

    auto pkgs = read_set_from_file(PKGS_FILE);
    std::string pkg_record;
    for (const auto& pkg : pkgs) {
        if (pkg.find(pkg_name + ":") == 0) {
            pkg_record = pkg;
            break;
        }
    }
    if (!pkg_record.empty()) {
        pkgs.erase(pkg_record);
        write_set_to_file(PKGS_FILE, pkgs);
    }

    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
    if (holdpkgs.count(pkg_name)) {
        holdpkgs.erase(pkg_name);
        write_set_to_file(HOLDPKGS_FILE, holdpkgs);
    }

    log_info(pkg_name + " 已成功移除");
}

std::unordered_set<std::string> get_required_packages() {
    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
    std::unordered_set<std::string> required_pkgs = holdpkgs;

    for (const auto& dep_file : fs::directory_iterator(DEP_DIR)) {
        std::ifstream file(dep_file.path());
        std::string dep;
        while (std::getline(file, dep)) {
            if (!dep.empty()) {
                required_pkgs.insert(dep);
            }
        }
    }
    return required_pkgs;
}

void autoremove() {
    log_info("正在检查可自动移除的包...");
    std::unordered_set<std::string> removed_pkgs;
    bool changed = true;
    while(changed) {
        changed = false;
        auto pkgs_current = read_set_from_file(PKGS_FILE);
        auto required = get_required_packages();
        for (const auto& pkg_rec : pkgs_current) {
            std::string pkg_name = pkg_rec.substr(0, pkg_rec.find(':'));
            if (!is_manually_installed(pkg_name) && !required.count(pkg_name)) {
                remove_package(pkg_name, true);
                removed_pkgs.insert(pkg_name);
                changed = true;
                break; 
            }
        }
    }

    if (removed_pkgs.empty()) {
        log_info("没有找到可自动移除的包。");
    } else {
        log_info("已自动移除 " + std::to_string(removed_pkgs.size()) + " 个包。");
    }
    fs::remove_all(TMP_DIR);
    ensure_dir_exists(TMP_DIR);
}

void upgrade_packages() {
    log_info("正在检查可升级的包...");
    auto pkgs = read_set_from_file(PKGS_FILE);
    int upgraded_count = 0;

    for (const auto& pkg : pkgs) {
        size_t pos = pkg.find(':');
        if (pos == std::string::npos) continue;

        std::string pkg_name = pkg.substr(0, pos);
        std::string current_version = pkg.substr(pos + 1);

        std::string latest_version;
        try {
            latest_version = get_latest_version(pkg_name);
        } catch (...) {
            log_error("无法获取 " + pkg_name + " 的最新版本，跳过");
            continue;
        }

        if (version_compare(current_version, latest_version)) {
            log_sync("发现可升级包: " + pkg_name + " (" + current_version + " -> " + latest_version + ")");
            bool was_manually_installed = is_manually_installed(pkg_name);
            remove_package(pkg_name, true);
            do_install(pkg_name, latest_version, was_manually_installed);
            upgraded_count++;
        }
    }

    if (upgraded_count == 0) {
        log_info("所有包都已是最新版本。");
    } else {
        log_info("已升级 " + std::to_string(upgraded_count) + " 个包。");
    }
}
