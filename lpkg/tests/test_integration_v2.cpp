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

        Config::instance().set_testing_mode(true);
    }

    void TearDown() override {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    fs::path suite_work_dir;
    fs::path mirror_dir;
};

TEST_F(IntegrationV2Test, ArchitectureOverride) {
    Config::instance().set_architecture("riscv64");
    EXPECT_EQ(Config::instance().get_architecture(), "riscv64");
    
    Config::instance().set_architecture("aarch64");
    EXPECT_EQ(Config::instance().get_architecture(), "aarch64");
    
    Config::instance().set_architecture(""); // Reset
    EXPECT_NO_THROW(Config::instance().get_architecture());
}

TEST_F(IntegrationV2Test, RepositoryIndexLoading) {
    std::string arch = Config::instance().get_architecture();
    fs::path arch_dir = mirror_dir / arch;
    fs::create_directories(arch_dir);

    // Create a dummy index file in new format: name|v:h:deps|provides
    std::ofstream index(arch_dir / "index.txt");
    index << "libfoo|1.0.0:hash123:||\n";
    index << "app|1.0.0:hash456:libfoo>=1.0.0||\n";
    index.close();

    // Configure mirror.conf to point to our local directory
    Config::instance().set_root_path(suite_work_dir.string());
    ensure_dir_exists(Config::instance().config_dir());
    {
        std::ofstream f(Config::instance().mirror_conf());
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
    ASSERT_EQ(pkg_app->dependencies[0].constraints.size(), 1);
    EXPECT_EQ(pkg_app->dependencies[0].constraints[0].op, ">=");
    EXPECT_EQ(pkg_app->dependencies[0].constraints[0].version, "1.0.0");
}
