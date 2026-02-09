#include <gtest/gtest.h>
#include "../main/src/hash.hpp"
#include "../main/src/package_manager.hpp"
#include "../main/src/config.hpp"
#include "../main/src/utils.hpp"
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

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
        fs::create_directories(work_dir / "content");
        std::ofstream f(work_dir / "content/file"); f << name; f.close();
        std::ofstream deps(work_dir / "deps.txt"); deps.close();
        std::ofstream files(work_dir / "files.txt"); files << "file\t/\n"; files.close();
        std::ofstream ml(work_dir / "man.txt"); ml << "man\n"; ml.close();

        std::string pkg_filename = name + "-1.0.tar.zst";
        std::string pkg_path = (pkg_dir / pkg_filename).string();
        std::string cmd = "tar --zstd -cf " + pkg_path + " -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
        return pkg_path;
    }
};

TEST_F(ParamOrderTest, OrderVariation) {
    std::string pkg = create_pkg("orderpkg");
    std::string actual_hash = calculate_sha256(pkg);
    fs::path hash_file = suite_work_dir / "order.hash";
    std::ofstream hf(hash_file); hf << actual_hash; hf.close();

    // 1. Install with hash provided
    EXPECT_NO_THROW(install_packages({pkg}, hash_file.string()));
    
    // Clean up for next try
    remove_package("orderpkg", true);
    fs::remove(test_root / "file");

    // 2. Install without hash provided
    EXPECT_NO_THROW(install_packages({pkg}, ""));
}

TEST_F(ParamOrderTest, MultiplePackagesWithOneHash) {
    std::string pkg1 = create_pkg("p1");
    std::string pkg2 = create_pkg("p2");
    std::string hash_file = suite_work_dir / "multi.hash";
    std::ofstream hf(hash_file); hf << "invalid-hash"; hf.close();

    // Should fail because hash mismatch for at least the first one
    EXPECT_THROW(install_packages({pkg1, pkg2}, hash_file), LpkgException);
}