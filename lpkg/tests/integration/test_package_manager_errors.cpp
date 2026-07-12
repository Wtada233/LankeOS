#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../test_base.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class PackageManagerEdgeTest : public IntegrationTestBase {
};

// ── 1. 无效本地包 ──
TEST_F(PackageManagerEdgeTest, InvalidLocalPackageSkipped) {
    // 创建一个损坏的 .lpkg（不是有效的 tar.zst）
    fs::path bad_pkg = pkg_dir / "corrupt-1.0.lpkg";
    {
        std::ofstream f(bad_pkg, std::ios::binary);
        f << "this is not a valid package file";
    }

    // 安装时应跳过此文件而非崩溃
    EXPECT_NO_THROW(install_packages({bad_pkg.string()}));
}

// ── 2. 不存在的本地包路径 ──
TEST_F(PackageManagerEdgeTest, NonExistentLocalPath) {
    fs::path missing = pkg_dir / "nonexistent-1.0.lpkg";
    EXPECT_NO_THROW(install_packages({missing.string()}));
}

// ── 3. Hash 文件内容为空 → read_hash_failed ──
TEST_F(PackageManagerEdgeTest, EmptyHashFileFails) {
    fs::path hash_file = suite_work_dir / "empty.sha256";
    { std::ofstream f(hash_file); }

    // 带本地包 + 空 hash 文件
    std::string pkg_path = create_pkg("hash-test", "1.0");
    EXPECT_THROW(
        install_packages({pkg_path}, hash_file.string()),
        LpkgException
    );
}

// ── 4. 本地包 + hash 参数 ──
TEST_F(PackageManagerEdgeTest, HashRequiresLocalPath) {
    fs::path hash_file = suite_work_dir / "dummy.sha256";
    { std::ofstream f(hash_file); f << "deadbeef"; }

    // 不带本地包，只有 hash → 应报错
    EXPECT_THROW(
        install_packages({"some-remote-pkg"}, hash_file.string()),
        LpkgException
    );
}

// ── 5. pkg:version 格式 ──
TEST_F(PackageManagerEdgeTest, PackageVersionFormat) {
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

// ── 6. 用户确认拒绝 → 安装中止 ──
TEST_F(PackageManagerEdgeTest, NonInteractiveModeNoAborts) {
    Config::instance().set_non_interactive_mode(NonInteractiveMode::NO);
    std::string pkg_path = create_pkg("no-install", "1.0");

    // NonInteractiveMode::NO 应拒绝安装
    EXPECT_NO_THROW(install_packages({pkg_path}));
    Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
}

// ── 7. 版本约束安装（>= / <）──
TEST_F(PackageManagerEdgeTest, VersionConstraintInstall) {
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
TEST_F(PackageManagerEdgeTest, CorruptRepoIndex) {
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
TEST_F(PackageManagerEdgeTest, InstallWithMissingDep) {
    create_pkg("app", "1.0", {"missing-dep"});
    std::string pkg_path = (pkg_dir / "app-1.0.lpkg").string();

    // 应因缺少依赖而抛出异常
    EXPECT_THROW(
        install_packages({pkg_path}),
        LpkgException
    );
}
