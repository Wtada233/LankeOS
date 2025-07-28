#include "downloader.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "utils.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <unistd.h>

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
    log_progress(get_string("info.downloading"), percentage);
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

void download_file(const std::string& url, const fs::path& output_path, bool show_progress) {
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
        throw LpkgException(string_format("error.download_failed", url) + ": " + curl_easy_strerror(res));
    }
}

void download_with_retries(const std::string& url, const fs::path& output_path, int max_retries, bool show_progress) {
    for (int i = 0; i < max_retries; ++i) {
        try {
            download_file(url, output_path, show_progress);
            return; // Success
        } catch (const LpkgException& e) {
            fs::remove(output_path); // Clean up failed download
            if (i < max_retries - 1) {
                log_warning(std::string(e.what()) + ". Retrying...");
            } else {
                throw; // Rethrow on last attempt
            }
        }
    }
}