#include <gtest/gtest.h>
#include "../main/src/packer.hpp"
#include "../main/src/scanner.hpp"
#include "../main/src/utils.hpp"
#include "../main/src/config.hpp"
#include "../main/src/localization.hpp"
#include "../main/src/package_manager.hpp"
#include "../main/src/archive.hpp"
#include "../main/src/cache.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;

class ToolsTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path source_dir;
    fs::path root_dir;
    fs::path hooks_dir;
    fs::path output_pkg;
    fs::path test_system_root;

    void SetUp() override {
        init_localization();
        suite_work_dir = fs::absolute("tmp_tools_test");
        source_dir = suite_work_dir / "lankepkg";
        root_dir = source_dir / "root";
        hooks_dir = source_dir / "hooks";
        output_pkg = suite_work_dir / "test.pkg.tar.zst";
        test_system_root = suite_work_dir / "sysroot";

        fs::create_directories(root_dir / "usr/bin");
        fs::create_directories(hooks_dir);
        fs::create_directories(test_system_root / "var/lib/lpkg"); // DB dir
        
        set_root_path(test_system_root.string());
        init_filesystem();
    }

    void TearDown() override {
        set_root_path("/");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
    }
};

TEST_F(ToolsTest, PackAndVerifyContent) {
    // 1. 准备源文件
    std::ofstream f(root_dir / "usr/bin/hello");
    f << "executable_content";
    f.close();

    std::ofstream h(hooks_dir / "postinst.sh");
    h << "echo hook";
    h.close();

    // 2. 执行打包
    EXPECT_NO_THROW(pack_package(output_pkg.string(), source_dir.string()));
    EXPECT_TRUE(fs::exists(output_pkg));

    // 3. 解压并验证内容
    fs::path verify_dir = suite_work_dir / "verify_pack";
    fs::create_directories(verify_dir);
    
    // 使用 lpkg 内部的解压函数进行验证
    extract_tar_zst(output_pkg, verify_dir);

    // 检查核心文件结构
    EXPECT_TRUE(fs::exists(verify_dir / "content/usr/bin/hello"));
    EXPECT_TRUE(fs::exists(verify_dir / "hooks/postinst.sh"));
    EXPECT_TRUE(fs::exists(verify_dir / "files.txt"));
    EXPECT_TRUE(fs::exists(verify_dir / "deps.txt"));
    EXPECT_TRUE(fs::exists(verify_dir / "man.txt"));

    // 检查 files.txt 内容是否正确
    std::ifstream files_txt(verify_dir / "files.txt");
    std::string line;
    bool found_bin = false;
    while (std::getline(files_txt, line)) {
        if (line.find("usr/bin/hello") != std::string::npos) {
            found_bin = true;
        }
    }
    EXPECT_TRUE(found_bin);
}

TEST_F(ToolsTest, ScanOrphansLogic) {
    // 1. 创建孤儿文件 (Orphan)
    fs::create_directories(test_system_root / "usr/bin");
    std::ofstream orphan(test_system_root / "usr/bin/orphan");
    orphan << "orphan";
    orphan.close();

    // 2. 创建被拥有的文件 (Owned) 并注册到数据库
    std::ofstream owned(test_system_root / "usr/bin/owned");
    owned << "owned";
    owned.close();
    
    // 手动注入数据库记录 (模拟已安装包)
    {
        std::ofstream db(test_system_root / "var/lib/lpkg/files.db");
        db << "/usr/bin/owned\ttest-pkg" << std::endl;
    }
    Cache::instance().load(); // 强制重载数据库

    // 3. 执行扫描并捕获输出
    testing::internal::CaptureStdout();
    scan_orphans(test_system_root.string());
    std::string output = testing::internal::GetCapturedStdout();

    // 4. 验证结果
    EXPECT_NE(output.find("orphan"), std::string::npos) << "Should verify orphan file is detected";
    EXPECT_EQ(output.find("owned"), std::string::npos) << "Should NOT report owned file as orphan";
}
