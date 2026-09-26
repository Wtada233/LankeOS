#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "archive.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "builder.hpp"
#include "i18n/localization.hpp"
#include "utils.hpp"

namespace fs = std::filesystem;

class BuilderTest : public ::testing::Test
{
protected:
    fs::path test_dir = fs::current_path() / "test_build_dir";
    fs::path staging_root = test_dir / "content";

    void SetUp() override
    {
        init_localization();  // 报错断言要落地真实文案（否则 get_string 吐 [MISSING_STRING: …]）
        fs::remove_all(test_dir);
        fs::create_directories(test_dir / "content/usr/bin");
        fs::create_directories(test_dir / "content/usr/lib");

        // Create dummy LankeBUILD.json
        std::ofstream json(test_dir / "LankeBUILD.json");
        json << "{\"name\": \"test-pkg\", \"version\": \"1.0.0\", \"man\": \"Manual content\", "
                "\"deps\": [], \"provides\": []}";
        json.close();

        // Create dummy LankeBUILD with placeholder
        std::ofstream sh(test_dir / "LankeBUILD");
        sh << "lankebuild_prepare() { :; }\n"
           << "lankebuild_build() { :; }\n"
           << "lankebuild_package() { echo \"{PKG_NAME}\" > \"{STAGING_ROOT}/usr/bin/test_bin\"; }";
        sh.close();

        // Create a dummy .la file for cleanup testing
        std::ofstream la(test_dir / "content/usr/lib/test.la");
        la << "dummy libtool file";
        la.close();
    }

    void TearDown() override
    {
        fs::remove_all(test_dir);
        // Clean up any generated package file if it exists
        fs::remove("test-pkg-1.0.0.lpkg");
    }
};

TEST_F(BuilderTest, CleanupLibtoolFiles)
{
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

/**
 * staging（**构建产物 = 包内容**）里有符号链接环 / 悬空链接时，收尾阶段不得抛。
 *
 * 收尾会扫 staging 三趟：清 `.la`、strip 二进制、在 `usr/lib` 生成 SONAME 链接。此前这三趟
 * 里的判定用的是 `directory_entry::is_regular_file()` / `entry.is_regular_file()` /
 * `entry.is_directory()` —— 它们走 `status()`（**跟随**末段链接），对**环**抛
 * `filesystem_error(ELOOP)` → **整个构建被打死**（上游源码树里自指链接很常见）。
 * 2026-09-26 改为不抛形态（`base/utils.hpp` 的谓词族），本用例钉住它。
 */
TEST_F(BuilderTest, SymlinkLoopInStagingDoesNotBreakFinalize)
{
    fs::create_directories(staging_root / "usr/share");         // SetUp 只建了 usr/bin 与 usr/lib
    fs::create_symlink("self", staging_root / "usr/lib/self");  // 自环（解不开）
    fs::create_symlink("nope", staging_root / "usr/share/dangling");  // 悬空（一样不该打断）

    // 前提复现：判定类调用今天对环**抛**（不是判 not-found）。哪天 libstdc++ 改了行为，
    // 这条会红 —— 那时请一并复核 base/utils.hpp 谓词族的说明。
    // 注意用 `{ (void)…; }` 包起来：`fs::exists` 是 `[[nodiscard]]`，而 `EXPECT_THROW` 会把语句
    // 塞进 try/catch —— 裸写就是 `-Werror=unused-result`（本仓库已踩过两次）。
    EXPECT_THROW(
        { (void)fs::exists(staging_root / "usr/lib/self"); }, fs::filesystem_error)
        << "前提变了：libstdc++ 不再对符号链接环抛异常";

    EXPECT_NO_THROW(run_build(test_dir)) << "staging 里的符号链接环不该把构建打死";
    EXPECT_FALSE(fs::exists(staging_root / "usr/lib/test.la")) << "环不该让 `.la` 清理趟中断";
}

TEST_F(BuilderTest, VariableSubstitutionWorks)
{
    std::string pkg_file = "test-pkg-1.0.0.lpkg";
    fs::remove(pkg_file);
    EXPECT_NO_THROW(run_build(test_dir));
    ASSERT_TRUE(fs::exists(pkg_file));

    // Extract to verify content
    fs::path extract_dir = test_dir / "extract";
    fs::create_directories(extract_dir);

    extract_tar_zst(pkg_file, extract_dir, pkg_file);

    fs::path test_bin = extract_dir / "content/usr/bin/test_bin";
    ASSERT_TRUE(fs::exists(test_bin)) << "test_bin not found in extracted package";

    std::ifstream f(test_bin);
    std::string content;
    std::getline(f, content);
    EXPECT_EQ(content, "test-pkg");

    fs::remove(pkg_file);
}

// ============================================================================
// run_build 的失败路径与 6.5 阶段（2026-09-26 拆 run_build 时抽出，此前无覆盖）
// ============================================================================

/** 元数据阶段：LankeBUILD.json 缺失 → 报错且**点名构建目录**（不是含糊的"构建失败"）。 */
TEST_F(BuilderTest, MissingLankeBuildJsonNamesTheBuildDir)
{
    const fs::path empty_recipe = test_dir / "empty-recipe";
    fs::create_directories(empty_recipe);

    try {
        run_build(empty_recipe);
        FAIL() << "没有 LankeBUILD.json 竟然构建成功";
    } catch (const LpkgException& e) {
        EXPECT_NE(std::string(e.what()).find(empty_recipe.string()), std::string::npos)
            << "报错没有点名构建目录: " << e.what();
    }
}

/** 打包阶段的产物名校验：`name: "../evil"` 绝不能把 .lpkg 写到构建目录之外。 */
TEST_F(BuilderTest, UnsafePackageNameIsRefusedBeforePacking)
{
    {
        std::ofstream json(test_dir / "LankeBUILD.json");
        json << "{\"name\": \"../evil\", \"version\": \"1.0.0\", \"man\": \"m\", "
                "\"deps\": [], \"provides\": []}";
    }

    try {
        run_build(test_dir);
        FAIL() << "`../evil` 这样的包名竟然通过了";
    } catch (const LpkgException& e) {
        EXPECT_NE(std::string(e.what()).find("../evil"), std::string::npos)
            << "报错没有点名那个非法包名: " << e.what();
    }
    EXPECT_FALSE(fs::exists(fs::current_path() / "evil-1.0.0.lpkg"))
        << "非法包名把产物写到了构建目录之外";
}

/**
 * 6.5 阶段的两个方向：Builder 造的 USR-Merge 辅助链接（`content/bin` 等）
 * 默认**不打包**；`keep_fs_layout=true` 时保留并一并打包。
 */
TEST_F(BuilderTest, KeepFsLayoutControlsUsrMergeSymlinksInPackage)
{
    const std::string pkg_file = "test-pkg-1.0.0.lpkg";

    // ① 默认 keep_fs_layout=false → 辅助链接不进包
    fs::remove(pkg_file);
    ASSERT_NO_THROW(run_build(test_dir));
    {
        const fs::path ex = test_dir / "extract-default";
        fs::create_directories(ex);
        extract_tar_zst(pkg_file, ex, pkg_file);
        EXPECT_FALSE(fs::exists(ex / "content/bin")) << "默认不该打包 usr-merge 辅助链接";
    }

    // ② keep_fs_layout=true → 链接保留在包里
    {
        std::ofstream json(test_dir / "LankeBUILD.json");
        json << "{\"name\": \"test-pkg\", \"version\": \"1.0.0\", \"man\": \"m\", "
                "\"keep_fs_layout\": true, \"deps\": [], \"provides\": []}";
    }
    fs::remove(pkg_file);
    ASSERT_NO_THROW(run_build(test_dir));
    {
        const fs::path ex = test_dir / "extract-keep";
        fs::create_directories(ex);
        extract_tar_zst(pkg_file, ex, pkg_file);
        EXPECT_TRUE(fs::is_symlink(ex / "content/bin"))
            << "keep_fs_layout=true 时辅助链接应留在包里（且仍是符号链接）";
        EXPECT_EQ(fs::read_symlink(ex / "content/bin").string(), "usr/bin");
    }

    fs::remove(pkg_file);
}

/** 构建脚本被删掉的时机：三个阶段都跑完后 processed 脚本不应残留。 */
TEST_F(BuilderTest, ProcessedScriptIsRemovedAfterBuild)
{
    ASSERT_NO_THROW(run_build(test_dir));
    EXPECT_FALSE(fs::exists(test_dir / constants::LANK_BUILD_PROCESSED))
        << "替换后的构建脚本残留在构建目录里";
}
