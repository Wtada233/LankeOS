#include <gtest/gtest.h>
#include "../../main/src/repo/repository.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/base/constants.hpp"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class AggregatedIndexTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path root;
    fs::path index_dir;

    void SetUp() override {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_aggregated_index_test");
        fs::remove_all(suite_work_dir);

        root = suite_work_dir / "root";
        index_dir = suite_work_dir / "mirror" / "x86_64";
        fs::create_directories(index_dir);
        fs::create_directories(root / "etc" / "lpkg");

        Config::instance().set_root_path(root.string());
        Config::instance().set_architecture("x86_64");
        Config::instance().init_filesystem();

        std::ofstream(root / "etc/lpkg/mirror.conf")
            << "file://" << (suite_work_dir / "mirror").string() << "/" << std::endl;
    }

    void write_index(const std::string& content) {
        std::ofstream f(index_dir / "index.txt");
        f << content;
    }

    void TearDown() override {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }
};

TEST_F(AggregatedIndexTest, SingleVersionPerLine) {
    write_index(
        "zlib|1.2.13:abc123::|\n"
        "libfoo|2.0:def456:glibc::|\n"
    );

    Repository repo;
    repo.load_index();

    auto pkg = repo.find_package("zlib");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->name, "zlib");
    EXPECT_EQ(pkg->version, "1.2.13");
    EXPECT_EQ(pkg->sha256, "abc123");

    pkg = repo.find_package("libfoo");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "2.0");
    ASSERT_EQ(pkg->dependencies.size(), 1);
    EXPECT_EQ(pkg->dependencies[0].name, "glibc");
}

TEST_F(AggregatedIndexTest, MultipleVersionsOneLine) {
    // New aggregated format: ver1:hash1:deps;ver2:hash2:deps
    write_index(
        "zlib|1.2.13:abc123::;1.3:def456::|\n"
        "libfoo|2.0:aaa111:glibc>=2.35:;2.1:bbb222:ncurses,glibc>=2.35:|\n"
    );

    Repository repo;
    repo.load_index();

    // Latest version via find_package(name)
    auto pkg = repo.find_package("zlib");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "1.3");
    EXPECT_EQ(pkg->sha256, "def456");

    // Exact version lookup
    pkg = repo.find_package("zlib", "1.2.13");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "1.2.13");
    EXPECT_EQ(pkg->sha256, "abc123");

    // libfoo with deps
    pkg = repo.find_package("libfoo", "2.0");
    ASSERT_TRUE(pkg.has_value());
    ASSERT_EQ(pkg->dependencies.size(), 1);
    EXPECT_EQ(pkg->dependencies[0].name, "glibc");
    ASSERT_EQ(pkg->dependencies[0].constraints.size(), 1);
    EXPECT_EQ(pkg->dependencies[0].constraints[0].op, ">=");
    EXPECT_EQ(pkg->dependencies[0].constraints[0].version, "2.35");

    pkg = repo.find_package("libfoo");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "2.1");
    ASSERT_EQ(pkg->dependencies.size(), 2);
}

TEST_F(AggregatedIndexTest, BestMatchingVersion) {
    write_index(
        "zlib|1.0:aaaa::;1.5:bbbb::;2.0:cccc::|\n"
    );

    Repository repo;
    repo.load_index();

    // >= 1.5 should return 2.0 (latest satisfying)
    auto pkg = repo.find_best_matching_version("zlib", ">=", "1.5");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "2.0");

    // < 2.0 should return 1.5
    pkg = repo.find_best_matching_version("zlib", "<", "2.0");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "1.5");

    // = 1.0 exact
    pkg = repo.find_best_matching_version("zlib", "=", "1.0");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "1.0");
}

TEST_F(AggregatedIndexTest, ProvidesParsedCorrectly) {
    write_index(
        "vim|9.1:aaa:glibc:editor:|\n"
        "busybox|1.36:bbb::sh,shell:|\n"
        "zlib|1.2:ccc::|\n"
    );

    Repository repo;
    repo.load_index();

    // find_provider for "editor"
    auto pkg = repo.find_provider("editor");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->name, "vim");

    // find_provider for "sh"
    pkg = repo.find_provider("sh");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->name, "busybox");
}

TEST_F(AggregatedIndexTest, MixedAggregatedAndSimpleLines) {
    // Mix: some packages single-version, some multi-version
    write_index(
        "zlib|1.2.13:a1::|\n"
        "glibc|2.35:b1::;2.36:b2::|\n"
        "coreutils|9.0:c1::|\n"
        "libfoo|1.0:d1:ncurses:;2.0:d2:ncurses,glibc:|\n"
    );

    Repository repo;
    repo.load_index();

    EXPECT_TRUE(repo.find_package("zlib").has_value());
    EXPECT_TRUE(repo.find_package("glibc").has_value());
    EXPECT_TRUE(repo.find_package("coreutils").has_value());
    EXPECT_TRUE(repo.find_package("libfoo").has_value());

    // glibc should have both versions loaded
    EXPECT_TRUE(repo.find_package("glibc", "2.35").has_value());
    EXPECT_TRUE(repo.find_package("glibc", "2.36").has_value());

    // libfoo with deps on version 2.0
    auto pkg = repo.find_package("libfoo", "2.0");
    ASSERT_TRUE(pkg.has_value());
    ASSERT_EQ(pkg->dependencies.size(), 2);
}

TEST_F(AggregatedIndexTest, ProvidesWithAggregatedVersions) {
    write_index(
        "vim|9.0:aaa:ncurses:editor,text-editor:;9.1:bbb:ncurses,glibc:editor,text-editor:|\n"
    );

    Repository repo;
    repo.load_index();

    // Both versions loaded
    EXPECT_TRUE(repo.find_package("vim", "9.0").has_value());
    EXPECT_TRUE(repo.find_package("vim", "9.1").has_value());

    // Provides set on both versions (line-level, shared)
    auto pkg = repo.find_provider("editor");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->name, "vim");

    pkg = repo.find_provider("text-editor");
    ASSERT_TRUE(pkg.has_value());
}

TEST_F(AggregatedIndexTest, EmptyIndex) {
    write_index("");

    Repository repo;
    repo.load_index();

    EXPECT_FALSE(repo.find_package("anything").has_value());
}

TEST_F(AggregatedIndexTest, CommentLines) {
    write_index(
        "# this is a comment\n"
        "zlib|1.0:a::|\n"
        "# another comment\n"
        "libfoo|2.0:b::|\n"
    );

    Repository repo;
    repo.load_index();

    EXPECT_TRUE(repo.find_package("zlib").has_value());
    EXPECT_TRUE(repo.find_package("libfoo").has_value());
}

TEST_F(AggregatedIndexTest, VersionSortingAcrossAggregatedLine) {
    write_index(
        "zlib|1.0:a:;1.5:b:;0.9:c:\n"
    );

    Repository repo;
    repo.load_index();

    // find_package(name) without version should return latest
    auto pkg = repo.find_package("zlib");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "1.5");
}
