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

namespace fs = std::filesystem;
using json = nlohmann::json;

class ComprehensiveTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        Config::instance().set_force_overwrite_mode(false);
        Config::instance().set_no_hooks_mode(false);
        Config::instance().set_no_deps_mode(false);
        init_localization();
        
        suite_work_dir = fs::absolute("tmp_comprehensive_test");
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);
        
        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
    }

    void TearDown() override {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    std::string create_pkg(const std::string& name, const std::string& ver, 
                        const std::vector<std::pair<std::string, std::string>>& files,
                        const std::vector<std::string>& deps = {},
                        const std::vector<std::string>& provides = {}) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name + "_" + ver);
        fs::remove_all(work_dir);
        fs::create_directories(work_dir / "content");
        
        for (const auto& [src, dest] : files) {
            fs::path p = work_dir / "content" / src;
            ensure_dir_exists(p.parent_path());
            std::ofstream f(p); f << "content of " << src; f.close();
        }

        std::string pkg_name = name + "-" + ver + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_name).string();
        
        pack_package(pkg_path, work_dir.string(), name, ver, deps, provides, "man " + name);
        
        fs::remove_all(work_dir);
        return pkg_path;
    }
};

TEST_F(ComprehensiveTest, UpgradeCleansObsoleteFiles) {
    std::string p1 = create_pkg("cleanup_test", "1.0", {{"usr/bin/file1", "/"},{"usr/bin/file2", "/"}});
    install_packages({p1});
    
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/file1"));
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/file2"));

    std::string p2 = create_pkg("cleanup_test", "2.0", {{"usr/bin/file1", "/"}});
    install_packages({p2});

    EXPECT_TRUE(fs::exists(test_root / "usr/bin/file1"));
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/file2")) << "Obsolete file was not removed during upgrade!";
}

TEST_F(ComprehensiveTest, ExplicitVersionDowngrade) {
    std::string p1 = create_pkg("vers_test", "1.0", {{"usr/bin/bin1", "/"}});
    std::string p2 = create_pkg("vers_test", "2.0", {{"usr/bin/bin1", "/"}});

    install_packages({p2});
    
    EXPECT_NO_THROW(install_packages({p1}));

    {
        std::ifstream pkgs(Config::instance().pkgs_file());
        std::string line;
        bool found = false;
        while (std::getline(pkgs, line)) {
            if (line == "vers_test:1.0") found = true;
        }
        EXPECT_TRUE(found) << "Failed to downgrade to 1.0";
    }
}

TEST_F(ComprehensiveTest, AutoremoveHandlesVirtualChains) {
    std::string p1 = create_pkg("openssl", "1.0", {{"usr/lib/libssl.so", "/"}}, {}, {"libssl"});
    std::string p2 = create_pkg("curl", "1.0", {{"usr/bin/curl", "/"}}, {"libssl"});

    install_packages({p1});
    install_packages({p2});

    autoremove();
    write_cache();

    EXPECT_TRUE(fs::exists(test_root / "usr/lib/libssl.so")) << "Provider of virtual dependency was incorrectly autoremoved!";
}

TEST_F(ComprehensiveTest, CircularDependencyResolution) {
    std::string pA = create_pkg("pkgA", "1.0", {{"usr/bin/A", "/"}}, {"pkgB"});
    std::string pB = create_pkg("pkgB", "1.0", {{"usr/bin/B", "/"}}, {"pkgA"});

    EXPECT_NO_THROW(install_packages({pA, pB}));
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/A"));
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/B"));
}

TEST_F(ComprehensiveTest, InterTransactionConflict) {
    std::string pA = create_pkg("conflictA", "1.0", {{"usr/bin/shared.bin", "/"}});
    std::string pB = create_pkg("conflictB", "1.0", {{"usr/bin/shared.bin", "/"}});

    EXPECT_THROW(install_packages({pA, pB}), LpkgException);
    
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/shared.bin")) << "Transaction rollback failed after inter-package conflict!";
}
