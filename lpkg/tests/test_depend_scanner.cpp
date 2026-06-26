#include <gtest/gtest.h>
#include "../main/src/pkg/depend_scanner.hpp"
#include "../main/src/config/config.hpp"
#include "../main/src/db/cache.hpp"
#include "../main/src/i18n/localization.hpp"

#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════
//  Test fixture: creates an isolated root and populates Cache directly.
//  No package files are created — all dependency state is set in-memory.
// ═══════════════════════════════════════════════════════════════════════════
class DependScannerTest : public ::testing::Test {
protected:
    fs::path suite_work_dir;
    fs::path test_root;

    void SetUp() override {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::temp_directory_path() / "lpkg_depscan_test";
        fs::remove_all(suite_work_dir);
        test_root = suite_work_dir / "root";
        fs::create_directories(test_root);

        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    // ---- helpers to set up cache state ----

    void add_pkg(const std::string& name, const std::string& version) {
        Cache::instance().add_installed(name, version, false);
    }

    void add_dep(const std::string& from, const std::string& to) {
        // "from depends on to" → "to" has reverse-dep "from"
        Cache::instance().add_reverse_dep(to, from);
    }

    void add_provides(const std::string& pkg, const std::string& cap) {
        Cache::instance().add_provider(cap, pkg);
    }

    // Write dep file so --all and remove_package can read deps
    void write_dep_file(const std::string& pkg, const std::vector<std::string>& deps) {
        fs::path dep_file = Config::instance().dep_dir() / pkg;
        fs::create_directories(dep_file.parent_path());
        std::ofstream f(dep_file);
        for (const auto& d : deps) f << d << "\n";
    }

    // Count nodes with a given status in the tree
    int count_status(const depscan::ScanNode& node, depscan::ScanStatus s) const {
        int n = (node.status == s) ? 1 : 0;
        for (const auto& c : node.children) n += count_status(c, s);
        return n;
    }

    int count_affected(const depscan::ScanNode& node) const {
        return count_status(node, depscan::ScanStatus::REMOVED)
             + count_status(node, depscan::ScanStatus::REBUILD)
             + count_status(node, depscan::ScanStatus::INSTALL);
    }

    int count_total(const depscan::ScanNode& node) const {
        int n = 1;
        for (const auto& c : node.children) n += count_total(c);
        return n;
    }

    bool has_child(const depscan::ScanNode& node, const std::string& name) const {
        for (const auto& c : node.children)
            if (c.name == name) return true;
        return false;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  1.  depend remove  — basic transitive closure
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(DependScannerTest, RemoveSimple) {
    // A ← B(dep:A)
    add_pkg("libA", "1.0");
    add_pkg("appB", "2.0");
    add_dep("appB", "libA");

    auto tree = depscan::scan_remove_tree("libA");

    EXPECT_EQ(tree.name, "libA");
    EXPECT_TRUE(tree.is_affected());
    EXPECT_EQ(tree.status, depscan::ScanStatus::REMOVED);
    ASSERT_EQ(tree.children.size(), 1u);
    EXPECT_EQ(tree.children[0].name, "appB");
    EXPECT_TRUE(tree.children[0].is_affected());
    EXPECT_EQ(count_affected(tree), 2);
}

TEST_F(DependScannerTest, RemoveTransitiveChain) {
    // A ← B(dep:A) ← C(dep:B)
    add_pkg("libA", "1.0");
    add_pkg("libB", "1.0");
    add_pkg("appC", "1.0");
    add_dep("libB", "libA");
    add_dep("appC", "libB");

    auto tree = depscan::scan_remove_tree("libA");

    EXPECT_EQ(count_affected(tree), 3);  // A + B + C
    EXPECT_EQ(count_total(tree), 3);
    EXPECT_TRUE(has_child(tree, "libB"));
}

TEST_F(DependScannerTest, RemoveIndependent) {
    // A, B (no relation)
    add_pkg("pkgA", "1.0");
    add_pkg("pkgB", "1.0");

    auto tree = depscan::scan_remove_tree("pkgA");

    EXPECT_EQ(count_affected(tree), 1);
    EXPECT_EQ(tree.children.size(), 0u);
}

TEST_F(DependScannerTest, RemoveCircular) {
    // A ←→ B  (A depends on B, B depends on A)
    add_pkg("pkgA", "1.0");
    add_pkg("pkgB", "1.0");
    add_dep("pkgA", "pkgB");
    add_dep("pkgB", "pkgA");

    auto tree = depscan::scan_remove_tree("pkgA");

    EXPECT_EQ(count_affected(tree), 2);  // no infinite loop
    EXPECT_TRUE(has_child(tree, "pkgB"));
}

TEST_F(DependScannerTest, RemoveViaProvider) {
    // A(prov:libx) ← B(dep:libx)
    add_pkg("pkgA", "1.0");
    add_pkg("pkgB", "1.0");
    add_provides("pkgA", "libx");
    add_dep("pkgB", "libx");   // B depends on virtual capability "libx"

    auto tree = depscan::scan_remove_tree("pkgA");

    EXPECT_EQ(count_affected(tree), 2);
    EXPECT_TRUE(has_child(tree, "pkgB"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  2.  depend abibreak  — direct-only, never transitive
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(DependScannerTest, AbibreakDirectOnly) {
    // A ← B(dep:A) ← C(dep:B)
    add_pkg("libA", "1.0");
    add_pkg("libB", "1.0");
    add_pkg("appC", "1.0");
    add_dep("libB", "libA");
    add_dep("appC", "libB");

    auto tree = depscan::scan_abibreak_tree("libA");

    EXPECT_EQ(tree.status, depscan::ScanStatus::ABI_CHANGED);
    EXPECT_FALSE(tree.is_affected());

    ASSERT_EQ(tree.children.size(), 1u);
    EXPECT_EQ(tree.children[0].name, "libB");
    EXPECT_EQ(tree.children[0].status, depscan::ScanStatus::REBUILD);

    // only B affected, NOT C
    EXPECT_EQ(count_affected(tree), 1);
    EXPECT_EQ(count_total(tree), 2);  // A + B without --all
}

TEST_F(DependScannerTest, AbibreakAllFlagShowsIndirect) {
    // A ← B(dep:A) ← C(dep:B)
    add_pkg("libA", "1.0");
    add_pkg("libB", "1.0");
    add_pkg("appC", "1.0");
    add_dep("libB", "libA");
    add_dep("appC", "libB");

    auto tree = depscan::scan_abibreak_tree("libA", /*show_all=*/true);

    // With --all, B's reverse deps are shown as KEEP
    ASSERT_EQ(tree.children.size(), 1u);
    EXPECT_EQ(tree.children[0].name, "libB");
    EXPECT_EQ(tree.children[0].status, depscan::ScanStatus::REBUILD);

    // C should be KEEP
    bool found_c = false;
    for (const auto& gc : tree.children[0].children) {
        if (gc.name == "appC") {
            EXPECT_EQ(gc.status, depscan::ScanStatus::KEEP);
            EXPECT_FALSE(gc.is_affected());
            found_c = true;
        }
    }
    EXPECT_TRUE(found_c);
}

TEST_F(DependScannerTest, AbibreakMultiple) {
    // A ← B(dep:A), A ← C(dep:A)
    add_pkg("base", "1.0");
    add_pkg("depB", "1.0");
    add_pkg("depC", "1.0");
    add_dep("depB", "base");
    add_dep("depC", "base");

    auto tree = depscan::scan_abibreak_tree("base");

    EXPECT_EQ(tree.children.size(), 2u);
    EXPECT_EQ(count_affected(tree), 2);
    EXPECT_TRUE(has_child(tree, "depB"));
    EXPECT_TRUE(has_child(tree, "depC"));
}

TEST_F(DependScannerTest, AbibreakViaProvider) {
    // A(prov:libssl) ← B(dep:libssl)
    add_pkg("openssl", "1.0");
    add_pkg("curl", "1.0");
    add_provides("openssl", "libssl");
    add_dep("curl", "libssl");

    auto tree = depscan::scan_abibreak_tree("openssl");

    ASSERT_GE(tree.children.size(), 1u);
    EXPECT_TRUE(has_child(tree, "curl"));
    for (const auto& c : tree.children) {
        if (c.name == "curl") {
            EXPECT_EQ(c.status, depscan::ScanStatus::REBUILD);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  3.  Edge cases
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(DependScannerTest, RemoveNonexistent) {
    auto tree = depscan::scan_remove_tree("ghost_pkg");

    EXPECT_EQ(tree.name, "ghost_pkg");
    EXPECT_TRUE(tree.is_affected());
    EXPECT_EQ(tree.children.size(), 0u);
}

TEST_F(DependScannerTest, AbibreakNonexistent) {
    auto tree = depscan::scan_abibreak_tree("ghost_pkg");

    EXPECT_EQ(tree.name, "ghost_pkg");
    EXPECT_EQ(tree.children.size(), 0u);
}

TEST_F(DependScannerTest, ComplexGraph) {
    // base(prov:core)
    //   ├─ midA(dep:core)
    //   │   └─ topAA(dep:midA)
    //   └─ midB(dep:core)
    //       └─ topBB(dep:midB)
    add_pkg("base",  "1.0");
    add_pkg("midA",  "1.0");
    add_pkg("midB",  "1.0");
    add_pkg("topAA", "1.0");
    add_pkg("topBB", "1.0");
    add_provides("base", "core");
    add_dep("midA", "core");
    add_dep("midB", "core");
    add_dep("topAA", "midA");
    add_dep("topBB", "midB");

    // Remove scan: all 5 affected
    auto tree = depscan::scan_remove_tree("base");
    EXPECT_EQ(count_affected(tree), 5);
    EXPECT_EQ(count_total(tree), 5);

    // ABI scan: only midA, midB affected (direct deps)
    auto abi = depscan::scan_abibreak_tree("base");
    EXPECT_EQ(count_affected(abi), 2);
    EXPECT_EQ(abi.children.size(), 2u);
}

TEST_F(DependScannerTest, StatusLabels) {
    EXPECT_EQ(depscan::status_label(depscan::ScanStatus::REMOVED), "WILL BE REMOVED");
    EXPECT_EQ(depscan::status_label(depscan::ScanStatus::REBUILD), "NEEDS REBUILD");
    EXPECT_EQ(depscan::status_label(depscan::ScanStatus::INSTALL), "WILL BE INSTALLED");
    EXPECT_EQ(depscan::status_label(depscan::ScanStatus::ABI_CHANGED), "ABI CHANGED");
    EXPECT_EQ(depscan::status_label(depscan::ScanStatus::KEEP), "UNCHANGED");
}
