#pragma once

#include <filesystem>

/**
 * 临时目录管理器（RAII）
 * 构造时创建临时目录，析构时自动清理
 */
class TmpDirManager
{
public:
    TmpDirManager();
    ~TmpDirManager();
    TmpDirManager(const TmpDirManager&) = delete;
    TmpDirManager& operator=(const TmpDirManager&) = delete;

private:
    std::filesystem::path tmp_dir_path_;  // 临时目录路径
};

/** 清理所有临时目录 */
void cleanup_tmp_dirs();
