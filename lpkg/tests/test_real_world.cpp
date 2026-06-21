#include <gtest/gtest.h>
#include "package_manager.hpp"
#include "cache.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "constants.hpp"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>
#include "../main/src/packer.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

class RealWorldScenarioTest : public ::testing::Test {
protected:
    fs::path test_root;
    fs::path repo_dir;

    void SetUp() override {
        test_root = fs::absolute("tmp_real_world_test");
        repo_dir = test_root / "repo";
        fs::remove_all(test_root);
        fs::create_directories(repo_dir);
        Config::instance().set_root_path(test_root.string() + "/root");
        
        fs::create_directories(Config::instance().root_dir() / "var/lib/lpkg/deps");
        fs::create_directories(Config::instance().root_dir() / "var/lib/lpkg/files");
        fs::create_directories(Config::instance().root_dir() / "var/lib/lpkg/info");
        fs::create_directories(Config::instance().docs_dir());
        
        std::ofstream(Config::instance().pkgs_file()).close();
        std::ofstream(Config::instance().holdpkgs_file()).close();
        std::ofstream(Config::instance().files_db()).close();
        std::ofstream(Config::instance().provides_db()).close();

        Cache::instance().load();
    }

    void TearDown() override {
        fs::remove_all(test_root);
    }

    std::string create_pkg(const std::string& name, const std::string& ver, 
                          std::vector<std::pair<std::string, std::string>> deps = {},
                          std::vector<std::string> provides = {},
                          std::vector<std::pair<std::string, std::string>> files = {}) {
        fs::path work_dir = test_root / ("work_" + name);
        fs::remove_all(work_dir);
        fs::create_directories(work_dir / "root");
        
        std::vector<std::string> deps_list;
        for (const auto& d : deps) deps_list.push_back(d.first + " " + d.second);

        for (auto const& file : files) {
            fs::path src = work_dir / "root" / file.first;
            ensure_dir_exists(src.parent_path());
            std::ofstream out(src);
            out << "content of " << file.first;
            out.close();
        }
        
        std::string pkg_path = (repo_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(pkg_path, work_dir.string(), name, ver, deps_list, provides, name + " man page");
        return pkg_path;
    }
};

TEST_F(RealWorldScenarioTest, VirtualPackageProviderSelection) {
    std::string p1 = create_pkg("openssl", "1.1", {}, {"libssl"});
    std::string p2 = create_pkg("libressl", "3.0", {}, {"libssl"});
    std::string cons = create_pkg("curl", "7.0", {{"libssl", ">= 1.0"}});

    install_packages({cons, p1, p2});
    
    EXPECT_TRUE(Cache::instance().is_installed("curl"));
    bool has_provider = Cache::instance().is_installed("openssl") || Cache::instance().is_installed("libressl");
    EXPECT_TRUE(has_provider);
}

TEST_F(RealWorldScenarioTest, BlockRemovalOfProvider) {
    std::string p1 = create_pkg("openssl", "1.1", {}, {"libssl"}, {{"usr/lib/libssl.so", "/"}});
    std::string cons = create_pkg("curl", "7.0", {{"libssl", ">= 1.0"}});

    install_packages({p1, cons});
    
    remove_package("openssl", false);
    
    EXPECT_TRUE(Cache::instance().is_installed("openssl"));
    EXPECT_TRUE(Cache::instance().is_installed("curl"));
}

TEST_F(RealWorldScenarioTest, ManualFileClobbering) {
    std::string pkg = create_pkg("clobber", "1.0", {}, {}, {{"usr/bin/tool", "/"}});
    
    fs::path target = Config::instance().root_dir() / "usr/bin/tool";
    ensure_dir_exists(target.parent_path());
    {
        std::ofstream f(target);
        f << "manual content";
    }
    
    Config::instance().set_force_overwrite_mode(false);
    EXPECT_THROW(install_packages({pkg}), LpkgException);
    
    Config::instance().set_force_overwrite_mode(true);
    install_packages({pkg});
    EXPECT_TRUE(Cache::instance().is_installed("clobber"));
    
    std::ifstream f(target);
    std::string content;
    std::getline(f, content);
    EXPECT_EQ(content, "content of usr/bin/tool");
}

TEST_F(RealWorldScenarioTest, ReinstallFromNewSource) {
    std::string pkg_v1 = create_pkg("myapp", "1.0", {}, {}, {{"usr/bin/app", "/"}});
    install_packages({pkg_v1});
    
    fs::path work_dir = test_root / "work_myapp_new";
    fs::remove_all(work_dir);
    fs::create_directories(work_dir / "root/usr/bin");
    {
        std::ofstream out(work_dir / "root/usr/bin/app");
        out << "new improved content";
    }
    
    std::string pkg_v1_fixed = (test_root / "myapp-1.0-fixed.lpkg").string();
    pack_package(pkg_v1_fixed, work_dir.string(), "myapp", "1.0", {}, {}, "man");

    reinstall_package(pkg_v1_fixed);
    
    std::ifstream f(Config::instance().root_dir() / "usr/bin/app");
    std::string content;
    std::getline(f, content);
    EXPECT_EQ(content, "new improved content");
}
