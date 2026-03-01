#include <gtest/gtest.h>
#include "../main/src/package_manager.hpp"
#include "../main/src/packer.hpp"
#include "../main/src/hash.hpp"
#include "../main/src/cache.hpp"
#include "../main/src/config.hpp"
#include "../main/src/utils.hpp"
#include "../main/src/localization.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;

class NewFeaturesTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        set_non_interactive_mode(NonInteractiveMode::YES);
        set_testing_mode(true);
        set_force_overwrite_mode(false);
        set_no_hooks_mode(false);
        set_no_deps_mode(false);
        init_localization();
        
        suite_work_dir = fs::absolute("tmp_new_features_test");
        if (fs::exists(suite_work_dir)) {
            std::string clean_cmd = "sudo rm -rf " + suite_work_dir.string();
            std::system(clean_cmd.c_str());
        }
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);
        
        set_root_path(test_root.string());
        set_architecture("x86_64");
        init_filesystem();

        // Setup mock mirror
        fs::path mirror_path = suite_work_dir / "mirror";
        fs::create_directories(mirror_path / "x86_64");
        std::ofstream(test_root / "etc/lpkg/mirror.conf") << "file://" << mirror_path.string() << "/" << std::endl;
        // Create initial empty index
        std::ofstream(mirror_path / "x86_64" / "index.txt").close();
    }

    std::string create_pkg(const std::string& name, const std::string& ver, 
                        const std::vector<std::pair<std::string, std::string>>& files) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name + "_" + ver);
        fs::create_directories(work_dir / "root");
        
        for (const auto& [src, dest] : files) {
            fs::path p = work_dir / "root" / src;
            fs::create_directories(p.parent_path());
            std::ofstream f(p); f << "content of " << src; f.close();
        }

        std::ofstream(work_dir / "man.txt") << "Manual for " << name << "\n";
        std::ofstream(work_dir / "deps.txt").close();

        std::string pkg_filename = name + "-" + ver + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_filename).string();
        pack_package(pkg_path, work_dir.string());

        // Also put it in the mirror
        fs::path mirror_pkg_dir = suite_work_dir / "mirror" / "x86_64" / name;
        fs::create_directories(mirror_pkg_dir);
        // New format: name/version.lpkg
        fs::copy_file(pkg_path, mirror_pkg_dir / (ver + ".lpkg"), fs::copy_options::overwrite_existing);
        
        std::string hash = calculate_sha256(pkg_path);

        // Update index.txt with aggregated format: name|v:h|deps|provides
        std::ofstream index(suite_work_dir / "mirror" / "x86_64" / "index.txt", std::ios::app);
        index << name << "|" << ver << ":" << hash << "||" << std::endl;
        
        fs::remove_all(work_dir);
        return pkg_path;
    }

    void TearDown() override {
        set_root_path("/");
        std::string clean_cmd = "sudo rm -rf " + suite_work_dir.string();
        std::system(clean_cmd.c_str());
    }
};

TEST_F(NewFeaturesTest, QueryFileAndPackage) {
    std::string pkg = create_pkg("query_test", "1.0", {{"usr/bin/query_target", "/"}});
    install_packages({pkg}, "", false);

    // Test query file (absolute)
    testing::internal::CaptureStdout();
    query_file("/usr/bin/query_target");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("query_test"), std::string::npos);

    // Test query file (relative with smart resolution)
    // We simulate being in /usr/bin and querying 'query_target'
    fs::path old_cwd = fs::current_path();
    fs::create_directories(test_root / "usr/bin");
    fs::current_path(test_root / "usr/bin");
    
    testing::internal::CaptureStdout();
    query_file("query_target");
    output = testing::internal::GetCapturedStdout();
    fs::current_path(old_cwd); // Restore CWD
    
    EXPECT_NE(output.find("query_test"), std::string::npos);
    EXPECT_NE(output.find("/usr/bin/query_target"), std::string::npos);

    // Test query package
    testing::internal::CaptureStdout();
    query_package("query_test");
    output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("/usr/bin/query_target"), std::string::npos);
}

TEST_F(NewFeaturesTest, ReinstallPackage) {
    std::string pkg = create_pkg("reinstall_test", "1.0", {{"usr/bin/reinstall_bin", "/"}});
    
    install_packages({pkg}, "", false);

    fs::path bin_path = test_root / "usr/bin/reinstall_bin";
    EXPECT_TRUE(fs::exists(bin_path));

    // Modify the file to see if it gets restored
    {
        std::ofstream f(bin_path);
        f << "modified";
    }

    // Reinstall
    reinstall_package("reinstall_test");

    {
        std::ifstream f(bin_path);
        std::string s;
        std::getline(f, s);
        EXPECT_EQ(s, "content of usr/bin/reinstall_bin");
    }

    // Additional Test: Reinstall via PATH
    {
        std::ofstream f(bin_path);
        f << "modified again";
    }
    reinstall_package(pkg); // Use path instead of name
    {
        std::ifstream f(bin_path);
        std::string s;
        std::getline(f, s);
        EXPECT_EQ(s, "content of usr/bin/reinstall_bin");
    }
}

TEST_F(NewFeaturesTest, ReinstallAtomicRollback) {
    // 1. Install version 1 of a package
    std::string pkg = create_pkg("rollback_test", "1.0", {{"usr/bin/app", "/"}});
    install_packages({pkg}, "", false);
    
    fs::path app_path = test_root / "usr/bin/app";
    ASSERT_TRUE(fs::exists(app_path));

    // 2. Sabotage: Make parent directory read-only to block overwrite
    fs::path bin_dir = test_root / "usr" / "bin";
    fs::permissions(bin_dir, fs::perms::owner_read | fs::perms::owner_exec);

    // 3. Attempt reinstall. It should fail during file copy.
    EXPECT_THROW(reinstall_package(pkg), LpkgException);

    // 4. Unlock for verification and cleanup
    fs::permissions(bin_dir, fs::perms::owner_all);

    // 5. VERIFY: The package should STILL be marked as installed
    EXPECT_EQ(Cache::instance().get_installed_version("rollback_test"), "1.0");
    
    // 6. VERIFY: The file should still exist and contain original content
    EXPECT_TRUE(fs::exists(app_path));
    std::ifstream f(app_path);
    std::string s;
    std::getline(f, s);
    EXPECT_EQ(s, "content of usr/bin/app");
}