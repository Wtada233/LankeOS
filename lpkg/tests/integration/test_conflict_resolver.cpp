#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
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

class ConflictResolverTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();
        
        suite_work_dir = fs::absolute("tmp_conflict_test");
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
                        const std::vector<std::string>& deps = {}) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name + "_" + ver);
        fs::create_directories(work_dir / "content");
        
        std::string dummy_name = "dummy_" + name + "_" + ver;
        {
            std::ofstream f(work_dir / "content" / dummy_name); f << "c"; f.close();
        }
        
        json meta;
        meta[std::string(constants::J_NAME)] = name;
        meta[std::string(constants::J_VERSION)] = ver;
        meta[std::string(constants::J_DEPS)] = deps;
        meta[std::string(constants::J_PROVIDES)] = json::array();
        meta[std::string(constants::J_MAN)] = "man " + name;
        {
            std::ofstream mf(work_dir / "metadata.json");
            mf << meta.dump(2) << std::endl;
        }

        std::string pkg_name = name + "-" + ver + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_name).string();
        std::string cmd = "tar --zstd -cf " + pkg_path + " -C " + work_dir.string() + " . > /dev/null 2>&1";
        run_shell(cmd);
        fs::remove_all(work_dir);
        return pkg_path;
    }
};

TEST_F(ConflictResolverTest, AutoUpgradeToSatisfyDependency) {
    std::string p_lib1 = create_pkg("libtest", "1.0");
    std::string p_lib2 = create_pkg("libtest", "2.0");
    std::string p_app = create_pkg("app", "1.0", {"libtest >= 2.0"});

    install_packages({p_lib1});
    
    EXPECT_NO_THROW(install_packages({p_app, p_lib2}));

    {
        std::ifstream pkgs(Config::instance().pkgs_file());
        std::string line;
        bool found_v2 = false;
        while (std::getline(pkgs, line)) {
            if (line == "libtest:2.0") found_v2 = true;
        }
        EXPECT_TRUE(found_v2) << "libtest was not auto-upgraded to 2.0!";
    }
}

TEST_F(ConflictResolverTest, PromptToRemoveBrokenExistingPackage) {
    std::string p_lib1 = create_pkg("libtest", "1.0");
    std::string p_old = create_pkg("oldapp", "1.0", {"libtest == 1.0"});
    install_packages({p_lib1, p_old});

    std::string p_lib2 = create_pkg("libtest", "2.0");
    std::string p_new = create_pkg("newapp", "1.0", {"libtest >= 2.0"});

    EXPECT_NO_THROW(install_packages({p_new, p_lib2}));

    {
        std::ifstream pkgs(Config::instance().pkgs_file());
        std::string line;
        bool found_old = false, found_new = false, found_lib2 = false;
        while (std::getline(pkgs, line)) {
            if (line.starts_with("oldapp:")) found_old = true;
            if (line.starts_with("newapp:")) found_new = true;
            if (line == "libtest:2.0") found_lib2 = true;
        }
        EXPECT_FALSE(found_old) << "Broken package oldapp was not removed!";
        EXPECT_TRUE(found_new);
        EXPECT_TRUE(found_lib2);
    }
}
