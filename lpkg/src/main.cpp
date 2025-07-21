// main.cpp
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cstdlib>
#include <curl/curl.h>
#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/utsname.h>

namespace fs = std::filesystem;

// 全局常量
const std::string CONFIG_DIR = "/etc/lpkg/";
const std::string DEP_DIR = CONFIG_DIR + "deps/";
const std::string FILES_DIR = CONFIG_DIR + "files/";
const std::string PKGS_FILE = CONFIG_DIR + "pkgs";
const std::string HOLDPKGS_FILE = CONFIG_DIR + "holdpkgs";
const std::string MIRROR_CONF = CONFIG_DIR + "mirror.conf";
const std::string DOCS_DIR = "/usr/share/lpkg/docs/";
const std::string TMP_DIR = "/tmp/lpkg/";

// 错误处理
void exit_with_error(const std::string& msg) {
    std::cerr << "错误: " << msg << std::endl;
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
    }
    
    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
    
    return (r == ARCHIVE_EOF);
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

    // 读取镜像配置
    std::ifstream mirror_file(MIRROR_CONF);
    std::string mirror_url;
    if (!std::getline(mirror_file, mirror_url) || mirror_url.empty()) {
        exit_with_error("无效的镜像配置");
    }
    if (mirror_url.back() != '/') mirror_url += '/';

    // 获取架构
    std::string arch = get_architecture();

    // 准备临时目录
    std::string tmp_pkg_dir = TMP_DIR + pkg_name;
    fs::remove_all(tmp_pkg_dir);
    ensure_dir_exists(tmp_pkg_dir);

    // 下载包
    std::string download_url = mirror_url + arch + "/" + pkg_name + "/" + version + "/app.tar.zst";
    std::string archive_path = tmp_pkg_dir + "/app.tar.zst";
    
    if (!download_file(download_url, archive_path)) {
        fs::remove_all(tmp_pkg_dir);
        exit_with_error("无法下载包: " + download_url);
    }

    // 解压包
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
    std::ifstream deps_file(tmp_pkg_dir + "/deps.txt");
    std::string dep;
    while (std::getline(deps_file, dep)) {
        if (!dep.empty()) {
            // 安装依赖包
            install_package(dep, "latest", false);
        }
    }

    // 复制文件
    std::ifstream files_list(tmp_pkg_dir + "/files.txt");
    std::string src, dest;
    while (files_list >> src >> dest) {
        std::string src_path = tmp_pkg_dir + "/content/" + src;
        std::string dest_path = dest + "/" + src;
        
        if (!fs::exists(src_path)) {
            fs::remove_all(tmp_pkg_dir);
            exit_with_error("包中缺少文件: " + src);
        }
        
        ensure_dir_exists(fs::path(dest_path).parent_path());
        fs::copy(src_path, dest_path, fs::copy_options::overwrite_existing);
    }

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
    pkgs.insert(pkg_name + ":" + version);
    write_set_to_file(PKGS_FILE, pkgs);
    
    if (explicit_install) {
        auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
        holdpkgs.insert(pkg_name);
        write_set_to_file(HOLDPKGS_FILE, holdpkgs);
    }

    // 清理临时文件
    fs::remove_all(tmp_pkg_dir);
}

// 执行卸载一个包
void remove_package(const std::string& pkg_name) {
    // 检查是否被依赖
    for (const auto& dep_file : fs::directory_iterator(DEP_DIR)) {
        std::ifstream file(dep_file.path());
        std::string dep;
        while (std::getline(file, dep)) {
            if (dep == pkg_name) {
                exit_with_error("包被依赖: " + dep_file.path().filename().string());
            }
        }
    }

    // 删除文件
    std::string files_list_path = FILES_DIR + pkg_name + ".txt";
    if (fs::exists(files_list_path)) {
        std::ifstream files_list(files_list_path);
        std::string file_path;
        while (std::getline(files_list, file_path)) {
            if (fs::exists(file_path)) {
                fs::remove(file_path);
            }
        }
        fs::remove(files_list_path);
    }

    // 删除依赖文件
    fs::remove(DEP_DIR + pkg_name);

    // 删除man文档
    fs::remove(DOCS_DIR + pkg_name + ".man");

    // 更新包列表
    auto pkgs = read_set_from_file(PKGS_FILE);
    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
    
    for (auto it = pkgs.begin(); it != pkgs.end(); ) {
        if (it->find(pkg_name + ":") == 0) {
            it = pkgs.erase(it);
        } else {
            ++it;
        }
    }
    
    holdpkgs.erase(pkg_name);
    
    write_set_to_file(PKGS_FILE, pkgs);
    write_set_to_file(HOLDPKGS_FILE, holdpkgs);
}

// 自动移除未使用的包
void autoremove() {
    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
    auto pkgs = read_set_from_file(PKGS_FILE);
    std::unordered_set<std::string> required_pkgs;
    
    // 收集被依赖的包
    for (const auto& dep_file : fs::directory_iterator(DEP_DIR)) {
        std::ifstream file(dep_file.path());
        std::string dep;
        while (std::getline(file, dep)) {
            required_pkgs.insert(dep);
        }
    }
    
    // 移除不需要的包
    for (const auto& pkg : pkgs) {
        size_t pos = pkg.find(':');
        std::string pkg_name = pkg.substr(0, pos);
        
        // 跳过手动安装的包
        if (holdpkgs.find(pkg_name) != holdpkgs.end()) continue;
        
        // 检查是否被需要
        if (required_pkgs.find(pkg_name) == required_pkgs.end()) {
            remove_package(pkg_name);
        }
    }
    
    // 清理临时目录
    fs::remove_all(TMP_DIR);
    ensure_dir_exists(TMP_DIR);
}

// 升级所有包
void upgrade_packages() {
    // TODO: 实现包升级逻辑
    // 此处为简化示例，实际实现需要：
    // 1. 获取所有已安装包
    // 2. 检查每个包的最新版本
    // 3. 下载新版本
    // 4. 移除旧版本
    // 5. 安装新版本
    std::cout << "升级功能尚未实现" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "用法: " << argv[0] << " <命令> [参数]\n"
                  << "命令:\n"
                  << "  install <包名>:<版本>\n"
                  << "  remove <包名>\n"
                  << "  autoremove\n"
                  << "  upgrade\n";
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
        remove_package(argv[2]);
    } 
    else if (command == "autoremove") {
        autoremove();
    } 
    else if (command == "upgrade") {
        upgrade_packages();
    } 
    else {
        std::cerr << "无效命令或参数\n";
        return 1;
    }

    return 0;
}
