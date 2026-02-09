#include <gtest/gtest.h>
#include "../main/src/package_manager.hpp"
#include "../main/src/config.hpp"
#include "../main/src/utils.hpp"
#include "../main/src/localization.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;

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

    std::string create_dummy_package(const std::string& name, const std::string& version) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::create_directories(work_dir / "content" / "usr" / "bin");
        
        // Create a dummy binary
        std::ofstream bin(work_dir / "content" / "usr" / "bin" / "hello");
        bin << "#!/bin/sh\necho Hello\n";
        bin.close();
        
        // Metadata
        std::ofstream deps(work_dir / "deps.txt");
        deps.close(); // No deps
        
        std::ofstream files(work_dir / "files.txt");
        files << "usr/bin/hello\t/" << std::endl;
        files.close();

        std::ofstream man(work_dir / "man.txt");
        man << "Man page for " << name << std::endl;
        man.close();

        // Pack it
        std::string pkg_name = name + "-" + version + ".tar.zst";
        std::string pkg_path = (pkg_dir / pkg_name).string();
        std::string cmd = "tar --zstd -cf " + pkg_path + " -C " + work_dir.string() + " .";
        int ret = std::system(cmd.c_str());
        if (ret != 0) throw std::runtime_error("tar failed");
        
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
    std::string p_prov;
    {
        fs::path work_dir = suite_work_dir / "pkg_provider";
        fs::create_directories(work_dir / "content");
        std::ofstream provides(work_dir / "provides.txt");
        provides << "libssl" << std::endl;
        provides.close();
        
        std::ofstream deps(work_dir / "deps.txt"); deps.close();
        std::ofstream files(work_dir / "files.txt"); files.close();
        std::ofstream man(work_dir / "man.txt"); man << "man" << std::endl; man.close();
        
        p_prov = (pkg_dir / "provider-1.0.tar.zst").string();
        std::string cmd = "tar --zstd -cf " + p_prov + " -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }
    
    // 2. Create a "consumer" package (e.g. curl) that depends on "libssl"
    std::string p_cons;
    {
        fs::path work_dir = suite_work_dir / "pkg_consumer";
        fs::create_directories(work_dir / "content");
        
        std::ofstream deps(work_dir / "deps.txt");
        deps << "libssl" << std::endl;
        deps.close();
        
        std::ofstream files(work_dir / "files.txt"); files.close();
        std::ofstream man(work_dir / "man.txt"); man << "man" << std::endl; man.close();
        
        p_cons = (pkg_dir / "consumer-1.0.tar.zst").string();
        std::string cmd = "tar --zstd -cf " + p_cons + " -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }

    // Install provider
    install_packages({p_prov});
    
    // Install consumer. It should check dependency "libssl". 
    // Since "provider" is installed and provides "libssl", dependency check should pass
    // without trying to install "libssl" from remote.
    
    // Note: If dependency resolution failed, it would try to fetch "libssl" and fail (or crash if no net).
    // We are testing local install so it should succeed.
    EXPECT_NO_THROW(install_packages({p_cons}));
}

// Need access to internal force_reload_cache, but it's internal.
// We can just rely on SetUp resetting paths.
// But we need to ensure the Cache singleton is reset.
// Since Cache is a static local in get_cache(), we can't easily reset it.
// However, Cache::load() re-reads from files. 
// If we can trigger load(), we are good.
// In our modified code, resolve_dependencies() calls force_reload_cache() which calls load().
// That might be enough.

// Actually, the issue might be that get_installed_version returns empty because cache wasn't reloaded
// after "lib" was installed.
// In "install_packages", we call write_cache at end.
// But do we ever call load()? Yes, get_cache calls load if !loaded.
// But if loaded is true, it doesn't reload.
// So subsequent install_packages calls reusing the same process (test suite) reuse the old cache 
// which might not have the new package if we didn't update the in-memory cache structure properly.
// Wait, register_package updates in-memory cache (cache.pkgs.insert).
// So it should be fine.

// Let's add a small sleep to ensure filesystem sync? Unlikely to be the issue.

// Re-verify the test logic for VersionConstraints failure.
// app_bad depends on lib >= 2.0.
// lib 1.0 is installed.
// resolve_package_dependencies for app_bad is called.
// It iterates deps. Finds "lib".
// get_installed_version("lib") returns "1.0".
// has_constraint is true. op=">=", req="2.0".
// version_satisfies("1.0", ">=", "2.0") -> false.
// It SHOULD throw.

// Why did "Actual: it throws nothing"?
// It means resolve_package_dependencies completed.
// Which means loop finished.
// Which means either:
// 1. getline failed / loop didn't run.
// 2. ss >> dep_name failed.
// 3. has_constraint was false.
// 4. installed_dep_ver was empty (or "virtual").
// 5. version_satisfies returned true.

// We verified version_satisfies is correct in unit test.
// We verified parsing seems correct in code review.
// Most likely installed_dep_ver was empty.
// Why?
// Because get_installed_version returned empty.
// Why?
// Cache didn't have "lib".
// Why?
// install_packages({"lib-1.0.tar.zst"}) was called just before.
// It should have updated cache.

// Let's verify if register_package was actually called for lib.
// In test_package_manager.cpp, install_packages is called.
// If it succeeded, it means commit -> register_package happened.

// Let's modify the test to verify lib is installed before trying app_bad.

TEST_F(PackageManagerTest, VersionConstraints) {
    // 1. Install lib v1.0
    std::string p_lib1;
    {
        std::string pkg_lib = "lib-1.0.tar.zst";
        p_lib1 = (pkg_dir / pkg_lib).string();
        fs::path work_dir = suite_work_dir / "pkg_lib";
        fs::create_directories(work_dir / "content");
        std::ofstream deps(work_dir / "deps.txt"); deps.close();
        std::ofstream files(work_dir / "files.txt"); files.close();
        std::ofstream man(work_dir / "man.txt"); man << "man" << std::endl; man.close();
        
        std::string cmd = "tar --zstd -cf " + p_lib1 + " -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }
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
    std::string p_bad;
    {
        std::string pkg_app_bad = "app_bad-1.0.tar.zst";
        p_bad = (pkg_dir / pkg_app_bad).string();
        fs::path work_dir = suite_work_dir / "pkg_app_bad";
        fs::create_directories(work_dir / "content");
        std::ofstream deps(work_dir / "deps.txt"); 
        deps << "lib >= 2.0" << std::endl;
        deps.close();
        std::ofstream files(work_dir / "files.txt"); files.close();
        std::ofstream man(work_dir / "man.txt"); man << "man" << std::endl; man.close();
        
        std::string cmd = "tar --zstd -cf " + p_bad + " -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }
    
    EXPECT_THROW(install_packages({p_bad}), LpkgException); 

    // 3. Try to install app requiring lib < 2.0 (Should Succeed)
    std::string p_good;
    {
        std::string pkg_app_good = "app_good-1.0.tar.zst";
        p_good = (pkg_dir / pkg_app_good).string();
        fs::path work_dir = suite_work_dir / "pkg_app_good";
        fs::create_directories(work_dir / "content");
        std::ofstream deps(work_dir / "deps.txt"); 
        deps << "lib < 2.0" << std::endl;
        deps.close();
        std::ofstream files(work_dir / "files.txt"); files.close();
        std::ofstream man(work_dir / "man.txt"); man << "man" << std::endl; man.close();
        
        std::string cmd = "tar --zstd -cf " + p_good + " -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }

    EXPECT_NO_THROW(install_packages({p_good}));
}

TEST_F(PackageManagerTest, AutoremoveWithVirtualPackages) {
    // 1. Create provider package 'openssl' providing 'libssl'
    std::string p_ossl;
    {
        std::string pkg_ossl = "openssl-1.0.tar.zst";
        p_ossl = (pkg_dir / pkg_ossl).string();
        fs::path work_dir = suite_work_dir / "pkg_openssl";
        fs::create_directories(work_dir / "content");
        std::ofstream provides(work_dir / "provides.txt"); provides << "libssl" << std::endl; provides.close();
        std::ofstream deps(work_dir / "deps.txt"); deps.close();
        std::ofstream files(work_dir / "files.txt"); files.close();
        std::ofstream man(work_dir / "man.txt"); man << "man" << std::endl; man.close();
        std::string cmd = "tar --zstd -cf " + p_ossl + " -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }

    // 2. Create consumer package 'curl' depending on 'libssl'
    std::string p_curl;
    {
        std::string pkg_curl = "curl-1.0.tar.zst";
        p_curl = (pkg_dir / pkg_curl).string();
        fs::path work_dir = suite_work_dir / "pkg_curl";
        fs::create_directories(work_dir / "content");
        std::ofstream deps(work_dir / "deps.txt"); deps << "libssl" << std::endl; deps.close();
        std::ofstream files(work_dir / "files.txt"); files.close();
        std::ofstream man(work_dir / "man.txt"); man << "man" << std::endl; man.close();
        std::string cmd = "tar --zstd -cf " + p_curl + " -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }

    // 3. Install curl (will pull openssl as a dependency)
    // Note: Since we are installing from local files, we must install openssl first
    // because our simple resolver doesn't auto-find local files not in its plan.
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

    // 5. Run autoremove. openssl should NOT be removed because curl (a holdpkg) depends on libssl (provided by openssl).
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


