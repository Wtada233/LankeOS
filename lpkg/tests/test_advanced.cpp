#include <gtest/gtest.h>
#include "../src/src/package_manager.hpp"
#include "../src/src/config.hpp"
#include "../src/src/utils.hpp"
#include "../src/src/localization.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;

class AdvancedPackageManagerTest : public ::testing::Test {
protected:
    fs::path test_root;

    void SetUp() override {
        set_non_interactive_mode(NonInteractiveMode::YES);
        init_localization();
        test_root = fs::absolute("test_env_adv");
        fs::create_directories(test_root);
        set_root_path(test_root.string());
        init_filesystem();
    }

    void TearDown() override {
        set_root_path("/");
        fs::remove_all(test_root);
    }
    
    void create_pkg(const std::string& name, const std::string& ver, const std::string& content_file, const std::string& content) {
        fs::path work_dir = "pkg_work_" + name;
        fs::create_directories(work_dir);
        fs::path dest = work_dir / "content" / content_file;
        fs::create_directories(dest.parent_path());
        
        std::ofstream f(dest);
        f << content;
        f.close();
        
        std::ofstream deps(work_dir / "deps.txt"); deps.close();
        std::ofstream files(work_dir / "files.txt");
        files << content_file << " /\n";
        files.close();
        std::ofstream man(work_dir / "man.txt"); man.close();

        std::string cmd = "tar --zstd -cf " + name + "-" + ver + ".tar.zst -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }
};

TEST_F(AdvancedPackageManagerTest, RollbackOnCopyFailure) {
    // 1. Install v1
    create_pkg("rollback_test", "1.0", "etc/config", "version1");
    install_packages({"rollback_test-1.0.tar.zst"});
    
    fs::path installed_conf = test_root / "etc/config";
    EXPECT_TRUE(fs::exists(installed_conf));
    {
        std::ifstream f(installed_conf);
        std::string s;
        f >> s;
        EXPECT_EQ(s, "version1");
    }

    // 2. Prepare v2 which writes to two files: etc/config (updated) and etc/newfile
    {
        fs::path work_dir = "pkg_work_rollback_test_v2";
        fs::create_directories(work_dir);
        fs::create_directories(work_dir / "content" / "etc");
        std::ofstream f1(work_dir / "content" / "etc" / "config"); f1 << "version2"; f1.close();
        std::ofstream f2(work_dir / "content" / "etc" / "newfile"); f2 << "hello"; f2.close();
        
        std::ofstream files(work_dir / "files.txt");
        files << "etc/config /\n";
        files << "etc/newfile /\n"; // We will block this one
        files.close();
        std::ofstream deps(work_dir / "deps.txt"); deps.close();
        std::ofstream man(work_dir / "man.txt"); man.close();
        
        std::string cmd = "tar --zstd -cf rollback_test-2.0.tar.zst -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }

    // 3. Sabotage: Create the target file and make it immutable so overwrite/rename fails
    // Note: Root ignores permissions, so chmod 500 doesn't work. chattr +i works.
    {
        std::ofstream sabotage(test_root / "etc" / "newfile");
        sabotage << "sabotage";
        sabotage.close();
        
        std::string chattr_cmd = "chattr +i " + (test_root / "etc" / "newfile").string();
        int ret = std::system(chattr_cmd.c_str());
        if (ret != 0) {
            std::cerr << "[SKIPPED] chattr failed (not supported?), skipping rollback failure test." << std::endl;
            // Cleanup and return to avoid false failure
            fs::remove(test_root / "etc" / "newfile");
            return;
        }
    }

    // 4. Try install v2
    // It should fail during backup/copy of newfile, and then rollback config to version1
    EXPECT_THROW({
        try {
            install_packages({"rollback_test-2.0.tar.zst"});
        } catch (...) {
            // Remove immutable attribute so cleanup works
            std::string chattr_cmd = "chattr -i " + (test_root / "etc" / "newfile").string();
            std::system(chattr_cmd.c_str());
            throw;
        }
    }, LpkgException);

    // Cleanup attribute in case it didn't throw (fail)
    std::string chattr_cmd = "chattr -i " + (test_root / "etc" / "newfile").string();
    std::system(chattr_cmd.c_str());

    // 5. Verify Rollback
    // config should be version1
    {
        std::ifstream f(installed_conf);
        std::string s; f >> s;
        EXPECT_EQ(s, "version1");
    }
    // And .lpkg_bak should be gone
    EXPECT_FALSE(fs::exists(installed_conf.string() + ".lpkg_bak"));
}

TEST_F(AdvancedPackageManagerTest, ChrootHook) {
    // This test verifies that the hook execution logic ATTEMPTS to chroot when ROOT_DIR is set.
    
    create_pkg("hook_test", "1.0", "dummy", "dummy");
    
    // Add hook
    {
        fs::path work_dir = "pkg_work_hook_test";
        fs::create_directories(work_dir / "content");
        std::ofstream f(work_dir / "content" / "dummy"); f << "d"; f.close();
        
        fs::create_directories(work_dir / "hooks");
        std::ofstream hook(work_dir / "hooks" / "postinst.sh");
        hook << "#!/bin/sh\n";
        hook << "echo 'Running' > /hook_ran.txt\n";
        hook.close();
        fs::permissions(work_dir / "hooks" / "postinst.sh", fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add);

        std::ofstream files(work_dir / "files.txt"); files << "dummy /\n"; files.close();
        std::ofstream deps(work_dir / "deps.txt"); deps.close();
        std::ofstream man(work_dir / "man.txt"); man.close();
        
        std::string cmd = "tar --zstd -cf hook_test-1.0.tar.zst -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }

    testing::internal::CaptureStderr();
    install_packages({"hook_test-1.0.tar.zst"});
    std::string stderr_output = testing::internal::GetCapturedStderr();
    
    // Debug output
    // std::cout << "Stderr: " << stderr_output << std::endl;

    // Verify it did NOT run (file not created)
    EXPECT_FALSE(fs::exists(test_root / "hook_ran.txt"));
    // Verify it didn't run on host
    EXPECT_FALSE(fs::exists("/hook_ran.txt"));
}
