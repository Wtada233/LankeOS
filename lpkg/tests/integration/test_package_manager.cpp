#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../test_base.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class PackageManagerTest : public IntegrationTestBase {
};

TEST_F(PackageManagerTest, InstallLocalPackage) {
    std::string pkg_file = create_pkg("testpkg", "1.0");
    ASSERT_TRUE(fs::exists(pkg_file));

    // Install using absolute path to local file
    std::vector<std::string> args = {pkg_file};
    
    EXPECT_NO_THROW(install_packages(args));

    // Verify installation
    fs::path installed_file = test_root / "usr" / "bin" / "testpkg";
    EXPECT_TRUE(fs::exists(installed_file));
    
    fs::path db_file = Config::instance().files_db();
    EXPECT_TRUE(fs::exists(db_file));
}

TEST_F(PackageManagerTest, SysrootIsolation) {
    // Ensure that files are NOT installed to real /usr/bin
    std::string pkg_name = "sysroot_iso_testpkg";
    std::string pkg_file = create_pkg(pkg_name, "1.0");
    std::vector<std::string> args = {pkg_file};
    install_packages(args);

    EXPECT_FALSE(fs::exists("/usr/bin/" + pkg_name)); 
    EXPECT_TRUE(fs::exists(test_root / "usr/bin" / pkg_name));
}

TEST_F(PackageManagerTest, VirtualPackages) {
    // 1. Create a "provider" package (e.g. openssl) that provides "libssl"
    std::string p_prov = create_pkg("provider", "1.0", {}, {"libssl"});
    
    // 2. Create a "consumer" package (e.g. curl) that depends on "libssl"
    std::string p_cons = create_pkg("consumer", "1.0", {"libssl"});

    // Install provider
    install_packages({p_prov});
    
    install_packages({p_cons});
}

TEST_F(PackageManagerTest, VersionConstraints) {
    // 1. Install lib v1.0
    std::string p_lib1 = create_pkg("lib", "1.0");
    install_packages({p_lib1});
    
    // Verify lib is installed by checking the pkgs file manually
    {
        std::ifstream pkgs_file(Config::instance().pkgs_file());
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
    std::string p_bad = create_pkg("app_bad", "1.0", {"lib >= 2.0"});
    
    EXPECT_THROW(install_packages({p_bad}), LpkgException); 

    // 3. Try to install app requiring lib < 2.0 (Should Succeed)
    std::string p_good = create_pkg("app_good", "1.0", {"lib < 2.0"});

    EXPECT_NO_THROW(install_packages({p_good}));
}

TEST_F(PackageManagerTest, AutoremoveWithVirtualPackages) {
    // 1. Create provider package 'openssl' providing 'libssl'
    std::string p_ossl = create_pkg("openssl", "1.0", {}, {"libssl"});

    // 2. Create consumer package 'curl' depending on 'libssl'
    std::string p_curl = create_pkg("curl", "1.0", {"libssl"});

    // 3. Install curl (will pull openssl as a dependency)
    install_packages({p_ossl});
    install_packages({p_curl});

    // 4. Verify both are installed
    {
        std::ifstream pkgs_file(Config::instance().pkgs_file());
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
        std::ifstream pkgs_file(Config::instance().pkgs_file());
        std::string line;
        bool found_openssl = false;
        while (std::getline(pkgs_file, line)) {
            if (line.starts_with("openssl:")) found_openssl = true;
        }
        EXPECT_TRUE(found_openssl) << "openssl was incorrectly autoremoved!";
    }
}
