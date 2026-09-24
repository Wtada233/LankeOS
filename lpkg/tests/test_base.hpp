#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

#include "../main/src/archive/packer.hpp"
#include "../main/src/base/utils.hpp"
#include "../main/src/config/config.hpp"
#include "../main/src/db/cache.hpp"
#include "../main/src/i18n/localization.hpp"

namespace fs = std::filesystem;

/**
 * "DB 一族"的文件清单：pkgs / files.db / provides.db / confhashes.db / holdpkgs。
 *
 * 这 5 个是同一族：`Cache::write(milestone)` 对它们逐个做"WAL 行 → 备份原文件 → 全量重写"
 * （cache.cpp，里程碑 `:batch-start` 与 `<pkg>:installed` 各写一次），于是"每里程碑一份
 * 备份""回滚后逐字节回到批次前"这类**整体**不变量必须对整族成立。
 *
 * 清单只放这一份：凡是"对 DB 一族做整体断言"的测试（备份计数、逐字节快照、里程碑枚举）
 * 都从这里取。第 5 个库 confhashes.db 加进来时，几处各自硬编码的 4 元素清单（备份计数、
 * 快照）正好就是漏掉它的地方 —— 那是口径分歧，不是"恰好没写"。
 */
inline std::vector<fs::path> db_family_files()
{
    auto& cfg = Config::instance();
    return {cfg.pkgs_file(), cfg.files_db(), cfg.provides_db(), cfg.conf_hashes_db(),
            cfg.holdpkgs_file()};
}

/** 集成测试基类：自动处理 Sandbox 环境 Setup/TearDown */
class IntegrationTestBase : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override
    {
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

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    /** 创建包含一个空 bin 文件的虚拟包 */
    std::string create_pkg(const std::string& name, const std::string& version,
                           const std::vector<std::string>& deps = {},
                           const std::vector<std::string>& provides = {},
                           const std::vector<std::string>& needed_so = {},
                           const std::vector<std::string>& hooks = {})
    {
        fs::path work_dir = suite_work_dir / ("_pkg_" + name);
        fs::create_directories(work_dir / "content" / "usr" / "bin");
        std::ofstream bin(work_dir / "content" / "usr" / "bin" / name);
        bin << "#!/bin/sh\necho " << name << "\n";
        bin.close();

        // hooks 非空时打进 hooks/<各 hook 文件名>。内容固定、只有名字有意义：沙盒 root 里
        // 没有 /bin/bash，run_hook 会提前 return，断言只能落在"钩子文件在不在/内容是什么"
        // 与"执行点断点有没有命中"上（见 test_hook_transaction.cpp 的取证说明）。
        if (!hooks.empty()) {
            fs::create_directories(work_dir / "hooks");
            for (const auto& h : hooks)
                std::ofstream(work_dir / "hooks" / h) << "#!/bin/sh\nexit 0\n";
        }

        std::string pkg_file = name + "-" + version + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_file).string();
        pack_package(pkg_path, work_dir.string(), name, version, deps, provides,
                     "Man page for " + name, needed_so);
        return pkg_path;
    }

    /** 创建本地镜像仓库，返回 mirror 目录 */
    fs::path setup_local_mirror()
    {
        fs::path mirror = suite_work_dir / "mirror" / "x86_64";
        fs::create_directories(mirror);
        {
            std::ofstream mc(Config::instance().mirror_conf());
            mc << "file://" << (suite_work_dir / "mirror").string() << "/\n";
        }
        return mirror;
    }

    /** 将已创建的包放入镜像 */
    void add_to_mirror(const std::string& name, const std::string& version)
    {
        fs::path mirror = suite_work_dir / "mirror" / "x86_64";
        fs::path pkg_subdir = mirror / name;
        fs::create_directories(pkg_subdir);
        fs::copy(pkg_dir / (name + "-" + version + ".lpkg"), pkg_subdir / (version + ".lpkg"));
    }
};
