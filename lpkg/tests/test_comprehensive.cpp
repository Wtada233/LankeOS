#include <gtest/gtest.h>
#include "../main/src/package_manager.hpp"
#include "../main/src/config.hpp"
#include "../main/src/utils.hpp"
#include "../main/src/localization.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;

class ComprehensiveTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        set_non_interactive_mode(NonInteractiveMode::YES);
        set_testing_mode(true);
        init_localization();
        
        suite_work_dir = fs::absolute("tmp_comprehensive_test");
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

    // 辅助函数：快速创建一个包含特定文件和元数据的包
    std::string create_pkg(const std::string& name, const std::string& ver, 
                        const std::vector<std::pair<std::string, std::string>>& files,
                        const std::vector<std::string>& deps = {},
                        const std::vector<std::string>& provides = {}) {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name + "_" + ver);
        fs::create_directories(work_dir / "content");
        
        std::ofstream fl(work_dir / "files.txt");
        for (const auto& [src, dest] : files) {
            fs::path p = work_dir / "content" / src;
            fs::create_directories(p.parent_path());
            std::ofstream f(p); f << "content of " << src; f.close();
            fl << src << " " << dest << "\n";
        }
        fl.close();

        std::ofstream dl(work_dir / "deps.txt");
        for (const auto& d : deps) dl << d << "\n";
        dl.close();

        if (!provides.empty()) {
            std::ofstream pl(work_dir / "provides.txt");
            for (const auto& p : provides) pl << p << "\n";
            pl.close();
        }

        std::ofstream ml(work_dir / "man.txt"); ml << "man " << name; ml.close();

        std::string pkg_name = name + "-" + ver + ".tar.zst";
        std::string pkg_path = (pkg_dir / pkg_name).string();
        std::string cmd = "tar --zstd -cf " + pkg_path + " -C " + work_dir.string() + " . > /dev/null 2>&1";
        std::system(cmd.c_str());
        fs::remove_all(work_dir);
        return pkg_path;
    }
};

// 1. 测试升级过程中的文件清理 (修复之前的 Bug)
TEST_F(ComprehensiveTest, UpgradeCleansObsoleteFiles) {
    // v1 有 file1, file2
    std::string p1 = create_pkg("cleanup_test", "1.0", {{"usr/bin/file1", "/"},{"usr/bin/file2", "/"}});
    install_packages({p1});
    
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/file1"));
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/file2"));

    // v2 只有 file1，删除了 file2
    std::string p2 = create_pkg("cleanup_test", "2.0", {{"usr/bin/file1", "/"}});
    install_packages({p2});

    EXPECT_TRUE(fs::exists(test_root / "usr/bin/file1"));
    // 关键点：file2 应该被删除
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/file2")) << "Obsolete file was not removed during upgrade!";
}

// 2. 测试显式版本安装/降级 (修复之前的 Bug)
TEST_F(ComprehensiveTest, ExplicitVersionDowngrade) {
    std::string p1 = create_pkg("vers_test", "1.0", {{"usr/bin/bin1", "/"}});
    std::string p2 = create_pkg("vers_test", "2.0", {{"usr/bin/bin1", "/"}});

    // 先装 2.0
    install_packages({p2});
    
    // 显式装 1.0 (模拟降级)
    EXPECT_NO_THROW(install_packages({p1}));

    // 检查版本是否回到了 1.0
    {
        std::ifstream pkgs(PKGS_FILE);
        std::string line;
        bool found = false;
        while (std::getline(pkgs, line)) {
            if (line == "vers_test:1.0") found = true;
        }
        EXPECT_TRUE(found) << "Failed to downgrade to 1.0";
    }
}

// 3. 复杂 Autoremove 场景 (涉及虚拟包)
TEST_F(ComprehensiveTest, AutoremoveHandlesVirtualChains) {
    // openssl 提供 libssl
    std::string p1 = create_pkg("openssl", "1.0", {{"usr/lib/libssl.so", "/"}}, {}, {"libssl"});
    // curl 依赖 libssl
    std::string p2 = create_pkg("curl", "1.0", {{"usr/bin/curl", "/"}}, {"libssl"});

    install_packages({p1});
    install_packages({p2}); // curl 是手动安装的

    // 执行 autoremove
    autoremove();
    write_cache();

    // openssl 不应该被删除，因为 curl (holdpkg) 依赖它提供的 libssl
    EXPECT_TRUE(fs::exists(test_root / "usr/lib/libssl.so")) << "Provider of virtual dependency was incorrectly autoremoved!";
}

// 4. 循环依赖测试
TEST_F(ComprehensiveTest, CircularDependencyResolution) {
    std::string pA = create_pkg("pkgA", "1.0", {{"usr/bin/A", "/"}}, {"pkgB"});
    std::string pB = create_pkg("pkgB", "1.0", {{"usr/bin/B", "/"}}, {"pkgA"});

    // 应该能处理循环并安装两者，而不是无限递归
    EXPECT_NO_THROW(install_packages({pA, pB}));
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/A"));
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/B"));
}

// 5. 事务内冲突测试 (两个新包冲突)
TEST_F(ComprehensiveTest, InterTransactionConflict) {
    std::string pA = create_pkg("conflictA", "1.0", {{"etc/shared.conf", "/"}});
    std::string pB = create_pkg("conflictB", "1.0", {{"etc/shared.conf", "/"}});

    // 当同时安装两个包含相同文件的包时，第二个包应该报错
    // 注意：目前的实现是在 commit 阶段逐个执行，所以第一个成功，第二个抛出异常并触发回滚
    EXPECT_THROW(install_packages({pA, pB}), LpkgException);
    
    // 验证回滚：如果 conflictB 失败导致整体失败，conflictA 也应该被回滚
    EXPECT_FALSE(fs::exists(test_root / "etc/shared.conf")) << "Transaction rollback failed after inter-package conflict!";
}
