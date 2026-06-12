#include <gtest/gtest.h>
#include "package_manager.hpp"
#include "cache.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "constants.hpp"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>

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
        set_root_path(test_root.string() + "/root");
        
        fs::create_directories(ROOT_DIR / "var/lib/lpkg/deps");
        fs::create_directories(ROOT_DIR / "var/lib/lpkg/files");
        fs::create_directories(ROOT_DIR / "var/lib/lpkg/info");
        fs::create_directories(DOCS_DIR);
        
        std::ofstream(PKGS_FILE).close();
        std::ofstream(HOLDPKGS_FILE).close();
        std::ofstream(FILES_DB).close();
        std::ofstream(PROVIDES_DB).close();

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
        fs::create_directories(work_dir / "content");
        
        {
            std::ofstream d(work_dir / "deps.txt");
            for (auto const& dep : deps) d << dep.first << "\t" << dep.second << "\n";
        }
        
        {
            std::ofstream p(work_dir / "provides.txt");
            for (auto const& prov : provides) p << prov << "\n";
        }
        
        json meta;
        meta["name"] = name;
        meta["version"] = ver;
        for (auto const& file : files) {
            fs::path src = work_dir / "content" / file.first;
            ensure_dir_exists(src.parent_path());
            std::ofstream out(src);
            out << "content of " << file.first;
            out.close();
        }
        {
            std::ofstream mf(work_dir / "metadata.json");
            mf << meta.dump(2) << std::endl;
        }
        
        std::ofstream(work_dir / "man.txt") << name << " man page";
        
        std::string pkg_path = (repo_dir / (name + "-" + ver + ".lpkg")).string();
        std::string cmd = "cd " + work_dir.string() + " && tar -cf - . | zstd -o " + pkg_path;
        run_shell(cmd);
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
    
    fs::path target = ROOT_DIR / "usr/bin/tool";
    ensure_dir_exists(target.parent_path());
    {
        std::ofstream f(target);
        f << "manual content";
    }
    
    set_force_overwrite_mode(false);
    EXPECT_THROW(install_packages({pkg}), LpkgException);
    
    set_force_overwrite_mode(true);
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
    fs::create_directories(work_dir / "content/usr/bin");
    {
        std::ofstream out(work_dir / "content/usr/bin/app");
        out << "new improved content";
    }
    
    json meta;
    meta["name"] = "myapp";
    meta["version"] = "1.0";
    {
        std::ofstream mf(work_dir / "metadata.json");
        mf << meta.dump(2) << std::endl;
    }
    std::ofstream(work_dir / "deps.txt").close();
    std::ofstream(work_dir / "provides.txt").close();
    std::ofstream(work_dir / "man.txt") << "man";
    
    std::string pkg_v1_fixed = (test_root / "myapp-1.0-fixed.lpkg").string();
    run_shell(("cd " + work_dir.string() + " && tar -cf - . | zstd -o " + pkg_v1_fixed));

    reinstall_package(pkg_v1_fixed);
    
    std::ifstream f(ROOT_DIR / "usr/bin/app");
    std::string content;
    std::getline(f, content);
    EXPECT_EQ(content, "new improved content");
}
