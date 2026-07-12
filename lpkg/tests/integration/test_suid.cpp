#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/archive/packer.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/base/constants.hpp"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sys/stat.h>
#include "../../main/src/archive/packer.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

class SUIDTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_suid_test");
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);
        
        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
    }

    void TearDown() override {
        Config::instance().set_root_path("/");
        std::string clean_cmd = "sudo rm -rf " + suite_work_dir.string();
        run_shell(clean_cmd);
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
        
        chmod(bin_path.c_str(), 04755);
        
        struct stat st;
        stat(bin_path.c_str(), &st);
        if (!(st.st_mode & S_ISUID)) {
            throw std::runtime_error("Failed to set SUID bit on dummy file");
        }

        std::string pkg_name = name + "-" + version + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_name).string();
        
        pack_package(pkg_path, work_dir.string(), name, version);
        
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
