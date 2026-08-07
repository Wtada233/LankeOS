#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/depend_scanner.hpp"

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════════════════
//  Test fixture: creates an isolated root and populates Cache directly.
//  No package files are created — all dependency state is set in-memory.
// ═══════════════════════════════════════════════════════════════════════════
class DependScannerTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;

    void SetUp() override
    {
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

        index_entries_.clear();
        index_versions_.clear();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    // ---- helpers to set up cache state ----

    void add_pkg(const std::string& name, const std::string& version)
    {
        Cache::instance().add_installed(name, version, false);
        index_versions_[name] = version;
    }

    // ---- repo index helpers ----
    // 重构后 scan_remove/scan_abibreak 恒用 build_repo_revdep_map()：从仓库 index 的
    // needed_so → provides 建反向依赖（不再读本地 Cache 的 reverse_dep）。故测试必须写
    // 仓库 index（Config::get_tmp_dir()/repo_index.txt），格式 name|ver:hash::provides:needed_so:
    struct IndexEntry {
        std::string provides;
        std::string needed_so;
    };
    std::map<std::string, IndexEntry> index_entries_;
    std::map<std::string, std::string> index_versions_;

    void add_provider_so(const std::string& pkg, const std::string& soname)
    {
        auto& e = index_entries_[pkg];
        if (!e.provides.empty()) e.provides += ",";
        e.provides += soname;
    }

    void add_needed_so(const std::string& pkg, const std::string& soname)
    {
        auto& e = index_entries_[pkg];
        if (!e.needed_so.empty()) e.needed_so += ",";
        e.needed_so += soname;
    }

    // 把累积的条目写入 build_repo_revdep_map() 读取的位置
    void write_index()
    {
        fs::path idx = Config::get_tmp_dir() / "repo_index.txt";
        fs::create_directories(idx.parent_path());
        std::ofstream f(idx);
        for (const auto& [name, e] : index_entries_) {
            auto it = index_versions_.find(name);
            std::string ver = (it != index_versions_.end()) ? it->second : "1.0";
            f << name << "|" << ver << ":hash::" << e.provides << ":" << e.needed_so << ":\n";
        }
    }

    // Count nodes with a given status in the tree
    int count_status(const depscan::ScanNode& node, depscan::ScanStatus s) const
    {
        int n = (node.status == s) ? 1 : 0;
        for (const auto& c : node.children) n += count_status(c, s);
        return n;
    }

    int count_affected(const depscan::ScanNode& node) const
    {
        return count_status(node, depscan::ScanStatus::REMOVED) +
               count_status(node, depscan::ScanStatus::REBUILD) +
               count_status(node, depscan::ScanStatus::INSTALL);
    }

    int count_total(const depscan::ScanNode& node) const
    {
        int n = 1;
        for (const auto& c : node.children) n += count_total(c);
        return n;
    }

    bool has_child(const depscan::ScanNode& node, const std::string& name) const
    {
        for (const auto& c : node.children)
            if (c.name == name) return true;
        return false;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  1.  depend remove  — basic transitive closure
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(DependScannerTest, RemoveSimple)
{
    // A ← B(needed_so:liba)
    add_pkg("libA", "1.0");
    add_pkg("appB", "2.0");
    add_provider_so("libA", "liba.so.1");
    add_needed_so("appB", "liba.so.1");
    write_index();

    auto tree = depscan::scan_remove_tree("libA");

    EXPECT_EQ(tree.name, "libA");
    EXPECT_TRUE(tree.is_affected());
    EXPECT_EQ(tree.status, depscan::ScanStatus::REMOVED);
    ASSERT_EQ(tree.children.size(), 1u);
    EXPECT_EQ(tree.children[0].name, "appB");
    EXPECT_TRUE(tree.children[0].is_affected());
    EXPECT_EQ(count_affected(tree), 2);
}

TEST_F(DependScannerTest, RemoveTransitiveChain)
{
    // A ← B(needed_so:liba) ← C(needed_so:libb)
    add_pkg("libA", "1.0");
    add_pkg("libB", "1.0");
    add_pkg("appC", "1.0");
    add_provider_so("libA", "liba.so.1");
    add_needed_so("libB", "liba.so.1");
    add_provider_so("libB", "libb.so.1");
    add_needed_so("appC", "libb.so.1");
    write_index();

    auto tree = depscan::scan_remove_tree("libA");

    EXPECT_EQ(count_affected(tree), 3);  // A + B + C
    EXPECT_EQ(count_total(tree), 3);
    EXPECT_TRUE(has_child(tree, "libB"));
}

TEST_F(DependScannerTest, RemoveIndependent)
{
    // A, B (no relation)
    add_pkg("pkgA", "1.0");
    add_pkg("pkgB", "1.0");

    auto tree = depscan::scan_remove_tree("pkgA");

    EXPECT_EQ(count_affected(tree), 1);
    EXPECT_EQ(tree.children.size(), 0u);
}

TEST_F(DependScannerTest, RemoveCircular)
{
    // A ←→ B  (A needed_so:b, B needed_so:a)
    add_pkg("pkgA", "1.0");
    add_pkg("pkgB", "1.0");
    add_provider_so("pkgA", "a.so.1");
    add_needed_so("pkgB", "a.so.1");
    add_provider_so("pkgB", "b.so.1");
    add_needed_so("pkgA", "b.so.1");
    write_index();

    auto tree = depscan::scan_remove_tree("pkgA");

    EXPECT_EQ(count_affected(tree), 2);  // no infinite loop
    EXPECT_TRUE(has_child(tree, "pkgB"));
}

TEST_F(DependScannerTest, RemoveViaProvider)
{
    // A(prov:libx) ← B(needed_so:libx)
    add_pkg("pkgA", "1.0");
    add_pkg("pkgB", "1.0");
    add_provider_so("pkgA", "libx");
    add_needed_so("pkgB", "libx");
    write_index();

    auto tree = depscan::scan_remove_tree("pkgA");

    EXPECT_EQ(count_affected(tree), 2);
    EXPECT_TRUE(has_child(tree, "pkgB"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  2.  depend abibreak  — direct-only, never transitive
// ═══════════════════════════════════════════════════════════════════════════

TEST_F(DependScannerTest, AbibreakDirectOnly)
{
    // A ← B(needed_so:liba) ← C(needed_so:libb)
    add_pkg("libA", "1.0");
    add_pkg("libB", "1.0");
    add_pkg("appC", "1.0");
    add_provider_so("libA", "liba.so.1");
    add_needed_so("libB", "liba.so.1");
    add_provider_so("libB", "libb.so.1");
    add_needed_so("appC", "libb.so.1");
    write_index();

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

TEST_F(DependScannerTest, AbibreakAllFlagShowsIndirect)
{
    // A ← B(needed_so:liba) ← C(needed_so:libb)
    add_pkg("libA", "1.0");
    add_pkg("libB", "1.0");
    add_pkg("appC", "1.0");
    add_provider_so("libA", "liba.so.1");
    add_needed_so("libB", "liba.so.1");
    add_provider_so("libB", "libb.so.1");
    add_needed_so("appC", "libb.so.1");
    write_index();

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

TEST_F(DependScannerTest, AbibreakMultiple)
{
    // A ← B(needed_so:base.so), A ← C(needed_so:base.so)
    add_pkg("base", "1.0");
    add_pkg("depB", "1.0");
    add_pkg("depC", "1.0");
    add_provider_so("base", "base.so.1");
    add_needed_so("depB", "base.so.1");
    add_needed_so("depC", "base.so.1");
    write_index();

    auto tree = depscan::scan_abibreak_tree("base");

    EXPECT_EQ(tree.children.size(), 2u);
    EXPECT_EQ(count_affected(tree), 2);
    EXPECT_TRUE(has_child(tree, "depB"));
    EXPECT_TRUE(has_child(tree, "depC"));
}

TEST_F(DependScannerTest, AbibreakViaProvider)
{
    // A(prov:libssl) ← B(needed_so:libssl)
    add_pkg("openssl", "1.0");
    add_pkg("curl", "1.0");
    add_provider_so("openssl", "libssl");
    add_needed_so("curl", "libssl");
    write_index();

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

TEST_F(DependScannerTest, RemoveNonexistent)
{
    auto tree = depscan::scan_remove_tree("ghost_pkg");

    EXPECT_EQ(tree.name, "ghost_pkg");
    EXPECT_TRUE(tree.is_affected());
    EXPECT_EQ(tree.children.size(), 0u);
}

TEST_F(DependScannerTest, AbibreakNonexistent)
{
    auto tree = depscan::scan_abibreak_tree("ghost_pkg");

    EXPECT_EQ(tree.name, "ghost_pkg");
    EXPECT_EQ(tree.children.size(), 0u);
}

TEST_F(DependScannerTest, ComplexGraph)
{
    // base(prov:core)
    //   ├─ midA(needed_so:core)
    //   │   └─ topAA(needed_so:mida)
    //   └─ midB(needed_so:core)
    //       └─ topBB(needed_so:midb)
    add_pkg("base", "1.0");
    add_pkg("midA", "1.0");
    add_pkg("midB", "1.0");
    add_pkg("topAA", "1.0");
    add_pkg("topBB", "1.0");
    add_provider_so("base", "core.so.1");
    add_needed_so("midA", "core.so.1");
    add_needed_so("midB", "core.so.1");
    add_provider_so("midA", "mida.so.1");
    add_needed_so("topAA", "mida.so.1");
    add_provider_so("midB", "midb.so.1");
    add_needed_so("topBB", "midb.so.1");
    write_index();

    // Remove scan: all 5 affected
    auto tree = depscan::scan_remove_tree("base");
    EXPECT_EQ(count_affected(tree), 5);
    EXPECT_EQ(count_total(tree), 5);

    // ABI scan: only midA, midB affected (direct deps)
    auto abi = depscan::scan_abibreak_tree("base");
    EXPECT_EQ(count_affected(abi), 2);
    EXPECT_EQ(abi.children.size(), 2u);
}

TEST_F(DependScannerTest, StatusLabels)
{
    EXPECT_EQ(depscan::status_label(depscan::ScanStatus::REMOVED), "WILL BE REMOVED");
    EXPECT_EQ(depscan::status_label(depscan::ScanStatus::REBUILD), "NEEDS REBUILD");
    EXPECT_EQ(depscan::status_label(depscan::ScanStatus::INSTALL), "WILL BE INSTALLED");
    EXPECT_EQ(depscan::status_label(depscan::ScanStatus::ABI_CHANGED), "ABI CHANGED");
    EXPECT_EQ(depscan::status_label(depscan::ScanStatus::KEEP), "UNCHANGED");
}

TEST_F(DependScannerTest, AbibreakNoDeps)
{
    add_pkg("standalone", "1.0");
    auto tree = depscan::scan_abibreak_tree("standalone");
    EXPECT_EQ(tree.children.size(), 0u);
}

TEST_F(DependScannerTest, PrintTreeNoCrash)
{
    depscan::ScanNode root;
    root.name = "test";
    root.status = depscan::ScanStatus::KEEP;
    EXPECT_NO_THROW(depscan::print_tree(root));
}
