#include "downloader.hpp"
#include "utils.hpp"
#include "localization.hpp"
#include <curl/curl.h>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <memory>

// HTTP download callback function (modern C++)
size_t write_data_cpp(void* ptr, size_t size, size_t nmemb, void* stream) {
    std::ostream* out = static_cast<std::ostream*>(stream);
    size_t bytes = size * nmemb;
    out->write(static_cast<char*>(ptr), bytes);
    return out->good() ? bytes : 0;
}

// Download progress bar callback
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

    std::cout << "\r" << COLOR_GREEN << "==> " << COLOR_WHITE << get_string("info.downloading") << " [";
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) std::cout << "#";
        else if (i == pos) std::cout << ">";
        else std::cout << "-";
    }
    std::cout << "] " << std::fixed << std::setprecision(1) << percentage << "%" << COLOR_RESET << std::flush;

    return 0;
}

// Custom deleter for the CURL handle
struct CurlDeleter {
    void operator()(CURL* curl) const {
        if (curl) {
            curl_easy_cleanup(curl);
        }
    }
};
using CurlHandle = std::unique_ptr<CURL, CurlDeleter>;

bool download_file(const std::string& url, const std::string& output_path, bool show_progress) {
    CurlHandle curl(curl_easy_init());
    if (!curl) {
        return false;
    }

    std::ofstream ofile(output_path, std::ios::binary);
    if (!ofile) {
        return false; // curl handle will be cleaned up automatically
    }

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_data_cpp);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &ofile);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);

    if (show_progress) {
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress_callback);
    } else {
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 1L);
    }

    CURLcode res = curl_easy_perform(curl.get());
    if (show_progress) {
        std::cout << std::endl;
    }
    // ofile is closed by its destructor
    // curl handle is cleaned up by its destructor

    return res == CURLE_OK;
}