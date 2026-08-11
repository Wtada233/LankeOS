#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "base/build_defaults.hpp"
#include "base/constants.hpp"
#include "base/utils.hpp"
#include "builder_config.hpp"
#include "builder_executor.hpp"
#include "i18n/localization.hpp"

namespace fs = std::filesystem;

class BuilderExecutorTest : public ::testing::Test
{
protected:
    fs::path test_dir;

    void SetUp() override
    {
        init_localization();
        test_dir = fs::current_path() / "tmp_builder_executor_test";
        fs::remove_all(test_dir);
        fs::create_directories(test_dir / "work");
        fs::create_directories(test_dir / "build");
    }

    void TearDown() override
    {
        fs::remove_all(test_dir);
    }

    void create_file(const fs::path& path, const std::string& content = "")
    {
        fs::create_directories(path.parent_path());
        std::ofstream f(path);
        f << content;
    }
};

TEST_F(BuilderExecutorTest, DetectSourceTree_NonexistentPath)
{
    fs::path missing = test_dir / "nonexistent";
    fs::path result = detect_source_tree(missing);
    EXPECT_EQ(result, missing);
}

TEST_F(BuilderExecutorTest, DetectSourceTree_EmptyDirectory)
{
    fs::path empty_dir = test_dir / "empty";
    fs::create_directories(empty_dir);
    fs::path result = detect_source_tree(empty_dir);
    EXPECT_EQ(result, empty_dir);
}

TEST_F(BuilderExecutorTest, DetectSourceTree_SingleSubdirectory)
{
    fs::path single = test_dir / "single";
    fs::create_directories(single / "src");
    fs::path result = detect_source_tree(single);
    EXPECT_EQ(result, single / "src");
}

TEST_F(BuilderExecutorTest, DetectSourceTree_MultipleSubdirectories)
{
    fs::path multi = test_dir / "multi";
    fs::create_directories(multi / "dir1");
    fs::create_directories(multi / "dir2");
    fs::path result = detect_source_tree(multi);
    EXPECT_EQ(result, multi);  // returns input path, not a single dir
}

TEST_F(BuilderExecutorTest, DetectSourceTree_HasFilesAtRoot)
{
    fs::path with_file = test_dir / "withfile";
    fs::create_directories(with_file);
    create_file(with_file / "README.txt");
    fs::path result = detect_source_tree(with_file);
    EXPECT_EQ(result, with_file);  // returns input, not a directory
}

TEST_F(BuilderExecutorTest, ExecuteBuildPhase_FailureThrows)
{
    create_file(test_dir / "bad_script.sh", "exit 1\n");
    EXPECT_THROW(execute_build_phase("lankebuild_build", test_dir, test_dir / "bad_script.sh"),
                 LpkgException);
    // 清理（execute_build_phase 的失败路径应已删除临时脚本）
    EXPECT_FALSE(fs::exists(test_dir / "bad_script.sh"));
}

TEST_F(BuilderExecutorTest, ExecuteBuildPhase_ValidScript)
{
    create_file(test_dir / "good_script.sh",
                "lankebuild_prepare() { :; }\n"
                "lankebuild_build() { :; }\n"
                "lankebuild_package() { :; }\n");
    EXPECT_NO_THROW(
        execute_build_phase("lankebuild_prepare", test_dir, test_dir / "good_script.sh"));
    // 成功路径不删除脚本
    EXPECT_TRUE(fs::exists(test_dir / "good_script.sh"));
}

TEST_F(BuilderExecutorTest, DownloadPrepareSources_WorkSourcesCopy)
{
    // 预先放置源文件到 build_dir，使 download_one 走已存在路径
    create_file(test_dir / "build" / "test_source.tar.gz", "fake tarball content");
    create_file(test_dir / "build" / "readme.txt", "readme content");

    // work_sources: 复制到 work_root
    std::vector<std::string> work_sources = {(test_dir / "build" / "readme.txt").string()};

    auto downloaded =
        download_and_prepare_sources({},  // sources 为空
                                     work_sources, test_dir / "build", test_dir / "work");

    // readme.txt 应已被复制到 work 目录
    EXPECT_TRUE(fs::exists(test_dir / "work" / "readme.txt"));
}

TEST_F(BuilderExecutorTest, ProcessBuildScript)
{
    create_file(test_dir / "script.sh", "{PKG_NAME} version {PKG_VER}\n");

    std::map<std::string, std::string> vars = {
        {"{PKG_NAME}", "test-pkg"},
        {"{PKG_VER}", "1.0.0+1"},
    };

    std::string result = process_build_script(test_dir / "script.sh", vars);
    EXPECT_EQ(result, "test-pkg version 1.0.0+1\n");
}

// ============================================================================
// 构建标志：默认值（Arch x86-64 generic）与覆盖
// ============================================================================

namespace
{
/// RAII：临时把 Config 根目录切到隔离目录，避免读到宿主机 /etc/lpkg/build.conf
class ConfigRootIsolation
{
public:
    explicit ConfigRootIsolation(const fs::path& root)
        : prev_(Config::instance().root_dir().string())
    {
        Config::instance().set_root_path(root.string());
    }
    ~ConfigRootIsolation()
    {
        Config::instance().set_root_path(prev_);
    }

private:
    std::string prev_;
};
}  // namespace

TEST_F(BuilderExecutorTest, ResolveBuildFlags_DefaultsAreV3Baseline)
{
    ConfigRootIsolation iso(test_dir / "iso_root");  // 无 build.conf → 内置默认
    BuildConfig cfg;                                 // 无任何覆盖
    auto flags = resolve_build_flags(cfg);

    // 默认基线必须是 x86-64-v3，绝不能是 native（发行版打包可移植性）
    EXPECT_NE(flags.cflags.find("-march=x86-64-v3"), std::string::npos);
    EXPECT_EQ(flags.cflags.find("-march=native"), std::string::npos);
    EXPECT_NE(flags.cflags.find("-mtune=generic"), std::string::npos);
    EXPECT_NE(flags.cxxflags.find("-D_GLIBCXX_ASSERTIONS"), std::string::npos);
    EXPECT_NE(flags.ldflags.find("-Wl,-z,now"), std::string::npos);
    EXPECT_EQ(flags.makeflags.rfind("-j", 0), 0);  // -jN
    EXPECT_EQ(flags.ltoflags, "-flto=auto");
}

TEST_F(BuilderExecutorTest, ResolveBuildFlags_OverrideAndLto)
{
    ConfigRootIsolation iso(test_dir / "iso_root");
    BuildConfig cfg;
    cfg.cflags = "-O3 -march=x86-64-v3";
    cfg.ldflags = "-Wl,--as-needed";
    cfg.makeflags = "-j2";
    cfg.lto = true;

    auto flags = resolve_build_flags(cfg);
    EXPECT_EQ(flags.cflags, "-O3 -march=x86-64-v3 -flto=auto");
    // cxxflags 未覆盖 → 默认 + LTO
    EXPECT_NE(flags.cxxflags.find("-march=x86-64"), std::string::npos);
    EXPECT_NE(flags.cxxflags.find("-flto=auto"), std::string::npos);
    EXPECT_EQ(flags.ldflags, "-Wl,--as-needed -flto=auto");
    EXPECT_EQ(flags.makeflags, "-j2");
}

// ============================================================================
// load_build_defaults：读 build.conf（makepkg.conf 风格），缺失回退内置默认
// ============================================================================

TEST_F(BuilderExecutorTest, LoadBuildDefaults_FallbackWhenConfigMissing)
{
    ConfigRootIsolation iso(test_dir / "iso_missing");  // 无 build.conf
    auto d = load_build_defaults();
    EXPECT_EQ(d.cflags, std::string(build_defaults::CFLAGS));
    EXPECT_EQ(d.cxxflags, std::string(build_defaults::CXXFLAGS));
    EXPECT_EQ(d.ldflags, std::string(build_defaults::LDFLAGS));
    EXPECT_EQ(d.makeflags.rfind("-j", 0), 0);
}

TEST_F(BuilderExecutorTest, LoadBuildDefaults_ReadsConfigFile)
{
    fs::path root = test_dir / "iso_file";
    ConfigRootIsolation iso(root);
    fs::create_directories(Config::instance().build_conf().parent_path());
    {
        std::ofstream f(Config::instance().build_conf());
        f << "# 注释行应被忽略\n"
             "CFLAGS=\"-O3 -march=znver4\"\n"
             "LDFLAGS=\"-Wl,--as-needed\"\n"
             "MAKEFLAGS=\"-j$(nproc)\"\n";
    }
    auto d = load_build_defaults();
    EXPECT_EQ(d.cflags, "-O3 -march=znver4");
    EXPECT_EQ(d.ldflags, "-Wl,--as-needed");
    EXPECT_EQ(d.makeflags.rfind("-j", 0), 0);  // $(nproc) 已展开为 -j<N>
    // 未配置的 cxxflags → 内置默认
    EXPECT_EQ(d.cxxflags, std::string(build_defaults::CXXFLAGS));
}
