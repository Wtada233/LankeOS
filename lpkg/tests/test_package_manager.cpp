#include <gtest/gtest.h>
#include "../src/src/package_manager.hpp"
#include "../src/src/config.hpp"
#include "../src/src/utils.hpp"
#include "../src/src/localization.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;

class PackageManagerTest : public ::testing::Test {
protected:
    fs::path test_root;
    fs::path tmp_pkg_dir;

    void SetUp() override {
        // Fix localization finding
        // We need to point to the source l10n directory if possible, or just not fail.
        // But the immediate issue is NonInteractiveMode.
        set_non_interactive_mode(NonInteractiveMode::YES);

        // Try to init localization.
        // Hack: Create a symlink ../l10n -> ../src/l10n so the default logic works?
        // Or just let it fail but ensure non-interactive doesn't block.
        init_localization();

        test_root = fs::absolute("test_env");
        fs::create_directories(test_root);
        set_root_path(test_root.string());
        
        // Setup minimal FS structure
        init_filesystem();

        // Create a dummy package for testing
        create_dummy_package("testpkg", "1.0");
    }

    void TearDown() override {
        set_root_path("/"); // Reset
        fs::remove_all(test_root);
        if (fs::exists("testpkg-1.0.tar.zst")) {
            fs::remove("testpkg-1.0.tar.zst");
        }
    }

    void create_dummy_package(const std::string& name, const std::string& version) {
        fs::path work_dir = "pkg_work";
        fs::create_directories(work_dir);
        fs::create_directories(work_dir / "content" / "usr" / "bin");
        
        // Create a dummy binary
        std::ofstream bin(work_dir / "content" / "usr" / "bin" / "hello");
        bin << "#!/bin/sh\necho Hello\n";
        bin.close();
        
        // Metadata
        std::ofstream deps(work_dir / "deps.txt");
        deps.close(); // No deps
        
        std::ofstream files(work_dir / "files.txt");
        files << "usr/bin/hello /" << std::endl;
        files.close();

        std::ofstream man(work_dir / "man.txt");
        man << "Man page for " << name << std::endl;
        man.close();

        // Pack it
        std::string cmd = "tar --zstd -cf " + name + "-" + version + ".tar.zst -C " + work_dir.string() + " .";
        int ret = std::system(cmd.c_str());
        ASSERT_EQ(ret, 0);
        
        fs::remove_all(work_dir);
    }
};

TEST_F(PackageManagerTest, InstallLocalPackage) {
    std::string pkg_file = "testpkg-1.0.tar.zst";
    ASSERT_TRUE(fs::exists(pkg_file));

    // Install using absolute path to local file
    std::vector<std::string> args = {fs::absolute(pkg_file).string()};
    
    EXPECT_NO_THROW(install_packages(args));

    // Verify installation
    // 1. Check DB
    // read_set_from_file returns unordered_set
    // We can't access cache internal directly easily without friend class, 
    // but we can check if files exist in the test root.
    
    fs::path installed_file = test_root / "usr" / "bin" / "hello";
    EXPECT_TRUE(fs::exists(installed_file));
    
    fs::path db_file = FILES_DB;
    EXPECT_TRUE(fs::exists(db_file));
}

TEST_F(PackageManagerTest, SysrootIsolation) {
    // Ensure that files are NOT installed to real /usr/bin
    std::string pkg_file = "testpkg-1.0.tar.zst";
    std::vector<std::string> args = {fs::absolute(pkg_file).string()};
    install_packages(args);

    EXPECT_FALSE(fs::exists("/usr/bin/hello")); // Assuming this file doesn't exist on host
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/hello"));
}

TEST_F(PackageManagerTest, VirtualPackages) {
    // 1. Create a "provider" package (e.g. openssl) that provides "libssl"
    {
        fs::path work_dir = "pkg_provider";
        fs::create_directories(work_dir / "content");
        std::ofstream provides(work_dir / "provides.txt");
        provides << "libssl" << std::endl;
        provides.close();
        
        std::ofstream deps(work_dir / "deps.txt"); deps.close();
        std::ofstream files(work_dir / "files.txt"); files.close();
        std::ofstream man(work_dir / "man.txt"); man << "man" << std::endl; man.close();
        
        std::string cmd = "tar --zstd -cf provider-1.0.tar.zst -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }
    
    // 2. Create a "consumer" package (e.g. curl) that depends on "libssl"
    {
        fs::path work_dir = "pkg_consumer";
        fs::create_directories(work_dir / "content");
        
        std::ofstream deps(work_dir / "deps.txt");
        deps << "libssl" << std::endl;
        deps.close();
        
        std::ofstream files(work_dir / "files.txt"); files.close();
        std::ofstream man(work_dir / "man.txt"); man << "man" << std::endl; man.close();
        
        std::string cmd = "tar --zstd -cf consumer-1.0.tar.zst -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }

    // Install provider
    install_packages({"provider-1.0.tar.zst"});
    
    // Install consumer. It should check dependency "libssl". 
    // Since "provider" is installed and provides "libssl", dependency check should pass
    // without trying to install "libssl" from remote.
    
    // Note: If dependency resolution failed, it would try to fetch "libssl" and fail (or crash if no net).
    // We are testing local install so it should succeed.
    EXPECT_NO_THROW(install_packages({"consumer-1.0.tar.zst"}));
    
    // Verify cleanup
    fs::remove("provider-1.0.tar.zst");
    fs::remove("consumer-1.0.tar.zst");
}

TEST_F(PackageManagerTest, VersionConstraints) {
    // 1. Install lib v1.0
    {
        fs::path work_dir = "pkg_lib";
        fs::create_directories(work_dir / "content");
        std::ofstream deps(work_dir / "deps.txt"); deps.close();
        std::ofstream files(work_dir / "files.txt"); files.close();
        std::ofstream man(work_dir / "man.txt"); man << "man" << std::endl; man.close();
        
        std::string cmd = "tar --zstd -cf lib-1.0.tar.zst -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }
    install_packages({"lib-1.0.tar.zst"});

    // 2. Try to install app requiring lib >= 2.0 (Should Fail)
    {
        fs::path work_dir = "pkg_app_bad";
        fs::create_directories(work_dir / "content");
        std::ofstream deps(work_dir / "deps.txt"); 
        deps << "lib >= 2.0" << std::endl;
        deps.close();
        std::ofstream files(work_dir / "files.txt"); files.close();
        std::ofstream man(work_dir / "man.txt"); man << "man" << std::endl; man.close();
        
        std::string cmd = "tar --zstd -cf app_bad-1.0.tar.zst -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }
    
    EXPECT_THROW(install_packages({"app_bad-1.0.tar.zst"}), std::exception);

    // 3. Try to install app requiring lib < 2.0 (Should Succeed)
    {
        fs::path work_dir = "pkg_app_good";
        fs::create_directories(work_dir / "content");
        std::ofstream deps(work_dir / "deps.txt"); 
        deps << "lib < 2.0" << std::endl;
        deps.close();
        std::ofstream files(work_dir / "files.txt"); files.close();
        std::ofstream man(work_dir / "man.txt"); man << "man" << std::endl; man.close();
        
        std::string cmd = "tar --zstd -cf app_good-1.0.tar.zst -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }

    EXPECT_NO_THROW(install_packages({"app_good-1.0.tar.zst"}));

    // Cleanup
    fs::remove("lib-1.0.tar.zst");
    fs::remove("app_bad-1.0.tar.zst");
    fs::remove("app_good-1.0.tar.zst");
}


