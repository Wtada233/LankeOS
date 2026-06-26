#include "depend_scanner.hpp"
#include "core/cache.hpp"
#include "core/repository.hpp"
#include "core/config.hpp"
#include "core/version.hpp"
#include "core/localization.hpp"
#include "core/constants.hpp"
#include "core/utils.hpp"
#include "core/exception.hpp"
#include "install_common.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <ranges>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace depscan {

// ═══════════════════════════════════════════════════════════════════════════
//  Internal helpers
// ═══════════════════════════════════════════════════════════════════════════

namespace {

std::string version_or_missing(const std::string& pkg) {
    auto ver = Cache::instance().get_installed_version(pkg);
    return ver.empty() ? "(not installed)" : ver;
}

// ── Reverse-dependency BFS (transitive closure) ────────────────────────────
// Returns every installed package that transitively depends on `root_pkg`.
void collect_transitive_rdeps(const std::string& root_pkg,
                              std::unordered_set<std::string>& result,
                              std::unordered_set<std::string>& visited)
{
    if (!visited.insert(root_pkg).second) return;

    auto rdeps = Cache::instance().get_reverse_deps(root_pkg);

    // Also check virtual capabilities owned by this package
    for (const auto& cap : Cache::instance().get_package_provides(root_pkg)) {
        auto cap_rdeps = Cache::instance().get_reverse_deps(cap);
        rdeps.insert(cap_rdeps.begin(), cap_rdeps.end());
    }

    for (const auto& rdep : rdeps) {
        if (rdep == root_pkg) continue;
        if (result.insert(rdep).second) {
            collect_transitive_rdeps(rdep, result, visited);
        }
    }
}

// ── Forward-dependency BFS (what pkg needs) ───────────────────────────────
// Returns transitive deps that are NOT yet installed.
// Reads deps from the Repository for a remote package, or from a parsed
// metadata.json for a local file.
struct ResolvedDep {
    std::string name;
    std::string version;
    bool already_installed;
    std::vector<DependencyInfo> deps;
};
using DepMap = std::unordered_map<std::string, ResolvedDep>;

void resolve_transitive_deps(const std::string& pkg_name,
                             const std::string& version_spec,
                             DepMap& plan,
                             std::set<std::string>& visited,
                             Repository& repo)
{
    if (visited.contains(pkg_name)) return;
    if (plan.contains(pkg_name)) return;

    // Already installed → no new work, but record it
    std::string installed_ver = Cache::instance().get_installed_version(pkg_name);
    if (!installed_ver.empty()) {
        // If the installed version satisfies the constraint, we're done
        if (version_spec.empty() || version_spec == constants::VER_LATEST) {
            // Mark as installed and skip resolution
            plan[pkg_name] = {pkg_name, installed_ver, true, {}};
            return;
        }
        // Otherwise, check if we need a newer version
    }

    visited.insert(pkg_name);

    // Look up in repository
    auto pkg_info = (version_spec == constants::VER_LATEST || version_spec.empty())
        ? repo.find_package(pkg_name)
        : repo.find_package(pkg_name, version_spec);

    if (!pkg_info) {
        // Try provider
        if (auto prov = repo.find_provider(pkg_name)) {
            pkg_info = repo.find_package(prov->name);
            if (!pkg_info) {
                visited.erase(pkg_name);
                return;
            }
        } else {
            visited.erase(pkg_name);
            return;
        }
    }

    bool already_installed = !Cache::instance().get_installed_version(pkg_name).empty();

    plan[pkg_name] = {pkg_info->name, pkg_info->version,
                       already_installed, pkg_info->dependencies};

    // Recurse into deps
    for (const auto& dep : pkg_info->dependencies) {
        if (!Config::instance().no_deps_mode()) {
            std::string dep_ver(constants::VER_LATEST);
            if (!dep.op.empty()) {
                if (auto match = repo.find_best_matching_version(dep.name, dep.op, dep.version_req))
                    dep_ver = match->version;
            }
            resolve_transitive_deps(dep.name, dep_ver, plan, visited, repo);
        }
    }

    visited.erase(pkg_name);
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Repository reverse-dependency helpers  (used when the target package
//  is NOT installed locally — fall back to the repo index)
// ═══════════════════════════════════════════════════════════════════════════

namespace {

// Split a string_view by delimiter (local helper, mirrors repository.cpp)
std::vector<std::string_view> split_sv(std::string_view s, char delim) {
    std::vector<std::string_view> res;
    size_t start = 0, end;
    while ((end = s.find(delim, start)) != std::string_view::npos) {
        res.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    res.push_back(s.substr(start));
    return res;
}

// Build a reverse-dependency map from the cached repository index.
// Returns: target_pkg → set of packages that DIRECTLY depend on it.
// Only the latest version of each package is considered.
std::unordered_map<std::string, std::unordered_set<std::string>>
build_repo_revdep_map() {
    std::unordered_map<std::string, std::unordered_set<std::string>> rev;
    fs::path index = Config::get_tmp_dir() / constants::REPO_INDEX_TMP;
    if (!fs::exists(index)) return rev;

    std::ifstream f(index);
    std::string line;
    static const std::vector<std::string_view> ops = {
        ">=", "<=", "!=", "==", ">", "<", "="
    };

    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.back() == '\r') line.pop_back();

        auto parts = split_sv(line, constants::PIPE_CHAR);
        if (parts.size() < 2) continue;
        std::string pkg_name(parts[0]);
        std::string_view version_blocks = parts[1];

        // Use the LATEST version block only (the last one after ';')
        auto blocks = split_sv(version_blocks, constants::SEMICOLON_CHAR);
        if (blocks.empty()) continue;
        auto& latest = blocks.back();

        auto vh_parts = split_sv(latest, constants::COLON_CHAR);
        if (vh_parts.size() < 3) continue;
        std::string_view deps_sv = vh_parts[2];

        for (auto dep_sv : split_sv(deps_sv, constants::COMMA_CHAR)) {
            std::string_view dep_name = dep_sv;
            for (const auto& op : ops) {
                if (auto p = dep_sv.find(op); p != std::string_view::npos) {
                    dep_name = dep_sv.substr(0, p);
                    break;
                }
            }
            if (!dep_name.empty())
                rev[std::string(dep_name)].insert(pkg_name);
        }
    }
    return rev;
}

// Recursively collect transitive reverse deps from a repo revdep map
void collect_repo_transitive_rdeps(
    const std::string& pkg,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& rev,
    std::unordered_set<std::string>& result,
    std::unordered_set<std::string>& visited)
{
    if (!visited.insert(pkg).second) return;
    auto it = rev.find(pkg);
    if (it == rev.end()) return;
    for (const auto& dep : it->second) {
        if (dep == pkg) continue;
        if (result.insert(dep).second)
            collect_repo_transitive_rdeps(dep, rev, result, visited);
    }
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  scan_remove_tree
// ═══════════════════════════════════════════════════════════════════════════

ScanNode scan_remove_tree(const std::string& pkg_name, bool show_all) {
    auto& cache = Cache::instance();
    // NOTE: every Cache method internally locks its own mutex, so
    // we must NOT hold an outer lock here — it would deadlock.

    // ── If package is NOT installed, fall back to repo index ────────────────
    if (cache.get_installed_version(pkg_name).empty()) {
        ensure_dir_exists(Config::get_tmp_dir());
        Repository repo;
        try { repo.load_index(); } catch (...) {}
        auto rev = build_repo_revdep_map();

        if (rev.find(pkg_name) == rev.end()) {
            // Not found in repo either → single-node tree
            ScanNode root;
            root.name = pkg_name;
            root.version = "(not installed)";
            root.status = ScanStatus::REMOVED;
            root.reason = "not found in repository";
            return root;
        }

        // Collect transitive reverse deps from repo
        std::unordered_set<std::string> affected;
        std::unordered_set<std::string> visited;
        collect_repo_transitive_rdeps(pkg_name, rev, affected, visited);
        affected.insert(pkg_name);

        // Build tree recursively
        std::function<void(ScanNode&, const std::string&)> build_repo;
        build_repo = [&](ScanNode& node, const std::string& name) {
            auto it = rev.find(name);
            if (it == rev.end()) return;
            for (const auto& dep : it->second) {
                if (dep == name) continue;
                if (!affected.contains(dep)) continue;
                affected.erase(dep);

                ScanNode child;
                child.name = dep;
                child.version = "(in repository)";
                child.status = ScanStatus::REMOVED;
                child.reason = "depends on " + name + " (repo)";
                build_repo(child, dep);
                node.children.push_back(std::move(child));
            }
        };

        ScanNode root;
        root.name = pkg_name;
        root.version = "(in repository)";
        root.status = ScanStatus::REMOVED;
        root.reason = "target package (repo)";
        affected.erase(pkg_name);
        build_repo(root, pkg_name);
        return root;
    }

    // ── Package IS installed locally → use local cache ─────────────────────

    // ── Phase 1: collect every package transitively reverse-dependent on pkg ──
    std::unordered_set<std::string> affected;
    std::unordered_set<std::string> visited;
    collect_transitive_rdeps(pkg_name, affected, visited);

    // ── Phase 2: build the tree via stable recursion (no dangling pointers) ───
    // affected doubles as the "available" pool — we erase entries as they
    // are placed into the tree so no node appears more than once.
    affected.insert(pkg_name);  // include the root itself

    std::function<void(ScanNode&, const std::string&)> build_children;
    build_children = [&](ScanNode& node, const std::string& node_name) {
        auto rdeps = cache.get_reverse_deps(node_name);

        // Also check virtual capabilities owned by this package
        for (const auto& cap : cache.get_package_provides(node_name)) {
            auto cap_rdeps = cache.get_reverse_deps(cap);
            rdeps.insert(cap_rdeps.begin(), cap_rdeps.end());
        }

        for (const auto& rdep : rdeps) {
            if (rdep == node_name) continue;
            if (!affected.contains(rdep)) continue;
            affected.erase(rdep);  // claim this node (prevents duplicate parents)

            ScanNode child;
            child.name = rdep;
            child.version = version_or_missing(rdep);
            child.status = ScanStatus::REMOVED;
            child.reason = "depends on " + node_name;

            // Recurse first (depth-first), then attach — this is safe because
            // each child lives in its parent's `children` vector, and no other
            // code touches the same parent's children during recursion.
            build_children(child, rdep);

            // With --all, also show forward deps that are SHARED (not affected).
            if (show_all) {
                fs::path dep_file = Config::instance().dep_dir() / rdep;
                if (fs::exists(dep_file)) {
                    std::ifstream f(dep_file);
                    std::string line;
                    std::set<std::string> seen_keep;
                    while (std::getline(f, line)) {
                        std::string d_name;
                        std::istringstream ss(line);
                        ss >> d_name;
                        if (d_name.empty()) continue;
                        if (!seen_keep.insert(d_name).second) continue;

                        ScanNode keep;
                        keep.name = d_name;
                        keep.version = version_or_missing(d_name);
                        keep.status = ScanStatus::KEEP;
                        keep.reason = "shared dependency, unchanged";
                        child.children.push_back(std::move(keep));
                    }
                }
            }

            node.children.push_back(std::move(child));
        }
    };

    ScanNode root;
    root.name = pkg_name;
    root.version = version_or_missing(pkg_name);
    root.status = ScanStatus::REMOVED;
    root.reason = "target package";

    affected.erase(pkg_name);
    build_children(root, pkg_name);

    return root;
}

// ═══════════════════════════════════════════════════════════════════════════
//  scan_abibreak_tree
// ═══════════════════════════════════════════════════════════════════════════

ScanNode scan_abibreak_tree(const std::string& pkg_name, bool show_all) {
    auto& cache = Cache::instance();
    // NOTE: every Cache method internally locks its own mutex, so
    // we must NOT hold an outer lock here — it would deadlock.

    // ── If package is NOT installed, fall back to repo index ────────────────
    if (cache.get_installed_version(pkg_name).empty()) {
        ensure_dir_exists(Config::get_tmp_dir());
        Repository repo;
        try { repo.load_index(); } catch (...) {}
        auto rev = build_repo_revdep_map();

        ScanNode root;
        root.name = pkg_name;
        root.version = "(in repository)";
        root.status = ScanStatus::ABI_CHANGED;
        root.reason = "ABI changed — direct dependents need rebuild";

        auto it = rev.find(pkg_name);
        if (it != rev.end()) {
            for (const auto& dep : it->second) {
                if (dep == pkg_name) continue;
                ScanNode child;
                child.name = dep;
                child.version = "(in repository)";
                child.status = ScanStatus::REBUILD;
                child.reason = "direct dependency of " + pkg_name + " (repo)";

                if (show_all) {
                    // Show indirect (they stay KEEP)
                    auto git = rev.find(dep);
                    if (git != rev.end()) {
                        for (const auto& gdep : git->second) {
                            if (gdep == dep || gdep == pkg_name) continue;
                            ScanNode keep;
                            keep.name = gdep;
                            keep.version = "(in repository)";
                            keep.status = ScanStatus::KEEP;
                            keep.reason = "indirect — ABI preserved through abstraction";
                            child.children.push_back(std::move(keep));
                        }
                    }
                }

                root.children.push_back(std::move(child));
            }
        }
        return root;
    }

    // ── Package IS installed locally → use local cache ─────────────────────
    ScanNode root;
    root.name = pkg_name;
    root.version = version_or_missing(pkg_name);
    root.status = ScanStatus::ABI_CHANGED;
    root.reason = "ABI changed — direct dependents need rebuild";

    // Direct reverse deps only (no transitive traversal)
    auto rdeps = cache.get_reverse_deps(pkg_name);
    // Also check capabilities
    for (const auto& cap : cache.get_package_provides(pkg_name)) {
        auto cap_rdeps = cache.get_reverse_deps(cap);
        rdeps.insert(cap_rdeps.begin(), cap_rdeps.end());
    }

    for (const auto& rdep : rdeps) {
        if (rdep == pkg_name) continue;

        ScanNode child;
        child.name = rdep;
        child.version = version_or_missing(rdep);
        child.status = ScanStatus::REBUILD;
        child.reason = "direct dependency of " + pkg_name;

        if (show_all) {
            // Show indirect reverse deps of this node — they DON'T need rebuild
            auto indirect = cache.get_reverse_deps(rdep);
            for (const auto& cap : cache.get_package_provides(rdep)) {
                auto cap_ir = cache.get_reverse_deps(cap);
                indirect.insert(cap_ir.begin(), cap_ir.end());
            }
            for (const auto& ir : indirect) {
                if (ir == rdep || ir == pkg_name) continue;
                ScanNode keep;
                keep.name = ir;
                keep.version = version_or_missing(ir);
                keep.status = ScanStatus::KEEP;
                keep.reason = "indirect — ABI preserved through abstraction";
                child.children.push_back(std::move(keep));
            }
        }

        root.children.push_back(std::move(child));
    }

    return root;
}

// ═══════════════════════════════════════════════════════════════════════════
//  scan_install_tree
// ═══════════════════════════════════════════════════════════════════════════

ScanNode scan_install_tree(const std::string& pkg_name, bool show_all) {
    // Load repository — ensure the temp download directory exists first
    ensure_dir_exists(Config::get_tmp_dir());
    Repository repo;
    try { repo.load_index(); }
    catch (const std::exception& e) {
        log_warning(string_format("warning.repo_index_load_failed", e.what()));
    }

    std::string target_name = pkg_name;
    std::string target_ver = std::string(constants::VER_LATEST);

    // Parse "name:version" format
    if (auto pos = pkg_name.find(':'); pos != std::string::npos) {
        target_name = pkg_name.substr(0, pos);
        target_ver = pkg_name.substr(pos + 1);
    }

    // Resolve the dependency tree
    DepMap plan;
    std::set<std::string> visited;
    resolve_transitive_deps(target_name, target_ver, plan, visited, repo);

    if (plan.empty()) {
        ScanNode root;
        root.name = target_name;
        root.version = version_or_missing(target_name);
        root.status = ScanStatus::KEEP;
        root.reason = "package not found in repository";
        return root;
    }

    // Build tree: root is the target package itself
    auto it = plan.find(target_name);
    ScanNode root;
    root.name = it->second.name;
    root.version = it->second.version;
    root.status = it->second.already_installed ? ScanStatus::KEEP : ScanStatus::INSTALL;
    root.reason = it->second.already_installed ? "already installed" : "target package";

    // Recursive builder
    std::set<std::string> seen_in_tree;
    seen_in_tree.insert(target_name);

    // Build tree recursively
    std::function<void(ScanNode*, const std::string&)> build_install_children;
    build_install_children = [&](ScanNode* parent_node, const std::string& parent_name) {
        auto pit = plan.find(parent_name);
        if (pit == plan.end()) return;

        for (const auto& dep : pit->second.deps) {
            auto dit = plan.find(dep.name);
            if (dit == plan.end()) continue;

            // Resolve provider if dep name uses virtual capability
            std::string real_name = dep.name;
            if (!Cache::instance().get_installed_version(dep.name).empty()
                || plan.contains(dep.name))
            {
                real_name = dep.name;
            } else {
                // Try provider lookup
                auto prov = repo.find_provider(dep.name);
                if (prov) real_name = prov->name;
            }

            auto pit2 = plan.find(real_name);
            if (pit2 == plan.end()) {
                // Try finding by checking if any planned package provides this dep
                for (const auto& [pn, pr] : plan) {
                    for (const auto& prov : pr.deps) {
                        (void)prov;
                    }
                }
                continue;
            }

            if (!seen_in_tree.insert(real_name).second) continue;

            ScanNode child;
            child.name = pit2->second.name;
            child.version = pit2->second.version;
            child.status = pit2->second.already_installed
                         ? ScanStatus::KEEP : ScanStatus::INSTALL;
            child.reason = pit2->second.already_installed
                         ? "already installed" : "dependency";

            // Recurse into children
            build_install_children(&child, real_name);

            if (child.is_affected() || show_all) {
                parent_node->children.push_back(std::move(child));
            }
        }
    };

    build_install_children(&root, target_name);

    return root;
}

ScanNode scan_install_from_file(const fs::path& lpkg_path, bool show_all) {
    // Read metadata from local .lpkg file
    json meta;
    try {
        meta = detail::read_archive_metadata(fs::absolute(lpkg_path));
    } catch (const std::exception& e) {
        ScanNode root;
        root.name = lpkg_path.filename().string();
        root.version = "";
        root.status = ScanStatus::KEEP;
        root.reason = std::string("error: ") + e.what();
        return root;
    }

    std::string name = meta.at(std::string(constants::J_NAME));
    std::string version = meta.at(std::string(constants::J_VERSION));
    std::vector<std::string> dep_strs = meta.value(
        std::string(constants::J_DEPS), std::vector<std::string>{});
    auto deps = detail::parse_dep_strings(dep_strs);

    ScanNode root;
    root.name = name;
    root.version = version;
    root.status = Cache::instance().get_installed_version(name).empty()
                ? ScanStatus::INSTALL : ScanStatus::KEEP;
    root.reason = Cache::instance().get_installed_version(name).empty()
                ? "target package (local)" : "already installed (local)";

    // Load repository for transitive resolution
    ensure_dir_exists(Config::get_tmp_dir());
    Repository repo;
    try { repo.load_index(); }
    catch (...) { /* remote resolution unavailable */ }

    // Resolve dependencies not yet installed
    DepMap plan;
    std::set<std::string> visited;
    for (const auto& dep : deps) {
        std::string dep_ver = std::string(constants::VER_LATEST);
        if (!dep.op.empty()) {
            if (auto match = repo.find_best_matching_version(dep.name, dep.op, dep.version_req))
                dep_ver = match->version;
        }
        resolve_transitive_deps(dep.name, dep_ver, plan, visited, repo);
    }

    // Build children from deps
    std::set<std::string> seen;
    seen.insert(name);
    for (const auto& dep : deps) {
        std::string real_name = dep.name;
        std::string installed_ver = Cache::instance().get_installed_version(dep.name);
        if (installed_ver.empty()) {
            // Try provider
            auto prov = repo.find_provider(dep.name);
            if (prov) real_name = prov->name;
        }

        auto pit = plan.find(real_name);
        if (pit == plan.end()) {
            // Not in plan → already installed or not found
            if (!installed_ver.empty()) {
                // Already installed, show with --all
                if (show_all && seen.insert(real_name).second) {
                    ScanNode child;
                    child.name = real_name;
                    child.version = installed_ver;
                    child.status = ScanStatus::KEEP;
                    child.reason = "already installed";
                    root.children.push_back(std::move(child));
                }
            }
            continue;
        }

        if (!seen.insert(real_name).second) continue;

        ScanNode child;
        child.name = pit->second.name;
        child.version = pit->second.version;
        child.status = pit->second.already_installed ? ScanStatus::KEEP : ScanStatus::INSTALL;
        child.reason = pit->second.already_installed ? "already installed" : "dependency";

        // Recurse children of this dep
        std::set<std::string> child_seen;
        child_seen.insert(real_name);
        std::function<void(ScanNode*, const std::string&)> add_grandchildren;
        add_grandchildren = [&](ScanNode* parent, const std::string& pn) {
            auto pt = plan.find(pn);
            if (pt == plan.end()) return;
            for (const auto& cd : pt->second.deps) {
                auto ct = plan.find(cd.name);
                if (ct == plan.end()) continue;
                if (!child_seen.insert(ct->second.name).second) continue;
                ScanNode gc;
                gc.name = ct->second.name;
                gc.version = ct->second.version;
                gc.status = ct->second.already_installed ? ScanStatus::KEEP : ScanStatus::INSTALL;
                gc.reason = ct->second.already_installed ? "already installed" : "dependency";
                add_grandchildren(&gc, ct->second.name);
                if (gc.is_affected() || show_all)
                    parent->children.push_back(std::move(gc));
            }
        };
        add_grandchildren(&child, pit->second.name);

        if (child.is_affected() || show_all)
            root.children.push_back(std::move(child));
    }

    return root;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Display helpers
// ═══════════════════════════════════════════════════════════════════════════

std::string_view status_label(ScanStatus s) {
    switch (s) {
        case ScanStatus::REMOVED:     return "WILL BE REMOVED";
        case ScanStatus::REBUILD:     return "NEEDS REBUILD";
        case ScanStatus::INSTALL:     return "WILL BE INSTALLED";
        case ScanStatus::ABI_CHANGED: return "ABI CHANGED";
        case ScanStatus::KEEP:        return "UNCHANGED";
    }
    return "UNKNOWN";
}

std::string_view status_color(ScanStatus s) {
    switch (s) {
        case ScanStatus::REMOVED:     return constants::COLOR_RED;
        case ScanStatus::REBUILD:     return constants::COLOR_YELLOW;
        case ScanStatus::INSTALL:     return constants::COLOR_GREEN;
        case ScanStatus::ABI_CHANGED: return constants::COLOR_WHITE;
        case ScanStatus::KEEP:        return constants::COLOR_RESET;
    }
    return constants::COLOR_RESET;
}

namespace {

void print_subtree(const ScanNode& node, const std::string& prefix,
                   bool /*is_last*/) {
    for (size_t i = 0; i < node.children.size(); ++i) {
        const auto& child = node.children[i];
        bool last_child = (i == node.children.size() - 1);

        std::cout << prefix
                  << (last_child ? "└── " : "├── ")
                  << status_color(child.status)
                  << child.name << " (" << child.version << ") "
                  << "[" << status_label(child.status) << "]"
                  << constants::COLOR_RESET;
        if (!child.reason.empty())
            std::cout << "  (" << child.reason << ")";
        std::cout << "\n";

        std::string child_prefix = prefix + (last_child ? "    " : "│   ");
        print_subtree(child, child_prefix, true);
    }
}

} // anonymous namespace

void print_tree(const ScanNode& node) {
    std::cout << status_color(node.status)
              << node.name << " (" << node.version << ") "
              << "[" << status_label(node.status) << "]"
              << constants::COLOR_RESET;
    if (!node.reason.empty())
        std::cout << "  (" << node.reason << ")";
    std::cout << "\n";
    print_subtree(node, "", true);
}

} // namespace depscan
