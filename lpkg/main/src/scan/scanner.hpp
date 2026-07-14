#pragma once
#include <string>

/**
 * @brief 扫描并清理孤立文件（未被任何已安装包拥有的文件）
 * @param scan_root_override 可选：指定扫描根目录覆盖默认值
 */
void scan_orphans(const std::string &scan_root_override = "");