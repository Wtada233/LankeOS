#include <gtest/gtest.h>
#include "../main/src/archive/packer.hpp"
#include "../main/src/scan/scanner.hpp"
#include "../main/src/base/utils.hpp"
#include "../main/src/config/config.hpp"
#include "../main/src/i18n/localization.hpp"
#include "../main/src/pkg/package_manager.hpp"
#include "../main/src/archive/archive.hpp"
#include "../main/src/db/cache.hpp"
#include "../main/src/base/constants.hpp"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>

namespace fs = std::filesystem;
using json = nlohmann::json;

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
        root_dir = source_dir / "content";
        hooks_dir = source_dir / "hooks";
        output_pkg = suite_work_dir / "test.pkg.lpkg";
        test_system_root = suite_work_dir / "sysroot";

        fs::create_directories(root_dir / "usr/bin");
        fs::create_directories(hooks_dir);
        fs::create_directories(test_system_root / "var/lib/lpkg"); // DB dir
        
        Config::instance().set_root_path(test_system_root.string());
        Config::instance().init_filesystem();
    }

    void TearDown() override {
        Config::instance().set_root_path("/");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
    }
};

TEST_F(ToolsTest, PackAndVerifyContent) {
    // 1. Prepare source files
    std::ofstream f(root_dir / "usr/bin/hello");
    f << "executable_content";
    f.close();

    std::ofstream h(hooks_dir / "postinst.sh");
    h << "echo hook";
    h.close();

    // 2. Execute pack with name/version
    EXPECT_NO_THROW(pack_package(output_pkg.string(), source_dir.string(), "test-pkg", "1.0"));
    EXPECT_TRUE(fs::exists(output_pkg));

    // 3. Extract and verify content
    fs::path verify_dir = suite_work_dir / "verify_pack";
    fs::create_directories(verify_dir);
    
    extract_tar_zst(output_pkg, verify_dir);

    // Check core file structure (metadata.json replaces files.txt)
    EXPECT_TRUE(fs::exists(verify_dir / "content/usr/bin/hello"));
    EXPECT_TRUE(fs::exists(verify_dir / "hooks/postinst.sh"));
    EXPECT_TRUE(fs::exists(verify_dir / "metadata.json"));

    // Verify metadata.json content
    {
        std::ifstream meta_f(verify_dir / "metadata.json");
        json meta;
        meta_f >> meta;
        EXPECT_EQ(meta[std::string(constants::J_NAME)], "test-pkg");
        EXPECT_EQ(meta[std::string(constants::J_VERSION)], "1.0");
        EXPECT_TRUE(meta.contains(std::string(constants::J_DEPS)));
        EXPECT_TRUE(meta.contains(std::string(constants::J_PROVIDES)));
        EXPECT_TRUE(meta.contains(std::string(constants::J_MAN)));
    }
}

TEST_F(ToolsTest, ScanOrphansLogic) {
    // 1. Create orphan file
    fs::create_directories(test_system_root / "usr/bin");
    std::ofstream orphan(test_system_root / "usr/bin/orphan");
    orphan << "orphan";
    orphan.close();

    // 2. Create owned file and register to database
    std::ofstream owned(test_system_root / "usr/bin/owned");
    owned << "owned";
    owned.close();
    
    {
        std::ofstream db(test_system_root / "var/lib/lpkg/files.db");
        db << "/usr/bin/owned\ttest-pkg" << std::endl;
    }
    Cache::instance().load();

    // 3. Run scan and capture output
    testing::internal::CaptureStdout();
    scan_orphans(test_system_root.string());
    std::string output = testing::internal::GetCapturedStdout();

    // 4. Verify
    EXPECT_NE(output.find("orphan"), std::string::npos) << "Should detect orphan file";
    EXPECT_EQ(output.find("owned"), std::string::npos) << "Should NOT report owned file as orphan";
}
