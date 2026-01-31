#include <gtest/gtest.h>
#include "../main/src/package_manager.hpp"
#include "../main/src/utils.hpp"
#include "../main/src/config.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>

namespace fs = std::filesystem;

class FeatureTest : public ::testing::Test {
protected:
    fs::path test_root;
    fs::path pkgs_dir;

    void SetUp() override {
        test_root = fs::absolute("tmp_feature_test");
        pkgs_dir = test_root / "pkgs";
        if (fs::exists(test_root)) fs::remove_all(test_root);
        fs::create_directories(pkgs_dir);
        
        // Mock configuration
        set_root_path(test_root.string());
        set_architecture("amd64");
        set_non_interactive_mode(NonInteractiveMode::YES);
        set_testing_mode(true);
        init_filesystem();
        
        // Create a dummy mirror config to avoid errors
        std::ofstream(test_root / "etc/lpkg/mirror.conf") << "file://" + pkgs_dir.string() + "/";
    }

    void TearDown() override {
        set_root_path("/"); // Reset
        set_force_overwrite_mode(false);
        if (fs::exists(test_root)) fs::remove_all(test_root);
    }

    // Helper to create a .tar.zst package
    std::string create_pkg(const std::string& name, const std::string& version, const std::vector<std::pair<std::string, std::string>>& files) {
        fs::path pkg_work_dir = pkgs_dir / (name + "_work");
        if (fs::exists(pkg_work_dir)) fs::remove_all(pkg_work_dir);
        fs::create_directories(pkg_work_dir / "content");
        
        std::ofstream f_deps(pkg_work_dir / "deps.txt");
        std::ofstream f_files(pkg_work_dir / "files.txt");
        std::ofstream f_man(pkg_work_dir / "man.txt");
        f_man << "Manual for " << name << "\n";

        for (const auto& [path, content] : files) {
            fs::path full_path = pkg_work_dir / "content" / path;
            ensure_dir_exists(full_path.parent_path());
            std::ofstream f(full_path);
            f << content;
            f_files << path << " /\n"; 
        }
        f_files.close();

        std::string pkg_path = (pkgs_dir / (name + "-" + version + ".tar.zst")).string();
        std::string tar_cmd = "cd " + pkg_work_dir.string() + " && tar --zstd -cf " + pkg_path + " . > /dev/null 2>&1";
        if (system(tar_cmd.c_str()) != 0) throw std::runtime_error("Tar failed");
        
        fs::remove_all(pkg_work_dir);
        return pkg_path;
    }
};

// 1. Config Protection Test
TEST_F(FeatureTest, ConfigFileProtection) {
    set_force_overwrite_mode(true);
    fs::path etc_dir = test_root / "etc";
    fs::create_directories(etc_dir);
    fs::path config_file = etc_dir / "my.conf";
    {
        std::ofstream f(config_file);
        f << "user_modified_content";
    }

    std::string pkg_path = create_pkg("config-pkg", "1.0", {{"etc/my.conf", "upstream_default_content"}});
    install_packages({pkg_path});

    {
        std::ifstream f_orig(config_file);
        std::stringstream buffer_orig;
        buffer_orig << f_orig.rdbuf();
        EXPECT_EQ(buffer_orig.str(), "user_modified_content");
    }

    fs::path new_config = etc_dir / "my.conf.lpkgnew";
    EXPECT_TRUE(fs::exists(new_config)) << "new_config " << new_config << " should exist";
    
    if (fs::exists(new_config)) {
        std::ifstream f_new(new_config);
        std::stringstream buffer_new;
        buffer_new << f_new.rdbuf();
        EXPECT_EQ(buffer_new.str(), "upstream_default_content");
    }
}

// 2. Hook/Trigger Test
TEST_F(FeatureTest, TriggerActivation) {
    std::string pkg_path = create_pkg("lib-pkg", "1.0", {{"usr/lib/libtest.so", "binary_data"}});

    testing::internal::CaptureStdout();
    install_packages({pkg_path});
    std::string output = testing::internal::GetCapturedStdout();

    // Check for either English or Chinese trigger message
    bool found = (output.find("Trigger: ldconfig") != std::string::npos) || 
                 (output.find("触发器: ldconfig") != std::string::npos);
    EXPECT_TRUE(found) << "Output was: " << output;
}

// 3. Bootstrap Test
TEST_F(FeatureTest, BootstrapInstallation) {
    fs::path bootstrap_root = test_root / "mnt" / "bootstrap";
    fs::create_directories(bootstrap_root);
    
    set_root_path(bootstrap_root.string());
    init_filesystem();
    
    std::string p1 = create_pkg("filesystem", "1.0", {{"usr/share/doc/dummy", ""}}); 
    std::string p2 = create_pkg("coreutils", "8.32", {{"usr/bin/ls", "elf_magic"}});

    std::ofstream(bootstrap_root / "etc/lpkg/mirror.conf") << "file://" + pkgs_dir.string() + "/";

    install_packages({p1, p2});

    EXPECT_TRUE(fs::exists(bootstrap_root / "usr/bin/ls"));
    EXPECT_TRUE(fs::exists(bootstrap_root / "var/lib/lpkg/files/coreutils.txt"));
    EXPECT_TRUE(fs::exists(PKGS_FILE));
    
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/ls"));
}
