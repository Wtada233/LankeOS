#include <gtest/gtest.h>
#include "../main/src/hash.hpp"
#include "../main/src/package_manager.hpp"
#include "../main/src/config.hpp"
#include "../main/src/utils.hpp"
#include "../main/src/localization.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class HashTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        set_non_interactive_mode(NonInteractiveMode::YES);
        set_testing_mode(true);
        init_localization();
        
        suite_work_dir = fs::absolute("tmp_hash_test");
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

    std::string create_dummy_file(const std::string& name, const std::string& content) {
        fs::path p = suite_work_dir / name;
        std::ofstream f(p);
        f << content;
        return p.string();
    }

    std::string create_pkg(const std::string& name, const std::string& ver) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::create_directories(work_dir / "content");
        std::ofstream f(work_dir / "content/dummy"); f << "data"; f.close();
        std::ofstream deps(work_dir / "deps.txt"); deps.close();
        std::ofstream files(work_dir / "files.txt"); files << "dummy /\n"; files.close();
        std::ofstream ml(work_dir / "man.txt"); ml << "man\n"; ml.close();

        std::string pkg_filename = name + "-" + ver + ".tar.zst";
        std::string pkg_path = (pkg_dir / pkg_filename).string();
        std::string cmd = "tar --zstd -cf " + pkg_path + " -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
        return pkg_path;
    }
};

TEST_F(HashTest, CalculateSHA256) {
    std::string path = create_dummy_file("test.txt", "hello world");
    // echo -n "hello world" | sha256sum
    // b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
    std::string hash = calculate_sha256(path);
    EXPECT_EQ(hash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST_F(HashTest, InstallWithCorrectHash) {
    std::string pkg = create_pkg("testhash", "1.0");
    std::string actual_hash = calculate_sha256(pkg);
    std::string hash_file = create_dummy_file("correct.hash", actual_hash);

    EXPECT_NO_THROW(install_packages({pkg}, hash_file));
    EXPECT_TRUE(fs::exists(test_root / "dummy"));
}

TEST_F(HashTest, InstallWithIncorrectHash) {
    std::string pkg = create_pkg("testhash_bad", "1.0");
    std::string hash_file = create_dummy_file("wrong.hash", "wronghashvalue");

    EXPECT_THROW(install_packages({pkg}, hash_file), LpkgException);
}

TEST_F(HashTest, HashParamOnlyForLocal) {
    std::string hash_file = create_dummy_file("any.hash", "somehash");
    EXPECT_THROW(install_packages({"some-remote-pkg"}, hash_file), LpkgException);
}