/**
 * test_path_safety.cpp — L3 回归测试
 *
 * L3: unique_bak_path 收敛到 utils 共享实现（去重），随机后缀 + 尝试上限。
 * 曾有两份复制（package_manager.cpp / installation_task.cpp），且都是无上限
 * while(true)。共享实现改用 UNIQUE_BAK_MAX_ATTEMPTS 上限，冲突即抛异常。
 */

#include <gtest/gtest.h>

#include <filesystem>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"

namespace fs = std::filesystem;

TEST(PathSafetyTest, UniqueBakPathReturnsNonExistingSuffixedPath)
{
    const fs::path phys = "/usr/bin/foo";
    const fs::path bak = unique_bak_path(phys, "mypkg");

    EXPECT_FALSE(fs::exists(bak));
    const std::string s = bak.string();
    // 命名规则：<原路径>.lpkg_bak_<包名>_<随机后缀>
    EXPECT_NE(s.find(".lpkg_bak_mypkg_"), std::string::npos);
    EXPECT_TRUE(s.starts_with(phys.string()));
}

TEST(PathSafetyTest, UniqueBakPathHandlesTrailingSlashDir)
{
    const fs::path phys = "/usr/share/doc/foo/";
    const fs::path bak = unique_bak_path(phys, "foo");
    // 尾部斜杠被去除，后缀追加到目录名而不是根
    EXPECT_TRUE(bak.string().starts_with("/usr/share/doc/foo"));
    EXPECT_FALSE(bak.string().ends_with("//"));
    EXPECT_FALSE(fs::exists(bak));
}
