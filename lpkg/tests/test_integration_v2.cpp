#include <gtest/gtest.h>
#include "repository.hpp"
#include "config.hpp"
#include "utils.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class IntegrationV2Test : public ::testing::Test {
protected:
    void SetUp() override {
        suite_work_dir = fs::current_path() / "tmp_int_v2_test";
        fs::remove_all(suite_work_dir);
        fs::create_directories(suite_work_dir);
        
        mirror_dir = suite_work_dir / "mirror";
        fs::create_directories(mirror_dir);

        set_testing_mode(true);
    }

    void TearDown() override {
        set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    fs::path suite_work_dir;
    fs::path mirror_dir;
};

TEST_F(IntegrationV2Test, ArchitectureOverride) {
    set_architecture("riscv64");
    EXPECT_EQ(get_architecture(), "riscv64");
    
    set_architecture("aarch64");
    EXPECT_EQ(get_architecture(), "aarch64");
    
    set_architecture(""); // Reset
    EXPECT_NO_THROW(get_architecture());
}

TEST_F(IntegrationV2Test, RepositoryIndexLoading) {
    std::string arch = get_architecture();
    fs::path arch_dir = mirror_dir / arch;
    fs::create_directories(arch_dir);

    // Create a dummy index file in aggregated format: name|v1:h1,v2:h2|deps|provides
    std::ofstream index(arch_dir / "index.txt");
    index << "libfoo|1.0.0:hash123||\n";
    index << "app|1.0.0:hash456|libfoo>=1.0.0|\n";
    index.close();

    // Configure mirror.conf to point to our local directory
    set_root_path(suite_work_dir.string());
    ensure_dir_exists(CONFIG_DIR);
    {
        std::ofstream f(MIRROR_CONF);
        f << "file://" << mirror_dir.string() << "/\n";
    }

    Repository repo;
    EXPECT_NO_THROW(repo.load_index());

    auto pkg_lib = repo.find_package("libfoo");
    ASSERT_TRUE(pkg_lib.has_value());
    EXPECT_EQ(pkg_lib->version, "1.0.0");
    EXPECT_EQ(pkg_lib->sha256, "hash123");

    auto pkg_app = repo.find_package("app");
    ASSERT_TRUE(pkg_app.has_value());
    EXPECT_EQ(pkg_app->version, "1.0.0");
    ASSERT_EQ(pkg_app->dependencies.size(), 1);
    EXPECT_EQ(pkg_app->dependencies[0].name, "libfoo");
    EXPECT_EQ(pkg_app->dependencies[0].version_req, "1.0.0");
    EXPECT_EQ(pkg_app->dependencies[0].op, ">=");
}
