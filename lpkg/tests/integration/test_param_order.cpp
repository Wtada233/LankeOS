#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/crypto/hash.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

class ParamOrderTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);

        suite_work_dir = fs::absolute("tmp_order_test_" + std::to_string(::getpid()));
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";

        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
    }

    std::string create_pkg(const std::string& name)
    {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::create_directories(work_dir / "content");
        std::ofstream f(work_dir / "content/file");
        f << name;
        f.close();

        std::string pkg_filename = name + "-1.0.lpkg";
        std::string pkg_path = (pkg_dir / pkg_filename).string();
        pack_package(pkg_path, work_dir.string(), name, "1.0");

        fs::remove_all(work_dir);
        return pkg_path;
    }
};

TEST_F(ParamOrderTest, OrderVariation)
{
    std::string pkg = create_pkg("orderpkg");
    std::string actual_hash = calculate_sha256(pkg);
    fs::path hash_file = suite_work_dir / "order.hash";
    std::ofstream hf(hash_file);
    hf << actual_hash;
    hf.close();

    EXPECT_NO_THROW(install_packages({pkg}, hash_file.string()));

    // 断言"参数顺序正确"必须落到**结果**上（只验不抛是空转：顺序接反也可能不抛）：
    // 第一个参数是包路径、第二个是哈希文件 → 包**真的装上**、盘上文件**真的出现**。
    EXPECT_EQ(Cache::instance().get_installed_version("orderpkg"), "1.0")
        << "带哈希文件（正确哈希）的安装没真正装上";
    EXPECT_TRUE(fs::exists(test_root / "file")) << "包内容（content/file）没落到盘上";

    remove_package("orderpkg", true);
    fs::remove(test_root / "file");
    EXPECT_TRUE(Cache::instance().get_installed_version("orderpkg").empty())
        << "移除后 orderpkg 仍登记在册";

    // 第二个参数为空 = 不做哈希校验，包同样应装上（换一种参数形态）
    EXPECT_NO_THROW(install_packages({pkg}, ""));
    EXPECT_EQ(Cache::instance().get_installed_version("orderpkg"), "1.0")
        << "哈希参数为空时安装没真正装上";
    EXPECT_TRUE(fs::exists(test_root / "file")) << "哈希参数为空时包内容没落到盘上";
}

TEST_F(ParamOrderTest, MultiplePackagesWithOneHash)
{
    std::string pkg1 = create_pkg("p1");
    std::string pkg2 = create_pkg("p2");
    std::string hash_file = suite_work_dir / "multi.hash";
    std::ofstream hf(hash_file);
    hf << "invalid-hash";
    hf.close();

    EXPECT_THROW(install_packages({pkg1, pkg2}, hash_file), LpkgException);
}
