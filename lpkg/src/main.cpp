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

        // 每100个文件输出一次进度
        if (++count % 100 == 0) {
            std::cout << ">>> 正在解压文件: " << count << " 个已处理" << std::endl;
        }
    }

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);

    std::cout << ">>> 解压完成，共处理 " << count << " 个文件" << std::endl;
    return (r == ARCHIVE_EOF);
}

// 比较版本号
bool version_compare(const std::string& v1, const std::string& v2) {
    std::regex version_regex(R"((\d+)(?:\.(\d+))*(?:[\.\-]?([a-zA-Z]*)(\d*))?)");
    std::smatch m1, m2;

    if (!std::regex_match(v1, m1, version_regex) || !std::regex_match(v2, m2, version_regex)) {
        return v1 < v2; // 如果不符合版本格式，按字符串比较
    }

    // 比较数字部分
    for (size_t i = 1; i < m1.size() && i < m2.size(); ++i) {
        if (m1[i].str().empty() && m2[i].str().empty()) continue;

        if (m1[i].str().empty()) return true;
        if (m2[i].str().empty()) return false;

        if (i == 3) { // 字母部分
            int cmp = m1[i].str().compare(m2[i].str());
            if (cmp != 0) return cmp < 0;
        } else { // 数字部分
            int num1 = m1[i].str().empty() ? 0 : std::stoi(m1[i].str());
            int num2 = m2[i].str().empty() ? 0 : std::stoi(m2[i].str());
            if (num1 != num2) return num1 < num2;
        }
    }

    return false;
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
    std::regex version_link_regex(R"(<a href="([^"/]+)/">)");

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

    std::cout << ">>> 开始安装 " << pkg_name << " (版本: " << version << ")" << std::endl;

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
        std::cout << ">>> 最新版本为: " << actual_version << std::endl;
    }

    // 准备临时目录
    std::string tmp_pkg_dir = TMP_DIR + pkg_name;
    fs::remove_all(tmp_pkg_dir);
    ensure_dir_exists(tmp_pkg_dir);

    // 下载包
    std::string download_url = mirror_url + arch + "/" + pkg_name + "/" + actual_version + "/app.tar.zst";
    std::string archive_path = tmp_pkg_dir + "/app.tar.zst";

    std::cout << ">>> 正在从 " << download_url << " 下载..." << std::endl;
    if (!download_file(download_url, archive_path)) {
        fs::remove_all(tmp_pkg_dir);
        exit_with_error("无法下载包: " + download_url);
    }
    std::cout << ">>> 下载完成" << std::endl;

    // 解压包
    std::cout << ">>> 正在解压到临时目录..." << std::endl;
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
    std::cout << ">>> 正在检查依赖..." << std::endl;
    std::ifstream deps_file(tmp_pkg_dir + "/deps.txt");
    std::string dep;
    while (std::getline(deps_file, dep)) {
        if (!dep.empty()) {
            std::cout << ">>> 发现依赖包: " << dep << "，开始安装..." << std::endl;
            // 安装依赖包
            install_package(dep, "latest", false);
            // 关键修复：更新本地包集合（确保包含新安装的依赖）
            pkgs = read_set_from_file(PKGS_FILE);
        }
    }

    // 修复：依赖安装后再次检查当前包（防止循环依赖安装）
    bool already_installed = false;
    for (const auto& pkg : pkgs) {
        if (pkg.find(pkg_name + ":") == 0) {
            already_installed = true;
            break;
        }
    }
    if (already_installed) {
        fs::remove_all(tmp_pkg_dir);
        std::cout << "警告: 跳过安装，包已在依赖安装过程中安装: " << pkg_name << std::endl;
        return;
    }

    // 复制文件 - 修改部分开始
    std::cout << ">>> 正在复制文件到系统..." << std::endl;
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

        // 确保目标父目录存在
        fs::path dest_parent = fs::path(dest_path).parent_path();
        if (!dest_parent.empty()) {
            ensure_dir_exists(dest_parent.string());
        }

        try {
            if (fs::is_directory(src_path)) {
                // 递归复制目录
                fs::copy(src_path, dest_path,
                         fs::copy_options::recursive |
                         fs::copy_options::overwrite_existing
                );
                // 计算目录中的文件数量用于进度显示
                for (auto& p : fs::recursive_directory_iterator(src_path)) {
                    if (fs::is_regular_file(p)) {
                        file_count++;
                    }
                }
            } else {
                // 复制单个文件
                fs::copy(src_path, dest_path,
                         fs::copy_options::overwrite_existing
                );
                file_count++;
            }

            // 每50个文件输出一次进度
            if (file_count % 50 == 0) {
                std::cout << ">>> 已复制 " << file_count << " 个文件" << std::endl;
            }
        } catch (const fs::filesystem_error& e) {
            fs::remove_all(tmp_pkg_dir);
            exit_with_error("复制失败: " + std::string(e.what()));
        }
    }
    std::cout << ">>> 文件复制完成，共复制 " << file_count << " 个文件" << std::endl;
    // 修改部分结束

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

    std::cout << "<<< " << pkg_name << " 已成功安装!" << std::endl;
}

// 执行卸载一个包
void remove_package(const std::string& pkg_name) {
    // 检查是否被依赖
    bool is_dependency = false;
    std::string dependent_pkg;
    for (const auto& dep_file : fs::directory_iterator(DEP_DIR)) {
        // 跳过当前包自己的依赖文件
        if (dep_file.path().filename() == pkg_name) {
            continue;
        }

        std::ifstream file(dep_file.path());
        std::string dep;
        while (std::getline(file, dep)) {
            if (dep == pkg_name) {
                dependent_pkg = dep_file.path().filename().string();
                is_dependency = true;
                break;
            }
        }
        if (is_dependency) break;
    }

    if (is_dependency) {
        std::cerr << "警告: 跳过删除，包 " << pkg_name
        << " 被 " << dependent_pkg << " 依赖\n";
        return;
    }

    // 读取当前包列表
    auto pkgs = read_set_from_file(PKGS_FILE);
    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);

    // 查找要删除的包的确切记录
    std::string pkg_record;
    for (const auto& pkg : pkgs) {
        if (pkg.find(pkg_name + ":") == 0) {
            pkg_record = pkg;
            break;
        }
    }

    if (pkg_record.empty()) {
        exit_with_error("找不到包记录: " + pkg_name);
    }

    // 删除文件
    std::cout << ">>> 正在移除文件..." << std::endl;
    int removed_count = 0;
    std::string files_list_path = FILES_DIR + pkg_name + ".txt";
    if (fs::exists(files_list_path)) {
        std::ifstream files_list(files_list_path);
        std::string file_path;
        while (std::getline(files_list, file_path)) {
            if (fs::exists(file_path)) {
                fs::remove(file_path);
                removed_count++;
                // 每50个文件输出一次进度
                if (removed_count % 50 == 0) {
                    std::cout << ">>> 已移除 " << removed_count << " 个文件" << std::endl;
                }
                // 尝试删除空目录
                std::string dir_path = fs::path(file_path).parent_path().string();
                while (dir_path != "/") {
                    if (fs::exists(dir_path) && fs::is_empty(dir_path)) {
                        fs::remove(dir_path);
                        dir_path = fs::path(dir_path).parent_path().string();
                    } else {
                        break;
                    }
                }
            }
        }
        fs::remove(files_list_path);
        std::cout << ">>> 文件移除完成，共移除 " << removed_count << " 个文件" << std::endl;
    }

    // 删除依赖文件
    fs::remove(DEP_DIR + pkg_name);

    // 删除man文档
    fs::remove(DOCS_DIR + pkg_name + ".man");

    // 更新包列表 - 只删除确切的包记录
    pkgs.erase(pkg_record);
    holdpkgs.erase(pkg_name);

    write_set_to_file(PKGS_FILE, pkgs);
    write_set_to_file(HOLDPKGS_FILE, holdpkgs);

    std::cout << ">>> " << pkg_name << " 已成功移除" << std::endl;
}

// 检查包是否是手动安装的
bool is_manually_installed(const std::string& pkg_name) {
    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
    return holdpkgs.find(pkg_name) != holdpkgs.end();
}

// 获取所有必需的包（显式安装的包+被依赖的包）
std::unordered_set<std::string> get_required_packages() {
    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);
    std::unordered_set<std::string> required_pkgs = holdpkgs;

    // 添加所有被依赖的包
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

    std::cout << "正在检查可自动移除的包..." << std::endl;

    // 循环直到找不到可移除的包
    while (found_removable) {
        found_removable = false;
        auto pkgs_current = read_set_from_file(PKGS_FILE);
        auto required_pkgs = get_required_packages();

        for (const auto& pkg : pkgs_current) {
            size_t pos = pkg.find(':');
            if (pos == std::string::npos) continue;

            std::string pkg_name = pkg.substr(0, pos);

            // 跳过已删除的包
            if (removed_pkgs.find(pkg_name) != removed_pkgs.end()) {
                continue;
            }

            // 如果是手动安装的包，跳过
            if (is_manually_installed(pkg_name)) {
                continue;
            }

            // 检查是否被其他包依赖
            bool is_required = (required_pkgs.find(pkg_name) != required_pkgs.end());

            // 如果未被依赖，则安全删除
            if (!is_required) {
                std::cout << "发现可移除包: " << pkg_name
                << " (手动安装: " << (is_manually_installed(pkg_name) ? "是" : "否")
                << ", 被依赖: " << (is_required ? "是" : "否") << ")" << std::endl;
                remove_package(pkg_name);
                removed_pkgs.insert(pkg_name);
                found_removable = true;
                break; // 重新开始循环
            }
        }
    }

    if (removed_pkgs.empty()) {
        std::cout << "没有找到可自动移除的包。" << std::endl;
    } else {
        std::cout << "已自动移除 " << removed_pkgs.size() << " 个包。" << std::endl;
    }

    // 清理临时目录
    fs::remove_all(TMP_DIR);
    ensure_dir_exists(TMP_DIR);
}

// 升级所有包
void upgrade_packages() {
    // 读取镜像配置
    std::ifstream mirror_file(MIRROR_CONF);
    std::string mirror_url;
    if (!std::getline(mirror_file, mirror_url) || mirror_url.empty()) {
        exit_with_error("无效的镜像配置");
    }
    if (mirror_url.back() != '/') mirror_url += '/';

    // 获取架构
    std::string arch = get_architecture();

    // 获取已安装的包
    auto pkgs = read_set_from_file(PKGS_FILE);
    auto holdpkgs = read_set_from_file(HOLDPKGS_FILE);

    std::cout << "正在检查可升级的包..." << std::endl;
    int upgraded_count = 0;

    for (const auto& pkg : pkgs) {
        size_t pos = pkg.find(':');
        if (pos == std::string::npos) continue;

        std::string pkg_name = pkg.substr(0, pos);
        std::string current_version = pkg.substr(pos + 1);

        // 只升级手动安装的包
        if (holdpkgs.find(pkg_name) == holdpkgs.end()) {
            continue;
        }

        // 获取最新版本
        std::string latest_version;
        try {
            latest_version = get_latest_version(mirror_url, arch, pkg_name);
        } catch (...) {
            std::cerr << "警告: 无法获取 " << pkg_name << " 的最新版本，跳过" << std::endl;
            continue;
        }

        // 比较版本
        if (version_compare(current_version, latest_version)) {
            std::cout << "发现可升级包: " << pkg_name
            << " (当前: " << current_version
            << ", 最新: " << latest_version << ")" << std::endl;

            // 先移除旧版本
            remove_package(pkg_name);

            // 安装新版本
            install_package(pkg_name, latest_version, true);
            upgraded_count++;
        }
    }

    if (upgraded_count == 0) {
        std::cout << "所有包都已是最新版本。" << std::endl;
    } else {
        std::cout << "已升级 " << upgraded_count << " 个包。" << std::endl;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "用法: " << argv[0] << " <命令> [参数]\n"
        << "命令:\n"
        << "  install <包名>[:版本] 安装指定包\n"
        << "  remove <包名>        移除指定包\n"
        << "  autoremove           自动移除不再需要的包\n"
        << "  upgrade              升级所有包\n";
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
