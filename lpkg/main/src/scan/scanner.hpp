#pragma once
#include <string>

/// 孤立文件 = 未被任何已安装包拥有的文件。
void scan_orphans(const std::string& scan_root_override = "");