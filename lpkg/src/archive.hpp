#ifndef ARCHIVE_HPP
#define ARCHIVE_HPP

#include <string>

bool extract_tar_zst(const std::string& archive_path, const std::string& output_dir);

#endif // ARCHIVE_HPP
