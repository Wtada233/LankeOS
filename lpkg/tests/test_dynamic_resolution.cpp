#include <gtest/gtest.h>
#include "../main/src/package_manager.hpp"
#include "../main/src/packer.hpp"
#include "../main/src/hash.hpp"
#include "../main/src/cache.hpp"
#include "../main/src/config.hpp"
#include "../main/src/utils.hpp"
#include "../main/src/localization.hpp"
#include "../main/src/constants.hpp"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;
using json = nlohmann::json;

class DynamicResolutionTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;
    fs::path mirror_dir;

    void SetUp() override {
        set_non_interactive_mode(NonInteractiveMode::YES);
        set_testing_mode(true);
        init_localization();
        
        suite_work_dir = fs::absolute("tmp_dynamic_res_test");
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        mirror_dir = suite_work_dir / "mirror" / "x86_64";
        
        fs::remove_all(suite_work_dir);
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);
        fs::create_directories(mirror_dir);
        
        set_root_path(test_root.string());
        set_architecture("x86_64");
        init_filesystem();
        
        std::ofstream(test_root / "etc/lpkg/mirror.conf") << "file://" << suite_work_dir.string() << "/mirror/" << std::endl;
    }

    void TearDown() override {
        set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    std::string create_pkg(const std::string& name, const std::string& ver, 
                        const std::vector<std::string>& deps = {},
                        const std::vector<std::string>& provides = {}) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::create_directories(work_dir / "root" / "usr" / "bin");
        std::ofstream(work_dir / "root" / "usr" / "bin" / name).close();
        
        std::string pkg_filename = name + "-" + ver + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_filename).string();
        pack_package(pkg_path, work_dir.string(), name, ver, deps, provides);
        
        // Put in mirror
        fs::path mirror_pkg_dir = mirror_dir / name;
        fs::create_directories(mirror_pkg_dir);
        fs::copy_file(pkg_path, mirror_pkg_dir / (ver + ".lpkg"), fs::copy_options::overwrite_existing);
        
        fs::remove_all(work_dir);
        return pkg_path;
    }

    void update_index(const std::vector<std::tuple<std::string, std::string, std::string, std::string>>& entries) {
        std::ofstream index(mirror_dir / "index.txt");
        for (const auto& [name, ver, deps, provides] : entries) {
            std::string pkg_filename = name + "-" + ver + ".lpkg";
            std::string pkg_path = (pkg_dir / pkg_filename).string();
            std::string hash = "unknown";
            if (fs::exists(pkg_path)) {
                hash = calculate_sha256(pkg_path);
            }
            index << name << "|" << ver << ":" << hash << ":" << deps << "|" << provides << "\n";
        }
    }
};

TEST_F(DynamicResolutionTest, DynamicDependencyChange) {
    // 1. Setup: Index says 'app' depends on 'libA'
    create_pkg("libA", "1.0");
    create_pkg("libB", "1.0");
    
    // Package 'app' in mirror actually depends on 'libB'
    create_pkg("app", "1.0", {"libB"});
    
    // But index incorrectly says it depends on 'libA'
    update_index({
        {"app", "1.0", "libA", ""},
        {"libA", "1.0", "", ""},
        {"libB", "1.0", "", ""}
    });

    // 2. Install 'app'
    EXPECT_NO_THROW(install_packages({"app"}));

    // 3. Verify: 'libB' should be installed, 'libA' should NOT be (if re-resolution cleared it)
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().is_installed("libB"));
    EXPECT_FALSE(Cache::instance().is_installed("libA"));
}

TEST_F(DynamicResolutionTest, DynamicProviderChange) {
    // 1. Setup: 
    // Index says 'app' depends on 'virtual-pkg'
    // Index says 'provA' provides 'virtual-pkg'
    // Package 'app' actually provides nothing special.
    // Package 'provB' provides 'virtual-pkg'.
    
    create_pkg("provA", "1.0", {}, {"other-pkg"}); // provA actually doesn't provide virtual-pkg
    create_pkg("provB", "1.0", {}, {"virtual-pkg"});
    create_pkg("app", "1.0", {"virtual-pkg"});
    
    update_index({
        {"app", "1.0", "virtual-pkg", ""},
        {"provA", "1.0", "", "virtual-pkg"},
        {"provB", "1.0", "", "virtual-pkg"}
    });

    // 2. Install 'app'. 
    // Initial resolution: app -> provA (because provA provides virtual-pkg in index)
    // After downloading provA, metadata says it provides 'other-pkg'.
    // System should re-resolve, find that virtual-pkg is missing, then find provB provides it.
    
    EXPECT_NO_THROW(install_packages({"app"}));

    // 3. Verify
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("app"));
    EXPECT_TRUE(Cache::instance().is_installed("provB"));
    EXPECT_FALSE(Cache::instance().is_installed("provA"));
}
