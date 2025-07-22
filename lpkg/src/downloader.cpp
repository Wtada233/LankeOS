#include "downloader.hpp"
#include "utils.hpp"
#include <curl/curl.h>
#include <iostream>
#include <iomanip>

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

    const std::string COLOR_GREEN = "\033[1;32m";
    const std::string COLOR_WHITE = "\033[1;37m";
    const std::string COLOR_RESET = "\033[0m";

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

bool download_file(const std::string& url, const std::string& output_path) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    FILE* fp = fopen(output_path.c_str(), "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        return false;
    }

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
            log_sync("HTTPS下载失败，尝试HTTP回退...");
            std::string http_url = "http://" + url.substr(8);
            return download_file(http_url, output_path);
        }
        return false;
    }
    return true;
}
