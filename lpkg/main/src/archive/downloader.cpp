#include "downloader.hpp"

#include <curl/curl.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>

#include "base/exception.hpp"
#include "base/utils.hpp"
#include "i18n/localization.hpp"

namespace fs = std::filesystem;

/** HTTP 下载回调函数，将 curl 接收到的数据写入 ostream 输出流 */
size_t write_data_cpp(void* ptr, size_t size, size_t nmemb, void* stream)
{
    std::ostream* out = static_cast<std::ostream*>(stream);
    size_t bytes = size * nmemb;
    out->write(static_cast<char*>(ptr), bytes);
    return out->good() ? bytes : 0;
}

/** 下载进度回调函数，计算并显示下载百分比进度 */
int progress_callback([[maybe_unused]] void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                      [[maybe_unused]] curl_off_t ultotal, [[maybe_unused]] curl_off_t ulnow)
{
    if (dltotal <= 0) {
        return 0;
    }
    double percentage = static_cast<double>(dlnow) / static_cast<double>(dltotal) * 100.0;
    log_progress(get_string("info.downloading"), percentage);
    return 0;
}

/** CURL 句柄的自定义删除器，用于智能指针自动清理 */
struct CurlDeleter {
    void operator()(CURL* curl) const
    {
        if (curl) {
            curl_easy_cleanup(curl);
        }
    }
};
using CurlHandle = std::unique_ptr<CURL, CurlDeleter>;

/** 查找系统 CA 证书包路径，依次检测常见发行版的证书文件位置 */
const char* find_ca_bundle()
{
    static constexpr std::array paths = {
        std::string_view{"/etc/ssl/certs/ca-certificates.crt"},  // Debian/Ubuntu/Arch/Alpine
        std::string_view{"/etc/pki/tls/certs/ca-bundle.crt"},    // RHEL/CentOS/Fedora
        std::string_view{"/etc/ssl/ca-bundle.pem"},              // OpenSUSE
        std::string_view{"/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"},  // New
                                                                                // Fedora/RHEL
        std::string_view{"/etc/ssl/cert.pem"}                                   // Others
    };
    for (auto path : paths) {
        if (access(path.data(), R_OK) == 0) {
            return path.data();
        }
    }
    return nullptr;
}

/** 下载单个文件，支持进度条显示和 CA 证书验证，失败时抛出异常 */
void download_file(const std::string& url, const fs::path& output_path, bool show_progress)
{
    CurlHandle curl(curl_easy_init());
    if (!curl) {
        throw LpkgException(string_format("error.download_failed", url));
    }

    std::ofstream ofile(output_path, std::ios::binary);
    if (!ofile) {
        throw LpkgException(string_format("error.create_file_failed", output_path.string()));
    }

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_data_cpp);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &ofile);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);

    // 找不到 CA 证书包时**硬失败**：曾静默把 VERIFYPEER/VERIFYHOST 关掉，等于让包与
    // index.txt 一起落到 MITM 手里（哈希也来自同一个被劫持的索引，防线全失）。
    // 缺 CA 包是可修的部署问题，不该用"降低安全性"来绕过。
    if (const char* ca_path = find_ca_bundle()) {
        curl_easy_setopt(curl.get(), CURLOPT_CAINFO, ca_path);
    } else {
        throw LpkgException(string_format("error.no_ca_bundle", url));
    }

    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, constants::CURL_CONNECT_TIMEOUT_SEC);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT,
                     100L);  // 最低速度限制 100 字节/秒
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME,
                     30L);  // 持续低于最低速度 30 秒则超时

    if (show_progress) {
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress_callback);
    } else {
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 1L);
    }

    CURLcode res = curl_easy_perform(curl.get());
    if (show_progress) {
        if (isatty(STDOUT_FILENO)) {
            std::cout << std::endl;
        }
    }

    if (res != CURLE_OK) {
        throw LpkgException(string_format("error.download_failed", url) + ": " +
                            curl_easy_strerror(res));
    }

    // 本地写盘错误（磁盘满/EIO）只会在 flush/close 时暴露：不检查就会留下被静默
    // 截断的文件，而 download_with_retries 会把它当成"下载成功"（TODO.md B4）。
    ofile.flush();
    if (!ofile) {
        throw LpkgException(string_format("error.create_file_failed", output_path.string()));
    }
    ofile.close();
}

/** 带重试机制的下载函数，最多重试 max_retries 次，每次失败后清理临时文件 */
void download_with_retries(const std::string& url, const fs::path& output_path, int max_retries,
                           bool show_progress)
{
    // max_retries <= 0 曾让循环体一次都不执行 → 静默返回且什么都没下载（调用方以为成功）
    if (max_retries < 1) max_retries = 1;
    for (int i = 0; i < max_retries; ++i) {
        try {
            download_file(url, output_path, show_progress);
            return;
        } catch (const LpkgException& e) {
            fs::remove(output_path);  // 清理失败的下载文件
            if (i < max_retries - 1) {
                log_warning(string_format("info.retrying", e.what()));
            } else {
                throw;  // 最后一次重试失败，向上抛出异常
            }
        }
    }
}
