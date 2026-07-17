#include <gtest/gtest.h>
#include "../../main/src/pkg/package_manager.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

class FeatureTest : public ::testing::Test {
protected:
    fs::path test_root;
    fs::path pkgs_dir;

    void SetUp() override {
        test_root = fs::absolute("tmp_feature_test");
        pkgs_dir = test_root / "pkgs";
        if (fs::exists(test_root)) fs::remove_all(test_root);
        fs::create_directories(pkgs_dir);

        init_localization();

        Config::instance().set_root_path(test_root.string());
        Config::instance().set_architecture("x86_64");
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        Config::instance().init_filesystem();
        
        std::ofstream(test_root / "etc/lpkg/mirror.conf") << "file://" + pkgs_dir.string() + "/";
        std::ofstream(test_root / "etc/lpkg/triggers.conf") <<
            R"(# Shared library update -> regenerate ldconfig links
^/usr/lib/.*\.so.*	ldconfig
)";
    }

    void TearDown() override {
        Config::instance().set_root_path("/");
        Config::instance().set_force_overwrite_mode(false);
        if (fs::exists(test_root)) fs::remove_all(test_root);
    }

    std::string create_pkg(const std::string& name, const std::string& version, const std::vector<std::pair<std::string, std::string>>& files) {
        fs::path pkg_work_dir = pkgs_dir / (name + "_work");
        if (fs::exists(pkg_work_dir)) fs::remove_all(pkg_work_dir);
        fs::create_directories(pkg_work_dir / "content");
        
        json meta;
        meta[std::string(constants::J_NAME)] = name;
        meta[std::string(constants::J_VERSION)] = version;
        meta[std::string(constants::J_DEPS)] = json::array();
        meta[std::string(constants::J_PROVIDES)] = json::array();
        meta[std::string(constants::J_MAN)] = "Manual for " + name;

        for (const auto& [path, content] : files) {
            fs::path full_path = pkg_work_dir / "content" / path;
            ensure_dir_exists(full_path.parent_path());
            std::ofstream f(full_path);
            f << content;
        }
        {
            std::ofstream mf(pkg_work_dir / "metadata.json");
            mf << meta.dump(2) << std::endl;
        }

        std::string pkg_path = (pkgs_dir / (name + "-" + version + ".lpkg")).string();
        std::string tar_cmd = "cd " + pkg_work_dir.string() + " && tar --zstd -cf " + pkg_path + " . > /dev/null 2>&1";
        if (run_shell(tar_cmd) != 0) throw std::runtime_error("Tar failed");
        
        fs::remove_all(pkg_work_dir);
        return pkg_path;
    }
};

TEST_F(FeatureTest, ConfigFileProtection) {
    Config::instance().set_force_overwrite_mode(true);
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
    EXPECT_TRUE(fs::exists(new_config)) << "new_config should exist";
    
    if (fs::exists(new_config)) {
        std::ifstream f_new(new_config);
        std::stringstream buffer_new;
        buffer_new << f_new.rdbuf();
        EXPECT_EQ(buffer_new.str(), "upstream_default_content");
    }
}

TEST_F(FeatureTest, TriggerActivation) {
    std::string pkg_path = create_pkg("lib-pkg", "1.0", {{"usr/lib/libtest.so", "binary_data"}});

    testing::internal::CaptureStdout();
    install_packages({pkg_path});
    std::string output = testing::internal::GetCapturedStdout();

    bool found = (output.find("Trigger: ldconfig") != std::string::npos) || 
                 (output.find("触发器: ldconfig") != std::string::npos);
    EXPECT_TRUE(found) << "Output was: " << output;
}

TEST_F(FeatureTest, BootstrapInstallation) {
    fs::path bootstrap_root = test_root / "mnt" / "bootstrap";
    fs::create_directories(bootstrap_root);
    
    Config::instance().set_root_path(bootstrap_root.string());
    Config::instance().init_filesystem();
    
    std::string p1 = create_pkg("filesystem", "1.0", {{"usr/share/doc/dummy", ""}}); 
    std::string p2 = create_pkg("coreutils", "8.32", {{"usr/bin/ls", "elf_magic"}});

    std::ofstream(bootstrap_root / "etc/lpkg/mirror.conf") << "file://" + pkgs_dir.string() + "/";

    install_packages({p1, p2});

    EXPECT_TRUE(fs::exists(bootstrap_root / "usr/bin/ls"));
    // files/ directory no longer exists; file ownership tracked in files.db only
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed("coreutils"));
    EXPECT_TRUE(Cache::instance().is_installed("filesystem"));
    EXPECT_TRUE(fs::exists(Config::instance().pkgs_file()));
    
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/ls"));
}
