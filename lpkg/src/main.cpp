// main.cpp
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <cstdlib>
#include <curl/curl.h>
#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <algorithm>
#include <regex>
#include <set>
#include <iomanip>

namespace fs = std::filesystem;

// 函数声明
void remove_package(const std::string& pkg_name, bool force = false);
bool is_manually_installed(const std::string& pkg_name);
void install_package(const std::string& pkg_name, const std::string& version, bool explicit_install);


// 全局常量
const std::string CONFIG_DIR = "/etc/lpkg/";
const std::string DEP_DIR = CONFIG_DIR + "deps/";
const std::string FILES_DIR = CONFIG_DIR + "files/";
const std::string PKGS_FILE = CONFIG_DIR + "pkgs";
const std::string HOLDPKGS_FILE = CONFIG_DIR + "holdpkgs";
const std::string MIRROR_CONF = CONFIG_DIR + "mirror.conf";
const std::string DOCS_DIR = "/usr/share/lpkg/docs/";
const std::string TMP_DIR = "/tmp/lpkg/";

// 颜色代码
const std::string COLOR_GREEN = "\033[1;32m";
const std::string COLOR_WHITE = "\033[1;37m";
const std::string COLOR_RED = "\033[1;31m";
const std::string COLOR_RESET = "\033[0m";

// 日志函数
void log_info(const std::string& msg) {
    std::cout << COLOR_GREEN << "==> " << COLOR_WHITE << msg << COLOR_RESET << std::endl;
}

void log_sync(const std::string& msg) {
    std::cout << COLOR_GREEN << ">>> " << COLOR_WHITE << msg << COLOR_RESET << std::endl;
}

void log_error(const std::string& msg) {
    std::cerr << COLOR_RED << "错误: " << COLOR_RESET << msg << std::endl;
}


// 错误处理
void exit_with_error(const std::string& msg) {
    log_error(msg);
    exit(1);
}

// 检查root权限
void check_root() {
    if (geteuid() != 0) {
        exit_with_error("需要root权限运行");
    }
}

// 确保目录存在
void ensure_dir_exists(const std::string& path) {
    if (!fs::exists(path)) {
        if (!fs::create_directories(path)) {
            exit_with_error("无法创建目录: " + path);
        }
    } else if (!fs::is_directory(path)) {
        exit_with_error("路径不是目录: " + path);
    }
}

// 确保文件存在
void ensure_file_exists(const std::string& path) {
    if (!fs::exists(path)) {
        std::ofstream file(path);
        if (!file) {
            exit_with_error("无法创建文件: " + path);
        }
    }
}

// 初始化文件系统
void init_filesystem() {
    ensure_dir_exists(CONFIG_DIR);
    ensure_dir_exists(DEP_DIR);
    ensure_dir_exists(FILES_DIR);
    ensure_dir_exists(DOCS_DIR);
    ensure_dir_exists(TMP_DIR);
    ensure_file_exists(PKGS_FILE);
    ensure_file_exists(HOLDPKGS_FILE);
}

// 读取文件到集合
std::unordered_set<std::string> read_set_from_file(const std::string& path) {
    std::ifstream file(path);
    std::unordered_set<std::string> result;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) result.insert(line);
    }
    return result;
}

// 写入集合到文件
void write_set_to_file(const std::string& path, const std::unordered_set<std::string>& data) {
    std::ofstream file(path);
    for (const auto& item : data) {
        file << item << "\n";
    }
}

// 获取系统架构
std::string get_architecture() {
    struct utsname buf;
    if (uname(&buf) != 0) {
        exit_with_error("无法获取系统架构");
    }
    std::string arch(buf.machine);
    if (arch != "x86_64" && arch != "aarch64") {
        exit_with_error("不支持的架构: " + arch);
    }
    return (arch == "x86_64") ? "amd64" : "arm64";
}

// HTTP下载回调函数
size_t write_data(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    return fwrite(ptr, size, nmemb, stream);
}

// 下载进度条回调
int progress_callback([[maybe_unused]] void* clientp, curl_off_t dltotal, curl_off_t dlnow, [[maybe_unused]] curl_off_t ultotal, [[maybe_unused]] curl_off_t ulnow) {
    if (dltotal <= 0) {
        return 0;
    }

    double percentage = static_cast<double>(dlnow) / static_cast<double>(dltotal) * 100.0;
    int bar_width = 50;
    int pos = static_cast<int>(bar_width * percentage / 100.0);

    std::cout << COLOR_GREEN << "==> " << COLOR_WHITE << "正在下载... [";
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) std::cout << "#";
        else if (i == pos) std::cout << ">";
        else std::cout << "-";
    }
    std::cout << "] " << std::fixed << std::setprecision(1) << percentage << "%\r";
    std::cout.flush();

    if (dlnow == dltotal) {
        std::cout << std::endl;
    }

    return 0;
}


// 下载文件
bool download_file(const std::string& url, const std::string& output_path) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    FILE* fp = fopen(output_path.c_str(), "wb");
    if (!fp) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);

    CURLcode res = curl_easy_perform(curl);
    fclose(fp);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        // 尝试HTTP回退
        if (url.find("https://") == 0) {
            std::string http_url = "http://" + url.substr(8);
            return download_file(http_url, output_path);
        }
        return false;
    }
    return true;
}

// 解压tar.zst文件
bool extract_tar_zst(const std::string& archive_path, const std::string& output_dir) {
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    struct archive* ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS);
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_filename(a, archive_path.c_str(), 10240) != ARCHIVE_OK) {
        return false;
    }

    struct archive_entry* entry;
    int r;
    int count = 0;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        std::string path = output_dir + "/" + archive_entry_pathname(entry);
        archive_entry_set_pathname(entry, path.c_str());
        r = archive_write_header(ext, entry);
        if (r < ARCHIVE_OK) {
            break;
        }

        const void* buff;
        size_t size;
        la_int64_t offset;
        while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
            if (archive_write_data_block(ext, buff, size, offset) < ARCHIVE_OK) {
                break;
            }
        }

        if (r < ARCHIVE_OK) break;
        archive_write_finish_entry(ext);

        if (++count % 100 == 0) {
            log_sync("正在解压文件: " + std::to_string(count) + " 个已处理");
        }
    }

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);

    log_sync("解压完成，共处理 " + std::to_string(count) + " 个文件");
    return (r == ARCHIVE_EOF);
}

// 比较版本号
bool version_compare(const std::string& v1_str, const std::string& v2_str) {
    // Split into main version and pre-release part
    std::string v1_main, v1_pre, v2_main, v2_pre;
    size_t v1_hyphen = v1_str.find('-');
    if (v1_hyphen != std::string::npos) {
        v1_main = v1_str.substr(0, v1_hyphen);
        v1_pre = v1_str.substr(v1_hyphen + 1);
    } else {
        v1_main = v1_str;
    }

    size_t v2_hyphen = v2_str.find('-');
    if (v2_hyphen != std::string::npos) {
        v2_main = v2_str.substr(0, v2_hyphen);
        v2_pre = v2_str.substr(v2_hyphen + 1);
    } else {
        v2_main = v2_str;
    }

    // Compare main versions
    std::regex re_dot("[.]");
    std::vector<std::string> p1_main{std::sregex_token_iterator(v1_main.begin(), v1_main.end(), re_dot, -1), std::sregex_token_iterator()};
    std::vector<std::string> p2_main{std::sregex_token_iterator(v2_main.begin(), v2_main.end(), re_dot, -1), std::sregex_token_iterator()};
    
    size_t main_len = std::max(p1_main.size(), p2_main.size());
    for (size_t i = 0; i < main_len; ++i) {
        int n1 = (i < p1_main.size() && !p1_main[i].empty()) ? std::stoi(p1_main[i]) : 0;
        int n2 = (i < p2_main.size() && !p2_main[i].empty()) ? std::stoi(p2_main[i]) : 0;
        if (n1 < n2) return true;
        if (n1 > n2) return false;
    }

    // Main versions are equal, compare pre-release
    if (v1_pre.empty() && !v2_pre.empty()) return false; // 1.0.0 > 1.0.0-alpha
    if (!v1_pre.empty() && v2_pre.empty()) return true;  // 1.0.0-alpha < 1.0.0
    if (v1_pre.empty() && v2_pre.empty()) return false; // equal

    // Both have pre-release tags, compare them
    std::regex re_pre("[.-]");
    std::vector<std::string> p1_pre{std::sregex_token_iterator(v1_pre.begin(), v1_pre.end(), re_pre, -1), std::sregex_token_iterator()};
    std::vector<std::string> p2_pre{std::sregex_token_iterator(v2_pre.begin(), v2_pre.end(), re_pre, -1), std::sregex_token_iterator()};

    size_t pre_len = std::max(p1_pre.size(), p2_pre.size());
    for (size_t i = 0; i < pre_len; ++i) {
        if (i >= p1_pre.size()) return true; // 1.0.0-alpha < 1.0.0-alpha.1
        if (i >= p2_pre.size()) return false;

        const std::string& part1 = p1_pre[i];
        const std::string& part2 = p2_pre[i];

        bool is_num1 = !part1.empty() && std::all_of(part1.begin(), part1.end(), ::isdigit);
        bool is_num2 = !part2.empty() && std::all_of(part2.begin(), part2.end(), ::isdigit);

        if (is_num1 && is_num2) {
            int n1 = std::stoi(part1);
            int n2 = std::stoi(part2);
            if (n1 < n2) return true;
            if (n1 > n2) return false;
        } else {
            // numeric identifiers always have lower precedence than non-numeric identifiers.
            if (is_num1 && !is_num2) return true;
            if (!is_num1 && is_num2) return false;
            
            // both are strings
            if (part1 < part2) return true;
            if (part1 > part2) return false;
        }
    }

    return false; // equal
}

// 获取最新版本
std::string get_latest_version(const std::string& mirror_url, const std::string& arch, const std::string& pkg_name) {
    std::string versions_url = mirror_url + arch + "/" + pkg_name + "/";
    std::string tmp_file = TMP_DIR + "versions.html";

    if (!download_file(versions_url, tmp_file)) {
        exit_with_error("无法获取版本列表: " + versions_url);
    }

    // 解析HTML获取版本列表
    std::ifstream file(tmp_file);
    std::string line;
    std::vector<std::string> versions;
    std::regex version_link_regex(R"(<a href="([^"]+)/">)");

    while (std::getline(file, line)) {
        std::smatch match;
        if (std::regex_search(line, match, version_link_regex)) {
            std::string version = match[1].str();
            if (version != "..") {
                versions.push_back(version);
            }
        }
    }

    fs::remove(tmp_file);

    if (versions.empty()) {
        exit_with_error("没有找到可用版本");
    }

    // 按版本号排序
    std::sort(versions.begin(), versions.end(), [](const std::string& a, const std::string& b) {
        return version_compare(a, b);
    });

    return versions.back();
}

// 获取当前安装的版本
std::string get_installed_version(const std::string& pkg_name) {
    auto pkgs = read_set_from_file(PKGS_FILE);
    for (const auto& pkg : pkgs) {
        if (pkg.find(pkg_name + ":") == 0) {
            return pkg.substr(pkg_name.length() + 1);
        }
    }
    return "";
}

// 执行安装一个包
void install_package(const std::string& pkg_name, const std::string& version, bool explicit_install) {
    // 检查是否已安装
    auto pkgs = read_set_from_file(PKGS_FILE);
    for (const auto& pkg : pkgs) {
        if (pkg.find(pkg_name + ":") == 0) {
            exit_with_error("包已安装: " + pkg_name);
        }
    }

    log_info("开始安装 " + pkg_name + " (版本: " + version + ")");

    // 读取镜像配置
    std::ifstream mirror_file(MIRROR_CONF);
    std::string mirror_url;
    if (!std::getline(mirror_file, mirror_url) || mirror_url.empty()) {
        exit_with_error("无效的镜像配置");
    }
    if (mirror_url.back() != '/') mirror_url += '/';

    // 获取架构
    std::string arch = get_architecture();

    // 如果是latest，获取最新版本
    std::string actual_version = version;
    if (version == "latest") {
        actual_version = get_latest_version(mirror_url, arch, pkg_name);
        log_info("最新版本为: " + actual_version);
    }

    // 准备临时目录
    std::string tmp_pkg_dir = TMP_DIR + pkg_name;
    fs::remove_all(tmp_pkg_dir);
    ensure_dir_exists(tmp_pkg_dir);

    // 下载包
    std::string download_url = mirror_url + arch + "/" + pkg_name + "/" + actual_version + "/app.tar.zst";
    std::string archive_path = tmp_pkg_dir + "/app.tar.zst";

    log_info("正在从 " + download_url + " 下载...");
    if (!download_file(download_url, archive_path)) {
        fs::remove_all(tmp_pkg_dir);
        exit_with_error("无法下载包: " + download_url);
    }

    // 解压包
    log_info("正在解压到临时目录...");
    if (!extract_tar_zst(archive_path, tmp_pkg_dir)) {
        fs::remove_all(tmp_pkg_dir);
        exit_with_error("解压失败: " + archive_path);
    }

    // 检查必需文件
    std::vector<std::string> required_files = {"man.txt", "deps.txt", "files.txt", "content/"};
    for (const auto& file : required_files) {
        if (!fs::exists(tmp_pkg_dir + "/" + file)) {
            fs::remove_all(tmp_pkg_dir);
            exit_with_error("包不完整，缺少文件: " + file);
        }
    }

    // 处理依赖
    log_info("正在检查依赖...");
    std::ifstream deps_file(tmp_pkg_dir + "/deps.txt");
    std::string dep;
    while (std::getline(deps_file, dep)) {
        if (dep.empty()) continue;

        log_sync("发现依赖包: " + dep);
        std::string installed_version = get_installed_version(dep);

        if (installed_version.empty()) {
            log_sync("依赖 " + dep + " 未安装，开始安装...");
            install_package(dep, "latest", false);
        } else {
            std::string latest_version;
            try {
                latest_version = get_latest_version(mirror_url, arch, dep);
            } catch (...) {
                log_error("无法获取 " + dep + " 的最新版本，跳过更新检查");
                continue;
            }

            if (version_compare(installed_version, latest_version)) {
                log_sync("发现可升级依赖: " + dep + " (" + installed_version + " -> " + latest_version + ")");
                bool is_held = is_manually_installed(dep);
                remove_package(dep, true); // 强制移除以进行升级
                install_package(dep, latest_version, is_held);
            } else {
                log_sync("依赖 " + dep + " 已是最新版本，跳过。");
            }
        }
        pkgs = read_set_from_file(PKGS_FILE); // 重新加载包列表
    }

    // 修复：依赖安装后再次检查当前包
    bool already_installed = false;
    for (const auto& pkg : pkgs) {
        if (pkg.find(pkg_name + ":") == 0) {
            already_installed = true;
            break;
        }
    }
    if (already_installed) {
        fs::remove_all(tmp_pkg_dir);
        log_info("警告: 跳过安装，包已在依赖安装过程中安装: " + pkg_name);
        return;
    }

    // 复制文件
    log_info("正在复制文件到系统...");
    std::ifstream files_list(tmp_pkg_dir + "/files.txt");
    std::string src, dest;
    int file_count = 0;
    while (files_list >> src >> dest) {
        std::string src_path = tmp_pkg_dir + "/content/" + src;
        std::string dest_path = dest + "/" + src;

        if (!fs::exists(src_path)) {
            fs::remove_all(tmp_pkg_dir);
            exit_with_error("包中缺少文件: " + src);
        }

        fs::path dest_parent = fs::path(dest_path).parent_path();
        if (!dest_parent.empty()) {
            ensure_dir_exists(dest_parent.string());
        }

        try {
            if (fs::is_directory(src_path)) {
                fs::copy(src_path, dest_path, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
                for (auto& p : fs::recursive_directory_iterator(src_path)) {
                    if (fs::is_regular_file(p)) file_count++;
                }
            } else {
                fs::copy(src_path, dest_path, fs::copy_options::overwrite_existing);
                file_count++;
            }

            if (file_count % 50 == 0) {
                log_sync("已复制 " + std::to_string(file_count) + " 个文件");
            }
        } catch (const fs::filesystem_error& e) {
            fs::remove_all(tmp_pkg_dir);
            exit_with_error("复制失败: " + std::string(e.what()));
        }
    }
    log_sync("文件复制完成，共复制 " + std::to_string(file_count) + " 个文件");

    // 保存文件列表
    std::ofstream pkg_files(FILES_DIR + pkg_name + ".txt");
    files_list.clear();
    files_list.seekg(0);
    while (files_list >> src >> dest) {
        pkg_files << dest << "/" << src << "\n";
    }

    // 保存依赖
    std::ofstream pkg_deps(DEP_DIR + pkg_name);
    deps_file.clear();
    deps_file.seekg(0);
    while (std::getline(deps_file, dep)) {
        if (!dep.empty()) pkg_deps << dep << "\n";
    }

    // 复制man文档
    fs::copy(tmp_pkg_dir + "/man.txt", DOCS_DIR + pkg_name + ".man", fs::copy_options::overwrite_existing);

    // 更新包列表
    pkgs.insert(pkg_name + ":" + actual_version);
    write_set_to_file(PKGS_FILE, pkgs);

    if (explicit_install) {
        auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
        holdpkgs.insert(pkg_name);
        write_set_to_file(HOLDPKGS_FILE, holdpkgs);
    }

    // 清理临时文件
    fs::remove_all(tmp_pkg_dir);

    log_info(pkg_name + " 已成功安装!");
}

// 执行卸载一个包
void remove_package(const std::string& pkg_name, bool force) {
    // 检查是否被依赖
    if (!force) {
        bool is_dependency = false;
        std::string dependent_pkg;
        for (const auto& dep_file : fs::directory_iterator(DEP_DIR)) {
            std::string current_pkg_name = dep_file.path().stem().string();
            if (current_pkg_name == pkg_name) continue;

            std::ifstream file(dep_file.path());
            std::string dep;
            while (std::getline(file, dep)) {
                if (dep == pkg_name) {
                    dependent_pkg = current_pkg_name;
                    is_dependency = true;
                    break;
                }
            }
            if (is_dependency) break;
        }

        if (is_dependency) {
            log_error("跳过删除，包 " + pkg_name + " 被 " + dependent_pkg + " 依赖");
            return;
        }
    }

    auto pkgs = read_set_from_file(PKGS_FILE);
    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);

    std::string pkg_record;
    for (const auto& pkg : pkgs) {
        if (pkg.find(pkg_name + ":") == 0) {
            pkg_record = pkg;
            break;
        }
    }

    if (pkg_record.empty()) {
        log_info("包 " + pkg_name + " 未安装，无需移除。");
        return;
    }

    log_info("正在移除文件...");
    int removed_count = 0;
    std::string files_list_path = FILES_DIR + pkg_name + ".txt";
    if (fs::exists(files_list_path)) {
        std::ifstream files_list(files_list_path);
        std::string file_path;
        std::vector<std::string> file_paths;
        while (std::getline(files_list, file_path)) {
            file_paths.push_back(file_path);
        }
        
        // Sort paths to remove files before directories
        std::sort(file_paths.rbegin(), file_paths.rend());

        for (const auto& path : file_paths) {
            if (fs::exists(path) || fs::is_symlink(path)) {
                try {
                    fs::remove(path);
                    removed_count++;
                    if (removed_count % 50 == 0) {
                        log_sync("已移除 " + std::to_string(removed_count) + " 个文件");
                    }
                } catch (const fs::filesystem_error& e) {
                    // Ignore errors, especially for directories that are not empty
                }
            }
        }
        fs::remove(files_list_path);
        log_sync("文件移除完成，共处理 " + std::to_string(removed_count) + " 个文件条目");
    }

    // Clean up empty parent directories
    if (fs::exists(files_list_path)) {
        std::ifstream files_list(files_list_path);
        std::string file_path;
        std::set<std::string> parent_dirs;
        while (std::getline(files_list, file_path)) {
            parent_dirs.insert(fs::path(file_path).parent_path().string());
        }

        std::vector<std::string> sorted_dirs(parent_dirs.begin(), parent_dirs.end());
        std::sort(sorted_dirs.rbegin(), sorted_dirs.rend());

        for(const auto& dir : sorted_dirs) {
            if (dir != "/" && fs::exists(dir) && fs::is_empty(dir)) {
                try {
                    fs::remove(dir);
                } catch (const fs::filesystem_error& e) {
                    // ignore
                }
            }
        }
    }


    fs::remove(DEP_DIR + pkg_name);
    fs::remove(DOCS_DIR + pkg_name + ".man");

    pkgs.erase(pkg_record);
    holdpkgs.erase(pkg_name);

    write_set_to_file(PKGS_FILE, pkgs);
    write_set_to_file(HOLDPKGS_FILE, holdpkgs);

    log_info(pkg_name + " 已成功移除");
}

// 检查包是否是手动安装的
bool is_manually_installed(const std::string& pkg_name) {
    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
    return holdpkgs.find(pkg_name) != holdpkgs.end();
}

// 获取所有必需的包
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

// 自动移除未使用的包
void autoremove() {
    std::unordered_set<std::string> removed_pkgs;
    bool found_removable = true;

    log_info("正在检查可自动移除的包...");

    while (found_removable) {
        found_removable = false;
        auto pkgs_current = read_set_from_file(PKGS_FILE);
        auto required_pkgs = get_required_packages();

        for (const auto& pkg : pkgs_current) {
            size_t pos = pkg.find(':');
            if (pos == std::string::npos) continue;

            std::string pkg_name = pkg.substr(0, pos);

            if (removed_pkgs.count(pkg_name)) continue;
            if (is_manually_installed(pkg_name)) continue;

            if (required_pkgs.find(pkg_name) == required_pkgs.end()) {
                log_sync("发现可移除包: " + pkg_name);
                remove_package(pkg_name);
                removed_pkgs.insert(pkg_name);
                found_removable = true;
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

// 升级所有包
void upgrade_packages() {
    std::ifstream mirror_file(MIRROR_CONF);
    std::string mirror_url;
    if (!std::getline(mirror_file, mirror_url) || mirror_url.empty()) {
        exit_with_error("无效的镜像配置");
    }
    if (mirror_url.back() != '/') mirror_url += '/';

    std::string arch = get_architecture();
    auto pkgs = read_set_from_file(PKGS_FILE);

    log_info("正在检查可升级的包...");
    int upgraded_count = 0;

    for (const auto& pkg : pkgs) {
        size_t pos = pkg.find(':');
        if (pos == std::string::npos) continue;

        std::string pkg_name = pkg.substr(0, pos);
        std::string current_version = pkg.substr(pos + 1);

        std::string latest_version;
        try {
            latest_version = get_latest_version(mirror_url, arch, pkg_name);
        } catch (...) {
            log_error("无法获取 " + pkg_name + " 的最新版本，跳过");
            continue;
        }

        if (version_compare(current_version, latest_version)) {
            log_sync("发现可升级包: " + pkg_name + " (" + current_version + " -> " + latest_version + ")");
            bool was_manually_installed = is_manually_installed(pkg_name);
            remove_package(pkg_name, true);
            install_package(pkg_name, latest_version, was_manually_installed);
            upgraded_count++;
        }
    }

    if (upgraded_count == 0) {
        log_info("所有包都已是最新版本。");
    } else {
        log_info("已升级 " + std::to_string(upgraded_count) + " 个包。");
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "用法: " << argv[0] << " <命令> [参数]\n"
        << "命令:\n"
        << "  install <包名>[:版本] 安装指定包\n"
        << "  remove <包名>        移除指定包\n"
        << "  autoremove           自动移除不再需要的包\n"
        << "    upgrade              升级所有包\n";
        return 1;
    }

    check_root();
    init_filesystem();

    std::string command = argv[1];

    if (command == "install" && argc == 3) {
        std::string arg = argv[2];
        size_t pos = arg.find(':');
        if (pos == std::string::npos) {
            install_package(arg, "latest", true);
        } else {
            install_package(arg.substr(0, pos), arg.substr(pos + 1), true);
        }
    }
    else if (command == "remove" && argc == 3) {
        remove_package(argv[2], false); // 默认非强制删除
    }
    else if (command == "autoremove") {
        autoremove();
    }
    else if (command == "upgrade") {
        upgrade_packages();
    }
    else {
        log_error("无效命令或参数");
        return 1;
    }

    return 0;
}
