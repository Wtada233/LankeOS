#include <gtest/gtest.h>
#include "../main/src/package_manager.hpp"
#include "../main/src/config.hpp"
#include "../main/src/utils.hpp"
#include "../main/src/localization.hpp"
#include "../main/src/constants.hpp"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include "../main/src/packer.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

class PackageManagerTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        set_non_interactive_mode(NonInteractiveMode::YES);
        set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_pm_test");
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);
        
        set_root_path(test_root.string());
        init_filesystem();
    }

    void TearDown() override {
        set_root_path("/"); // Reset
        fs::remove_all(suite_work_dir);
    }

    std::string create_dummy_package(const std::string& name, const std::string& version, const std::vector<std::string>& deps = {}, const std::vector<std::string>& provides = {}) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::create_directories(work_dir / "content" / "usr" / "bin");
        
        // Create a dummy binary
        std::ofstream bin(work_dir / "content" / "usr" / "bin" / "hello");
        bin << "#!/bin/sh\necho Hello\n";
        bin.close();
        
        // Use pack_package to handle metadata consolidation
        std::string pkg_name = name + "-" + version + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_name).string();
        
        pack_package(pkg_path, work_dir.string(), name, version, deps, provides, "Man page for " + name);
        
        fs::remove_all(work_dir);
        return pkg_path;
    }
};

TEST_F(PackageManagerTest, InstallLocalPackage) {
    std::string pkg_file = create_dummy_package("testpkg", "1.0");
    ASSERT_TRUE(fs::exists(pkg_file));

    // Install using absolute path to local file
    std::vector<std::string> args = {pkg_file};
    
    EXPECT_NO_THROW(install_packages(args));

    // Verify installation
    fs::path installed_file = test_root / "usr" / "bin" / "hello";
    EXPECT_TRUE(fs::exists(installed_file));
    
    fs::path db_file = FILES_DB;
    EXPECT_TRUE(fs::exists(db_file));
}

TEST_F(PackageManagerTest, SysrootIsolation) {
    // Ensure that files are NOT installed to real /usr/bin
    std::string pkg_file = create_dummy_package("testpkg", "1.0");
    std::vector<std::string> args = {pkg_file};
    install_packages(args);

    EXPECT_FALSE(fs::exists("/usr/bin/hello")); 
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/hello"));
}

TEST_F(PackageManagerTest, VirtualPackages) {
    // 1. Create a "provider" package (e.g. openssl) that provides "libssl"
    std::string p_prov = create_dummy_package("provider", "1.0", {}, {"libssl"});
    
    // 2. Create a "consumer" package (e.g. curl) that depends on "libssl"
    std::string p_cons = create_dummy_package("consumer", "1.0", {"libssl"});

    // Install provider
    install_packages({p_prov});
    
    install_packages({p_cons});
}

TEST_F(PackageManagerTest, VersionConstraints) {
    // 1. Install lib v1.0
    std::string p_lib1 = create_dummy_package("lib", "1.0");
    install_packages({p_lib1});
    
    // Verify lib is installed by checking the pkgs file manually
    {
        std::ifstream pkgs_file(PKGS_FILE);
        std::string line;
        bool found = false;
        while (std::getline(pkgs_file, line)) {
            if (line == "lib:1.0") {
                found = true;
                break;
            }
        }
        ASSERT_TRUE(found) << "lib:1.0 was not found in pkgs file after installation";
    }
    
    // 2. Try to install app requiring lib >= 2.0 (Should Fail)
    std::string p_bad = create_dummy_package("app_bad", "1.0", {"lib >= 2.0"});
    
    EXPECT_THROW(install_packages({p_bad}), LpkgException); 

    // 3. Try to install app requiring lib < 2.0 (Should Succeed)
    std::string p_good = create_dummy_package("app_good", "1.0", {"lib < 2.0"});

    EXPECT_NO_THROW(install_packages({p_good}));
}

TEST_F(PackageManagerTest, AutoremoveWithVirtualPackages) {
    // 1. Create provider package 'openssl' providing 'libssl'
    std::string p_ossl = create_dummy_package("openssl", "1.0", {}, {"libssl"});

    // 2. Create consumer package 'curl' depending on 'libssl'
    std::string p_curl = create_dummy_package("curl", "1.0", {"libssl"});

    // 3. Install curl (will pull openssl as a dependency)
    install_packages({p_ossl});
    install_packages({p_curl});

    // 4. Verify both are installed
    {
        std::ifstream pkgs_file(PKGS_FILE);
        std::string line;
        int count = 0;
        while (std::getline(pkgs_file, line)) {
            if (line.starts_with("openssl:") || line.starts_with("curl:")) count++;
        }
        ASSERT_EQ(count, 2);
    }

    // 5. Run autoremove
    autoremove();
    write_cache();

    // 6. Verify openssl is still there
    {
        std::ifstream pkgs_file(PKGS_FILE);
        std::string line;
        bool found_openssl = false;
        while (std::getline(pkgs_file, line)) {
            if (line.starts_with("openssl:")) found_openssl = true;
        }
        EXPECT_TRUE(found_openssl) << "openssl was incorrectly autoremoved!";
    }
}
