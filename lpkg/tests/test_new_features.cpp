#include <gtest/gtest.h>
#include "../main/src/package_manager.hpp"
#include "../main/src/packer.hpp"
#include "../main/src/hash.hpp"
#include "../main/src/config.hpp"
#include "../main/src/utils.hpp"
#include "../main/src/localization.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;

class NewFeaturesTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        set_non_interactive_mode(NonInteractiveMode::YES);
        set_testing_mode(true);
        init_localization();
        
        suite_work_dir = fs::absolute("tmp_new_features_test");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);
        
        set_root_path(test_root.string());
        set_architecture("amd64");
        init_filesystem();

        // Setup mock mirror
        fs::path mirror_path = suite_work_dir / "mirror";
        fs::create_directories(mirror_path / "amd64");
        std::ofstream(test_root / "etc/lpkg/mirror.conf") << "file://" << mirror_path.string() << "/" << std::endl;
        // Create initial empty index
        std::ofstream(mirror_path / "amd64" / "index.txt").close();
    }

    std::string create_pkg(const std::string& name, const std::string& ver, 
                        const std::vector<std::pair<std::string, std::string>>& files) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name + "_" + ver);
        fs::create_directories(work_dir / "root");
        
        for (const auto& [src, dest] : files) {
            fs::path p = work_dir / "root" / src;
            fs::create_directories(p.parent_path());
            std::ofstream f(p); f << "content of " << src; f.close();
        }

        std::ofstream(work_dir / "man.txt") << "Manual for " << name << "\n";
        std::ofstream(work_dir / "deps.txt").close();

        std::string pkg_filename = name + "-" + ver + ".tar.zst";
        std::string pkg_path = (pkg_dir / pkg_filename).string();
        pack_package(pkg_path, work_dir.string());

        // Also put it in the mirror
        fs::path mirror_pkg_dir = suite_work_dir / "mirror" / "amd64" / name / ver;
        fs::create_directories(mirror_pkg_dir);
        fs::copy_file(pkg_path, mirror_pkg_dir / "app.tar.zst", fs::copy_options::overwrite_existing);
        
        std::string hash = calculate_sha256(pkg_path);
        std::ofstream(mirror_pkg_dir / "hash.txt") << hash;
        std::ofstream(suite_work_dir / "mirror" / "amd64" / name / "latest.txt") << ver;

        // Update index.txt
        std::ofstream index(suite_work_dir / "mirror" / "amd64" / "index.txt", std::ios::app);
        index << name << "|" << ver << "|" << hash << "||" << std::endl;
        
        fs::remove_all(work_dir);
        return pkg_path;
    }
};

TEST_F(NewFeaturesTest, QueryFileAndPackage) {
    std::string pkg = create_pkg("query_test", "1.0", {{"usr/bin/query_target", "/"}});
    install_packages({pkg});

    // Test query file (absolute)
    testing::internal::CaptureStdout();
    query_file("/usr/bin/query_target");
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("query_test"), std::string::npos);

    // Test query file (relative with smart resolution)
    // We simulate being in /usr/bin and querying 'query_target'
    fs::path old_cwd = fs::current_path();
    fs::create_directories(test_root / "usr/bin");
    fs::current_path(test_root / "usr/bin");
    
    testing::internal::CaptureStdout();
    query_file("query_target");
    output = testing::internal::GetCapturedStdout();
    fs::current_path(old_cwd); // Restore CWD
    
    EXPECT_NE(output.find("query_test"), std::string::npos);
    EXPECT_NE(output.find("/usr/bin/query_target"), std::string::npos);

    // Test query package
    testing::internal::CaptureStdout();
    query_package("query_test");
    output = testing::internal::GetCapturedStdout();
    EXPECT_NE(output.find("/usr/bin/query_target"), std::string::npos);
}

TEST_F(NewFeaturesTest, ReinstallPackage) {
    std::string pkg = create_pkg("reinstall_test", "1.0", {{"etc/reinstall.conf", "/"}});
    
    // Clear any previous attempts if necessary, but here we just install normally first
    install_packages({pkg});

    fs::path conf = test_root / "etc/reinstall.conf";
    EXPECT_TRUE(fs::exists(conf));

    // Modify the file to see if it gets restored
    {
        std::ofstream f(conf);
        f << "modified";
    }

    // Force reload cache/repo to ensure it sees the pkg in mirror
    reinstall_package("reinstall_test");

    {
        std::ifstream f(conf);
        std::string s;
        std::getline(f, s);
        EXPECT_EQ(s, "content of etc/reinstall.conf");
    }

    // Additional Test: Reinstall via PATH
    {
        std::ofstream f(conf);
        f << "modified again";
    }
    reinstall_package(pkg); // Use path instead of name
    {
        std::ifstream f(conf);
        std::string s;
        std::getline(f, s);
        EXPECT_EQ(s, "content of etc/reinstall.conf");
    }
}
