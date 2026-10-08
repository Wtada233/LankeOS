#pragma once

#include <filesystem>

/// RAII：构造时创建临时目录，析构时自动清理。
class TmpDirManager
{
public:
    TmpDirManager();
    ~TmpDirManager();
    TmpDirManager(const TmpDirManager&) = delete;
    TmpDirManager& operator=(const TmpDirManager&) = delete;

private:
    std::filesystem::path tmp_dir_path_;
};

void cleanup_tmp_dirs();
