#include <gtest/gtest.h>
#include "builder_executor.hpp"
#include "base/constants.hpp"
#include "base/utils.hpp"
#include "i18n/localization.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class BuilderExecutorTest : public ::testing::Test {
protected:
    fs::path test_dir;

    void SetUp() override {
        init_localization();
        test_dir = fs::current_path() / "tmp_builder_executor_test";
        fs::remove_all(test_dir);
        fs::create_directories(test_dir / "work");
        fs::create_directories(test_dir / "build");
    }

    void TearDown() override {
        fs::remove_all(test_dir);
    }

    void create_file(const fs::path& path, const std::string& content = "") {
        fs::create_directories(path.parent_path());
        std::ofstream f(path);
        f << content;
    }
};

TEST_F(BuilderExecutorTest, DetectSourceTree_NonexistentPath) {
    fs::path missing = test_dir / "nonexistent";
    fs::path result = detect_source_tree(missing);
    EXPECT_EQ(result, missing);
}

TEST_F(BuilderExecutorTest, DetectSourceTree_EmptyDirectory) {
    fs::path empty_dir = test_dir / "empty";
    fs::create_directories(empty_dir);
    fs::path result = detect_source_tree(empty_dir);
    EXPECT_EQ(result, empty_dir);
}

TEST_F(BuilderExecutorTest, DetectSourceTree_SingleSubdirectory) {
    fs::path single = test_dir / "single";
    fs::create_directories(single / "src");
    fs::path result = detect_source_tree(single);
    EXPECT_EQ(result, single / "src");
}

TEST_F(BuilderExecutorTest, DetectSourceTree_MultipleSubdirectories) {
    fs::path multi = test_dir / "multi";
    fs::create_directories(multi / "dir1");
    fs::create_directories(multi / "dir2");
    fs::path result = detect_source_tree(multi);
    EXPECT_EQ(result, multi);  // returns input path, not a single dir
}

TEST_F(BuilderExecutorTest, DetectSourceTree_HasFilesAtRoot) {
    fs::path with_file = test_dir / "withfile";
    fs::create_directories(with_file);
    create_file(with_file / "README.txt");
    fs::path result = detect_source_tree(with_file);
    EXPECT_EQ(result, with_file);  // returns input, not a directory
}

TEST_F(BuilderExecutorTest, ExecuteBuildPhase_FailureThrows) {
    create_file(test_dir / "bad_script.sh", "exit 1\n");
    EXPECT_THROW(
        execute_build_phase("lankebuild_build", test_dir, test_dir / "bad_script.sh"),
        LpkgException
    );
    // 清理（execute_build_phase 的失败路径应已删除临时脚本）
    EXPECT_FALSE(fs::exists(test_dir / "bad_script.sh"));
}

TEST_F(BuilderExecutorTest, ExecuteBuildPhase_ValidScript) {
    create_file(test_dir / "good_script.sh",
        "lankebuild_prepare() { :; }\n"
        "lankebuild_build() { :; }\n"
        "lankebuild_package() { :; }\n");
    EXPECT_NO_THROW(
        execute_build_phase("lankebuild_prepare", test_dir, test_dir / "good_script.sh")
    );
    // 成功路径不删除脚本
    EXPECT_TRUE(fs::exists(test_dir / "good_script.sh"));
}

TEST_F(BuilderExecutorTest, DownloadPrepareSources_WorkSourcesCopy) {
    // 预先放置源文件到 build_dir，使 download_one 走已存在路径
    create_file(test_dir / "build" / "test_source.tar.gz", "fake tarball content");
    create_file(test_dir / "build" / "readme.txt", "readme content");

    // work_sources: 复制到 work_root
    std::vector<std::string> work_sources = {
        (test_dir / "build" / "readme.txt").string()
    };

    auto downloaded = download_and_prepare_sources(
        {},                    // sources 为空
        work_sources,
        test_dir / "build",
        test_dir / "work"
    );

    // readme.txt 应已被复制到 work 目录
    EXPECT_TRUE(fs::exists(test_dir / "work" / "readme.txt"));
}

TEST_F(BuilderExecutorTest, ProcessBuildScript) {
    create_file(test_dir / "script.sh",
        "{PKG_NAME} version {PKG_VER}\n");

    std::map<std::string, std::string> vars = {
        {"{PKG_NAME}", "test-pkg"},
        {"{PKG_VER}", "1.0.0+1"},
    };

    std::string result = process_build_script(test_dir / "script.sh", vars);
    EXPECT_EQ(result, "test-pkg version 1.0.0+1\n");
}
