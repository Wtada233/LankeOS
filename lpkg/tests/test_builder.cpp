#include <gtest/gtest.h>
#include "builder.hpp"
#include "utils.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class BuilderTest : public ::testing::Test {
protected:
    fs::path test_dir = fs::current_path() / "test_build_dir";
    fs::path staging_root = test_dir / "root";

    void SetUp() override {
        fs::remove_all(test_dir);
        fs::create_directories(test_dir / "root/usr/bin");
        fs::create_directories(test_dir / "root/usr/lib");
        
        // Create dummy LankeBUILD.json
        std::ofstream json(test_dir / "LankeBUILD.json");
        json << "{\"name\": \"test-pkg\", \"version\": \"1.0.0\"}";
        json.close();

        // Create dummy LankeBUILD.sh
        std::ofstream sh(test_dir / "LankeBUILD.sh");
        sh << "lankebuild_prepare() { :; }\n"
           << "lankebuild_build() { :; }\n"
           << "lankebuild_package() { echo 'test' > $STAGING_ROOT/usr/bin/test_bin; }";
        sh.close();

        // Create a dummy .la file for cleanup testing
        std::ofstream la(test_dir / "root/usr/lib/test.la");
        la << "dummy libtool file";
        la.close();
    }

    void TearDown() override {
        fs::remove_all(test_dir);
        // Clean up any generated package file if it exists
        fs::remove("test-pkg-1.0.0.lpkg");
    }
};

TEST_F(BuilderTest, CleanupLibtoolFiles) {
    // Manually trigger the finalize_staging-like behavior or test it via run_build
    // Since run_build does it internally, let's call it.
    // Note: This will require a real LankeBUILD environment setup, which is tricky.
    // Given the constraints, let's assume we can call the function if it were public or
    // via an integration test.
    
    // For this test, let's assume we can mock or call the function directly
    // based on our knowledge of builder.cpp's internal structure.
    
    // As builder.cpp's finalize_staging is a lambda inside run_build,
    // we have to test run_build.
    
    EXPECT_NO_THROW(run_build(test_dir));

    // Verify .la file is gone
    EXPECT_FALSE(fs::exists(staging_root / "usr/lib/test.la"));
}
