#include "package_manager.hpp"

#include "install_common.hpp"

#include "archive.hpp"
#include "cache.hpp"
#include "trigger.hpp"
#include "config.hpp"
#include "downloader.hpp"
#include "exception.hpp"
#include "hash.hpp"
#include "localization.hpp"
#include "utils.hpp"
#include "version.hpp"
#include "repository.hpp"
#include "constants.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

// =====================================================================
// Public API
// =====================================================================

void write_cache() {
    Cache::instance().write();
}

void install_packages(const std::vector<std::string>& pkg_args,
                      const std::string& hash_file_path, bool force_reinstall) {
    Cache::instance().load();
    TmpDirManager tmp;
    Repository repo;
    try { repo.load_index(); }
    catch (const std::exception& e) {
        log_warning(string_format("warning.repo_index_load_failed", e.what()));
    }

    std::map<std::string, InstallPlan> plan;
    std::vector<std::string> order;
    std::map<std::string, fs::path> locals;
    std::vector<std::pair<std::string, std::string>> targets;

    std::string provided_hash;
    if (!hash_file_path.empty()) {
        std::ifstream hf(hash_file_path);
        if (!(hf >> provided_hash))
            throw LpkgException(get_string("error.read_hash_failed"));
    }

    for (const auto& arg : pkg_args) {
        const fs::path p(arg);
        if (p.extension() == constants::EXT_ZST
            || p.extension() == constants::EXT_LPKG
            || arg.find('/') != std::string::npos) {
            if (fs::exists(p)) {
                try {
                    json meta = detail::read_archive_metadata(fs::absolute(p));
                    std::string n = meta.at(std::string(constants::J_NAME));
                    std::string v = meta.at(std::string(constants::J_VERSION));
                    locals[n] = fs::absolute(p);
                    targets.emplace_back(n, v);
                } catch (const std::exception& e) {
                    log_error(string_format("warning.skip_invalid_local_pkg", arg, e.what()));
                }
            } else {
                log_error(string_format("error.local_pkg_not_found", arg));
            }
        } else {
            std::string n = arg, v = std::string(constants::VER_LATEST);
            if (const auto pos = arg.find(':'); pos != std::string::npos) {
                n = arg.substr(0, pos);
                v = arg.substr(pos + 1);
            }
            targets.emplace_back(n, v);
        }
    }

    InstallContext ctx{repo, plan, order, locals, targets,
                       force_reinstall, /*top_level=*/true, {}};

    // Phase 1: Initial dependency resolution
    for (const auto& [n, v] : targets) {
        std::set<std::string> vs;
        detail::resolve_package_dependencies(n, v, true, ctx, vs);
    }

    if (!provided_hash.empty()) {
        if (locals.empty())
            throw LpkgException(get_string("error.hash_requires_local"));
        for (auto& [n, p] : plan)
            if (!p.local_path.empty()) p.sha256 = provided_hash;
    }

    // Phase 2: Static consistency check (no download — pure dep-graph analysis)
    // Dynamic metadata verification happens later, inside install_packages_internal,
    // after the user has confirmed.

    if (plan.empty()) {
        log_info(get_string("info.all_packages_already_installed"));
        return;
    }

    if (auto broken = detail::check_plan_consistency(plan); !broken.empty()) {
        log_error(get_string("error.dependency_conflict_title"));
        if (user_confirms(get_string("prompt.remove_conflict_pkgs"))) {
            for (const auto& pkg : broken) remove_package(pkg, true);
            Cache::instance().write();
            install_packages(pkg_args, hash_file_path, force_reinstall);
            return;
        }
        log_info(get_string("info.installation_aborted"));
        return;
    }

    // Show plan and ask for confirmation
    std::string prompt;
    for (const auto& n : order) {
        const auto& p = plan.at(n);
        prompt += "  "
            + string_format(p.is_explicit ? "info.package_list_item"
                                          : "info.package_list_item_dep",
                            p.name, p.actual_version)
            + "\n";
    }
    if (!user_confirms(prompt + get_string("info.confirm_proceed"))) {
        log_info(get_string("info.installation_aborted"));
        return;
    }

    // Phase 3: Actual installation with inline metadata verification
    ctx.successfully_installed.clear();
    try {
        install_packages_internal(ctx);
    } catch (const std::exception& e) {
        log_error(get_string("error.installation_failed_rolling_back"));
        for (const auto& name : ctx.successfully_installed | std::views::reverse) {
            try { remove_package(name, true); } catch (...) {}
        }
        Cache::instance().write();
        throw;
    }

    Cache::instance().write();
    TriggerManager::instance().run_all();
    log_info(get_string("info.install_complete"));
}

void install_packages_internal(InstallContext& ctx) {
    size_t i = 0;
    while (i < ctx.install_order.size()) {
        const std::string& n = ctx.install_order[i];
        ++i;

        if (std::find(ctx.successfully_installed.begin(),
                      ctx.successfully_installed.end(), n)
            != ctx.successfully_installed.end()) {
            continue;
        }

        auto& p = ctx.plan.at(n);

        // Inline metadata verification: download & check real metadata before install
        if (!p.metadata_verified) {
            InstallationTask check_task(p.name, p.actual_version, p.is_explicit,
                Cache::instance().get_installed_version(p.name),
                p.local_path, p.sha256, p.force_reinstall);
            ensure_dir_exists(check_task.tmp_pkg_dir());
            check_task.download_and_verify_package();

            // Read metadata.json directly from the archive without full extraction
            json meta = detail::read_archive_metadata(check_task.archive_path());
            std::vector<std::string> dep_strs = meta.value(
                std::string(constants::J_DEPS), std::vector<std::string>{});
            auto actual_deps = detail::parse_dep_strings(dep_strs);
            std::vector<std::string> actual_provides = meta.value(
                std::string(constants::J_PROVIDES), std::vector<std::string>{});

            bool metadata_differs = (actual_deps.size() != p.dependencies.size())
                || (actual_provides != p.provides);
            if (!metadata_differs) {
                for (size_t di = 0; di < actual_deps.size(); ++di) {
                    if (actual_deps[di].name != p.dependencies[di].name
                        || actual_deps[di].op != p.dependencies[di].op
                        || actual_deps[di].version_req != p.dependencies[di].version_req) {
                        metadata_differs = true; break;
                    }
                }
            }

            if (metadata_differs) {
                log_info(string_format("info.resolving_metadata", p.name));
                ctx.repo.update_package_info(p.name, p.actual_version,
                    actual_deps, actual_provides);
                ctx.local_candidates[p.name] = check_task.archive_path();

                // Rollback any packages already installed in this transaction
                for (const auto& done_name : ctx.successfully_installed | std::views::reverse) {
                    try { remove_package(done_name, true); } catch (...) {}
                }
                ctx.successfully_installed.clear();

                ctx.plan.clear();
                ctx.install_order.clear();
                for (const auto& [tn, tv] : ctx.targets) {
                    std::set<std::string> vs;
                    detail::resolve_package_dependencies(tn, tv, true, ctx, vs);
                }
                i = 0; // restart from beginning with new plan
                continue;
            }

            // Save the downloaded archive path so the real task doesn't re-download
            p.local_path = check_task.archive_path();
            p.metadata_verified = true;
        }

        // Now install
        InstallationTask task(p.name, p.actual_version, p.is_explicit,
                              Cache::instance().get_installed_version(p.name),
                              p.local_path, p.sha256, p.force_reinstall);
        try {
            task.run(&ctx);
            ctx.successfully_installed.push_back(p.name);
        } catch (const std::exception& e) {
            throw;
        }
    }
}

void remove_package(const std::string& pkg_name, bool force) {
    const std::string ver = Cache::instance().get_installed_version(pkg_name);
    if (ver.empty()) {
        log_info(string_format("info.package_not_installed", pkg_name));
        return;
    }

    if (!force) {
        if (Cache::instance().is_essential(pkg_name)) {
            log_error(string_format("error.skip_remove_essential", pkg_name));
            return;
        }
        if (auto rdeps = Cache::instance().get_reverse_deps(pkg_name); !rdeps.empty()) {
            std::string list;
            for (const auto& d : rdeps) list += d + " ";
            log_info(string_format("info.skip_remove_dependency", pkg_name, list));
            return;
        }
        for (const auto& cap : Cache::instance().get_package_provides(pkg_name)) {
            if (auto rdeps = Cache::instance().get_reverse_deps(cap); !rdeps.empty()) {
                std::string list;
                for (const auto& d : rdeps) list += d + " ";
                log_info(string_format("info.skip_remove_dependency", cap, list));
                return;
            }
        }
    }

    log_info(string_format("info.removing_package", pkg_name));
    detail::run_hook(pkg_name, std::string(constants::PRERM_SH));
    remove_package_files(pkg_name, force);

    auto& cache = Cache::instance();
    const fs::path dep_file = Config::instance().dep_dir() / pkg_name;
    if (fs::exists(dep_file)) {
        std::ifstream f(dep_file);
        std::string l;
        while (std::getline(f, l)) {
            std::stringstream ss(l);
            std::string dn;
            if (ss >> dn) cache.remove_reverse_dep(dn, pkg_name);
        }
    }

    fs::remove(dep_file);
    fs::remove(Config::instance().docs_dir() / (pkg_name + std::string(constants::SUFFIX_MAN)));
    fs::remove_all(Config::instance().hooks_dir() / pkg_name);
    cache.remove_installed(pkg_name);
    log_info(string_format("info.package_removed_successfully", pkg_name));
}

void remove_package_files(const std::string& pkg_name, bool force) {
    auto& cache = Cache::instance();
    auto owned_files = cache.get_package_files(pkg_name);
    if (owned_files.empty()) return;

    if (!force) {
        std::vector<std::string> shared;
        for (const auto& f : owned_files) {
            auto owners = cache.get_file_owners(f);
            for (const auto& owner : owners) {
                if (owner != pkg_name) {
                    shared.push_back(f);
                    break;
                }
            }
        }
        if (!shared.empty()) {
            std::string msg = get_string("error.shared_file_header")
                            + std::string(constants::NL);
            for (const auto& file : shared) {
                msg += "  "
                    + string_format("error.shared_file_entry", file, "other packages")
                    + std::string(constants::NL);
            }
            throw LpkgException(msg + get_string("error.removal_aborted"));
        }
    }

    std::vector<fs::path> paths;
    for (const auto& f : owned_files) paths.emplace_back(f);
    std::ranges::sort(paths, std::greater<>{});

    int count = 0;
    for (const auto& p : paths) {
        const fs::path phys = p.is_absolute()
            ? Config::instance().root_dir() / fs::path(p).relative_path()
            : Config::instance().root_dir() / p;
        if (fs::exists(phys) || fs::is_symlink(phys)) {
            fs::remove(phys);
            ++count;
        }
        cache.remove_file_owner(p.string(), pkg_name);
    }
    log_info(string_format("info.files_removed", count));

    for (const auto& cap : cache.get_package_provides(pkg_name)) {
        cache.remove_provider(cap, pkg_name);
    }
}

void autoremove() {
    log_info(get_string("info.checking_autoremove"));
    const auto req = detail::get_all_required_packages();
    std::vector<std::string> to_rem;
    auto& cache = Cache::instance();
    {
        std::lock_guard lock(cache.get_mutex());
        for (const auto& name : cache.get_all_installed() | std::views::keys) {
            if (!req.contains(name)) to_rem.push_back(name);
        }
    }

    if (to_rem.empty()) {
        log_info(get_string("info.no_autoremove_packages"));
    } else {
        log_info(string_format("info.autoremove_candidates", to_rem.size()));
        for (const auto& n : to_rem) {
            try { remove_package(n, true); } catch (...) {}
        }
        log_info(string_format("info.autoremove_complete", to_rem.size()));
    }
}

void upgrade_packages() {
    log_info(get_string("info.checking_upgradable"));
    TmpDirManager tmp;
    Repository repo;
    try {
        repo.load_index();
    } catch (const std::exception& e) {
        log_warning(string_format("warning.repo_index_load_failed", e.what()));
        return;
    }

    std::vector<std::pair<std::string, std::string>> installed;
    {
        std::lock_guard lock(Cache::instance().get_mutex());
        for (const auto& [name, ver] : Cache::instance().get_all_installed()) {
            installed.emplace_back(name, ver);
        }
    }

    int count = 0;
    for (const auto& [n, curr] : installed) {
        auto opt = repo.find_package(n);
        if (!opt) continue;
        const std::string& lat = opt->version;

        if (version_compare(curr, lat)) {
            log_info(string_format("info.upgradable_found", n, curr, lat));
            try {
                log_info(string_format("info.upgrading_package", n, curr, lat));
                const std::string hash = opt->sha256;
                const bool held = Cache::instance().is_held(n);
                InstallationTask t(n, lat, held, curr, "", hash, false);
                t.run();
                ++count;
            } catch (const std::exception& e) {
                log_error(string_format("error.upgrade_failed", n, e.what()));
            }
        }
    }

    if (count > 0)
        log_info(string_format("info.upgraded_packages", count));
    else
        log_info(get_string("info.all_packages_latest"));
    Cache::instance().write();
}

void show_man_page(const std::string& pkg_name) {
    const fs::path p = Config::instance().docs_dir() / (pkg_name + ".man");
    if (!fs::exists(p))
        throw LpkgException(string_format("error.no_man_page", pkg_name));
    std::ifstream f(p);
    if (!f.is_open())
        throw LpkgException(string_format("error.open_man_page_failed", p.string()));
    std::cout << f.rdbuf();
}

void reinstall_package(const std::string& arg) {
    std::string name = arg;
    if (arg.find('/') != std::string::npos || arg.ends_with(".lpkg")) {
        try {
            json meta = detail::read_archive_metadata(fs::absolute(arg));
            name = meta.at(std::string(constants::J_NAME)).get<std::string>();
        } catch (...) {}
    }

    if (Cache::instance().get_installed_version(name).empty()) {
        install_packages({arg});
        return;
    }

    log_info(string_format("info.reinstalling_package", name));
    const bool old_ovr = Config::instance().force_overwrite_mode();
    Config::instance().set_force_overwrite_mode(true);
    try {
        install_packages({arg}, "", true);
    } catch (...) {
        Config::instance().set_force_overwrite_mode(old_ovr);
        throw;
    }
    Config::instance().set_force_overwrite_mode(old_ovr);
}

void query_package(const std::string& pkg_name) {
    if (Cache::instance().get_installed_version(pkg_name).empty()) {
        log_info(string_format("info.package_not_installed", pkg_name));
        return;
    }
    log_info(string_format("info.package_files", pkg_name));
    auto files = Cache::instance().get_package_files(pkg_name);
    for (const auto& f : files) {
        std::cout << "  " << f << "\n";
    }
}

void query_file(const std::string& filename) {
    auto& cache = Cache::instance();
    std::string target = filename;
    auto owners = cache.get_file_owners(target);

    if (owners.empty()) {
        try {
            const fs::path abs_p = fs::absolute(filename);
            if (abs_p.string().starts_with(Config::instance().root_dir().string())) {
                const std::string logical = "/" + fs::relative(abs_p, Config::instance().root_dir()).string();
                owners = cache.get_file_owners(logical);
                if (!owners.empty()) target = logical;
            }
        } catch (...) {}
    }

    if (owners.empty() && !fs::path(filename).is_absolute()) {
        const std::string fallback = (fs::path("/") / filename).string();
        owners = cache.get_file_owners(fallback);
        if (!owners.empty()) target = fallback;
    }

    if (owners.empty()) {
        log_info(string_format("info.file_not_owned", filename));
    } else {
        std::string os;
        for (auto it = owners.begin(); it != owners.end(); ++it) {
            os += *it + (std::next(it) == owners.end() ? "" : ", ");
        }
        log_info(string_format("info.file_owned_by", target, os));
    }
}
