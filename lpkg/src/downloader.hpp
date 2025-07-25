#ifndef DOWNLOADER_HPP
#define DOWNLOADER_HPP

#include <string>

bool download_file(const std::string& url, const std::string& output_path, bool show_progress = true);

#endif // DOWNLOADER_HPP
