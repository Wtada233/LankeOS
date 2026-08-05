#include "install_common.hpp"

#include "solver.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <random>
#include <sstream>
#include <string>

#include "base/exception.hpp"
#include "base/utils.hpp"
#include "i18n/localization.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace detail
{

/** 从 lpkg 归档文件中读取 metadata.json 并解析为 JSON 对象 */
json read_archive_metadata(const fs::path& archive_path)
{
    std::string meta_json =
        extract_file_from_archive(archive_path, std::string(constants::PKG_METADATA_FILE));
    if (meta_json.empty())
        throw LpkgException(
            string_format("error.local_pkg_missing_metadata", archive_path.string()));
    return json::parse(meta_json);
}

/**
 * 执行包的钩子脚本（如 post-install、pre-remove）
 * 支持 chroot 环境下运行，使用 mount namespace 隔离
 */
void run_hook(std::string_view pkg_name, std::string_view hook_name)
{
    if (Config::instance().no_hooks_mode()) return;

    const fs::path hook_path = Config::instance().hooks_dir() / pkg_name / hook_name;
    if (!fs::exists(hook_path) || !fs::is_regular_file(hook_path)) return;

    log_info(string_format("info.running_hook", std::string(hook_name)));

    const bool use_chroot =
        (Config::instance().root_dir() != "/" && Config::instance().root_dir().string() != "/");
    std::vector<std::string> args = {std::string(constants::BIN_BASH), "-c"};

    if (use_chroot) {
        // 钩子由 BIN_BASH 执行，chroot 后按 /bin/bash 解析——必须检查 bash 而非 sh
        const fs::path bash_rel = std::string(constants::BIN_BASH).substr(1);  // "bin/bash"
        if (!fs::exists(Config::instance().root_dir() / bash_rel)) {
            log_warning(string_format("warning.hook_failed_setup", std::string(hook_name),
                                      get_string("error.bash_not_found")));
            return;
        }
        const fs::path hook_rel = fs::relative(hook_path, Config::instance().root_dir());
        args.push_back("/" + hook_rel.string());
    } else {
        args.push_back(hook_path.string());
    }

    pid_t pid = fork();
    if (pid == -1) return;
    if (pid == 0) {
        if (use_chroot) {
            // 创建独立的 mount namespace，避免影响主机挂载
            if (unshare(CLONE_NEWNS) != 0) _exit(1);
            mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);
            if (chroot(Config::instance().root_dir().c_str()) != 0) _exit(1);
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
        log_warning(
            string_format("warning.hook_failed_exec", std::string(hook_name), std::to_string(ret)));
    }
}

/** 从已解压的包目录读取 metadata.json，提取包名、版本、依赖等信息 */
void read_package_metadata(const fs::path& tmp_pkg_dir, std::string& name, std::string& version,
                           std::vector<std::string>& deps, std::vector<std::string>& provides,
                           std::vector<std::string>& needed_so, std::string& man)
{
    fs::path meta_path = tmp_pkg_dir / constants::PKG_METADATA_FILE;
    json meta;
    {
        std::ifstream f(meta_path);
        if (!f.is_open())
            throw LpkgException(string_format("error.open_file_failed", meta_path.string()));
        f >> meta;
    }
    name = meta.at(std::string(constants::J_NAME)).get<std::string>();
    version = meta.at(std::string(constants::J_VERSION)).get<std::string>();
    deps = meta.value(std::string(constants::J_DEPS), std::vector<std::string>{});
    provides = meta.value(std::string(constants::J_PROVIDES), std::vector<std::string>{});
    needed_so = meta.value(std::string(constants::J_NEEDED_SO), std::vector<std::string>{});
    man = meta.value(std::string(constants::J_MAN), "");
}

/**
 * 扫描包内容目录，返回所有可注册路径的相对路径列表。
 *
 * 实现 pacman 风格：目录以斜杠结尾（如 "usr/bin/"），普通文件不带斜杠。
 * 目录支持多包共同持有，在移除时计数归零才删除。
 *
 * 包含：
 *  - 普通文件 → "usr/bin/bash"
 *  - 符号链接（含指向目录的，如 jvm/conf → /etc/java）→ "jvm/conf"
 *  - 普通目录 → "usr/bin/", "usr/"
 *
 * 不包含：
 *  - content/ 目录本身
 *
 * 这样做的原因：
 *   builder.cpp 清理 USR-Merge 符号链后，包内的目录就是包的真实内容。
 *   目录共享（如多包共享 /usr/bin/）通过引用计数管理，在最后持有者
 *   移除时删除目录。
 */
std::vector<std::string> scan_content_files(const fs::path& content_dir)
{
    std::vector<std::string> entries;
    for (const auto& entry : fs::recursive_directory_iterator(content_dir)) {
        std::string rel = entry.path().lexically_relative(content_dir).string();
        if (entry.is_directory() && !entry.is_symlink()) {
            // 目录 → 末尾加 /，和普通文件区分
            entries.push_back(rel + "/");
        } else {
            // 文件或符号链接 → 原样保留
            entries.push_back(rel);
        }
    }
    return entries;
}

/** 解析依赖字符串列表为 DependencyInfo 结构体，支持复合约束 */
// 实现在 vercmp/dep_parser.cpp 中，此处仅为函数声明转发

// 收集已装包的 requires（deps/ + needed_so/ 文件）与 provides（provides_db）用于建模
// installed repo。provides 必须建模：libsolv 的 dontfix 反向一致性只强制"之前有已装
// provider"的 requires，installed 包不 provide 自己的能力（如 libc.so.6），该 requires
// 就被视为"之前已 broken"而忽略——升级破坏它也不报冲突。
static void collect_installed_requires(const std::string& name, solv::InstalledPkg& p)
{
    const fs::path dep_f = Config::instance().dep_dir() / name;
    if (fs::exists(dep_f)) {
        std::ifstream f(dep_f);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(f, line))
            if (!line.empty()) lines.push_back(line);
        p.deps = parse_dep_strings(lines);
    }
    const fs::path nso_f = Config::instance().needed_so_dir() / name;
    if (fs::exists(nso_f)) {
        std::ifstream f(nso_f);
        std::string so;
        while (std::getline(f, so))
            if (!so.empty()) p.needed_so.push_back(so);
    }
    for (const auto& cap : Cache::instance().get_package_provides(name))
        p.provides.push_back(cap);
}

// 枚举系统 /usr/lib（或 /usr/lib64）下的 SONAME（--use-system-soname 用）
static std::vector<std::string> collect_system_sonames()
{
    std::vector<std::string> out;
    for (const std::string_view sub : { constants::USR_LIB, constants::USR_LIB64 }) {
        std::error_code ec;
        fs::path dir = Config::instance().root_dir() / sub;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file(ec) && !e.is_symlink(ec)) continue;
            const std::string fn = e.path().filename().string();
            if (fn.rfind("lib", 0) == 0 && fn.find(".so") != std::string::npos)
                out.push_back(fn);
        }
    }
    return out;
}

static bool is_explicit_target(const std::vector<std::pair<std::string, std::string>>& targets,
                               const std::string& name)
{
    for (const auto& [n, v] : targets)
        if (n == name) return true;
    return false;
}

/// 用 libsolv 求解安装/升级/重装计划，填充 InstallContext 的 plan + install_order。
/// 取代旧的手动递归解析 resolve_package_dependencies 及其配套手动校验
/// （check_plan_consistency / check_needed_so_consistency / check_forward_soname_integrity）。
void resolve_with_solver(InstallContext& ctx)
{
    // 1. 已装状态（版本 + requires——使 solver 能检测升级破坏已装依赖）
    std::map<std::string, solv::InstalledPkg> installed;
    for (const auto& [name, ver] : Cache::instance().get_all_installed()) {
        solv::InstalledPkg p;
        p.version = ver;
        collect_installed_requires(name, p);
        installed[name] = std::move(p);
    }

    // 2. 本地候选包（读 .lpkg 元数据 → PackageInfo，记 name→path）
    std::vector<PackageInfo> local_pkgs;
    std::map<std::string, fs::path> local_paths;
    for (const auto& [name, path] : ctx.local_candidates) {
        json meta = read_archive_metadata(path);
        PackageInfo pi;
        pi.name = name;
        pi.version = meta.at(std::string(constants::J_VERSION)).get<std::string>();
        pi.dependencies = parse_dep_strings(
            meta.value(std::string(constants::J_DEPS), std::vector<std::string>{}));
        pi.provides =
            meta.value(std::string(constants::J_PROVIDES), std::vector<std::string>{});
        pi.needed_so =
            meta.value(std::string(constants::J_NEEDED_SO), std::vector<std::string>{});
        local_pkgs.push_back(std::move(pi));
        local_paths[name] = path;
    }

    // 3. 选项
    solv::SolveOptions opts;
    opts.force_reinstall = ctx.force_reinstall;
    opts.missing_so_no_error = Config::instance().missing_so_no_error_mode();
    opts.use_system_soname = Config::instance().use_system_soname_mode();
    opts.no_deps = Config::instance().no_deps_mode();
    if (opts.use_system_soname) opts.system_sonames = collect_system_sonames();

    // 4. 求解
    auto result = solv::solve_install(ctx.repo, local_pkgs, installed, ctx.targets, opts);

    // 5. 报错
    if (!result.ok()) {
        std::string msg;
        for (const auto& p : result.problems) msg += p + "\n";
        throw LpkgException(msg);
    }

    // 6. 映射 plan + order
    for (const auto& rp : result.order) {
        auto lp = local_paths.find(rp.name);
        PackageInfo info;
        if (lp != local_paths.end()) {
            for (const auto& pi : local_pkgs)
                if (pi.name == rp.name) { info = pi; break; }
        } else if (auto repo_info = ctx.repo.find_package(rp.name, rp.version)) {
            info = *repo_info;
        }

        InstallPlan p;
        p.name = rp.name;
        p.actual_version = rp.version;
        p.sha256 = info.sha256;
        p.is_explicit = is_explicit_target(ctx.targets, rp.name);
        if (lp != local_paths.end()) p.local_path = lp->second;
        p.dependencies = info.dependencies;
        p.provides = info.provides;
        p.needed_so = info.needed_so;
        p.force_reinstall = (ctx.force_reinstall && p.is_explicit);
        ctx.plan[rp.name] = std::move(p);
        ctx.install_order.push_back(rp.name);
    }
}

std::unordered_set<std::string> get_all_required_packages()
{
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
        auto check_and_add = [&](const std::string& name) {
            if (cache.is_installed(name) && !req.contains(name)) {
                req.insert(name);
                q.push_back(name);
            }
        };

        // 命名依赖：deps/ 文件
        const fs::path p = Config::instance().dep_dir() / curr;
        if (fs::exists(p)) {
            std::ifstream f(p);
            std::string line;
            while (std::getline(f, line)) {
                std::string d_name = line;
                if (const auto pos = line.find_first_of(" \t<>="); pos != std::string::npos)
                    d_name = line.substr(0, pos);
                if (cache.is_installed(d_name))
                    check_and_add(d_name);
                else
                    for (const auto& prov : cache.get_providers(d_name)) check_and_add(prov);
            }
        }

        // SONAME 依赖：needed_so/ 文件 → 提供者包同样是"被依赖"的。
        // 缺这段时纯 SONAME 链路拉入的包（如 gcc←libmpc.so.3→mpc）会被 autoremove
        // 误判为孤儿而删除。
        const fs::path nso_f = Config::instance().needed_so_dir() / curr;
        if (fs::exists(nso_f)) {
            std::ifstream nf(nso_f);
            std::string so;
            while (std::getline(nf, so)) {
                if (so.empty()) continue;
                for (const auto& prov : cache.get_providers(so)) check_and_add(prov);
            }
        }
    }
    return req;
}

/**
 * 向前 needed_so 完整性校验。
 *
 * 对计划中的每个包，检查其每个 SONAME 的提供链：
 *   plan（版本精准）→ 已安装缓存 → repo（版本精准）
 *
 * 版本精准的含义：
 *   - plan 中同时升级的包以新版本计算 provides
 *   - 缓存中的包以当前安装版本计算
 *   - repo 中只取实际提供该 SONAME 的版本（find_provider 返回的版本必须提供该
 * SONAME）
 */
}  // namespace detail
