/**
 * test_removal_and_symlinks.cpp
 *
 * Comprehensive test suite for package removal, directory symlink handling,
 * file ownership tracking, and the shared-file check logic.
 *
 * Fix strategy: builder.cpp cleans up USR-Merge compatibility symlinks
 * (bin → usr/bin, lib → usr/lib, etc.) from the staging root BEFORE
 * packing, so they never enter the .lpkg archive and thus are never
 * registered as owned by every package.  Package-internal directory
 * symlinks (e.g. jvm/conf → /etc/java-21-openjdk) remain tracked as
 * package artifacts — they ARE part of the package and should be
 * cleaned up on removal.
 */

#include <gtest/gtest.h>
#include "../main/src/pkg/package_manager.hpp"
#include "../main/src/pkg/install_common.hpp"
#include "../main/src/archive/packer.hpp"
#include "../main/src/archive/archive.hpp"
#include "../main/src/config/config.hpp"
#include "../main/src/base/utils.hpp"
#include "../main/src/i18n/localization.hpp"
#include "../main/src/base/constants.hpp"
#include "../main/src/db/cache.hpp"
#include "../main/src/repo/repository.hpp"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// =========================================================================
// Fixture: set up isolated test root with Config + Cache ready
// =========================================================================

class RemovalSymlinkTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        Config::instance().set_force_overwrite_mode(false);
        Config::instance().set_no_hooks_mode(false);
        Config::instance().set_no_deps_mode(false);
        init_localization();

        suite_work_dir = fs::absolute("tmp_removal_symlink_test");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";

        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override {
        Config::instance().set_root_path("/");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
    }

    // Helper: build a minimal .lpkg archive from explicit content
    std::string create_package_with_content(
        const std::string& name,
        const std::string& version,
        const std::map<std::string, std::string>& content_map,
        const std::map<std::string, std::string>& dir_symlinks = {},
        const std::vector<std::string>& deps = {},
        const std::vector<std::string>& provides = {},
        const std::vector<std::string>& needed_so = {})
    {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::remove_all(work_dir);
        fs::create_directories(work_dir / "root");

        for (const auto& [path, data] : content_map) {
            fs::path full = work_dir / "root" / path;
            ensure_dir_exists(full.parent_path());
            std::ofstream f(full);
            f << data;
        }

        for (const auto& [src, target] : dir_symlinks) {
            fs::path link_path = work_dir / "root" / src;
            ensure_dir_exists(link_path.parent_path());
            if (fs::exists(link_path)) fs::remove_all(link_path);
            fs::create_directory_symlink(target, link_path);
        }

        std::string pkg_filename = name + "-" + version + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_filename).string();

        pack_package(pkg_path, work_dir.string(), name, version,
                     deps, provides, "Man page for " + name, needed_so);
        fs::remove_all(work_dir);
        return pkg_path;
    }

    // Helper: create a simple package with one bin file
    std::string create_simple_package(const std::string& name,
                                       const std::string& version = "1.0",
                                       const std::vector<std::string>& deps = {},
                                       const std::vector<std::string>& provides = {})
    {
        return create_package_with_content(
            name, version,
            {{std::string("usr/bin/") + name, "#!/bin/sh\necho " + name}},
            {}, deps, provides);
    }

    bool file_installed(const std::string& logical_path) {
        return fs::exists(test_root / fs::path(logical_path).relative_path());
    }

    bool is_registered(const std::string& name) {
        return Cache::instance().is_installed(name);
    }
};

// =========================================================================
// SECTION 1: scan_content_files unit tests
// =========================================================================

class ScanContentTest : public ::testing::Test {
protected:
    fs::path temp_dir;

    void SetUp() override {
        temp_dir = fs::absolute("lpkg_scan_test");
        fs::remove_all(temp_dir);
        fs::create_directories(temp_dir);
    }

    void TearDown() override {
        fs::remove_all(temp_dir);
    }

    std::vector<std::string> scan() {
        return detail::scan_content_files(temp_dir);
    }
};

TEST_F(ScanContentTest, IncludesRegularFiles) {
    std::ofstream(temp_dir / "file1.txt") << "hello";
    fs::create_directories(temp_dir / "sub");
    std::ofstream(temp_dir / "sub" / "file2.txt") << "world";

    auto result = scan();
    // Entries: file1.txt, sub/, sub/file2.txt
    EXPECT_EQ(result.size(), 3);
    EXPECT_NE(std::find(result.begin(), result.end(), "file1.txt"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "sub/"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "sub/file2.txt"), result.end());
}

TEST_F(ScanContentTest, IncludesRegularDirectories) {
    fs::create_directories(temp_dir / "empty_dir");
    fs::create_directories(temp_dir / "a" / "b" / "c");

    auto result = scan();
    // Directories are now tracked: a/, a/b/, a/b/c/, empty_dir/
    EXPECT_EQ(result.size(), 4);
    EXPECT_NE(std::find(result.begin(), result.end(), "empty_dir/"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "a/"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "a/b/"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "a/b/c/"), result.end());
}

TEST_F(ScanContentTest, IncludesSymlinksToRegularFiles) {
    std::ofstream(temp_dir / "target.txt") << "real";
    fs::create_symlink("target.txt", temp_dir / "link.txt");
    auto result = scan();
    ASSERT_EQ(result.size(), 2);
    EXPECT_NE(std::find(result.begin(), result.end(), "link.txt"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "target.txt"), result.end());
}

TEST_F(ScanContentTest, IncludesSymlinksToDirectories) {
    fs::create_directories(temp_dir / "real_dir");
    std::ofstream(temp_dir / "real_dir" / "inner.txt") << "deep";
    fs::create_directory_symlink("real_dir", temp_dir / "link_to_dir");

    auto result = scan();
    // The symlink itself IS a package artifact + directory + file inside
    ASSERT_EQ(result.size(), 3);
    EXPECT_NE(std::find(result.begin(), result.end(), "link_to_dir"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "real_dir/"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "real_dir/inner.txt"), result.end());
}

TEST_F(ScanContentTest, MixedContent) {
    fs::create_directories(temp_dir / "lib");
    std::ofstream(temp_dir / "lib" / "libfoo.so.1") << "elf";
    fs::create_directories(temp_dir / "regular_dir");
    fs::create_symlink("libfoo.so.1", temp_dir / "lib" / "libfoo.so");
    fs::create_directory_symlink("lib", temp_dir / "lib64");

    auto result = scan();
    // Entries: lib/, lib/libfoo.so.1, lib/libfoo.so, regular_dir/, lib64
    ASSERT_EQ(result.size(), 5);
    EXPECT_NE(std::find(result.begin(), result.end(), "lib/"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "lib/libfoo.so.1"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "lib/libfoo.so"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "regular_dir/"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "lib64"), result.end());
}

TEST_F(ScanContentTest, ReturnsEmptyOnEmptyDirectory) {
    EXPECT_TRUE(scan().empty());
}

TEST_F(ScanContentTest, ReturnsDirectoriesForOnlySubdirs) {
    fs::create_directories(temp_dir / "a" / "b" / "c");
    auto result = scan();
    // Directories are tracked: a/, a/b/, a/b/c/
    ASSERT_EQ(result.size(), 3);
    EXPECT_NE(std::find(result.begin(), result.end(), "a/"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "a/b/"), result.end());
    EXPECT_NE(std::find(result.begin(), result.end(), "a/b/c/"), result.end());
}

// =========================================================================
// SECTION 2: File database unit tests
// =========================================================================

TEST_F(RemovalSymlinkTest, FileOwnerRegistrationAndQuery) {
    Cache::instance().add_file_owner("/usr/bin/foo", "pkgA");
    Cache::instance().add_file_owner("/usr/bin/bar", "pkgA");
    Cache::instance().add_file_owner("/usr/bin/baz", "pkgB");

    auto owners_foo = Cache::instance().get_file_owners("/usr/bin/foo");
    ASSERT_EQ(owners_foo.size(), 1);
    EXPECT_TRUE(owners_foo.contains("pkgA"));

    EXPECT_TRUE(Cache::instance().is_file_owned_by("/usr/bin/foo", "pkgA"));
    EXPECT_FALSE(Cache::instance().is_file_owned_by("/usr/bin/foo", "pkgB"));

    Cache::instance().add_file_owner("/usr/bin/shared", "pkgA");
    Cache::instance().add_file_owner("/usr/bin/shared", "pkgB");
    auto owners_shared = Cache::instance().get_file_owners("/usr/bin/shared");
    ASSERT_EQ(owners_shared.size(), 2);
    EXPECT_TRUE(owners_shared.contains("pkgA"));
    EXPECT_TRUE(owners_shared.contains("pkgB"));
}

TEST_F(RemovalSymlinkTest, RemoveFileOwner) {
    Cache::instance().add_file_owner("/usr/bin/foo", "pkgA");
    Cache::instance().remove_file_owner("/usr/bin/foo", "pkgA");
    EXPECT_TRUE(Cache::instance().get_file_owners("/usr/bin/foo").empty());
}

TEST_F(RemovalSymlinkTest, RemoveFileOwnerOnlyClearsOnePackage) {
    Cache::instance().add_file_owner("/usr/bin/shared", "pkgA");
    Cache::instance().add_file_owner("/usr/bin/shared", "pkgB");
    Cache::instance().remove_file_owner("/usr/bin/shared", "pkgA");
    auto owners = Cache::instance().get_file_owners("/usr/bin/shared");
    ASSERT_EQ(owners.size(), 1);
    EXPECT_TRUE(owners.contains("pkgB"));
}

TEST_F(RemovalSymlinkTest, GetPackageFilesCollectsAllOwnedFiles) {
    Cache::instance().add_file_owner("/usr/bin/a", "pkgA");
    Cache::instance().add_file_owner("/usr/bin/b", "pkgA");
    Cache::instance().add_file_owner("/usr/bin/c", "pkgB");

    auto files = Cache::instance().get_package_files("pkgA");
    ASSERT_EQ(files.size(), 2);
    EXPECT_TRUE(files.contains("/usr/bin/a"));
    EXPECT_TRUE(files.contains("/usr/bin/b"));
}

TEST_F(RemovalSymlinkTest, RemoveFileOwnerFromNonExistentEntry) {
    EXPECT_NO_THROW(Cache::instance().remove_file_owner("/does/not/exist", "pkgX"));
}

// =========================================================================
// SECTION 3: Package installation — file tracking verification
// =========================================================================

TEST_F(RemovalSymlinkTest, InstallRegistersRegularFiles) {
    std::string pkg = create_simple_package("hello-pkg", "1.0");
    ASSERT_NO_THROW(install_packages({pkg}));
    write_cache();

    EXPECT_TRUE(is_registered("hello-pkg"));
    EXPECT_TRUE(file_installed("usr/bin/hello-pkg"));
    EXPECT_TRUE(Cache::instance().get_file_owners("/usr/bin/hello-pkg").contains("hello-pkg"));
}

TEST_F(RemovalSymlinkTest, InstallRegistersDirectorySymlinks) {
    // Package-internal dir symlinks (like jvm/conf → /etc/java) ARE package
    // artifacts and must be tracked in the file database.
    std::string pkg = create_package_with_content(
        "dirsym-pkg", "1.0",
        {{"usr/bin/dirsym-pkg", "#!/bin/sh\necho works"}},
        {{"bin", "usr/bin"}});

    ASSERT_NO_THROW(install_packages({pkg}));
    write_cache();

    // The symlink /bin IS a package artifact — must be registered
    EXPECT_TRUE(Cache::instance().get_file_owners("/bin").contains("dirsym-pkg"));
    // The regular file is also registered
    EXPECT_TRUE(Cache::instance().get_file_owners("/usr/bin/dirsym-pkg").contains("dirsym-pkg"));
}

TEST_F(RemovalSymlinkTest, DirSymlinksCausesSharedFileBlockWithoutBuilderFix) {
    // Without the builder fix (i.e. when pack_package is called directly),
    // packages contain USR-Merge dir symlinks, causing removal to fail.
    std::string pkg1 = create_package_with_content(
        "blocked-pkg1", "1.0",
        {{"usr/bin/p1", "data"}},
        {{"bin", "usr/bin"}});
    std::string pkg2 = create_package_with_content(
        "blocked-pkg2", "1.0",
        {{"usr/bin/p2", "data"}},
        {{"bin", "usr/bin"}});

    install_packages({pkg1});
    write_cache();

    // Second install fails due to file conflict on /bin symlink
    // (conflict detection catches it at install time)
    ASSERT_THROW(install_packages({pkg2}), LpkgException);

    // But if both WERE somehow installed (e.g. via direct DB manipulation):
    // removal should be BLOCKED by shared /bin symlink
    Cache::instance().add_file_owner("/bin", "blocked-pkg2");
    Cache::instance().add_installed("blocked-pkg2", "1.0", false);
    EXPECT_THROW(remove_package("blocked-pkg1", false), LpkgException);

    // --force bypasses the check
    EXPECT_NO_THROW(remove_package("blocked-pkg1", true));
    // Clean up
    remove_package("blocked-pkg2", true);
    write_cache();
}

TEST_F(RemovalSymlinkTest, ForceRemoveBypassesSharedDirSymlinks) {
    Cache::instance().add_file_owner("/bin", "pkgA");
    Cache::instance().add_file_owner("/bin", "pkgB");
    Cache::instance().add_installed("pkgA", "1.0", true);
    Cache::instance().add_installed("pkgB", "2.0", true);

    EXPECT_NO_THROW(remove_package("pkgA", true));
}

// =========================================================================
// SECTION 4: Package removal — real file sharing
// =========================================================================

TEST_F(RemovalSymlinkTest, RemovePackageWithNoSharedFiles) {
    std::string pkg = create_simple_package("alone-pkg", "1.0");
    install_packages({pkg});
    write_cache();

    EXPECT_NO_THROW(remove_package("alone-pkg", false));
    write_cache();
    EXPECT_FALSE(is_registered("alone-pkg"));
    EXPECT_FALSE(file_installed("usr/bin/alone-pkg"));
}

TEST_F(RemovalSymlinkTest, RemoveBlockedOnSharedRegularFile) {
    Cache::instance().add_file_owner("/usr/bin/shared-bin", "pkgA");
    Cache::instance().add_file_owner("/usr/bin/shared-bin", "pkgB");
    Cache::instance().add_file_owner("/usr/bin/a-only", "pkgA");
    Cache::instance().add_installed("pkgA", "1.0", true);
    Cache::instance().add_installed("pkgB", "2.0", true);

    EXPECT_THROW(remove_package("pkgA", false), LpkgException);
}

TEST_F(RemovalSymlinkTest, ForceRemoveBypassesSharedFileCheck) {
    Cache::instance().add_file_owner("/usr/bin/shared-bin", "pkgA");
    Cache::instance().add_file_owner("/usr/bin/shared-bin", "pkgB");
    Cache::instance().add_installed("pkgA", "1.0", false);
    Cache::instance().add_installed("pkgB", "2.0", false);

    EXPECT_NO_THROW(remove_package("pkgA", true));
    write_cache();
}

TEST_F(RemovalSymlinkTest, RemoveNonExistentPackage) {
    EXPECT_NO_THROW(remove_package("nonexistent-pkg", false));
    EXPECT_NO_THROW(remove_package("nonexistent-pkg", true));
}

// =========================================================================
// SECTION 5: Removal blocked by reverse dependencies
// =========================================================================

TEST_F(RemovalSymlinkTest, RemoveBlockedOnReverseDeps) {
    // remove_package does not throw when blocked by reverse deps;
    // it logs the info and returns. Verify by checking the package
    // is still installed after removal attempt.
    Cache::instance().add_installed("lib-base", "1.0", true);
    Cache::instance().add_installed("consumer-app", "1.0", false);

    // Write the dep file for consumer-app so ensure_reverse_deps finds it
    ensure_dir_exists(Config::instance().dep_dir());
    {
        std::ofstream f(Config::instance().dep_dir() / "consumer-app");
        f << "lib-base\n";
    }
    write_cache();
    Cache::instance().load();

    EXPECT_NO_THROW(remove_package("lib-base", false));
    EXPECT_TRUE(is_registered("lib-base")) << "lib-base should not have been removed";
}

TEST_F(RemovalSymlinkTest, RemoveBlockedOnVirtualProvides) {
    Cache::instance().add_installed("openssl-pkg", "1.0", true);
    Cache::instance().add_installed("curl-pkg", "1.0", false);

    ensure_dir_exists(Config::instance().dep_dir());
    {
        std::ofstream f(Config::instance().dep_dir() / "curl-pkg");
        f << "libssl.so.1\n";
    }
    Cache::instance().add_provider("libssl.so.1", "openssl-pkg");
    write_cache();
    Cache::instance().load();

    EXPECT_NO_THROW(remove_package("openssl-pkg", false));
    EXPECT_TRUE(is_registered("openssl-pkg")) << "openssl-pkg should not have been removed";
}

// =========================================================================
// SECTION 6: Autoremove
// =========================================================================

TEST_F(RemovalSymlinkTest, AutoremoveFindsOrphans) {
    // app-a is installed explicitly (held=true); lib-b is its auto-installed
    // dependency (held=false). After removing app-a, lib-b should be orphaned.
    Cache::instance().add_installed("app-a", "1.0", true);
    Cache::instance().add_installed("lib-b", "1.0", false);

    // Write dep file so dependency tracking works
    ensure_dir_exists(Config::instance().dep_dir());
    {
        std::ofstream f(Config::instance().dep_dir() / "app-a");
        f << "lib-b\n";
    }
    write_cache();
    Cache::instance().load();

    EXPECT_TRUE(is_registered("app-a"));
    EXPECT_TRUE(is_registered("lib-b"));

    // Remove app-a explicitly — uses force to bypass reverse dep checks
    remove_package("app-a", true);
    write_cache();
    EXPECT_FALSE(is_registered("app-a"));

    // Now autoremove should find lib-b as an orphan (not held, not required)
    autoremove();
    write_cache();
    EXPECT_FALSE(is_registered("lib-b")) << "lib-b should have been autoremoved as an orphan";
}

// =========================================================================
// SECTION 7: File conflict detection during install
// =========================================================================

TEST_F(RemovalSymlinkTest, ConflictDetectedOnTwoPackagesSameFile) {
    std::string pkg1 = create_simple_package("first-pkg", "1.0");
    install_packages({pkg1});
    write_cache();

    // Second package claims the SAME file path
    std::string pkg2 = create_package_with_content(
        "second-pkg", "1.0",
        {{"usr/bin/first-pkg", "#!/bin/sh\necho collision"}});

    EXPECT_THROW(install_packages({pkg2}), LpkgException);
}

TEST_F(RemovalSymlinkTest, ForceOverwriteBypassesConflict) {
    std::string pkg1 = create_simple_package("pkg1", "1.0");
    install_packages({pkg1});
    write_cache();

    std::string pkg2 = create_package_with_content(
        "pkg2", "1.0",
        {{"usr/bin/pkg1", "#!/bin/sh\necho different"}});

    Config::instance().set_force_overwrite_mode(true);
    EXPECT_NO_THROW(install_packages({pkg2}));
    Config::instance().set_force_overwrite_mode(false);
}

TEST_F(RemovalSymlinkTest, NoConflictWhenFileAlreadyOwnedBySelf) {
    std::string pkg = create_simple_package("self-pkg", "1.0");
    install_packages({pkg});
    write_cache();
    EXPECT_NO_THROW(install_packages({pkg}));
}

// =========================================================================
// SECTION 8: Shared file error message verification
// =========================================================================

TEST_F(RemovalSymlinkTest, SharedFileErrorShowsActualPackageNames) {
    Cache::instance().add_file_owner("/usr/bin/overlap", "pkgX");
    Cache::instance().add_file_owner("/usr/bin/overlap", "pkgY");
    Cache::instance().add_installed("pkgX", "1.0", true);
    Cache::instance().add_installed("pkgY", "1.0", false);

    try {
        remove_package("pkgX", false);
        FAIL() << "Expected LpkgException was not thrown";
    } catch (const LpkgException& e) {
        std::string msg = e.what();
        EXPECT_TRUE(msg.find("pkgY") != std::string::npos)
            << "Error should contain actual package name (pkgY), got: " << msg;
    }
}

// =========================================================================
// SECTION 9: Package query verification
// =========================================================================

TEST_F(RemovalSymlinkTest, QueryShowsPackageFiles) {
    Cache::instance().add_file_owner("/usr/bin/query-test", "query-test");
    Cache::instance().add_file_owner("/etc/query-test.conf", "query-test");
    Cache::instance().add_file_owner("/usr/bin/other", "other-pkg");

    auto files = Cache::instance().get_package_files("query-test");
    EXPECT_TRUE(files.contains("/usr/bin/query-test"));
    EXPECT_TRUE(files.contains("/etc/query-test.conf"));
    EXPECT_FALSE(files.contains("/usr/bin/other"));
}

// =========================================================================
// SECTION 10: Upgrade
// =========================================================================

TEST_F(RemovalSymlinkTest, UpgradeRemovesObsoleteFile) {
    std::string pkg_v1 = create_package_with_content(
        "upgrade-me", "1.0",
        {{"usr/bin/upgrade-me", "v1 binary"},
         {"usr/bin/old-utility-v1", "legacy"}});
    install_packages({pkg_v1});
    write_cache();

    std::string pkg_v2 = create_package_with_content(
        "upgrade-me", "2.0",
        {{"usr/bin/upgrade-me", "v2 binary"}});
    install_packages({pkg_v2});
    write_cache();

    EXPECT_FALSE(file_installed("usr/bin/old-utility-v1"));
    EXPECT_TRUE(file_installed("usr/bin/upgrade-me"));
}

// =========================================================================
// SECTION 11: Edge cases
// =========================================================================

TEST_F(RemovalSymlinkTest, InstallThenRemoveCycle) {
    std::string pkg_a = create_simple_package("cycle-a", "1.0");
    std::string pkg_b = create_simple_package("cycle-b", "1.0");

    install_packages({pkg_a});
    install_packages({pkg_b});
    write_cache();

    EXPECT_TRUE(is_registered("cycle-a"));
    EXPECT_TRUE(is_registered("cycle-b"));

    EXPECT_NO_THROW(remove_package("cycle-a", false));
    write_cache();
    EXPECT_FALSE(is_registered("cycle-a"));
    EXPECT_TRUE(is_registered("cycle-b"));

    EXPECT_NO_THROW(remove_package("cycle-b", false));
    write_cache();
    EXPECT_FALSE(is_registered("cycle-b"));
}

TEST_F(RemovalSymlinkTest, MultipleInstallsSamePackage) {
    std::string pkg = create_simple_package("twice-pkg", "1.0");
    install_packages({pkg});
    write_cache();
    install_packages({pkg});
    write_cache();

    EXPECT_TRUE(is_registered("twice-pkg"));
    EXPECT_NO_THROW(remove_package("twice-pkg", false));
    write_cache();
    EXPECT_FALSE(is_registered("twice-pkg"));
}

TEST_F(RemovalSymlinkTest, ReinstallWorksAfterRemoval) {
    std::string pkg = create_simple_package("reinstall-test", "1.0");

    install_packages({pkg});
    write_cache();
    EXPECT_TRUE(is_registered("reinstall-test"));

    remove_package("reinstall-test", false);
    write_cache();
    EXPECT_FALSE(is_registered("reinstall-test"));

    install_packages({pkg});
    write_cache();
    EXPECT_TRUE(is_registered("reinstall-test"));
    EXPECT_TRUE(file_installed("usr/bin/reinstall-test"));

    remove_package("reinstall-test", true);
    write_cache();
}

TEST_F(RemovalSymlinkTest, RemoveWithNoHooks) {
    Config::instance().set_no_hooks_mode(true);
    std::string pkg = create_simple_package("no-hooks-pkg", "1.0");
    install_packages({pkg});
    write_cache();

    EXPECT_NO_THROW(remove_package("no-hooks-pkg", false));
    write_cache();
    EXPECT_FALSE(is_registered("no-hooks-pkg"));
    Config::instance().set_no_hooks_mode(false);
}

TEST_F(RemovalSymlinkTest, ForceRemovePreservesOtherPackagesFiles) {
    Cache::instance().add_file_owner("/usr/bin/shared", "pkgA");
    Cache::instance().add_file_owner("/usr/bin/shared", "pkgB");
    Cache::instance().add_file_owner("/usr/bin/a-only", "pkgA");
    Cache::instance().add_installed("pkgA", "1.0", true);
    Cache::instance().add_installed("pkgB", "1.0", false);

    EXPECT_NO_THROW(remove_package("pkgA", true));

    auto owners = Cache::instance().get_file_owners("/usr/bin/shared");
    EXPECT_TRUE(owners.contains("pkgB"));
    EXPECT_FALSE(owners.contains("pkgA"));
}

// =========================================================================
// SECTION 12: Packer verification
// =========================================================================

TEST_F(RemovalSymlinkTest, PackerIncludesDirectorySymlinks) {
    // pack_package does NOT filter directory symlinks — that's the
    // builder's job. Verify that raw pack_package includes them.
    fs::path work_dir = suite_work_dir / "packer_test_raw";
    fs::remove_all(work_dir);
    fs::create_directories(work_dir / "root" / "usr" / "bin");
    std::ofstream(work_dir / "root" / "usr" / "bin" / "real_bin") << "content";
    fs::create_directory_symlink("usr/bin", work_dir / "root" / "bin");

    std::string pkg_path = (pkg_dir / "packer-test.lpkg").string();
    pack_package(pkg_path, work_dir.string(), "packer-test", "1.0");

    fs::path extract_dir = suite_work_dir / "packer_extract_raw";
    fs::remove_all(extract_dir);
    extract_tar_zst(pkg_path, extract_dir);

    // The directory symlink IS in the archive (packer doesn't filter)
    EXPECT_TRUE(fs::exists(extract_dir / "content" / "bin"))
        << "Raw pack_package should include directory symlinks in archives";
    EXPECT_TRUE(fs::exists(extract_dir / "content" / "usr" / "bin" / "real_bin"));

    fs::remove_all(work_dir);
    fs::remove_all(extract_dir);
}

TEST_F(RemovalSymlinkTest, PackerIncludesSymlinksToRegularFiles) {
    fs::path work_dir = suite_work_dir / "packer_sym_test";
    fs::remove_all(work_dir);
    fs::create_directories(work_dir / "root" / "usr" / "lib");
    std::ofstream(work_dir / "root" / "usr" / "lib" / "libfoo.so.1") << "elf";
    fs::create_symlink("libfoo.so.1", work_dir / "root" / "usr" / "lib" / "libfoo.so");

    std::string pkg_path = (pkg_dir / "packer-sym-test.lpkg").string();
    pack_package(pkg_path, work_dir.string(), "packer-sym-test", "1.0");

    fs::path extract_dir = suite_work_dir / "packer_sym_extract";
    fs::remove_all(extract_dir);
    extract_tar_zst(pkg_path, extract_dir);

    EXPECT_TRUE(fs::exists(extract_dir / "content" / "usr" / "lib" / "libfoo.so"));
    EXPECT_TRUE(fs::exists(extract_dir / "content" / "usr" / "lib" / "libfoo.so.1"));
    EXPECT_TRUE(fs::is_symlink(extract_dir / "content" / "usr" / "lib" / "libfoo.so"));

    fs::remove_all(work_dir);
    fs::remove_all(extract_dir);
}

TEST_F(RemovalSymlinkTest, PackedWithCleanupHasNoDirSymlinks) {
    // Simulate what the builder does: create dir symlinks, then clean them up.
    fs::path work_dir = suite_work_dir / "packer_clean_test";
    fs::remove_all(work_dir);
    fs::create_directories(work_dir / "root" / "usr" / "bin");
    std::ofstream(work_dir / "root" / "usr" / "bin" / "real_bin") << "content";
    fs::create_directory_symlink("usr/bin", work_dir / "root" / "bin");

    // Clean up the dir symlink (as builder does before packing)
    std::error_code ec;
    fs::remove(work_dir / "root" / "bin", ec);

    std::string pkg_path = (pkg_dir / "packer-clean-test.lpkg").string();
    pack_package(pkg_path, work_dir.string(), "packer-clean-test", "1.0");

    fs::path extract_dir = suite_work_dir / "packer_clean_extract";
    fs::remove_all(extract_dir);
    extract_tar_zst(pkg_path, extract_dir);

    // After clean-up, dir symlink should NOT be in archive
    EXPECT_FALSE(fs::exists(extract_dir / "content" / "bin"))
        << "After builder cleanup, dir symlink should not be in archive";
    EXPECT_TRUE(fs::exists(extract_dir / "content" / "usr" / "bin" / "real_bin"));

    fs::remove_all(work_dir);
    fs::remove_all(extract_dir);
}

// =========================================================================
// SECTION 13: Removal with needed_so dependencies
// =========================================================================

TEST_F(RemovalSymlinkTest, NeededSoDepsBlockRemoval) {
    // remove_package returns early if reverse deps exist, it doesn't throw.
    Cache::instance().add_installed("glibc", "2.35", true);
    Cache::instance().add_installed("app-using-libc", "1.0", false);

    // Write needed_so file
    ensure_dir_exists(Config::instance().needed_so_dir());
    {
        std::ofstream f(Config::instance().needed_so_dir() / "app-using-libc");
        f << "libc.so.6\n";
    }
    // Write dep file so ensure_reverse_deps finds it
    ensure_dir_exists(Config::instance().dep_dir());
    {
        std::ofstream f(Config::instance().dep_dir() / "app-using-libc");
        f << "libc.so.6\n";
    }

    Cache::instance().add_provider("libc.so.6", "glibc");
    write_cache();
    Cache::instance().load();

    EXPECT_NO_THROW(remove_package("glibc", false));
    EXPECT_TRUE(is_registered("glibc")) << "glibc should not have been removed";
}

// =========================================================================
// SECTION 14: Package with dependency tree
// =========================================================================

TEST_F(RemovalSymlinkTest, DependTreeRemoval) {
    std::string leaf = create_simple_package("dep-leaf", "1.0");
    std::string mid = create_simple_package("dep-mid", "1.0", {"dep-leaf"});
    std::string top = create_simple_package("dep-top", "1.0", {"dep-mid"});

    install_packages({leaf});
    install_packages({mid});
    install_packages({top});
    write_cache();

    // Write dep files so reverse deps work
    ensure_dir_exists(Config::instance().dep_dir());
    {
        std::ofstream f(Config::instance().dep_dir() / "dep-mid");
        f << "dep-leaf\n";
    }
    {
        std::ofstream f(Config::instance().dep_dir() / "dep-top");
        f << "dep-mid\n";
    }
    Cache::instance().load();

    // Can't remove mid because top depends on it -> returns early (no throw)
    EXPECT_NO_THROW(remove_package("dep-mid", false));
    EXPECT_TRUE(is_registered("dep-mid"));

    // Can remove top
    EXPECT_NO_THROW(remove_package("dep-top", false));
    write_cache();
    EXPECT_FALSE(is_registered("dep-top"));

    // Now mid is free
    EXPECT_NO_THROW(remove_package("dep-mid", false));
    write_cache();
    EXPECT_FALSE(is_registered("dep-mid"));

    // And leaf
    EXPECT_NO_THROW(remove_package("dep-leaf", false));
    write_cache();
    EXPECT_FALSE(is_registered("dep-leaf"));
}

// =========================================================================
// SECTION 15: Symlink-to-file tracking
// =========================================================================

TEST_F(RemovalSymlinkTest, SymlinkToFileIsRegistered) {
    fs::path work_dir = suite_work_dir / "lib_sym_work";
    fs::remove_all(work_dir);
    fs::create_directories(work_dir / "root" / "usr" / "lib");
    std::ofstream(work_dir / "root" / "usr" / "lib" / "libbar.so.1") << "elf";
    fs::create_symlink("libbar.so.1", work_dir / "root" / "usr" / "lib" / "libbar.so");

    std::string pkg_path = (pkg_dir / "lib-sym-pkg.lpkg").string();
    pack_package(pkg_path, work_dir.string(), "lib-sym-pkg", "1.0");
    fs::remove_all(work_dir);

    install_packages({pkg_path});
    write_cache();

    auto files = Cache::instance().get_package_files("lib-sym-pkg");
    EXPECT_TRUE(files.contains("/usr/lib/libbar.so.1")) << "regular file must be tracked";
    EXPECT_TRUE(files.contains("/usr/lib/libbar.so"))   << "symlink to regular file must be tracked";

    remove_package("lib-sym-pkg", true);
    write_cache();
}

// =========================================================================
// SECTION 16: Query file finds owner
// =========================================================================

TEST_F(RemovalSymlinkTest, QueryFileFindsOwner) {
    std::string pkg = create_simple_package("query-file-pkg", "1.0");
    install_packages({pkg});
    write_cache();

    auto owners = Cache::instance().get_file_owners("/usr/bin/query-file-pkg");
    EXPECT_TRUE(owners.contains("query-file-pkg"));

    auto no_owners = Cache::instance().get_file_owners("/usr/bin/nonexistent");
    EXPECT_TRUE(no_owners.empty());
}

// =========================================================================
// SECTION 17: While-true convergence loop
// =========================================================================

TEST_F(RemovalSymlinkTest, ConsistencyCheckDetectsBrokenDeps) {
    // app depends on lib >= 2.0 (written in dep file)
    Cache::instance().add_installed("app", "1.0", true);
    Cache::instance().add_installed("lib", "2.0", true);
    Cache::instance().add_file_owner("/usr/bin/app", "app");
    Cache::instance().add_file_owner("/usr/lib/lib.so", "lib");
    ensure_dir_exists(Config::instance().dep_dir());
    {
        std::ofstream f(Config::instance().dep_dir() / "app");
        f << "lib >= 2.0\n";
    }
    write_cache();
    Cache::instance().load();

    // Plan: lib at 1.0 — too old for app
    std::map<std::string, InstallPlan> plan;
    InstallPlan p;
    p.name = "lib";
    p.actual_version = "1.0";
    plan["lib"] = p;

    // check_plan_consistency should find app is broken
    auto broken = detail::check_plan_consistency(plan);
    EXPECT_TRUE(broken.contains("app")) << "app should be broken by lib downgrade";
}

TEST_F(RemovalSymlinkTest, ConsistencyCheckOkWhenSatisfied) {
    Cache::instance().add_installed("app", "1.0", true);
    Cache::instance().add_installed("lib", "2.0", true);
    Cache::instance().add_file_owner("/usr/bin/app", "app");
    Cache::instance().add_file_owner("/usr/lib/lib.so", "lib");
    ensure_dir_exists(Config::instance().dep_dir());
    {
        std::ofstream f(Config::instance().dep_dir() / "app");
        f << "lib >= 1.0\n";
    }
    write_cache();
    Cache::instance().load();

    // Plan: lib at 2.0 — satisfies app's constraint
    std::map<std::string, InstallPlan> plan;
    InstallPlan p;
    p.name = "lib";
    p.actual_version = "2.0";
    plan["lib"] = p;

    auto broken = detail::check_plan_consistency(plan);
    EXPECT_FALSE(broken.contains("app")) << "app should NOT be broken by lib 2.0";
}

TEST_F(RemovalSymlinkTest, ConsistencyCheckSkipsPlanPackages) {
    Cache::instance().add_installed("app", "1.0", true);
    Cache::instance().add_installed("lib", "1.0", true);
    ensure_dir_exists(Config::instance().dep_dir());
    {
        std::ofstream f(Config::instance().dep_dir() / "app");
        f << "lib >= 1.0\n";
    }
    write_cache();
    Cache::instance().load();

    // Both app and lib are in plan → consistency check should skip app
    std::map<std::string, InstallPlan> plan;
    InstallPlan p1, p2;
    p1.name = "app"; p1.actual_version = "1.0";
    p2.name = "lib"; p2.actual_version = "2.0";
    plan["app"] = p1;
    plan["lib"] = p2;

    auto broken = detail::check_plan_consistency(plan);
    EXPECT_TRUE(broken.empty()) << "packages in plan should not be reported as broken";
}

TEST_F(RemovalSymlinkTest, NeededSoConsistencyDetectsDroppedSoname) {
    // app needs libc.so.6 provided by glibc
    Cache::instance().add_installed("app", "1.0", false);
    Cache::instance().add_installed("glibc", "2.35", true);
    ensure_dir_exists(Config::instance().needed_so_dir());
    {
        std::ofstream f(Config::instance().needed_so_dir() / "app");
        f << "libc.so.6\n";
    }
    Cache::instance().add_provider("libc.so.6", "glibc");
    write_cache();
    Cache::instance().load();

    // Plan: new glibc at 3.0 that does NOT provide libc.so.6
    std::map<std::string, InstallPlan> plan;
    InstallPlan p;
    p.name = "glibc";
    p.actual_version = "3.0";
    p.provides = {}; // no libc.so.6
    plan["glibc"] = p;

    auto broken = detail::check_needed_so_consistency(plan);
    EXPECT_TRUE(broken.contains("app")) << "app should be broken if glibc drops libc.so.6";
}

TEST_F(RemovalSymlinkTest, NeededSoConsistencyOkWhenSonameKept) {
    Cache::instance().add_installed("app", "1.0", false);
    Cache::instance().add_installed("glibc", "2.35", true);
    ensure_dir_exists(Config::instance().needed_so_dir());
    {
        std::ofstream f(Config::instance().needed_so_dir() / "app");
        f << "libc.so.6\n";
    }
    Cache::instance().add_provider("libc.so.6", "glibc");
    write_cache();
    Cache::instance().load();

    // Plan: new glibc at 3.0 that STILL provides libc.so.6
    std::map<std::string, InstallPlan> plan;
    InstallPlan p;
    p.name = "glibc";
    p.actual_version = "3.0";
    p.provides = {"libc.so.6"};
    plan["glibc"] = p;

    auto broken = detail::check_needed_so_consistency(plan);
    EXPECT_FALSE(broken.contains("app")) << "app should NOT be broken if soname is kept";
}

// =========================================================================
// SECTION 18: Directory permission warning
// =========================================================================

TEST_F(RemovalSymlinkTest, DirPermWarningOnMismatch) {
    // Create a package with a directory having specific permissions
    fs::path work_dir = suite_work_dir / "dirperm_pkg_work";
    fs::remove_all(work_dir);
    fs::create_directories(work_dir / "root" / "usr" / "lib" / "testapp");
    std::ofstream(work_dir / "root" / "usr" / "lib" / "testapp" / "data") << "content";

    // Set the directory to 0700
    fs::permissions(work_dir / "root" / "usr" / "lib" / "testapp",
        fs::perms::owner_all);

    std::string pkg_path = (pkg_dir / "dirperm-test.lpkg").string();
    pack_package(pkg_path, work_dir.string(), "dirperm-test", "1.0");
    fs::remove_all(work_dir);

    // Pre-create the target directory with different permissions (0755)
    fs::path target_dir = test_root / "usr" / "lib" / "testapp";
    fs::create_directories(target_dir);
    fs::permissions(target_dir,
        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
        fs::perms::others_read | fs::perms::others_exec);

    // Capture stderr during install
    testing::internal::CaptureStderr();
    ASSERT_NO_THROW(install_packages({pkg_path}));
    std::string stderr_output = testing::internal::GetCapturedStderr();

    // Verify warning was printed
    EXPECT_TRUE(stderr_output.find("permission") != std::string::npos
             || stderr_output.find("权限") != std::string::npos)
        << "Should warn about directory permission mismatch, got: " << stderr_output;
}

TEST_F(RemovalSymlinkTest, NoDirPermWarningWhenMatch) {
    // Same dir permission in package and on disk → no warning
    fs::path work_dir = suite_work_dir / "dirperm_ok_pkg_work";
    fs::remove_all(work_dir);
    fs::create_directories(work_dir / "root" / "usr" / "lib" / "testapp2");
    std::ofstream(work_dir / "root" / "usr" / "lib" / "testapp2" / "data") << "content";

    fs::permissions(work_dir / "root" / "usr" / "lib" / "testapp2",
        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
        fs::perms::others_read | fs::perms::others_exec);

    std::string pkg_path = (pkg_dir / "dirperm-ok.lpkg").string();
    pack_package(pkg_path, work_dir.string(), "dirperm-ok", "1.0");
    fs::remove_all(work_dir);

    // Pre-create with same permissions
    fs::path target_dir = test_root / "usr" / "lib" / "testapp2";
    fs::create_directories(target_dir);
    fs::permissions(target_dir,
        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
        fs::perms::others_read | fs::perms::others_exec);

    testing::internal::CaptureStderr();
    ASSERT_NO_THROW(install_packages({pkg_path}));
    std::string stderr_output = testing::internal::GetCapturedStderr();

    // Should NOT contain permission warning
    EXPECT_TRUE(stderr_output.find("permission") == std::string::npos
             && stderr_output.find("权限") == std::string::npos)
        << "Should NOT warn when permissions match, got: " << stderr_output;
}

// =========================================================================
// SECTION 19: Repository provider dedup
// =========================================================================

TEST_F(RemovalSymlinkTest, RepoProviderDedupOnDuplicateIndexEntries) {
    // Create a repo index where the same package version provides the same
    // capability multiple times (simulating a badly-generated index).
    fs::path repo_dir = suite_work_dir / "test_repo";
    fs::create_directories(repo_dir / "x86_64");
    {
        std::ofstream idx(repo_dir / "x86_64" / "index.txt");
        // Format: pkg|ver:hash:deps:provides:needed_so
        // Note: provides "libssl.so.1" appears only once for this pkg+ver
        idx << "openssl|3.0.0:abc123::libssl.so.1,libcrypto.so.1:\n";
        // Adding duplicate for same pkg+ver — should be deduped
        // (cannot truly duplicate in flat index, but test that
        //  update_package_info doesn't double-count)
    }

    // Configure mirror to point at our test repo
    {
        std::ofstream f(Config::instance().mirror_conf());
        f << "file://" << repo_dir.string() << "/\n";
    }

    // Load the repo
    Repository repo;
    ASSERT_NO_THROW(repo.load_index());

    // find_provider should work (dedup means entry exists once)
    auto prov = repo.find_provider("libssl.so.1");
    ASSERT_TRUE(prov.has_value()) << "should find provider for libssl.so.1";
    EXPECT_EQ(prov->name, "openssl");
    EXPECT_EQ(prov->version, "3.0.0");

    // update_package_info with same info should not double count
    std::vector<DependencyInfo> deps;
    repo.update_package_info("openssl", "3.0.0", deps, {"libssl.so.1", "libcrypto.so.1"}, {});

    // Should still find the provider (not lost after incremental rebuild)
    prov = repo.find_provider("libssl.so.1");
    ASSERT_TRUE(prov.has_value()) << "provider should survive incremental rebuild";
}
