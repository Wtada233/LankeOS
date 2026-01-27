#include <gtest/gtest.h>
#include "../main/src/config.hpp"
#include "../main/src/repository.hpp"
#include "../main/src/exception.hpp"
#include "../main/src/utils.hpp"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

class IntegrationV2Test : public ::testing::Test {
protected:
    fs::path test_root;
    fs::path mirror_dir;

    void SetUp() override {
        test_root = fs::absolute("test_env_v2");
        mirror_dir = test_root / "mirror";
        fs::create_directories(test_root);
        fs::create_directories(mirror_dir);
        
        set_root_path(test_root.string());
        set_testing_mode(true);
        
        // Setup a mock mirror config pointing to local dir
        fs::create_directories(test_root / "etc" / "lpkg");
        std::ofstream mirror_conf(test_root / "etc" / "lpkg" / "mirror.conf");
        mirror_conf << "file://" << mirror_dir.string() << "/" << std::endl;
        mirror_conf.close();
    }

    void TearDown() override {
        set_root_path("/");
        fs::remove_all(test_root);
    }
};

TEST_F(IntegrationV2Test, ArchitectureOverride) {
    // Default behavior (depends on host, but let's assume it doesn't throw)
    EXPECT_NO_THROW(get_architecture());

    // Override
    set_architecture("riscv64");
    EXPECT_EQ(get_architecture(), "riscv64");

    set_architecture("arm64");
    EXPECT_EQ(get_architecture(), "arm64");
}

TEST_F(IntegrationV2Test, RepositoryIndexLoading) {
    set_architecture("amd64");
    fs::path arch_dir = mirror_dir / "amd64";
    fs::create_directories(arch_dir);

    // Create a dummy index file
    // Format: name|version|hash|deps|provides
    std::ofstream index(arch_dir / "index.txt");
    index << "libfoo|1.0.0|hash123||\n";
    index << "appbar|2.0.0|hash456|libfoo>=1.0.0|\n";
    index.close();

    Repository repo;
    // Should load from the configured mirror
    EXPECT_NO_THROW(repo.load_index());

    auto pkg_lib = repo.find_package("libfoo");
    ASSERT_TRUE(pkg_lib.has_value());
    EXPECT_EQ(pkg_lib->version, "1.0.0");
    EXPECT_EQ(pkg_lib->sha256, "hash123");

    auto pkg_app = repo.find_package("appbar");
    ASSERT_TRUE(pkg_app.has_value());
    EXPECT_EQ(pkg_app->dependencies.size(), 1);
    EXPECT_EQ(pkg_app->dependencies[0].name, "libfoo");
    EXPECT_EQ(pkg_app->dependencies[0].version_req, "1.0.0");
    EXPECT_EQ(pkg_app->dependencies[0].op, ">=");
}
