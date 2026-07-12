#pragma once

#include "../main/src/config/config.hpp"
#include "../main/src/base/utils.hpp"
#include "../main/src/i18n/localization.hpp"
#include "../main/src/db/cache.hpp"
#include "../main/src/archive/packer.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

/** 集成测试基类：自动处理 Sandbox 环境 Setup/TearDown */
class IntegrationTestBase : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_lpkg_itest");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";

        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    /** 创建包含一个空 bin 文件的虚拟包 */
    std::string create_pkg(const std::string& name, const std::string& version,
                           const std::vector<std::string>& deps = {},
                           const std::vector<std::string>& provides = {},
                           const std::vector<std::string>& needed_so = {}) {
        fs::path work_dir = suite_work_dir / ("_pkg_" + name);
        fs::create_directories(work_dir / "content" / "usr" / "bin");
        std::ofstream bin(work_dir / "content" / "usr" / "bin" / name);
        bin << "#!/bin/sh\necho " << name << "\n";
        bin.close();

        std::string pkg_file = name + "-" + version + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_file).string();
        pack_package(pkg_path, work_dir.string(), name, version, deps, provides,
                     "Man page for " + name, needed_so);
        return pkg_path;
    }

    /** 创建本地镜像仓库，返回 mirror 目录 */
    fs::path setup_local_mirror() {
        fs::path mirror = suite_work_dir / "mirror" / "x86_64";
        fs::create_directories(mirror);
        {
            std::ofstream mc(Config::instance().mirror_conf());
            mc << "file://" << (suite_work_dir / "mirror").string() << "/\n";
        }
        return mirror;
    }

    /** 将已创建的包放入镜像 */
    void add_to_mirror(const std::string& name, const std::string& version) {
        fs::path mirror = suite_work_dir / "mirror" / "x86_64";
        fs::path pkg_subdir = mirror / name;
        fs::create_directories(pkg_subdir);
        fs::copy(pkg_dir / (name + "-" + version + ".lpkg"),
                 pkg_subdir / (version + ".lpkg"));
    }
};
