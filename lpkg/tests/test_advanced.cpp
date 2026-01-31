#include <gtest/gtest.h>
#include "../main/src/package_manager.hpp"
#include "../main/src/config.hpp"
#include "../main/src/utils.hpp"
#include "../main/src/localization.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;

class AdvancedPackageManagerTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        set_non_interactive_mode(NonInteractiveMode::YES);
        set_testing_mode(true);
        init_localization();
        
        suite_work_dir = fs::absolute("tmp_advanced_test");
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);
        
        set_root_path(test_root.string());
        init_filesystem();
    }

    void TearDown() override {
        set_root_path("/");
        // Unlock any immutable files that might have been created during tests
        std::string unlock_cmd = "find " + suite_work_dir.string() + " -exec chattr -i {} + 2>/dev/null";
        std::system(unlock_cmd.c_str());
        fs::remove_all(suite_work_dir);
    }
    
    std::string create_pkg(const std::string& name, const std::string& ver, const std::string& content_file, const std::string& content) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::create_directories(work_dir / "content");
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

        std::string pkg_name = name + "-" + ver + ".tar.zst";
        std::string pkg_path = (pkg_dir / pkg_name).string();
        std::string cmd = "tar --zstd -cf " + pkg_path + " -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
        return pkg_path;
    }
};

TEST_F(AdvancedPackageManagerTest, RollbackOnCopyFailure) {
    // 1. Prepare a package with two files: file_ok and file_blocked
    std::string pkg;
    {
        std::string pkg_name = "rollback_new-1.0.tar.zst";
        pkg = (pkg_dir / pkg_name).string();
        fs::path work_dir = suite_work_dir / "pkg_work_rollback_new";
        fs::create_directories(work_dir / "content" / "usr" / "bin");
        std::ofstream f1(work_dir / "content" / "usr" / "bin" / "file_ok"); f1 << "ok"; f1.close();
        std::ofstream f2(work_dir / "content" / "usr" / "bin" / "file_blocked"); f2 << "blocked"; f2.close();
        
        std::ofstream files(work_dir / "files.txt");
        files << "usr/bin/file_ok /\n";
        files << "usr/bin/file_blocked /\n";
        files.close();
        std::ofstream deps(work_dir / "deps.txt"); deps.close();
        std::ofstream man(work_dir / "man.txt"); man.close();
        
        std::string cmd = "tar --zstd -cf " + pkg + " -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }

    // 2. Sabotage: Create file_blocked and lock it
    {
        fs::create_directories(test_root / "usr/bin");
        std::ofstream sabotage(test_root / "usr/bin/file_blocked");
        sabotage << "original";
        sabotage.close();
        
        std::string chattr_cmd = "chattr +i " + (test_root / "usr/bin/file_blocked").string();
        std::system(chattr_cmd.c_str());
    }

    // 3. Try install. Use --force-overwrite to pass the pre-check
    set_force_overwrite_mode(true);
    EXPECT_THROW(install_packages({pkg}), LpkgException);

    // 4. Verify Rollback: file_ok should be GONE (cleaned up), file_blocked should be 'original'
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/file_ok"));
    {
        std::ifstream f(test_root / "usr/bin/file_blocked");
        std::string s; f >> s;
        EXPECT_EQ(s, "original");
    }
}

TEST_F(AdvancedPackageManagerTest, ChrootHook) {
    std::string pkg = create_pkg("hook_test", "1.0", "dummy", "dummy");
    
    // Add hook to the package (Re-creating with hook)
    {
        fs::path work_dir = suite_work_dir / "pkg_work_hook_test_with_hook";
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
        
        std::string cmd = "tar --zstd -cf " + pkg + " -C " + work_dir.string() + " .";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
    }

    testing::internal::CaptureStderr();
    install_packages({pkg});
    testing::internal::GetCapturedStderr();
    
    EXPECT_FALSE(fs::exists(test_root / "hook_ran.txt"));
    EXPECT_FALSE(fs::exists("/hook_ran.txt"));
}
