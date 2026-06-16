#include <gtest/gtest.h>
#include "../main/src/hash.hpp"
#include "../main/src/package_manager.hpp"
#include "../main/src/packer.hpp"
#include "../main/src/config.hpp"
#include "../main/src/utils.hpp"
#include "../main/src/constants.hpp"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include "../main/src/packer.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

class ParamOrderTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        set_non_interactive_mode(NonInteractiveMode::YES);
        set_testing_mode(true);
        
        suite_work_dir = fs::absolute("tmp_order_test");
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);
        
        set_root_path(test_root.string());
        init_filesystem();
    }

    void TearDown() override {
        set_root_path("/");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
    }

    std::string create_pkg(const std::string& name) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::create_directories(work_dir / "root");
        std::ofstream f(work_dir / "root/file"); f << name; f.close();
        
        std::string pkg_filename = name + "-1.0.lpkg";
        std::string pkg_path = (pkg_dir / pkg_filename).string();
        pack_package(pkg_path, work_dir.string(), name, "1.0");
        
        fs::remove_all(work_dir);
        return pkg_path;
    }
};

TEST_F(ParamOrderTest, OrderVariation) {
    std::string pkg = create_pkg("orderpkg");
    std::string actual_hash = calculate_sha256(pkg);
    fs::path hash_file = suite_work_dir / "order.hash";
    std::ofstream hf(hash_file); hf << actual_hash; hf.close();

    EXPECT_NO_THROW(install_packages({pkg}, hash_file.string()));
    
    remove_package("orderpkg", true);
    fs::remove(test_root / "file");

    EXPECT_NO_THROW(install_packages({pkg}, ""));
}

TEST_F(ParamOrderTest, MultiplePackagesWithOneHash) {
    std::string pkg1 = create_pkg("p1");
    std::string pkg2 = create_pkg("p2");
    std::string hash_file = suite_work_dir / "multi.hash";
    std::ofstream hf(hash_file); hf << "invalid-hash"; hf.close();

    EXPECT_THROW(install_packages({pkg1, pkg2}, hash_file), LpkgException);
}
