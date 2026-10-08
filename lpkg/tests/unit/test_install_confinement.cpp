/**
 * test_install_confinement.cpp — 落位目标的**祖先链约束**与路径判据的边角
 *
 * 钉的是这条隔离缺陷：`--root <R>` 下 `root_dir()/rel` 只约束
 * **词法**归属，而拷贝/让开都会**跟随中间段符号链接** —— 只要有包发过 `content/usr -> <外部>`
 * （绝对目标链接，成员名消毒**有意**放行），后续任何含 `usr/...` 的包都会被写到 root 之外，
 * `--no-hooks` 也挡不住（那条走的是文件拷贝）。
 *
 * 本文件只测**判据本身**（纯函数级、快、可复跑）。为什么不在集成层测：要让"落位"真的走到
 * 那一步，得先让冲突预检放行"盘上有个不属于任何包的 `usr` 符号链接"这种前置状态 —— 预检
 * 自己就会先拒掉它，于是集成用例即便绿了也分不清是哪一道闸拦下的。判据单独拎出来钉死，才是
 * "它不失效"的那个保证。
 */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <string>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/pkg/install_common.hpp"

namespace fs = std::filesystem;

namespace
{

/// 每个测试进程独占一份沙箱（名字带 PID）—— 两个进程并发跑时不会互删（见 test_base.hpp）。
fs::path make_sandbox(const char* name)
{
    const fs::path base = fs::absolute(std::string(name) + "_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

class InstallConfinementTest : public ::testing::Test
{
protected:
    fs::path base;     ///< <沙箱>
    fs::path root;     ///< <沙箱>/root —— 充当 `--root`
    fs::path outside;  ///< <沙箱>/outside —— 模拟 root 之外的宿主目录

    void SetUp() override
    {
        base = make_sandbox("tmp_install_confinement_test");
        root = base / "root";
        outside = base / "outside";
        fs::create_directories(root / "usr" / "lib");
        fs::create_directories(outside);
        Config::instance().set_root_path(root.string());
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(base);
    }
};

}  // namespace

TEST_F(InstallConfinementTest, PlainPathIsJoinedUnderRoot)
{
    // 最普通的一格：祖先链全在 root 内 → 原样返回（不解析、不改写调用方给的路径）。
    EXPECT_EQ(detail::confine_target_path("usr/bin/x"), root / "usr/bin/x");
}

TEST_F(InstallConfinementTest, InRootSymlinkAncestorIsStillAllowed)
{
    // usr-merge 形态：`<root>/lib -> usr/lib`（**相对**链接，解析后仍在 root 内）。
    // 这正是"要允许写穿的那种目录链接"—— 修这一条缺陷时**不能**把 usr-merge 一起禁掉。
    std::error_code ec;
    fs::create_directory_symlink("usr/lib", root / "lib", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(detail::confine_target_path("lib/x"), root / "lib/x");
}

TEST_F(InstallConfinementTest, AncestorSymlinkLeavingRootIsRefused)
{
    // 攻击/事故形态：`<root>/usr -> <root 之外>` —— 后续包的 `usr/...` 会被写到 root 外面。
    std::error_code ec;
    fs::remove_all(root / "usr", ec);
    fs::create_directory_symlink(outside, root / "usr", ec);
    ASSERT_FALSE(ec) << ec.message();

    EXPECT_THROW(detail::confine_target_path("usr/bin/x"), LpkgException);

    // 深一层同样拦：穿透发生在**更靠上**的祖先段时，判据不能只看紧邻的父目录。
    fs::create_directories(root / "deep");
    fs::create_directory_symlink(outside, root / "deep/out", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_THROW(detail::confine_target_path("deep/out/y"), LpkgException);
}

TEST_F(InstallConfinementTest, RefusalNamesTheOffendingPathAndTheRoot)
{
    // 报错必须能定位（本仓库的硬要求）：点名**相对路径**与**目标 root**，
    // 否则用户拿到的是一句"拒绝安装"却不知道该改哪个包 / 哪个 root。
    std::error_code ec;
    fs::remove_all(root / "usr", ec);
    fs::create_directory_symlink(outside, root / "usr", ec);
    ASSERT_FALSE(ec) << ec.message();

    try {
        detail::confine_target_path("usr/bin/x");
        FAIL() << "越界必须抛异常";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("usr/bin/x"), std::string::npos) << msg;
        EXPECT_NE(msg.find(root.string()), std::string::npos) << msg;
    }
}

TEST_F(InstallConfinementTest, ProductionRootSkipsTheWholeCheck)
{
    // `root_dir() == "/"`（常规安装，不带 --root）：任何绝对路径都在其内 —— 恒成立、
    // 不做任何 stat/canonical（这条是热路径）。同时也保证这一层**不会**在生产路径上触发。
    Config::instance().set_root_path("/");
    EXPECT_EQ(detail::confine_target_path("usr/bin/x"), fs::path("/usr/bin/x"));
}

TEST_F(InstallConfinementTest, LastSegmentSymlinkIsNotResolved)
{
    // 末段**不解析**：落位/让开/rename 都不跟随紧邻的那一段。包里合法的绝对目标链接
    // （`<root>/usr/bin/foo -> /etc/foo`）必须照常放行 —— 解析它会误伤整类包。
    std::error_code ec;
    fs::create_directories(root / "usr/bin");
    fs::create_symlink("/etc/foo", root / "usr/bin/foo", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(detail::confine_target_path("usr/bin/foo"), root / "usr/bin/foo");
}

// ---------------------------------------------------------------------------
// `path_within_resolved`：confinement 判据本体（回滚侧与安装侧**共用**的唯一实现）
// ---------------------------------------------------------------------------

TEST(PathWithinResolvedTest, SiblingPrefixIsNotWithin)
{
    // 分量级比较：`/a/bc` **不**以 `/a/b` 为前缀（字符串前缀写法会把它判成"在根内"）。
    EXPECT_FALSE(path_within_resolved("/a/bc", "/a/b"));
    EXPECT_TRUE(path_within_resolved("/a/b/c", "/a/b"));
}

TEST(PathWithinResolvedTest, TrailingSlashOnRootIsEquivalent)
{
    // `--root /mnt/base/`（带尾斜杠）必须与不带时行为一致：否则整批路径都会被判越界跳过。
    EXPECT_TRUE(path_within_resolved("/mnt/base/usr/x", "/mnt/base/"));
    EXPECT_TRUE(path_within_resolved("/mnt/base/usr/x", "/mnt/base"));
}

TEST(PathWithinResolvedTest, EmptyOperandsPass)
{
    // 空路径/空 root 的语义由调用方处理（WAL 侧历史上就是"放行"）—— 这里要求不放行才是错的。
    EXPECT_TRUE(path_within_resolved("", "/a"));
    EXPECT_TRUE(path_within_resolved("/a/b", ""));
}

TEST(PathWithinResolvedTest, SymlinkedAncestorLeavingRootIsNotWithin)
{
    // 词法上在 root 内、实际却穿透出去：`<root>/evil -> <root 之外>`。
    const fs::path base = make_sandbox("tmp_path_within_resolved_test");
    const fs::path root = base / "root";
    const fs::path outside = base / "outside";
    fs::create_directories(root);
    fs::create_directories(outside);
    std::error_code ec;
    fs::create_directory_symlink(outside, root / "evil", ec);
    ASSERT_FALSE(ec) << ec.message();

    EXPECT_FALSE(path_within_resolved(root / "evil/shadow", root));
    fs::remove_all(base);
}

TEST(PathWithinResolvedTest, SymlinkInLastSegmentIsNotResolved)
{
    // 末段指向外面**不算**越界：那一段是要处置的对象本身（rename/unlink 不跟随它）。
    const fs::path base = make_sandbox("tmp_path_within_resolved_last_test");
    const fs::path root = base / "root";
    const fs::path outside = base / "outside";
    fs::create_directories(root);
    fs::create_directories(outside);
    std::error_code ec;
    fs::create_symlink(outside, root / "link", ec);
    ASSERT_FALSE(ec) << ec.message();

    EXPECT_TRUE(path_within_resolved(root / "link", root));
    fs::remove_all(base);
}

TEST(PathResolvesWithinTest, FollowsTheLastSegmentToo)
{
    // 与 `path_within_resolved` 的**分工**：那个不解析末段（用于 rename/unlink 的目标名），
    // 这个解析**整条路径** —— 因为它的用途是"要在这个目录里 create_symlink/fs::remove"
    // （典型：`apply_soname_links()` 用 `is_directory_follow` 判定入参，是**跟随**语义）。
    // 若 `<root>/usr/lib` 是一条指向 root 之外的链接，就该被判成"越界"。
    const fs::path base = make_sandbox("tmp_path_resolves_within_test");
    const fs::path root = base / "root";
    const fs::path outside = base / "outside";
    fs::create_directories(root / "usr");
    fs::create_directories(outside);
    std::error_code ec;

    fs::create_directory_symlink(outside, root / "usr" / "lib", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_FALSE(path_resolves_within(root / "usr/lib", root))
        << "末段是逃逸链接 ⇒ 跟随它就会写到 root 之外，必须判越界";

    // 相对链接、解析后仍在 root 内（usr-merge 形态）→ 放行。
    fs::create_directories(root / "usr" / "lib64");
    fs::create_directory_symlink("lib64", root / "usr" / "lib2", ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_TRUE(path_resolves_within(root / "usr/lib2", root));

    // 解不开（不存在）→ 放行（与其余 confinement 同一条纪律：判定不得因"解不开"而拒合法操作）。
    EXPECT_TRUE(path_resolves_within(root / "usr" / "not-there", root));
    // 生产 root：恒真、不付检索代价。
    EXPECT_TRUE(path_resolves_within("/usr/lib", "/"));

    fs::remove_all(base);
}

// ---------------------------------------------------------------------------
// `is_safe_path_component`：包名/版本号是**分帧字符**的唯一入口
// ---------------------------------------------------------------------------

TEST(SafePathComponentTest, RejectsFramingCharacters)
{
    // `,` 会切坏 files.db 的属主集合（`a,b,c`）与索引行的 deps 字段；`|`/`;` 是索引行的
    // 一/二级分隔符；`:` 是 pkgs 集合与版本块的分隔符。任何一条带进去 = 把一条记录重新分帧。
    for (const char* bad : {"coreutils,evil", "a|b", "a;b", "a:b"})
        EXPECT_FALSE(is_safe_path_component(bad)) << bad;
}

TEST(SafePathComponentTest, AcceptsNormalNamesAndVersions)
{
    for (const char* ok : {"coreutils", "1.5.1+5", "kf-karchive", "gcc-13.2.1", "libfoo.so.1"})
        EXPECT_TRUE(is_safe_path_component(ok)) << ok;
    // 依赖串里的逗号（`"cmake >= 3.20, < 4.0"`）**不经过**这个函数 —— 它只用于包名与版本号，
    // 所以拒 `,` 不会误伤 deps（见 `vercmp/dep_parser.cpp` 对逗号的处理）。
    EXPECT_TRUE(is_safe_path_component("cmake"));
}
