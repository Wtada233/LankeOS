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

// ── Reverse-dependency BFS (transitive closure, local cache) ──────────────
void collect_transitive_rdeps(const std::string& root_pkg,
                              std::unordered_set<std::string>& result,
                              std::unordered_set<std::string>& visited)
{
    if (!visited.insert(root_pkg).second) return;
    auto rdeps = Cache::instance().get_reverse_deps(root_pkg);
    for (const auto& cap : Cache::instance().get_package_provides(root_pkg)) {
        auto cap_rdeps = Cache::instance().get_reverse_deps(cap);
        rdeps.insert(cap_rdeps.begin(), cap_rdeps.end());
    }
    for (const auto& rdep : rdeps) {
        if (rdep == root_pkg) continue;
        if (result.insert(rdep).second)
            collect_transitive_rdeps(rdep, result, visited);
    }
}

// ── Forward-dependency resolution (for install scan) ──────────────────────
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
    if (visited.contains(pkg_name) || plan.contains(pkg_name)) return;

    std::string installed_ver = Cache::instance().get_installed_version(pkg_name);
    if (!installed_ver.empty()
        && (version_spec.empty() || version_spec == constants::VER_LATEST)) {
        plan[pkg_name] = {pkg_name, installed_ver, true, {}};
        return;
    }

    visited.insert(pkg_name);

    auto pkg_info = (version_spec == constants::VER_LATEST || version_spec.empty())
        ? repo.find_package(pkg_name)
        : repo.find_package(pkg_name, version_spec);

    if (!pkg_info) {
        if (auto prov = repo.find_provider(pkg_name)) {
            pkg_info = repo.find_package(prov->name);
        }
        if (!pkg_info) { visited.erase(pkg_name); return; }
    }

    bool already = !Cache::instance().get_installed_version(pkg_name).empty();
    plan[pkg_info->name] = {pkg_info->name, pkg_info->version,
                            already, pkg_info->dependencies};

    for (const auto& dep : pkg_info->dependencies) {
        if (!Config::instance().no_deps_mode()) {
            std::string dv(constants::VER_LATEST);
            if (!dep.op.empty()) {
                if (auto m = repo.find_best_matching_version(dep.name, dep.op, dep.version_req))
                    dv = m->version;
            }
            resolve_transitive_deps(dep.name, dv, plan, visited, repo);
        }
    }
    visited.erase(pkg_name);
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Repository reverse-dependency helpers
// ═══════════════════════════════════════════════════════════════════════════

namespace {

std::vector<std::string_view> sv_split(std::string_view s, char d) {
    std::vector<std::string_view> r;
    size_t start = 0, end;
    while ((end = s.find(d, start)) != std::string_view::npos) {
        r.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    r.push_back(s.substr(start));
    return r;
}

// Build a reverse-dep map from the cached repo index.
std::unordered_map<std::string, std::unordered_set<std::string>>
build_repo_revdep_map() {
    std::unordered_map<std::string, std::unordered_set<std::string>> rev;
    fs::path idx = Config::get_tmp_dir() / constants::REPO_INDEX_TMP;
    if (!fs::exists(idx)) return rev;

    std::ifstream f(idx);
    std::string line;
    static const std::vector<std::string_view> ops = {
        ">=", "<=", "!=", "==", ">", "<", "="
    };

    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.back() == '\r') line.pop_back();
        auto parts = sv_split(line, constants::PIPE_CHAR);
        if (parts.size() < 2) continue;

        std::string name(parts[0]);
        auto blocks = sv_split(parts[1], constants::SEMICOLON_CHAR);
        if (blocks.empty()) continue;
        auto vh = sv_split(blocks.back(), constants::COLON_CHAR);
        if (vh.size() < 3) continue;

        for (auto ds : sv_split(vh[2], constants::COMMA_CHAR)) {
            std::string_view dn = ds;
            for (const auto& op : ops) {
                if (auto p = ds.find(op); p != std::string_view::npos) {
                    dn = ds.substr(0, p); break;
                }
            }
            if (!dn.empty()) rev[std::string(dn)].insert(name);
        }
    }
    return rev;
}

void repo_transitive_rdeps(
    const std::string& pkg,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& rev,
    std::unordered_set<std::string>& result,
    std::unordered_set<std::string>& visited)
{
    if (!visited.insert(pkg).second) return;
    auto it = rev.find(pkg);
    if (it == rev.end()) return;
    for (const auto& dep : it->second) {
        if (dep != pkg && result.insert(dep).second)
            repo_transitive_rdeps(dep, rev, result, visited);
    }
}

// ── Load repository and build revdep map; returns empty map on failure ────
auto load_repo_revdep()
    -> std::unordered_map<std::string, std::unordered_set<std::string>>
{
    ensure_dir_exists(Config::get_tmp_dir());
    Repository repo;
    try { repo.load_index(); } catch (...) {}
    return build_repo_revdep_map();
}

// ── Recursive tree builder for local-cache remove scan ────────────────────
void build_remove_tree_local(
    ScanNode& node,
    const std::string& node_name,
    std::unordered_set<std::string>& affected,
    bool show_all)
{
    auto rdeps = Cache::instance().get_reverse_deps(node_name);
    for (const auto& cap : Cache::instance().get_package_provides(node_name)) {
        auto cr = Cache::instance().get_reverse_deps(cap);
        rdeps.insert(cr.begin(), cr.end());
    }

    for (const auto& rdep : rdeps) {
        if (rdep == node_name || !affected.contains(rdep)) continue;
        affected.erase(rdep);

        ScanNode child;
        child.name = rdep;
        child.version = version_or_missing(rdep);
        child.status = ScanStatus::REMOVED;
        child.reason = "depends on " + node_name;

        build_remove_tree_local(child, rdep, affected, show_all);

        if (show_all) {
            fs::path df = Config::instance().dep_dir() / rdep;
            if (fs::exists(df)) {
                std::ifstream f(df); std::string l;
                std::set<std::string> seen;
                while (std::getline(f, l)) {
                    std::string dn; std::istringstream(l) >> dn;
                    if (dn.empty() || !seen.insert(dn).second) continue;
                    ScanNode k;
                    k.name = dn; k.version = version_or_missing(dn);
                    k.status = ScanStatus::KEEP;
                    k.reason = "shared dependency, unchanged";
                    child.children.push_back(std::move(k));
                }
            }
        }

        node.children.push_back(std::move(child));
    }
}

// ── Recursive tree builder for repo-fallback remove scan ──────────────────
void build_remove_tree_repo(
    ScanNode& node,
    const std::string& node_name,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& rev,
    std::unordered_set<std::string>& affected)
{
    auto it = rev.find(node_name);
    if (it == rev.end()) return;
    for (const auto& dep : it->second) {
        if (dep == node_name || !affected.contains(dep)) continue;
        affected.erase(dep);
        ScanNode child;
        child.name = dep;
        child.version = "(in repository)";
        child.status = ScanStatus::REMOVED;
        child.reason = "depends on " + node_name + " (repo)";
        build_remove_tree_repo(child, dep, rev, affected);
        node.children.push_back(std::move(child));
    }
}

// ── Recursive tree builder for install scan ───────────────────────────────
void build_install_tree(
    ScanNode* parent,
    const std::string& parent_name,
    const DepMap& plan,
    std::set<std::string>& seen,
    Repository& repo,
    bool show_all)
{
    auto pit = plan.find(parent_name);
    if (pit == plan.end()) return;

    for (const auto& dep : pit->second.deps) {
        // Resolve virtual provider
        std::string real = dep.name;
        auto dver = Cache::instance().get_installed_version(dep.name);
        if (dver.empty() && !plan.contains(dep.name)) {
            if (auto prov = repo.find_provider(dep.name))
                real = prov->name;
        }

        auto dit = plan.find(real);
        if (dit == plan.end()) continue;
        if (!seen.insert(real).second) continue;

        ScanNode child;
        child.name = dit->second.name;
        child.version = dit->second.version;
        child.status = dit->second.already_installed
                     ? ScanStatus::KEEP : ScanStatus::INSTALL;
        child.reason = dit->second.already_installed
                     ? "already installed" : "dependency";

        build_install_tree(&child, real, plan, seen, repo, show_all);

        if (child.is_affected() || show_all)
            parent->children.push_back(std::move(child));
    }
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
//  scan_remove_tree
// ═══════════════════════════════════════════════════════════════════════════

ScanNode scan_remove_tree(const std::string& pkg_name, bool show_all) {
    auto& cache = Cache::instance();

    // Not installed → fall back to repository index
    if (cache.get_installed_version(pkg_name).empty()) {
        auto rev = load_repo_revdep();
        if (rev.find(pkg_name) == rev.end()) {
            ScanNode r; r.name = pkg_name;
            r.version = "(not installed)";
            r.status = ScanStatus::REMOVED;
            r.reason = "not found in repository";
            return r;
        }

        std::unordered_set<std::string> affected, visited;
        repo_transitive_rdeps(pkg_name, rev, affected, visited);
        affected.insert(pkg_name);

        ScanNode root;
        root.name = pkg_name; root.version = "(in repository)";
        root.status = ScanStatus::REMOVED; root.reason = "target package (repo)";
        affected.erase(pkg_name);
        build_remove_tree_repo(root, pkg_name, rev, affected);
        return root;
    }

    // Installed locally → use cache
    std::unordered_set<std::string> affected, visited;
    collect_transitive_rdeps(pkg_name, affected, visited);
    affected.insert(pkg_name);

    ScanNode root;
    root.name = pkg_name; root.version = version_or_missing(pkg_name);
    root.status = ScanStatus::REMOVED; root.reason = "target package";
    affected.erase(pkg_name);
    build_remove_tree_local(root, pkg_name, affected, show_all);
    return root;
}

// ═══════════════════════════════════════════════════════════════════════════
//  scan_abibreak_tree
// ═══════════════════════════════════════════════════════════════════════════

ScanNode scan_abibreak_tree(const std::string& pkg_name, bool show_all) {
    auto& cache = Cache::instance();

    // Not installed → fall back to repository index
    if (cache.get_installed_version(pkg_name).empty()) {
        auto rev = load_repo_revdep();
        ScanNode root;
        root.name = pkg_name; root.version = "(in repository)";
        root.status = ScanStatus::ABI_CHANGED;
        root.reason = "ABI changed — direct dependents need rebuild";

        auto it = rev.find(pkg_name);
        if (it != rev.end()) {
            for (const auto& dep : it->second) {
                if (dep == pkg_name) continue;
                ScanNode child;
                child.name = dep; child.version = "(in repository)";
                child.status = ScanStatus::REBUILD;
                child.reason = "direct dependency of " + pkg_name + " (repo)";

                if (show_all) {
                    auto git = rev.find(dep);
                    if (git != rev.end()) {
                        for (const auto& gdep : git->second) {
                            if (gdep == dep || gdep == pkg_name) continue;
                            ScanNode k; k.name = gdep;
                            k.version = "(in repository)";
                            k.status = ScanStatus::KEEP;
                            k.reason = "indirect — ABI preserved through abstraction";
                            child.children.push_back(std::move(k));
                        }
                    }
                }
                root.children.push_back(std::move(child));
            }
        }
        return root;
    }

    // Installed locally → use cache
    ScanNode root;
    root.name = pkg_name; root.version = version_or_missing(pkg_name);
    root.status = ScanStatus::ABI_CHANGED;
    root.reason = "ABI changed — direct dependents need rebuild";

    auto rdeps = cache.get_reverse_deps(pkg_name);
    for (const auto& cap : cache.get_package_provides(pkg_name)) {
        auto cr = cache.get_reverse_deps(cap);
        rdeps.insert(cr.begin(), cr.end());
    }

    for (const auto& rdep : rdeps) {
        if (rdep == pkg_name) continue;
        ScanNode child;
        child.name = rdep; child.version = version_or_missing(rdep);
        child.status = ScanStatus::REBUILD;
        child.reason = "direct dependency of " + pkg_name;

        if (show_all) {
            auto ind = cache.get_reverse_deps(rdep);
            for (const auto& cap : cache.get_package_provides(rdep)) {
                auto ci = cache.get_reverse_deps(cap);
                ind.insert(ci.begin(), ci.end());
            }
            for (const auto& ir : ind) {
                if (ir == rdep || ir == pkg_name) continue;
                ScanNode k; k.name = ir; k.version = version_or_missing(ir);
                k.status = ScanStatus::KEEP;
                k.reason = "indirect — ABI preserved through abstraction";
                child.children.push_back(std::move(k));
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
    ensure_dir_exists(Config::get_tmp_dir());
    Repository repo;
    try { repo.load_index(); }
    catch (const std::exception& e) {
        log_warning(string_format("warning.repo_index_load_failed", e.what()));
    }

    std::string target_name = pkg_name;
    std::string target_ver(constants::VER_LATEST);
    if (auto pos = pkg_name.find(':'); pos != std::string_view::npos) {
        target_name = pkg_name.substr(0, pos);
        target_ver = pkg_name.substr(pos + 1);
    }

    DepMap plan;
    std::set<std::string> visited;
    resolve_transitive_deps(target_name, target_ver, plan, visited, repo);

    if (plan.empty()) {
        ScanNode r; r.name = target_name;
        r.version = version_or_missing(target_name);
        r.status = ScanStatus::KEEP;
        r.reason = "package not found in repository";
        return r;
    }

    auto it = plan.find(target_name);
    ScanNode root;
    root.name = it->second.name;
    root.version = it->second.version;
    root.status = it->second.already_installed ? ScanStatus::KEEP : ScanStatus::INSTALL;
    root.reason = it->second.already_installed ? "already installed" : "target package";

    std::set<std::string> seen{target_name};
    build_install_tree(&root, target_name, plan, seen, repo, show_all);
    return root;
}

ScanNode scan_install_from_file(const fs::path& lpkg_path, bool show_all) {
    json meta;
    try {
        meta = detail::read_archive_metadata(fs::absolute(lpkg_path));
    } catch (const std::exception& e) {
        ScanNode r; r.name = lpkg_path.filename().string();
        r.status = ScanStatus::KEEP;
        r.reason = std::string("error: ") + e.what();
        return r;
    }

    std::string name = meta.at(std::string(constants::J_NAME));
    std::string version = meta.at(std::string(constants::J_VERSION));
    auto deps = detail::parse_dep_strings(
        meta.value(std::string(constants::J_DEPS), std::vector<std::string>{}));

    bool not_installed = Cache::instance().get_installed_version(name).empty();
    ScanNode root;
    root.name = name; root.version = version;
    root.status = not_installed ? ScanStatus::INSTALL : ScanStatus::KEEP;
    root.reason = not_installed ? "target package (local)" : "already installed (local)";

    // Resolve transitive deps from repo
    ensure_dir_exists(Config::get_tmp_dir());
    Repository repo;
    try { repo.load_index(); } catch (...) {}

    DepMap plan;
    std::set<std::string> visited;
    for (const auto& dep : deps) {
        std::string dv(constants::VER_LATEST);
        if (!dep.op.empty()) {
            if (auto m = repo.find_best_matching_version(dep.name, dep.op, dep.version_req))
                dv = m->version;
        }
        resolve_transitive_deps(dep.name, dv, plan, visited, repo);
    }

    std::set<std::string> seen{name};
    for (const auto& dep : deps) {
        std::string real = dep.name;
        std::string iv = Cache::instance().get_installed_version(dep.name);
        if (iv.empty()) {
            if (auto prov = repo.find_provider(dep.name))
                real = prov->name;
        }

        auto pit = plan.find(real);
        if (pit == plan.end()) {
            if (!iv.empty() && show_all && seen.insert(real).second) {
                ScanNode c; c.name = real; c.version = iv;
                c.status = ScanStatus::KEEP; c.reason = "already installed";
                root.children.push_back(std::move(c));
            }
            continue;
        }
        if (!seen.insert(real).second) continue;

        ScanNode child;
        child.name = pit->second.name;
        child.version = pit->second.version;
        child.status = pit->second.already_installed ? ScanStatus::KEEP : ScanStatus::INSTALL;
        child.reason = pit->second.already_installed ? "already installed" : "dependency";

        std::set<std::string> cs{real};
        build_install_tree(&child, real, plan, cs, repo, show_all);

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

namespace {

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

void print_subtree(const ScanNode& node, const std::string& prefix) {
    for (size_t i = 0; i < node.children.size(); ++i) {
        const auto& child = node.children[i];
        bool last = (i == node.children.size() - 1);
        std::cout << prefix << (last ? "└── " : "├── ")
                  << status_color(child.status)
                  << child.name << " (" << child.version << ") "
                  << "[" << status_label(child.status) << "]"
                  << constants::COLOR_RESET;
        if (!child.reason.empty())
            std::cout << "  (" << child.reason << ")";
        std::cout << "\n";
        print_subtree(child, prefix + (last ? "    " : "│   "));
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
    print_subtree(node, "");
}

} // namespace depscan
