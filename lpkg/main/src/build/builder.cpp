#include "builder.hpp"

#include <sys/wait.h>

#include <array>
#include <atomic>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>

#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "builder_config.hpp"
#include "builder_executor.hpp"
#include "config/config.hpp"  // Config::non_interactive_mode()（本文件直接用到，别靠传递包含）
#include "i18n/localization.hpp"
#include "lib_utils.hpp"
#include "packer.hpp"
#include "pkg/package_manager.hpp"
#include "repo/repository.hpp"
#include "strip.hpp"  // strip_binary（原住 base/utils.hpp，2026-09-26 挪回本层）
#include "vercmp/dep_parser.hpp"

/** 在 main/src/main_cli.cpp 中定义，由 SIGINT 信号处理函数设置（SigIntGuard 生命周期内生效） */
extern std::atomic<bool> sigint_graceful;

/**
 * Ctrl+C 检查点（**唯一形态**）：`lpkg build`/`pack` 挂在 `SigIntGuard` 之下，本进程只置标志、
 * 不打断当前动作 —— 每个可能长时间停留的**进程内**步骤（阶段之间、strip 循环、打包前）都要
 * 在这里把它翻成 `UserAbort`，否则那个窗口里的 Ctrl+C 会被静默吞掉（用户以为没反应）。
 * 下载期间那一份在 `downloader.cpp` 的 curl 进度回调里（curl 是进程内的，没有子进程会替我们死）。
 */
void check_sigint_abort()
{
    if (sigint_graceful.load()) throw UserAbort(get_string("info.sigint_aborted"));
}

namespace fs = std::filesystem;

// =====================================================================
// 内部辅助函数
// =====================================================================

namespace
{

/**
 * 设置构建目录结构
 * 创建工作目录、staging 目录和 hooks 目录，初始化 UsrMerge 符号链接
 * 创建默认的 post-install 脚本占位符
 */
fs::path setup_build_directories(const fs::path& build_dir)
{
    fs::path work_root = build_dir / constants::DIR_WORK;
    fs::path staging_root = build_dir / constants::DIR_CONTENT;
    fs::path staging_hooks = build_dir / constants::DIR_HOOKS;

    fs::remove_all(work_root);
    fs::remove_all(staging_root);
    fs::remove_all(staging_hooks);

    ensure_dir_exists(work_root);
    ensure_dir_exists(staging_root);

    // 路径规范化（UsrMerge 合并）
    // 注意：不预创建 usr/local/* — /usr/local 已被排除出 PATH 和 ld 搜索路径，
    //       发行版打包中不应出现该路径。
    log_info(get_string("info.path_normalization"));
    for (const auto& d :
         {constants::BIN, constants::LIB, constants::INCLUDE, constants::SHARE_MAN}) {
        ensure_dir_exists(staging_root / constants::USR / d);
    }
    fs::create_directory_symlink(constants::USR_BIN, staging_root / constants::BIN);
    fs::create_directory_symlink(constants::USR_BIN, staging_root / constants::SBIN);
    fs::create_directory_symlink(constants::USR_LIB, staging_root / constants::LIB);
    fs::create_directory_symlink(constants::USR_LIB, staging_root / constants::LIB64);
    fs::create_directory_symlink(constants::BIN, staging_root / constants::USR_SBIN);
    fs::create_directory_symlink(constants::LIB, staging_root / constants::USR_LIB64);

    ensure_dir_exists(staging_hooks);
    {
        const fs::path postinst = staging_hooks / constants::POSTINST_SH;
        std::ofstream h(postinst);
        // 必须先查 open：不查的话下面那句 fs::permissions 会作用在一个根本没建成的（或空的）
        // 文件上，失败点也因此报不到真正的原因。点名路径以便定位。
        if (!h.is_open()) {
            throw LpkgException(string_format("error.create_file_failed", postinst.string()));
        }
        h << "#!/bin/bash" << constants::NL;
    }
    fs::permissions(staging_hooks / constants::POSTINST_SH,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add);

    return staging_hooks;
}

/** 构建模板变量映射表，将占位符替换为实际的路径和配置值 */
std::map<std::string, std::string> build_variable_map(
    const BuildConfig& cfg, const fs::path& work_root, const fs::path& actual_work_dir,
    const fs::path& staging_root, const fs::path& staging_hooks,
    const std::string& effective_version, const build_defaults::BuildFlags& flags)
{
    return {
        {"{PKG_NAME}", cfg.name},
        {"{PKG_VER}", effective_version},
        {"{WORK_DIR}", fs::absolute(work_root).string()},
        {"{SRC_DIR}", fs::absolute(actual_work_dir).string()},
        {"{STAGING_ROOT}", fs::absolute(staging_root).string()},
        {"{STAGING_HOOKS}", fs::absolute(staging_hooks).string()},
        {"{PREFIX}", "/usr"},
        {"{BINDIR}", "/usr/bin"},
        {"{SBINDIR}", "/usr/bin"},
        {"{LIBDIR}", "/usr/lib"},
        {"{INCLUDEDIR}", "/usr/include"},
        {"{MANDIR}", "/usr/share/man"},
        {"{LOCALSTATEDIR}", "/var"},
        {"{DATADIR}", "/usr/share"},
        {"{SYSCONFDIR}", "/etc"},
        {"{ORIG_PKG_VER}", cfg.version},
        {"{NO_STRIP}", cfg.no_strip ? "1" : "0"},
        // 编译/链接标志（与 execute_build_phase 注入的环境变量一致，
        // 供 LankeBUILD 脚本显式引用，如 export CFLAGS="{CFLAGS}"）
        {"{CFLAGS}", flags.cflags},
        {"{CXXFLAGS}", flags.cxxflags},
        {"{LDFLAGS}", flags.ldflags},
        {"{MAKEFLAGS}", flags.makeflags},
    };
}

/**
 * 构建后处理：清理 libtool 文件、对 ELF 二进制文件进行 strip、
 * 生成 SONAME 符号链接
 */
void finalize_staging(const fs::path& staging_root, bool no_strip)
{
    log_info(get_string("info.finalizing_staging"));

    log_info(get_string("info.cleaning_libtool_files"));
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(staging_root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        // 逐个文件检查 Ctrl+C：这一趟要对**整棵 staging** 做 strip（libelf，进程内、
        // 大二进制上可达秒级/文件），是 build 里最容易"按了没反应"的窗口之一。
        check_sigint_abort();
        // 判定走**不抛**形态（见 base/path_predicates.hpp
        // 的谓词族）：`directory_entry::is_regular_file()` 走
        // `status()`（**跟随**末段链接），而这里扫的是**构建 staging = 包内容** —— 源码树里
        // 有一个符号链接环（上游自指链接很常见）就会抛 ELOOP，把整个构建打死。
        std::error_code fec;
        if (it->path().extension() == constants::EXT_LA && fs::is_regular_file(it->path(), fec) &&
            !fec) {
            // 独立的 ec：循环把同一个 `ec` 交给了 `it.increment(ec)`，而 increment 进入时会
            // 先清空它 —— 若用循环那个 ec 承接 remove 的失败，下一轮 increment 会把错误码
            // 抹掉，删除失败就被静默。用独立 ec 承接并**在原地**判定告警。
            std::error_code rec;
            fs::remove(it->path(), rec);
            if (rec) log_warning(string_format("warning.cleanup_failed", it->path().string()));
        }
    }

    if (!no_strip) {
        log_info(get_string("info.stripping_binaries"));
        fs::path usr_root = staging_root / constants::USR;
        // 同上：`is_directory_follow` 是不抛的跟随语义判定（`exists && is_directory` 的等价形）
        if (is_directory_follow(usr_root)) {
            for (const auto& entry : fs::recursive_directory_iterator(usr_root)) {
                // `is_symlink()` 放前面**短路**，`is_regular_file` 用不抛形态：staging 里的环
                // 同样不该把构建打死。语义不变 —— 符号链接一律跳过，只 strip 普通文件。
                std::error_code fec;
                if (entry.is_symlink() || !fs::is_regular_file(entry.path(), fec) || fec) continue;

                // 对 ELF（可执行/共享库）和 ar 归档（静态库 .a）进行 strip
                bool is_strippable = false;
                std::ifstream f(entry.path(), std::ios::binary);
                if (f) {
                    std::array<char, 4> magic{};
                    f.read(magic.data(), magic.size());
                    if (f.gcount() == 4) {
                        // ELF: \x7fELF
                        if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' &&
                            magic[3] == 'F') {
                            is_strippable = true;
                        }
                        // ar 归档: !<arch>\n
                        if (magic[0] == '!' && magic[1] == '<') {
                            is_strippable = true;
                        }
                    }
                }
                if (!is_strippable) continue;

                // 二次防护（strip_binary 内部已 try/catch）：任何意外都只告警、继续处理其余文件
                try {
                    strip_binary(entry.path());
                } catch (const std::exception& e) {
                    log_warning(
                        string_format("warning.strip_failed", entry.path().string(), e.what()));
                }
            }
        }
    }

    fs::remove(staging_root / "usr/share/info/dir", ec);

    // 不抛判定（staging 是包内容；环不该打断构建）
    const fs::path soname_lib_dir = staging_root / constants::USR / constants::LIB;
    if (exists_follow(soname_lib_dir)) {
        log_info(get_string("info.generating_soname_links"));
        // **staging 内的 `usr/lib` 可能是一条指向 staging 之外的符号链接**（配方/上游产物
        // 里带绝对目标链接）——`apply_soname_links` 用**跟随**语义处理这个目录，不挡就会
        // 往宿主目录里建/删链接。与触发器侧同一判据（见 `path_resolves_within`）；这里
        // 与下面 catch 的处置一致：**只告警不抛**（这一趟本来就把 soname 失败当可容忍）。
        if (!path_resolves_within(soname_lib_dir, staging_root)) {
            log_warning(string_format("warning.soname_dir_outside_root", soname_lib_dir.string()));
            return;
        }
        try {
            apply_soname_links(soname_lib_dir);
        } catch (const std::exception& e) {
            log_warning(
                string_format("warning.soname_links_failed", soname_lib_dir.string(), e.what()));
        }
    }
}

/**
 * 清理构建过程中产生的临时文件和工作目录。
 *
 * 全部走**不抛**重载 + 独立 `ec`：本函数现在也在 run_build 的**异常路径**上被调用 ——
 * 若这里抛异常，就会在栈展开中顶替（甚至 terminate 掉）正在传播的构建异常，真实失败
 * 原因（编译错误等）被吞。清理失败只告警（沿用 warning.cleanup_failed 的既有语义）。
 */
void cleanup_build([[maybe_unused]] const fs::path& build_dir, const fs::path& work_root,
                   const fs::path& staging_root, const fs::path& staging_hooks,
                   const std::vector<fs::path>& downloaded_files)
{
    log_info(get_string("info.cleaning_up_build"));
    for (const auto& dir : {work_root, staging_root, staging_hooks}) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        if (ec) log_warning(string_format("warning.cleanup_failed", dir.string()));
    }
    for (const auto& f : downloaded_files) {
        std::error_code ec;
        fs::remove(f, ec);
        if (ec) log_warning(string_format("warning.cleanup_failed", f.string()));
    }
}

// ── run_build 的零件（2026-09-26 从 172 行的函数里按构建阶段抽出）───────────────
// 全都是**纯搬移**：语句、顺序、日志、异常与清理行为与抽之前逐行一致。

/** 解析并校验过的构建元数据（run_build 后续各阶段只读）。 */
struct BuildMetadata {
    fs::path json_path;
    fs::path script_path;
    BuildConfig cfg;
    std::string effective_version;
    build_defaults::BuildFlags flags;
};

/** 阶段 1：校验 LankeBUILD/LankeBUILD.json 存在、解析元数据、算有效版本号与构建标志。 */
BuildMetadata resolve_build_metadata(const fs::path& build_dir)
{
    fs::path json_path = build_dir / constants::LANK_BUILD_JSON;
    fs::path script_path = build_dir / constants::LANK_BUILD_SCRIPT;

    if (!fs::exists(json_path))
        throw LpkgException(string_format("error.missing_lankebuild_json", build_dir.string()));
    if (!fs::exists(script_path))
        throw LpkgException(string_format("error.missing_lankebuild", build_dir.string()));

    log_info(get_string("info.parsing_lankebuild"));
    auto cfg = parse_build_config(json_path);

    // 计算有效版本号：如有 release 修订号，附加到版本号后
    std::string effective_version = cfg.version;
    if (cfg.release > 0) {
        effective_version =
            cfg.version + "-" + std::to_string(cfg.release);  // rpm 语义：release 用 `-`
    }
    log_info(string_format("info.building_package", cfg.name, effective_version));

    // 构建标志：默认 = build_defaults（Arch x86-64 generic），可被 LankeBUILD.json 覆盖
    auto flags = resolve_build_flags(cfg);
    log_info(string_format("info.build_flags", flags.cflags));

    // 如果 LankeBUILD.json 中未指定 man 页面内容，则自动生成
    if (cfg.man_content.empty()) {
        std::time_t t = std::time(nullptr);
        // localtime_r（可重入）代替 localtime：后者的返回指向内部静态缓冲，非线程安全；
        // 且两者失败都返回 nullptr，直接把它喂给 strftime 是 UB。失败时点名时间戳与包，
        // 让用户能定位（这种失败极罕见，但一旦发生不能是崩溃或静默的伪日期）。
        std::tm tm_buf{};
        if (localtime_r(&t, &tm_buf) == nullptr) {
            const std::string detail =
                "localtime_r failed while formatting the auto-generated man build date "
                "(time_t=" +
                std::to_string(static_cast<long long>(t)) + ", package '" + cfg.name + "')";
            throw LpkgException(string_format("error.unexpected_error", detail));
        }
        // 零初始化：strftime 失败（返回 0，或结果放不下缓冲）时缓冲区内容**未定义**，
        // 绝不能把未初始化的栈内容写进 man 页。失败即按上面的 localtime_r 分支同样处理 ——
        // 这是一种"不可能发生"的时间格式化失败，点名时间戳与包，别让坏数据静默流进产物。
        char date_buf[32] = {};
        if (std::strftime(date_buf, sizeof(date_buf), "%Y%m%d", &tm_buf) == 0) {
            const std::string detail =
                "strftime failed while formatting the auto-generated man build date (time_t=" +
                std::to_string(static_cast<long long>(t)) + ", package '" + cfg.name + "')";
            throw LpkgException(string_format("error.unexpected_error", detail));
        }
        cfg.man_content = get_string("man.info_name") + ": " + cfg.name + "\n" +
                          get_string("man.info_version") + ": " + effective_version + "\n" +
                          get_string("man.info_build_date") + ": " + date_buf + "\n";
        log_info(string_format("info.auto_generated_man", cfg.name));
    }

    return {json_path, script_path, cfg, effective_version, flags};
}

/** 阶段 4.5：安装构建时依赖（支持版本约束 "cmake >= 3.20"）。 */
void install_build_deps(const BuildConfig& cfg)
{
    if (cfg.build_deps.empty()) {
        return;
    }
    log_info(string_format("info.checking_deps", cfg.name));
    // 解析版本约束为 name:version 格式
    std::vector<std::string> resolved;
    auto parsed = detail::parse_dep_strings(cfg.build_deps);
    Repository repo;
    repo.load_index();
    for (const auto& dep : parsed) {
        std::string arg = dep.name;
        if (!dep.constraints.empty()) {
            if (auto match = repo.find_best_matching_version(dep.name, dep.constraints)) {
                arg += ":" + match->version;
            } else {
                // ⚠️ 此前这里回落到 `dep.constraints[0].version`（把**约束里的字面版本号**
                // 当成一个"精确版本"去要），却**不检查它是否满足该约束**：仓库里只有
                // cmake 4.5 / 4.0 而配方写 `cmake < 4.0` 时，会去装 **4.0** —— 而 4.0
                // 并不满足 `< 4.0`，约束形同虚设。仓库里没有任何版本满足时，正确做法是
                // **报出来**（点名依赖与它声明的约束），而不是随手挑一个"看起来最接近"的。
                std::string want;
                for (const auto& c : dep.constraints) {
                    if (!want.empty()) want += " ";
                    want += c.op + " " + c.version;
                }
                throw LpkgException(string_format("error.build_dep_unsatisfiable", dep.name, want));
            }
        }
        resolved.push_back(arg);
    }
    // 在 -n 模式下，如果有 build_deps 则拒绝构建
    if (Config::instance().non_interactive_mode() == NonInteractiveMode::NO) {
        throw LpkgException(get_string("error.build_deps_refused_n"));
    }
    install_packages(resolved, "", false);
}

/** 阶段 4.75：autohacks —— 执行构建前的环境修补脚本（`hacks.sh`，不存在则什么都不做）。 */
void run_autohacks(const fs::path& json_path)
{
    fs::path hacks_path = json_path.parent_path() / "hacks.sh";
    if (!fs::exists(hacks_path)) {
        return;
    }
    log_info(string_format("info.autohacks_found", hacks_path.string()));

    // 读取并显示脚本内容
    std::ifstream hacks_file(hacks_path);
    std::string hacks_content((std::istreambuf_iterator<char>(hacks_file)),
                              std::istreambuf_iterator<char>());
    std::cout << "─────────────────────────────────────────\n";
    std::cout << hacks_content << "\n";
    std::cout << "─────────────────────────────────────────\n";

    auto& cfg_ref = Config::instance();
    bool should_run = false;
    switch (cfg_ref.non_interactive_mode()) {
        case NonInteractiveMode::YES:
            should_run = true;
            break;
        case NonInteractiveMode::NO:
            should_run = false;
            log_info(get_string("info.autohacks_skipped"));
            break;
        case NonInteractiveMode::INTERACTIVE:
        default:
            should_run = user_confirms(get_string("prompt.autohacks_run"));
            break;
    }

    if (should_run) {
        log_info(get_string("info.autohacks_running"));
        std::string cmd = "bash \"" + hacks_path.string() + "\"";
        // std::system 返回的是 **wait status**（不是退出码）：直接拿它报错会把退出码 1 显示成
        // 256、127 显示成 32512。WIFEXITED 为真时取 WEXITSTATUS；被信号杀死或调用失败（-1）
        // 时原样保留 status（据此可区分"正常退出但非 0"与"异常终止"）。
        const int status = std::system(cmd.c_str());
        const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : status;
        if (rc != 0) {
            throw LpkgException(string_format("error.autohacks_failed", rc));
        }
        log_info(get_string("info.autohacks_done"));
    }
}

/** 阶段 5：替换占位符写出构建脚本，依次执行 prepare/build/package 三个阶段。 */
void run_build_phases(const fs::path& build_dir, const BuildMetadata& meta, const BuildConfig& cfg,
                      const fs::path& work_root, const fs::path& actual_work_dir,
                      const fs::path& staging_root, const fs::path& staging_hooks)
{
    auto vars = build_variable_map(cfg, work_root, actual_work_dir, staging_root, staging_hooks,
                                   meta.effective_version, meta.flags);
    fs::path processed_script = build_dir / constants::LANK_BUILD_PROCESSED;
    // 用原子写助手（写 .tmp → 检查 → fsync → rename）：裸 ofstream 在磁盘满时会
    // 静默产出**截断的构建脚本**，随后被 source 执行 —— 失败方式极难定位。
    write_string_to_file(processed_script, process_build_script(meta.script_path, vars));

    try {
        // 阶段之间检查 Ctrl+C：`lpkg build` / `pack` 在 `SigIntGuard` 之下（见 main_cli.cpp
        // 的 `needs_sigint_guard`），**正在跑的子进程**（make/cc）属于同一前台进程组，
        // Ctrl+C 会直接送到它手里；本进程只置标志、不打断当前阶段，所以在每个可能长时间
        // 停留的地方把它翻成 `UserAbort`（收尾走带 cleanup 的异常路径；下载期间那一份在
        // `downloader.cpp` 的进度回调里 —— curl 是进程内的，没有子进程会替我们死）。
        // 只检查阶段边界会让 finalize_staging / pack 这两个**进程内**的长步骤把 Ctrl+C 吃掉。
        for (const char* phase : {"lankebuild_prepare", "lankebuild_build", "lankebuild_package"}) {
            check_sigint_abort();
            execute_build_phase(phase, actual_work_dir, processed_script, meta.flags);
        }
    } catch (...) {
        // 用不抛重载 + 独立 ec：这里的删除若抛（EACCES/ROFS/路径变目录…），会**顶替**正在
        // 传播的构建异常，下面那行 `throw;` 就永远执行不到 —— 真实的失败原因（例如编译器
        // 报错）被一个新异常盖掉。删除失败只告警，原异常一定继续往外抛。
        std::error_code ec;
        fs::remove(processed_script, ec);
        if (ec) log_warning(string_format("warning.cleanup_failed", processed_script.string()));
        throw;
    }
    std::error_code ec;
    fs::remove(processed_script, ec);
    if (ec) log_warning(string_format("warning.cleanup_failed", processed_script.string()));
}

/**
 * 阶段 6.5：移除 USR-Merge 兼容符号链接（除非 keep_fs_layout=true）。
 * 这些链接是构建阶段的辅助设施，不应打包入包：
 *       Builder 在 setup_build_directories() 中创建这些链接使构建脚本能够
 *       向 bin/、lib/ 等路径安装文件（实际写入 usr/bin/、usr/lib/）。
 *       现在打包前清理它们，避免每个包都声称"拥有"这些系统级符号链接，
 *       从而导致卸载时因"文件被其他包共享"而拒绝移除。
 *       若 LankeBUILD.json 中 keep_fs_layout=true，则保留这些符号链接
 *       并将其一并打包（用于需要真正声明该文件布局的包）。
 */
void remove_usr_merge_symlinks(const fs::path& staging_root)
{
    for (const auto& link :
         {staging_root / constants::BIN, staging_root / constants::SBIN,
          staging_root / constants::LIB, staging_root / constants::LIB64,
          staging_root / constants::USR_SBIN, staging_root / constants::USR_LIB64}) {
        std::error_code ec;
        if (fs::is_symlink(link, ec) || (!ec && fs::exists(link))) {
            fs::remove(link, ec);
        }
    }
}

/**
 * 阶段 7 的产物文件名：name/version 来自配方 JSON（可被 LankeBUILD.json 直接控制），
 * 未校验就拼进相对路径会让 `"name": "../x"` 把 .lpkg 写到构建目录之外。
 */
std::string output_package_filename(const BuildConfig& cfg, const std::string& effective_version)
{
    if (!is_safe_path_component(cfg.name) || !is_safe_path_component(effective_version)) {
        throw LpkgException(string_format("error.unsafe_path_component", "package name",
                                          cfg.name + " / " + effective_version));
    }
    return cfg.name + "-" + effective_version + std::string(constants::EXT_LPKG);
}

}  // anonymous namespace

// =====================================================================
// 公开 API
// =====================================================================

/**
 * 执行完整的包构建流程：
 * 1. 解析 LankeBUILD.json 配置（校验存在性 + 有效版本号 + 构建标志 + man 内容）
 * 2. 准备构建目录结构和 UsrMerge 符号链接
 * 3. 下载并解压源码
 * 4. 安装构建时依赖 → autohacks → 检测源码树结构
 * 5. 处理构建脚本并执行各构建阶段（prepare/build/package）
 * 6. 后处理：strip、清理 libtool 文件、生成 SONAME 链接、移除 UsrMerge 辅助链接
 * 7. 打包为 .lpkg 文件
 * 8. 清理临时文件
 *
 * 每一步的实现在上方匿名命名空间里同名的小函数（`resolve_build_metadata` /
 * `install_build_deps` / `run_autohacks` / `run_build_phases` / `remove_usr_merge_symlinks` /
 * `output_package_filename`），本函数只负责把它们按顺序串起来。
 */
void run_build(const fs::path& build_dir)
{
    // 1. 解析元数据（校验 + 有效版本号 + 构建标志 + man 内容）
    const BuildMetadata meta = resolve_build_metadata(build_dir);
    const BuildConfig& cfg = meta.cfg;

    // 2. 准备目录
    fs::path work_root = build_dir / constants::DIR_WORK;
    fs::path staging_root = build_dir / constants::DIR_CONTENT;
    auto staging_hooks = setup_build_directories(build_dir);

    std::vector<fs::path> downloaded;
    try {
        // 3. 下载并解压源码
        downloaded =
            download_and_prepare_sources(cfg.sources, cfg.work_sources, build_dir, work_root);

        // 4. 安装构建时依赖（版本约束见 install_build_deps）
        install_build_deps(cfg);

        // 4.5. autohacks —— 执行构建前的环境修补脚本
        run_autohacks(meta.json_path);

        // 4.75. 检测源码树（放在最后：构建时依赖与 autohacks 都可能改动 work_root）
        auto actual_work_dir = detect_source_tree(work_root);

        // 5. 处理脚本并执行构建阶段（prepare/build/package）
        run_build_phases(build_dir, meta, cfg, work_root, actual_work_dir, staging_root,
                         staging_hooks);

        // 6. 后处理（strip、清理 libtool 文件、生成 SONAME 链接）
        check_sigint_abort();  // 这一步是**进程内**的（libelf 逐个文件 strip），可能很久
        finalize_staging(staging_root, cfg.no_strip);

        // 6.5. 打包前移除构建期造的 USR-Merge 辅助符号链接（keep_fs_layout=true 时保留）
        if (!cfg.keep_fs_layout) {
            remove_usr_merge_symlinks(staging_root);
        }

        // 7. 打包（同样是进程内长步骤：压缩整棵 staging）
        check_sigint_abort();
        log_info(get_string("info.packing_built_pkg"));
        const std::string output_filename = output_package_filename(cfg, meta.effective_version);
        pack_package(output_filename, build_dir.string(), cfg.name, meta.effective_version,
                     cfg.deps, cfg.provides, cfg.provides_soname, cfg.man_content, cfg.needed_so);
        log_info(string_format("info.build_success", output_filename));
    } catch (...) {
        // 异常路径也必须清理：否则 <dir>/build 下会残留 work/content/hooks 与已下载的源码
        // （可达数百 MB）。cleanup_build 内部不抛、失败只告警，所以既不会掩盖这里的异常，
        // 也不会在栈展开中 terminate。清理完再把原异常继续抛出去。
        cleanup_build(build_dir, work_root, staging_root, staging_hooks, downloaded);
        throw;
    }

    // 8. 清理（成功路径）
    cleanup_build(build_dir, work_root, staging_root, staging_hooks, downloaded);
}
