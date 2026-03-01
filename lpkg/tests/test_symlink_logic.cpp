#include <gtest/gtest.h>
#include "package_manager.hpp"
#include "config.hpp"
#include "utils.hpp"
#include <filesystem>
#include <fstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

class SymlinkLogicTest : public ::testing::Test {
protected:
    void Set_Up_Test_Env(const std::string& root_name) {
        // Use /tmp to avoid polluting project root
        test_root = fs::temp_directory_path() / ("lpkg_test_" + root_name + "_" + std::to_string(getpid()));
        fs::remove_all(test_root);
        fs::create_directories(test_root);
        set_root_path(test_root.string());
        init_filesystem();
        
        fs::create_directories(test_root / "etc/lpkg");
        {
            std::ofstream f(test_root / "etc/lpkg/mirror.conf");
            f << "http://localhost/lpkg/\n";
        }

        set_testing_mode(true);
        set_non_interactive_mode(NonInteractiveMode::YES);
    }

    void TearDown() override {
        set_root_path("/");
        if (!test_root.empty()) {
            fs::remove_all(test_root);
        }
    }

    fs::path test_root;
};

TEST_F(SymlinkLogicTest, PreventsSymlinkChmodFollow) {
    Set_Up_Test_Env("test_symlink_chmod");

    // 1. Create a "target" file with specific permissions (SUID)
    fs::path target_phys = test_root / "usr/bin/target_binary";
    ensure_dir_exists(target_phys.parent_path());
    { std::ofstream f(target_phys); f << "binary content"; }
    chmod(target_phys.c_str(), 04755);
    
    struct stat st_init;
    lstat(target_phys.c_str(), &st_init);
    ASSERT_TRUE(st_init.st_mode & S_ISUID);

    // 2. Prepare a mock extraction directory
    fs::path pkg_tmp_dir = get_tmp_dir() / "symlink_pkg";
    fs::remove_all(pkg_tmp_dir);
    fs::create_directories(pkg_tmp_dir / "content");
    
    // Create symlink directly in content/ to match files.txt simple entry
    fs::create_symlink("usr/bin/target_binary", pkg_tmp_dir / "content/my_link");
    
    {
        std::ofstream f(pkg_tmp_dir / "files.txt");
        f << "my_link\t/usr/bin/\n";
    }

    InstallationTask task("symlink_pkg", "1.0", true);
    task.tmp_pkg_dir_ = pkg_tmp_dir;
    
    ASSERT_NO_THROW(task.copy_package_files());

    // 4. Verify the link was created at destination: /usr/bin/my_link
    fs::path link_phys = test_root / "usr/bin/my_link";
    EXPECT_TRUE(fs::is_symlink(link_phys));

    // 5. Verify target remains untouched
    struct stat st_final;
    lstat(target_phys.c_str(), &st_final);
    EXPECT_EQ(st_init.st_mode, st_final.st_mode);
}

TEST_F(SymlinkLogicTest, HandlesConfigSymlinkConflict) {
    Set_Up_Test_Env("test_symlink_config");

    // 1. Create existing config as regular file
    fs::path conf_phys = test_root / "etc/os-release";
    ensure_dir_exists(conf_phys.parent_path());
    { std::ofstream f(conf_phys); f << "old release"; }

    // 2. Mock package with os-release as symlink
    fs::path pkg_tmp_dir = get_tmp_dir() / "filesystem_pkg";
    fs::remove_all(pkg_tmp_dir);
    fs::create_directories(pkg_tmp_dir / "content/etc");
    fs::create_symlink("/usr/lib/os-release", pkg_tmp_dir / "content/etc/os-release");
    
    { std::ofstream f(pkg_tmp_dir / "files.txt"); f << "etc/os-release\t/\n"; }

    InstallationTask task("filesystem_pkg", "1.0", true);
    task.tmp_pkg_dir_ = pkg_tmp_dir;

    // Trigger config conflict logic: existing file at etc/os-release.lpkgnew
    fs::path lpkgnew_path = test_root / "etc/os-release.lpkgnew";
    ensure_dir_exists(lpkgnew_path.parent_path());
    { std::ofstream f(lpkgnew_path); f << "stale new file"; }

    ASSERT_NO_THROW(task.copy_package_files());

    EXPECT_TRUE(fs::is_symlink(lpkgnew_path));
    EXPECT_EQ(fs::read_symlink(lpkgnew_path), "/usr/lib/os-release");
}
