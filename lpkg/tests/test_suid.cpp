#include <gtest/gtest.h>
#include "../main/src/package_manager.hpp"
#include "../main/src/config.hpp"
#include "../main/src/utils.hpp"
#include "../main/src/localization.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sys/stat.h>

namespace fs = std::filesystem;

class SUIDTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        set_non_interactive_mode(NonInteractiveMode::YES);
        set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_suid_test");
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);
        
        set_root_path(test_root.string());
        init_filesystem();
    }

    void TearDown() override {
        set_root_path("/"); // Reset
        std::string clean_cmd = "sudo rm -rf " + suite_work_dir.string();
        std::system(clean_cmd.c_str());
    }

    std::string create_suid_package(const std::string& name, const std::string& version) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::create_directories(work_dir / "content" / "usr" / "bin");
        
        fs::path bin_path = work_dir / "content" / "usr" / "bin" / "suid_bin";
        {
            std::ofstream bin(bin_path);
            bin << "#!/bin/sh\n"
                << "echo SUID\n";
            bin.close();
        }
        
        // Set SUID bit
        chmod(bin_path.c_str(), 04755);
        
        // Verify bit is set in work_dir
        struct stat st;
        stat(bin_path.c_str(), &st);
        if (!(st.st_mode & S_ISUID)) {
            throw std::runtime_error("Failed to set SUID bit on dummy file");
        }

        // Metadata
        std::ofstream deps(work_dir / "deps.txt");
        deps.close();
        
        // IMPORTANT: Use TAB separator
        std::ofstream files(work_dir / "files.txt");
        files << "usr/bin/suid_bin\t/" << std::endl;
        files.close();

        std::ofstream man(work_dir / "man.txt");
        man << "Man page" << std::endl;
        man.close();

        // Pack it using tar which should preserve permissions (-p)
        std::string pkg_name = name + "-" + version + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_name).string();
        std::string cmd = "tar --zstd -p -cf " + pkg_path + " -C " + work_dir.string() + " .";
        int ret = std::system(cmd.c_str());
        if (ret != 0) throw std::runtime_error("tar failed");
        
        fs::remove_all(work_dir);
        return pkg_path;
    }
};

TEST_F(SUIDTest, PreserveSUID) {
    std::string pkg_file = create_suid_package("suidpkg", "1.0");
    ASSERT_TRUE(fs::exists(pkg_file));

    install_packages({pkg_file});

    fs::path installed_file = test_root / "usr" / "bin" / "suid_bin";
    ASSERT_TRUE(fs::exists(installed_file)) << "Installed file not found at " << installed_file;

    struct stat st;
    ASSERT_EQ(stat(installed_file.c_str(), &st), 0);
    
    EXPECT_TRUE(st.st_mode & S_ISUID) << "SUID bit lost after installation! Mode: " << std::oct << st.st_mode;
}
