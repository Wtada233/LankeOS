/**
 * test_soname_link_refresh.cpp — SONAME 链接“存在 ≠ 正确”回归
 *
 * 缺陷：apply_soname_links 的判据是“链接**存在**就跳过”
 * （`!fs::exists(link_path) && !fs::is_symlink(link_path)`），而 `fs::exists` 会**跟随**
 * 符号链接 —— 悬空链接 exists() 为 false、is_symlink() 为 true，于是“既不建也不修”，
 * 一个悬空的 /usr/lib/libX.so.N 可以永久留着。ldconfig 的语义相反：它按“目录里现在有
 * 哪些库”重建链接，链接指向不存在的文件就纠正过来。
 *
 * 本组直接在 apply_soname_links 这一层覆盖各种形态（真实 ELF：gcc 不可用则跳过）。
 */

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../../main/src/elf/lib_utils.hpp"

namespace fs = std::filesystem;

class SonameLinkRefreshTest : public ::testing::Test
{
protected:
    fs::path dir;

    void SetUp() override
    {
        dir = fs::absolute("tmp_soname_refresh_test");
        if (fs::exists(dir)) fs::remove_all(dir);
        fs::create_directories(dir);
    }

    void TearDown() override
    {
        fs::remove_all(dir);
    }

    /** 造一个带真实 DT_SONAME 的共享库；gcc 不可用返回 false */
    bool make_lib(const std::string& file_name, const std::string& soname, int tag = 1)
    {
        const fs::path src = dir / ("probe" + std::to_string(tag) + ".c");
        std::ofstream(src) << "int probe" << tag << "(void){return " << tag << ";}\n";
        const fs::path out = dir / file_name;
        if (fs::exists(out) || fs::is_symlink(out)) fs::remove(out);
        const std::string cmd = "gcc -shared -fPIC -Wl,-soname," + soname + " -o " + out.string() +
                                " " + src.string() + " 2>/dev/null";
        return std::system(cmd.c_str()) == 0 && fs::exists(out);
    }

    /** 建符号链接（失败直接判测试失败；fs::create_symlink 无返回值，不能直接断言） */
    void make_link(const std::string& target, const fs::path& link)
    {
        fs::create_symlink(target, link);
        ASSERT_TRUE(fs::is_symlink(link));
    }
};

// 悬空链接（指向已被删掉的旧文件名）→ 必须重建到现在这个同 SONAME 的库上
TEST_F(SonameLinkRefreshTest, DanglingLinkIsRepointedToThePresentLibrary)
{
    if (!make_lib("librep.so.1.2.3", "librep.so.1", /*tag=*/1)) GTEST_SKIP() << "gcc 不可用";
    make_link("librep.so.1.2.3", dir / "librep.so.1");
    apply_soname_links(dir);
    ASSERT_EQ(fs::read_symlink(dir / "librep.so.1").string(), "librep.so.1.2.3");

    // 升级：同 SONAME、不同文件名（旧文件被删）
    ASSERT_TRUE(make_lib("librep.so.1.4.5", "librep.so.1", /*tag=*/2));
    ASSERT_TRUE(fs::remove(dir / "librep.so.1.2.3"));
    ASSERT_FALSE(fs::exists(dir / "librep.so.1")) << "前置条件：此刻链接应当是悬空的";

    apply_soname_links(dir);

    EXPECT_TRUE(fs::exists(dir / "librep.so.1"))
        << "悬空链接没有被重建：exists() 跟随符号链接 → 悬空即 false，is_symlink() → true，"
           "旧判据于是永不重建（依赖它的二进制报 cannot open shared object file）";
    EXPECT_EQ(fs::read_symlink(dir / "librep.so.1").string(), "librep.so.1.4.5");
}

// 链接指向存在的**错误**目标（目标 SONAME ≠ 链接名）→ 纠正到正确库文件
TEST_F(SonameLinkRefreshTest, LinkPointingAtWrongSonameIsCorrected)
{
    if (!make_lib("libstale.so.9", "libstale.so.9", /*tag=*/1)) GTEST_SKIP() << "gcc 不可用";
    if (!make_lib("libgood.so.1.0", "libgood.so.1", /*tag=*/2)) GTEST_SKIP() << "gcc 不可用";
    make_link("libstale.so.9", dir / "libgood.so.1");

    apply_soname_links(dir);

    EXPECT_EQ(fs::read_symlink(dir / "libgood.so.1").string(), "libgood.so.1.0")
        << "链接存在但指错了库；只判“存在”就跳过 = 永不纠正（ldconfig 会纠正）";
}

// 悬空且目录里没有任何库提供该 SONAME（= 库被移除了）→ 链接必须被清掉，不能留着悬空
TEST_F(SonameLinkRefreshTest, DanglingLinkWithoutAnyProviderIsPruned)
{
    if (!make_lib("libgone.so.3.4.5", "libgone.so.3", /*tag=*/1)) GTEST_SKIP() << "gcc 不可用";
    const fs::path link = dir / "libgone.so.3";
    make_link("libgone.so.3.4.5", link);
    ASSERT_TRUE(fs::remove(dir / "libgone.so.3.4.5"));

    apply_soname_links(dir);

    EXPECT_FALSE(fs::is_symlink(link))
        << "库已被移除，SONAME 链接仍悬空留在 /usr/lib —— 删库文件后无人清理";
}

// 正确链接不得被无谓地重建（inode 不变 → 没有 remove+create 的抖动）
TEST_F(SonameLinkRefreshTest, CorrectLinkIsLeftUntouched)
{
    if (!make_lib("libkeep.so.1.2.3", "libkeep.so.1", /*tag=*/1)) GTEST_SKIP() << "gcc 不可用";
    const fs::path link = dir / "libkeep.so.1";
    make_link("libkeep.so.1.2.3", link);
    struct stat before{};
    ASSERT_EQ(lstat(link.c_str(), &before), 0);

    apply_soname_links(dir);

    struct stat after{};
    ASSERT_EQ(lstat(link.c_str(), &after), 0);
    EXPECT_EQ(fs::read_symlink(link).string(), "libkeep.so.1.2.3");
    EXPECT_EQ(before.st_ino, after.st_ino) << "正确链接被无谓地删除重建（inode 变了）";
}

// 包自己提供的实体文件（不是符号链接）绝不能被换成链接
TEST_F(SonameLinkRefreshTest, PackageOwnedRegularFileIsNotReplaced)
{
    if (!make_lib("libreal.so.1", "libreal.so.1", /*tag=*/1)) GTEST_SKIP() << "gcc 不可用";
    if (!make_lib("libreal.so.1.2.3", "libreal.so.1", /*tag=*/2)) GTEST_SKIP() << "gcc 不可用";
    const fs::path real = dir / "libreal.so.1";
    struct stat before{};
    ASSERT_EQ(lstat(real.c_str(), &before), 0);

    apply_soname_links(dir);

    EXPECT_FALSE(fs::is_symlink(real)) << "实体文件被换成了符号链接（覆盖包自己的产物）";
    struct stat after{};
    ASSERT_EQ(lstat(real.c_str(), &after), 0);
    EXPECT_EQ(before.st_ino, after.st_ino);
    EXPECT_EQ(get_elf_soname(real), "libreal.so.1");
}

// 包自己提供的**合法**符号链接不得被反复重建
TEST_F(SonameLinkRefreshTest, PackageOwnedCorrectSymlinkIsKept)
{
    if (!make_lib("libown.so.1.2.3", "libown.so.1", /*tag=*/1)) GTEST_SKIP() << "gcc 不可用";
    const fs::path link = dir / "libown.so.1";
    make_link("libown.so.1.2.3", link);
    apply_soname_links(dir);  // 第一轮：建立/确认
    struct stat before{};
    ASSERT_EQ(lstat(link.c_str(), &before), 0);

    apply_soname_links(dir);  // 第二轮：不得重建

    struct stat after{};
    ASSERT_EQ(lstat(link.c_str(), &after), 0);
    EXPECT_EQ(fs::read_symlink(link).string(), "libown.so.1.2.3");
    EXPECT_EQ(before.st_ino, after.st_ino);
}
