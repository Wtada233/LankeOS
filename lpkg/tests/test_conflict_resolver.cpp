#include <gtest/gtest.h>
#include "../main/src/package_manager.hpp"
#include "../main/src/config.hpp"
#include "../main/src/utils.hpp"
#include "../main/src/localization.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;

class ConflictResolverTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        set_non_interactive_mode(NonInteractiveMode::YES); // Default to auto-agree
        set_testing_mode(true);
        init_localization();
        
        suite_work_dir = fs::absolute("tmp_conflict_test");
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);
        
        set_root_path(test_root.string());
        init_filesystem();
    }

    void TearDown() override {
        set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    std::string create_pkg(const std::string& name, const std::string& ver, 
                        const std::vector<std::string>& deps = {}) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name + "_" + ver);
        fs::create_directories(work_dir / "content");
        
        std::ofstream fl(work_dir / "files.txt");
        std::string dummy_name = "dummy_" + name + "_" + ver;
        fl << dummy_name << " /\n";
        std::ofstream f(work_dir / "content" / dummy_name); f << "c"; f.close();
        fl.close();

        std::ofstream dl(work_dir / "deps.txt");
        for (const auto& d : deps) dl << d << "\n";
        dl.close();

        std::ofstream ml(work_dir / "man.txt"); ml << "man " << name; ml.close();

        std::string pkg_name = name + "-" + ver + ".tar.zst";
        std::string pkg_path = (pkg_dir / pkg_name).string();
        std::string cmd = "tar --zstd -cf " + pkg_path + " -C " + work_dir.string() + " . > /dev/null 2>&1";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
        return pkg_path;
    }
};

// 1. 测试自动升级：已安装 v1，新包需要 >= 2.0，解析器应自动寻找并计划升级到 v2
TEST_F(ConflictResolverTest, AutoUpgradeToSatisfyDependency) {
    // 模拟本地仓库
    std::string p_lib1 = create_pkg("libtest", "1.0");
    std::string p_lib2 = create_pkg("libtest", "2.0");
    std::string p_app = create_pkg("app", "1.0", {"libtest >= 2.0"});

    // 先安装 libtest v1.0
    install_packages({p_lib1});
    
    // 安装 app，这应该触发 libtest 自动升级到 2.0
    EXPECT_NO_THROW(install_packages({p_app, p_lib2}));

    // 验证 libtest 是否变为了 2.0
    {
        std::ifstream pkgs(PKGS_FILE);
        std::string line;
        bool found_v2 = false;
        while (std::getline(pkgs, line)) {
            if (line == "libtest:2.0") found_v2 = true;
        }
        EXPECT_TRUE(found_v2) << "libtest was not auto-upgraded to 2.0!";
    }
}

// 2. 测试破坏现有包时的交互式删除
TEST_F(ConflictResolverTest, PromptToRemoveBrokenExistingPackage) {
    // libtest v1.0 被 oldapp 依赖 (需要 == 1.0)
    std::string p_lib1 = create_pkg("libtest", "1.0");
    std::string p_old = create_pkg("oldapp", "1.0", {"libtest == 1.0"});
    install_packages({p_lib1, p_old});

    // 现在尝试安装 newapp，它需要 libtest >= 2.0
    std::string p_lib2 = create_pkg("libtest", "2.0");
    std::string p_new = create_pkg("newapp", "1.0", {"libtest >= 2.0"});

    // 预期：升级 libtest 到 2.0 会破坏 oldapp。系统应提示并删除 oldapp。
    EXPECT_NO_THROW(install_packages({p_new, p_lib2}));

    // 验证 oldapp 被删除了，而 newapp 和 libtest v2.0 在
    {
        std::ifstream pkgs(PKGS_FILE);
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
