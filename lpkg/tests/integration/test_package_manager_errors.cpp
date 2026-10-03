#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class PackageManagerEdgeTest : public IntegrationTestBase
{
};

// ── 1. 无效本地包 ──
TEST_F(PackageManagerEdgeTest, InvalidLocalPackageIsRejectedNotSkipped)
{
    // 创建一个损坏的 .lpkg（不是有效的 tar.zst）
    fs::path bad_pkg = pkg_dir / "corrupt-1.0.lpkg";
    {
        std::ofstream f(bad_pkg, std::ios::binary);
        f << "this is not a valid package file";
    }

    // 原用例名是 `...Skipped`、断言 `EXPECT_NO_THROW` —— 那是在**pin 缺陷**：参数被静默丢掉，
    // 然后走到"所有包都已安装"分支 ⇒ `lpkg install ./corrupt.lpkg` 打一行错误再**退出码 0**，
    // 用户以为装上了。**2026-10-02 修**：点名拒绝（异常 → `Error:` + 非零退出码）。
    // pacman 对找不到/装不了的目标同样是中止，不是跳过。
    bool threw = false;
    try {
        install_packages({bad_pkg.string()});
    } catch (const LpkgException& e) {
        threw = true;
        EXPECT_NE(std::string(e.what()).find("corrupt-1.0.lpkg"), std::string::npos)
            << "报错必须点名是哪个文件：\n"
            << e.what();
    }
    EXPECT_TRUE(threw) << "损坏的本地包必须被拒绝，而不是静默跳过";
}

// ── 2. 不存在的本地包路径 ──
TEST_F(PackageManagerEdgeTest, NonExistentLocalPathIsRejected)
{
    fs::path missing = pkg_dir / "nonexistent-1.0.lpkg";
    bool threw = false;
    try {
        install_packages({missing.string()});
    } catch (const LpkgException& e) {
        threw = true;
        EXPECT_NE(std::string(e.what()).find("nonexistent-1.0.lpkg"), std::string::npos)
            << "报错必须点名路径：\n"
            << e.what();
    }
    EXPECT_TRUE(threw) << "点名了却不存在的本地包必须被拒绝（旧行为是静默跳过 + 退出码 0）";
}

// ── 3. Hash 文件内容为空 → read_hash_failed ──
TEST_F(PackageManagerEdgeTest, EmptyHashFileFails)
{
    fs::path hash_file = suite_work_dir / "empty.sha256";
    {
        std::ofstream f(hash_file);
    }

    // 带本地包 + 空 hash 文件。报错必须点名是哪个 hash 文件（与上面两条同一锚点纪律）。
    std::string pkg_path = create_pkg("hash-test", "1.0");
    bool threw = false;
    try {
        install_packages({pkg_path}, hash_file.string());
    } catch (const LpkgException& e) {
        threw = true;
        EXPECT_NE(std::string(e.what()).find(hash_file.filename().string()), std::string::npos)
            << "报错必须点名 hash 文件：\n"
            << e.what();
    }
    EXPECT_TRUE(threw) << "空 hash 文件必须被拒绝";
}

// ── 4. 本地包 + hash 参数 ──
TEST_F(PackageManagerEdgeTest, HashRequiresLocalPath)
{
    fs::path hash_file = suite_work_dir / "dummy.sha256";
    {
        std::ofstream f(hash_file);
        f << "deadbeef";
    }

    // 不带本地包，只有 hash → 应报错
    EXPECT_THROW(install_packages({"some-remote-pkg"}, hash_file.string()), LpkgException);
}

// ── 5. pkg:version 格式 ──
TEST_F(PackageManagerEdgeTest, PackageVersionFormat)
{
    // 创建一个包并加入镜像
    std::string pkg_path = create_pkg("ver-pkg", "2.0.0");

    // 创建本地镜像索引（repo 用 mirror/arch/pkg_name/version.lpkg）
    fs::path mirror_dir = suite_work_dir / "mirror" / "x86_64";
    fs::path pkg_mirror = mirror_dir / "ver-pkg";
    fs::create_directories(pkg_mirror);
    fs::copy(pkg_path, pkg_mirror / "2.0.0.lpkg");

    // 写入 index.txt（hash 留空，跳过哈希校验）
    {
        std::ofstream idx(mirror_dir / "index.txt");
        idx << "ver-pkg|2.0.0:::|\n";
    }

    // 写入 mirror.conf
    {
        std::ofstream mc(Config::instance().mirror_conf());
        mc << "file://" << (suite_work_dir / "mirror").string() << "/\n";
    }

    // 使用 pkg:version 格式安装应不崩溃
    EXPECT_NO_THROW(install_packages({"ver-pkg:2.0.0"}));
    EXPECT_TRUE(Cache::instance().is_installed("ver-pkg"));
}

// ── 6. 用户确认拒绝 → 安装中止（**取消**语义：抛 UserAbort，而不是"正常返回"）──
TEST_F(PackageManagerEdgeTest, NonInteractiveModeNoAborts)
{
    Config::instance().set_non_interactive_mode(NonInteractiveMode::NO);
    std::string pkg_path = create_pkg("no-install", "1.0");

    // 2026-10-03 订正：这里原先断言 `EXPECT_NO_THROW` —— 那是**弱断言**（"取消"与"装好了"
    // 都成立），也正是"取消后仍报完成、退出码 0"那个缺陷能存活至今的原因。现在取消要么抛
    // `UserAbort`（库层判据），要么由 `run_cli` 翻成**非零退出码**。
    EXPECT_THROW(install_packages({pkg_path}), UserAbort);
    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("no-install")) << "取消 = 什么都没做";
    Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
}

// ── 7. 版本约束安装（>= / <）──
TEST_F(PackageManagerEdgeTest, VersionConstraintInstall)
{
    fs::path mirror_dir = suite_work_dir / "mirror" / "x86_64";
    fs::path pkg_mirror = mirror_dir / "constraint-pkg";
    fs::create_directories(pkg_mirror);

    create_pkg("constraint-pkg", "1.0");
    create_pkg("constraint-pkg", "2.0");
    create_pkg("constraint-pkg", "3.0");

    fs::copy(pkg_dir / "constraint-pkg-1.0.lpkg", pkg_mirror / "1.0.lpkg");
    fs::copy(pkg_dir / "constraint-pkg-2.0.lpkg", pkg_mirror / "2.0.lpkg");
    fs::copy(pkg_dir / "constraint-pkg-3.0.lpkg", pkg_mirror / "3.0.lpkg");

    {
        std::ofstream idx(mirror_dir / "index.txt");
        idx << "constraint-pkg|1.0:::;2.0:::;3.0:::|\n";
    }
    {
        std::ofstream mc(Config::instance().mirror_conf());
        mc << "file://" << (suite_work_dir / "mirror").string() << "/\n";
    }

    EXPECT_NO_THROW(install_packages({"constraint-pkg:2.0"}));
    auto ver = Cache::instance().get_installed_version("constraint-pkg");
    EXPECT_EQ(ver, "2.0");
}

// ── 8. 损坏的仓库索引 → 不崩溃 ──
TEST_F(PackageManagerEdgeTest, CorruptRepoIndex)
{
    fs::path mirror_dir = suite_work_dir / "mirror" / "x86_64";
    fs::create_directories(mirror_dir);
    {
        std::ofstream idx(mirror_dir / "index.txt");
        idx << "this is not a valid index format\n";
    }
    {
        std::ofstream mc(Config::instance().mirror_conf());
        mc << "file://" << (suite_work_dir / "mirror").string() << "/\n";
    }

    // 损坏的索引不应导致崩溃（会记录 warning）
    std::string pkg_path = create_pkg("standalone", "1.0");
    EXPECT_NO_THROW(install_packages({pkg_path}));
}

// ── 9. 依赖不满足的安装（缺少依赖）──
TEST_F(PackageManagerEdgeTest, InstallWithMissingDep)
{
    create_pkg("app", "1.0", {"missing-dep"});
    std::string pkg_path = (pkg_dir / "app-1.0.lpkg").string();

    // 应因缺少依赖而抛出异常
    EXPECT_THROW(install_packages({pkg_path}), LpkgException);
}
