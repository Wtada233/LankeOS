#include "install_common.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace detail {

void run_hook(std::string_view pkg_name, std::string_view hook_name) {
    if (get_no_hooks_mode()) return;

    const fs::path hook_path = HOOKS_DIR / pkg_name / hook_name;
    if (!fs::exists(hook_path) || !fs::is_regular_file(hook_path)) return;

    log_info(string_format("info.running_hook", std::string(hook_name)));

    const bool use_chroot = (ROOT_DIR != "/" && !ROOT_DIR.empty());
    std::vector<std::string> args = {std::string(constants::BIN_SH), "-c"};

    if (use_chroot) {
        if (!fs::exists(ROOT_DIR / "bin/sh")) {
            log_warning(string_format("warning.hook_failed_setup", std::string(hook_name), get_string("error.sh_not_found")));
            return;
        }
        const fs::path hook_rel = fs::relative(hook_path, ROOT_DIR);
        args.push_back("/" + hook_rel.string());
    } else {
        args.push_back(hook_path.string());
    }

    pid_t pid = fork();
    if (pid == -1) return;
    if (pid == 0) {
        if (use_chroot) {
            if (unshare(CLONE_NEWNS) != 0) _exit(1);
            mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
            if (chroot(ROOT_DIR.c_str()) != 0) _exit(1);
            if (chdir("/") != 0) _exit(1);
        }

        std::vector<char*> c_args;
        for (const auto& arg : args) c_args.push_back(const_cast<char*>(arg.c_str()));
        c_args.push_back(nullptr);

        execv(c_args[0], c_args.data());
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    int ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (ret != 0) {
        log_warning(string_format("warning.hook_failed_exec", std::string(hook_name), std::to_string(ret)));
    }
}

void read_package_metadata(const fs::path& tmp_pkg_dir, std::string& name, std::string& version,
                           std::vector<std::string>& deps, std::vector<std::string>& provides,
                           std::string& man) {
    fs::path meta_path = tmp_pkg_dir / constants::PKG_METADATA_FILE;
    json meta;
    {
        std::ifstream f(meta_path);
        if (!f.is_open()) throw LpkgException(string_format("error.open_file_failed", meta_path.string()));
        f >> meta;
    }
    name = meta.at(std::string(constants::J_NAME)).get<std::string>();
    version = meta.at(std::string(constants::J_VERSION)).get<std::string>();
    deps = meta.value(std::string(constants::J_DEPS), std::vector<std::string>{});
    provides = meta.value(std::string(constants::J_PROVIDES), std::vector<std::string>{});
    man = meta.value(std::string(constants::J_MAN), "");
}

std::vector<std::string> scan_content_files(const fs::path& content_dir) {
    std::vector<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(content_dir)) {
        if (entry.is_directory()) continue;
        files.push_back(entry.path().lexically_relative(content_dir).string());
    }
    return files;
}

std::vector<DependencyInfo> parse_dep_strings(const std::vector<std::string>& dep_strs) {
    std::vector<DependencyInfo> deps;
    for (const auto& d_str : dep_strs) {
        std::stringstream ss(d_str);
        std::string dn, op, rv;
        if (ss >> dn) {
            DependencyInfo d{.name = dn, .op = "", .version_req = ""};
            if (ss >> op >> rv) { d.op = op; d.version_req = rv; }
            deps.push_back(std::move(d));
        }
    }
    return deps;
}

void resolve_package_dependencies(const std::string& pkg_name, const std::string& version_spec,
                                  bool is_explicit, InstallContext& ctx,
                                  std::set<std::string>& visited_stack) {
    if (visited_stack.contains(pkg_name)) {
        log_warning(string_format("warning.circular_dependency", pkg_name, pkg_name));
        return;
    }
    if (ctx.plan.contains(pkg_name)) {
        if (is_explicit) ctx.plan.at(pkg_name).is_explicit = true;
        return;
    }

    const std::string installed_version = Cache::instance().get_installed_version(pkg_name);
    fs::path local_path;
    std::string latest_version, pkg_hash;
    std::vector<DependencyInfo> deps;
    std::vector<std::string> provides;

    if (auto it = ctx.local_candidates.find(pkg_name); it != ctx.local_candidates.end()) {
        local_path = it->second;
        std::string m_json = extract_file_from_archive(local_path, std::string(constants::PKG_METADATA_FILE));
        if (m_json.empty()) throw LpkgException(string_format("error.local_pkg_missing_metadata", local_path.string()));

        json meta = json::parse(m_json);
        latest_version = meta.at(std::string(constants::J_VERSION)).get<std::string>();

        for (const auto& d_str : meta.value(std::string(constants::J_DEPS), std::vector<std::string>{})) {
            std::stringstream ss(d_str);
            std::string dn, op, rv;
            if (ss >> dn) {
                DependencyInfo d{.name = dn, .op = "", .version_req = ""};
                if (ss >> op >> rv) { d.op = op; d.version_req = rv; }
                deps.push_back(std::move(d));
            }
        }
        provides = meta.value(std::string(constants::J_PROVIDES), std::vector<std::string>{});
    } else {
        auto pkg_info = (version_spec == constants::VER_LATEST) ? ctx.repo.find_package(pkg_name)
                         : ctx.repo.find_package(pkg_name, version_spec);
        if (!pkg_info) {
            if (auto prov = ctx.repo.find_provider(pkg_name)) {
                resolve_package_dependencies(prov->name, prov->version, is_explicit, ctx, visited_stack);
                return;
            }
            if (installed_version.empty()) log_warning(string_format("warning.package_not_in_repo", pkg_name));
            return;
        }
        latest_version = pkg_info->version;
        pkg_hash = pkg_info->sha256;
        deps = pkg_info->dependencies;
        provides = pkg_info->provides;
    }

    if (latest_version.empty()) latest_version = std::string(constants::VER_DEFAULT);

    if (!ctx.force_reinstall || !is_explicit) {
        if (!is_explicit && !installed_version.empty() && !version_compare(installed_version, latest_version)) return;
        if (is_explicit && !installed_version.empty() && installed_version == latest_version) return;
    }

    visited_stack.insert(pkg_name);
    InstallPlan p{
        .name = pkg_name, .actual_version = latest_version, .sha256 = pkg_hash,
        .is_explicit = is_explicit, .local_path = local_path, .dependencies = deps,
        .provides = provides, .force_reinstall = (ctx.force_reinstall && is_explicit)
    };

    if (!get_no_deps_mode()) {
        for (const auto& dep : deps) {
            const std::string idv = Cache::instance().get_installed_version(dep.name);
            bool needs_resolution = idv.empty();
            if (!needs_resolution && !dep.op.empty() && idv != "virtual" && !version_satisfies(idv, dep.op, dep.version_req)) {
                if (!ctx.plan.contains(dep.name)) {
                    log_info(string_format("info.adding_upgrade_to_plan", dep.name, dep.version_req));
                    needs_resolution = true;
                }
            }
            if (needs_resolution) {
                std::string req_ver = std::string(constants::VER_LATEST);
                if (!dep.op.empty()) {
                    if (auto matching = ctx.repo.find_best_matching_version(dep.name, dep.op, dep.version_req))
                        req_ver = matching->version;
                }
                resolve_package_dependencies(dep.name, req_ver, false, ctx, visited_stack);
            }

            std::string cand_v = ctx.plan.contains(dep.name) ? ctx.plan[dep.name].actual_version
                                : Cache::instance().get_installed_version(dep.name);
            if (!dep.op.empty() && !cand_v.empty() && cand_v != "virtual" && !version_satisfies(cand_v, dep.op, dep.version_req))
                throw LpkgException(string_format("error.candidate_dep_version_mismatch", dep.name, cand_v, dep.op, dep.version_req));
        }
    }
    ctx.plan[pkg_name] = std::move(p);
    ctx.install_order.push_back(pkg_name);
    visited_stack.erase(pkg_name);
}

std::set<std::string> check_plan_consistency(const std::map<std::string, InstallPlan>& plan) {
    std::set<std::string> broken;
    auto& cache = Cache::instance();
    std::lock_guard lock(cache.get_mutex());
    for (const auto& [pkg, ver] : cache.get_all_installed()) {
        if (plan.contains(pkg)) continue;
        const fs::path dep_file = DEP_DIR / pkg;
        if (!fs::exists(dep_file)) continue;
        std::ifstream f(dep_file);
        std::string line;
        while (std::getline(f, line)) {
            std::stringstream ss(line);
            std::string dep_name, op, req_v;
            if (ss >> dep_name && plan.contains(dep_name)) {
                const std::string& new_v = plan.at(dep_name).actual_version;
                if (ss >> op >> req_v && !version_satisfies(new_v, op, req_v)) {
                    log_error(string_format("error.conflict_breaks_existing", dep_name, new_v, pkg, op, req_v));
                    broken.insert(pkg);
                }
            }
        }
    }
    return broken;
}

std::unordered_set<std::string> get_all_required_packages() {
    auto& cache = Cache::instance();
    std::unordered_set<std::string> req;
    {
        std::lock_guard lock(cache.get_mutex());
        req = cache.get_all_held();
    }
    std::vector q(req.begin(), req.end());
    size_t head = 0;
    while (head < q.size()) {
        const std::string curr = q[head++];
        const fs::path p = DEP_DIR / curr;
        if (!fs::exists(p)) continue;
        std::ifstream f(p);
        std::string line;
        while (std::getline(f, line)) {
            std::string d_name = line;
            if (const auto pos = line.find_first_of(" \t<>="); pos != std::string::npos) d_name = line.substr(0, pos);
            auto check_and_add = [&](const std::string& name) {
                if (cache.is_installed(name) && !req.contains(name)) {
                    req.insert(name);
                    q.push_back(name);
                }
            };
            if (cache.is_installed(d_name)) check_and_add(d_name);
            else for (const auto& prov : cache.get_providers(d_name)) check_and_add(prov);
        }
    }
    return req;
}

} // namespace detail
