/**
 * test_path_safety.cpp — 备份目标路径安全（stash 时代，替代原 unique_bak_path 契约）
 *
 * 原 L3 回归：备份名共享实现 + 随机后缀 + 尝试上限 + 目录尾斜杠处理。
 * stash 重构后备份不再原位生成，改由 detail::stash_bak_target 在**每文件系统 stash**
 * （<root>/.lpkg_bak_<pkg>_<pid>）里分配扁平文件名 <basename>.lpkg_bak_<pkg>_<rand>。
 * 本文件 pin 新契约的路径安全性质：
 *   - 结果必须落在该 phys 的 stash 目录内（同设备、不出 root_dir）；
 *   - 文件名 = 原名 + `.lpkg_bak_<pkg>_` + 随机后缀，不带任何原路径成分（basename 之外
 *     不泄露目录）；同 basename 不同目录靠随机后缀在共享 stash 内唯一；
 *   - 返回不存在的路径。
 *   （注：stash_bak_target 只服务**文件**；目录删除走 DIR_RM，不生成备份名——
 *     旧 unique_bak_path 对尾斜杠目录的 trim 语义已随整目录实体备份一并移除。）
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "../../main/src/base/utils.hpp"
#include "../../main/src/pkg/install_common.hpp"

namespace fs = std::filesystem;

class PathSafetyTest : public ::testing::Test
{
protected:
    fs::path root;

    void SetUp() override
    {
        root = fs::absolute("tmp_path_safety");
        if (fs::exists(root)) fs::remove_all(root);
        fs::create_directories(root);
        Config::instance().set_root_path(root.string());
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        if (fs::exists(root)) fs::remove_all(root);
    }
};

TEST_F(PathSafetyTest, StashBakTargetFlatUniqueInsideStashDir)
{
    const fs::path phys = root / "usr" / "bin" / "foo";
    fs::create_directories(phys.parent_path());

    const fs::path bak = detail::stash_bak_target(phys, "mypkg");

    // 落在 stash 目录内：父目录名 = <fsroot>/.lpkg_bak_<pkg>_<pid>
    const fs::path stash_dir = bak.parent_path();
    EXPECT_TRUE(stash_dir.filename().string().rfind(".lpkg_bak_mypkg_", 0) == 0)
        << "bak 必须在 stash 目录里，got parent: " << stash_dir;
    EXPECT_TRUE(detail::stash_parent_dir(phys) == stash_dir.parent_path())
        << "stash 必须直接挂在同设备顶层（phys 的 stash_parent）下";

    // 扁平命名：文件名 = <原名>.lpkg_bak_<pkg>_<rand>，不带原路径成分
    const std::string fn = bak.filename().string();
    EXPECT_TRUE(fn.rfind("foo.lpkg_bak_mypkg_", 0) == 0) << "got: " << fn;

    EXPECT_FALSE(fs::exists(bak)) << "分配的目标路径必须尚不存在";
}

TEST_F(PathSafetyTest, StashBakTargetUniqueAcrossDirsNoPathLeak)
{
    // 同 basename 的两个不同目录文件 → 扁平进同一 stash、结果互异、文件名不含原目录
    // （stash_bak_target 前置：文件本体存在——生产只在搬已存在文件时调用）
    const fs::path a = root / "usr" / "lib" / "foo";
    const fs::path b = root / "usr" / "share" / "foo";
    fs::create_directories(a.parent_path());
    fs::create_directories(b.parent_path());
    std::ofstream(a) << "a";
    std::ofstream(b) << "b";

    const fs::path bak_a = detail::stash_bak_target(a, "pkg");
    const fs::path bak_b = detail::stash_bak_target(b, "pkg");

    EXPECT_NE(bak_a, bak_b) << "同 basename 必须分配不同 bak（扁平防撞靠随机后缀）";
    EXPECT_TRUE(bak_a.filename().string().rfind("foo.lpkg_bak_pkg_", 0) == 0);
    EXPECT_TRUE(bak_b.filename().string().rfind("foo.lpkg_bak_pkg_", 0) == 0);
    // 文件名不含任何原路径成分（basename 之外无目录名混入）
    EXPECT_EQ(bak_a.filename().string().find('/'), std::string::npos);
    // 都在同一个 stash 目录（同设备顶层），不出 root_dir
    EXPECT_EQ(bak_a.parent_path(), bak_b.parent_path());
    EXPECT_EQ(bak_a.parent_path().parent_path().lexically_normal(), root.lexically_normal());
    EXPECT_FALSE(fs::exists(bak_a));
    EXPECT_FALSE(fs::exists(bak_b));
}
