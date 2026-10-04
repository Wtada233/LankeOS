#include "config.hpp"

#include <fnmatch.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>

#include "base/constants.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "utils.hpp"

namespace fs = std::filesystem;

namespace
{
/**
 * pacman `_alpm_fnmatch_patterns` 的语义（lib/libalpm/util.c）：
 *   · **倒序**遍历模式列表 —— 后写的先判，先命中的赢（"后续匹配覆盖先前的"）；
 *   · `!` 前缀 = 取反（命中即"明确**不**豁免"）；
 *   · 前导 `\` 是转义（剥掉后交给 fnmatch —— pacman 也是剥掉再 fnmatch，故 `\!x`
 *     匹配字面量 `!x`）。
 *
 * 返回 1 = 命中普通模式（豁免）、-1 = 命中取反模式（明确不豁免）、0 = 没有模式命中。
 *
 * 模式侧与路径侧都归一到"**唯一的**一种形态"（相对 root、无前导斜杠）：模式写不写前导
 * 斜杠等价，`!` 因此永远只有一个匹配目标、必然生效。pacman 的
 * `_alpm_can_overwrite_file` 是"两种形态各判一次再取或"，那会让带前导斜杠的 `!` 模式被
 * 另一形态的普通命中盖掉（取反实际失效）—— 这里收紧为单形态。
 */
int match_overwrite_pattern(const std::vector<std::string>& patterns, const std::string& path)
{
    for (auto it = patterns.rbegin(); it != patterns.rend(); ++it) {
        std::string_view pat = *it;
        const bool inverted = !pat.empty() && pat.front() == '!';
        if (inverted || (!pat.empty() && pat.front() == '\\')) pat.remove_prefix(1);
        const auto first = pat.find_first_not_of('/');
        pat = (first == std::string_view::npos) ? std::string_view{} : pat.substr(first);
        // **尾斜杠也要剥**（2026-10-03 修）：路径侧在 `overwrite_allows` 里已经统一成"裸形态"
        // （归档目录条目带尾斜杠，不该影响判定），模式侧此前只剥前导斜杠 —— 于是
        // `--overwrite=usr/lib/foo/` 这种写法会**静默不匹配**（fnmatch 拿 `usr/lib/foo/` 去
        // 比 `usr/lib/foo` 永远失败），而上面那段注释声明的是"两侧做同一件事"。
        //
        // 但**剥完只剩通配符的就不剥**（`*/` → `*`、`??/` → `??`）：`*` 在 fnmatch 里本就跨
        // `/` 匹配，剥了等于把"用户以为只针对目录"的写法**静默放大成"豁免一切冲突"** ——
        // 那是危险的方向（豁免会跳过文件冲突检查）。保持不匹配 = 照常报冲突（fail-closed），
        // 用户看得见冲突就有机会把模式写清楚。
        //
        // ⚠️ **剥尾必须落成真实串**：`fnmatch(3)` 只吃 C 串（没有长度参数），而 view 的"余下
        // 部分一直读到 NUL"这个技巧**只对剥前缀成立** —— 剪掉尾巴时 NUL 还在被剪掉的那段之后，
        // fnmatch 照样看得见它（第一版就是这么写的：`usr/lib/foo/` 依旧不匹配，是**假修**）。
        std::string pat_owned;
        if (!pat.empty() && pat.back() == '/') {
            std::string_view stripped = pat;
            while (!stripped.empty() && stripped.back() == '/') stripped.remove_suffix(1);
            const bool only_wildcards =
                !stripped.empty() && stripped.find_first_not_of("*/?") == std::string_view::npos;
            if (!only_wildcards) {
                pat_owned.assign(stripped);
                pat = pat_owned;
            }
        }
        // fnmatch(3) 默认 flags=0 的 shell 语义：`*` 跨 `/` 匹配、`\` 转义、`?`/`[]` 同
        // shell —— 与 pacman 完全一致（它也是 fnmatch(pattern, string, 0)）。
        // pat.data() 可直接当 C 串用：std::string 的缓冲以 NUL 结尾，剥前缀后依然如此。
        if (::fnmatch(pat.data(), path.c_str(), 0) == 0) return inverted ? -1 : 1;
    }
    return 0;
}
}  // namespace

/**
 * 获取 Config 单例实例
 */
Config& Config::instance()
{
    static Config cfg;
    return cfg;
}

/**
 * 构造函数：只初始化基础路径成员，派生路径通过 rebase_paths() 统一计算
 *
 * 设计说明：
 *   hooks_dir_ / dep_dir_ / pkgs_file_ 等"派生路径"不应在本阶段硬编码赋值，
 *   而是全部交由 rebase_paths() 根据基础路径统一推导，避免两处逻辑不一致。
 *   当需要新增派生路径时，只需在 rebase_paths() 中添加，无需修改构造函数。
 */
Config::Config()
    : root_dir_("/"),
      config_dir_(fs::path{LPKG_CONF_DIR}),
      state_dir_("/var/lib/lpkg"),
      l10n_dir_(fs::path{LPKG_L10N_DIR}),
      docs_dir_(fs::path{LPKG_DOCS_DIR}),
      lock_dir_(fs::path{LPKG_LOCK_DIR})
{
    rebase_paths();  // 统一计算所有派生路径（hooks、dep、pkgs 等）
}

/**
 * 重新计算所有路径，将相对路径改为相对于 root_dir_ 的绝对路径
 * 在设置了新的根目录后调用此方法
 */
void Config::rebase_paths()
{
    auto rebase = [&](const std::string& default_path) -> fs::path {
        fs::path p(default_path);
        if (p.is_absolute()) {
            return root_dir_ / p.relative_path();
        }
        return root_dir_ / p;
    };

    config_dir_ = rebase(LPKG_CONF_DIR);
    state_dir_ = rebase("/var/lib/lpkg");
    l10n_dir_ = rebase(LPKG_L10N_DIR);
    docs_dir_ = rebase(LPKG_DOCS_DIR);
    lock_dir_ = rebase(LPKG_LOCK_DIR);

    hooks_dir_ = config_dir_ / "hooks/";
    dep_dir_ = state_dir_ / "deps/";
    needed_so_dir_ = state_dir_ / "needed_so/";
    pkgs_file_ = state_dir_ / "pkgs";
    holdpkgs_file_ = state_dir_ / "holdpkgs";
    essential_file_ = config_dir_ / "essential";
    mirror_conf_ = config_dir_ / "mirror.conf";
    triggers_conf_ = config_dir_ / "triggers.conf";
    build_conf_ = config_dir_ / "build.conf";
    files_db_ = state_dir_ / "files.db";
    provides_db_ = state_dir_ / "provides.db";
    provides_soname_db_ = state_dir_ / "provides_soname.db";
    conf_hashes_db_ = state_dir_ / "confhashes.db";
    xattr_keys_db_ = state_dir_ / "xattrkeys.db";
    lock_file_ = lock_dir_ / "db.lck";
}

/**
 * 设置软件包安装根目录，并重新计算所有派生路径
 */
void Config::set_root_path(const std::string& root_path)
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    root_dir_ = fs::path(root_path).lexically_normal();
    if (root_dir_.empty()) root_dir_ = "/";
    rebase_paths();
}

namespace
{
/**
 * 确保暂存根**此刻可用**：真目录（lstat 语义）+ 属主是本进程 + mode `0700`。
 *
 * 为什么每次用之前都要复核（2026-10-03，全量测试里实测出来的）：`get_tmp_dir()` 的 `mkdtemp`
 * 只管**第一次**创建。而 `~TmpDirManager()` 会 `remove_all` 掉这个根（同一进程内的生命周期
 * 管理），之后任何一句 `create_directories(<根>/<pkg>/…)` 都会把根**重新建出来 —— 用
 * `0777 & ~umask`（实测 0755）**，于是 §1.7 的加固在进程活着的中途就悄悄没了；更要紧的是，
 * 那一格又回到了"静默接受一个已存在的同名路径"（= 最初那条可劫持缺陷的形态）。
 *
 * 所以：不存在 → `mkdir(2)` 原子建（`EEXIST` 说明有人抢先，走下面的复核，**绝不"接管"**别人
 * 的目录）；mode 被放宽过 → `chmod` 收回来；是符号链接 / 别人的目录 → 抛错。
 */
void ensure_tmp_dir_usable(const fs::path& dir)
{
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0) {
        if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            throw LpkgException(string_format("error.create_dir_failed", dir.string()) + ": " +
                                std::strerror(errno));
        }
        if (::lstat(dir.c_str(), &st) != 0) {
            throw LpkgException(string_format("error.create_dir_failed", dir.string()) + ": " +
                                std::strerror(errno));
        }
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != ::geteuid())
        throw LpkgException(string_format("error.create_dir_failed", dir.string()));
    if ((st.st_mode & 07777) != 0700) (void)::chmod(dir.c_str(), 0700);
}
}  // namespace

/**
 * 获取当前进程的临时目录：`/tmp/lpkg_<PID>_<随机后缀>`（**原子创建、mode 0700**）
 */
fs::path Config::get_tmp_dir()
{
    static const fs::path tmp_dir = []() {
        // 名字形状与 `cleanup_tmp_dirs` 的解析约定绑定：那里在**首个 `_`** 处切开取 PID 段
        // （用 parse_pid_strict，**不是** stoi —— stoi 在首个非数字处停止，`83_abc` 会被读成
        // 83）。`mkdtemp` 只把结尾的 `XXXXXX` 换成随机串，`lpkg_<pid>_` 前缀原样保留 ⇒ 清理
        // 逻辑一行都不用改。
        //
        // **必须是原子创建**（2026-10-03 修）：此前是
        // `exists()` 探测 + `fs::create_directories()`，那是 TOCTOU —— 本地无权用户可以在
        // 探测与创建之间预建同名路径（一个他拥有的目录，或一个指向 `/etc` 的符号链接），
        // root 随后把包内容/索引解压进去、再拷进系统 ⇒ 任意内容以 root 安装。
        // 而且 `create_directories` 对"已存在"与"symlink→目录"**都静默成功**，等于白送。
        // `mkdtemp` 底层是 `mkdir(2)`：名字被占就自己换一个（不跟随末段符号链接），并且
        // 建出来必是 `0700` + 属主为本进程（umask 只能清位，不会放宽 0700）。
        //
        // 注意"建完再清空目录内容"**不是**这个缺陷的修法：攻击者若真持有那个目录 inode，
        // 清空之后他照样能再放东西（能 unlink/替换靠的是目录的 w 权限，不是内容）。
        std::string tmpl = "/tmp/lpkg_" + std::to_string(getpid()) + "_XXXXXX";
        if (::mkdtemp(tmpl.data()) == nullptr) {
            throw LpkgException(string_format("error.create_dir_failed", tmpl) + ": " +
                                std::strerror(errno));
        }
        return fs::path(tmpl);
    }();
    // **每次取用都复核**（不只是创建那一次）：根被 `~TmpDirManager()` 删掉之后，下一次
    // `create_directories(<根>/…)` 会用 0755 把它重建、且不再有任何人检查它是不是我们的
    // （见上面 helper 的说明）。这里是"用它之前"的唯一漏斗。
    ensure_tmp_dir_usable(tmp_dir);
    return tmp_dir;
}

void Config::set_non_interactive_mode(NonInteractiveMode m) noexcept
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    non_interactive_mode_ = m;
}

void Config::set_overwrite_patterns(std::vector<std::string> patterns)
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    overwrite_patterns_ = std::move(patterns);
}

std::vector<std::string> Config::compose_overwrite_patterns(
    bool force_overwrite, const std::vector<std::string>& explicit_patterns)
{
    std::vector<std::string> out;
    // `--force-overwrite` ≡ 在**最前面**追加 `*`（最宽松的兜底）：倒序遍历判定时它排在
    // 最后，于是后给的 `--overwrite` 模式（含 `!` 取反）都能覆盖它 —— 更具体的赢。
    if (force_overwrite) out.emplace_back("*");
    out.insert(out.end(), explicit_patterns.begin(), explicit_patterns.end());
    return out;
}

bool Config::overwrite_allows(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    if (overwrite_patterns_.empty()) return false;

    // 归一到唯一形态：剥掉前导斜杠（相对 root 的形态）与尾斜杠（归档目录条目是否带尾斜杠
    // 不该影响判定）。模式侧由 match_overwrite_pattern 做同一件事。
    const auto first = path.find_first_not_of('/');
    if (first == std::string::npos) return false;
    std::string bare = path.substr(first);
    while (!bare.empty() && bare.back() == '/') bare.pop_back();
    if (bare.empty()) return false;

    return match_overwrite_pattern(overwrite_patterns_, bare) == 1;
}

void Config::set_force_overwrite_mode(bool v) noexcept
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    overwrite_patterns_.clear();
    if (v) overwrite_patterns_.emplace_back("*");
}

void Config::set_no_hooks_mode(bool v) noexcept
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    no_hooks_mode_ = v;
}

void Config::set_no_deps_mode(bool v) noexcept
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    no_deps_mode_ = v;
}

void Config::set_missing_so_no_error_mode(bool v) noexcept
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    missing_so_no_error_mode_ = v;
}

void Config::set_use_system_soname_mode(bool v) noexcept
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    use_system_soname_mode_ = v;
}

bool Config::has_system_soname(const std::string& soname) const noexcept
{
    // 系统 /usr/lib（或 /usr/lib64）已有该 .so 文件（如 ABI 过渡备份的旧 SONAME）→ 视为已满足。
    for (const std::string_view sub : {constants::USR_LIB, constants::USR_LIB64}) {
        // SONAME 来自不可信元数据/ELF：绝对路径或 `..` 会让 `root/usr/lib / soname`
        // 逃出该目录（甚至到宿主），从而把宿主上的同名文件当成"系统已有该 so"
        const fs::path lib_dir = (root_dir_ / fs::path(sub)).lexically_normal();
        const fs::path cand = (lib_dir / soname).lexically_normal();
        // 不抛判定：`cand` 是**目标 root 的 /usr/lib 下**的一个候选路径（= 包内容），
        // 盘上那个名字可能是符号链接环 → `fs::exists` 会抛（见 base/path_predicates.hpp
        // 的谓词族）。 语义不变：仍是"跟随"语义的"这个路径通不通"。
        if (path_within(cand, lib_dir) && exists_follow(cand)) {
            return true;
        }
    }
    return false;
}

void Config::set_testing_mode(bool v) noexcept
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    testing_mode_ = v;
}

/**
 * **仅供测试**：复位到"测试用的中性值"。
 *
 * 为什么需要它：这些字段挂在**进程级单例**上，一个用例改了、它后面**每一个**用例都看得见，
 * 只有当"负责复位的那个用例"恰好也在本次过滤范围内时才会被清掉 —— 症状就是**单跑绿、全量红**。
 * 本仓库实测撞过三次，前两次记在 `tests/test_hygiene.hpp` 顶部；第三次（2026-10-03）是
 * `no_hooks_mode`：上一个用例在自己的 TearDown 里置 true，于是下一个用例里"运行安装后钩子"
 * 那一节整段静默 —— 更糟的是它那条"没有钩子就不该出现该阶段"的断言变成了**空转的绿**。
 *
 * 两处**有意不取构造默认值**（构造默认 = `testing_mode_ false` / `non_interactive_ INTERACTIVE`）：
 *   · `testing_mode_` → **true**：整个测试二进制都是测试，断点与测试分支要生效；
 *   · `non_interactive_mode_` → **YES**：用例绝不能卡在 stdin 上（INTERACTIVE 会让"依赖上一个
 *     用例留下 YES"的用例**永久挂起**，那比一条红断言难查得多）。
 */
void Config::reset_for_test()
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    root_dir_ = "/";
    rebase_paths();
    architecture_override_.clear();
    overwrite_patterns_.clear();
    no_hooks_mode_ = false;
    no_deps_mode_ = false;
    missing_so_no_error_mode_ = false;
    use_system_soname_mode_ = false;
    testing_mode_ = true;
    non_interactive_mode_ = NonInteractiveMode::YES;
}

/**
 * 初始化配置所需的文件系统结构
 * 创建所有必要的目录和空文件（如包数据库、锁定文件等）
 */
void Config::init_filesystem()
{
    ensure_dir_exists(config_dir_);
    ensure_dir_exists(state_dir_);
    ensure_dir_exists(dep_dir_);
    ensure_dir_exists(needed_so_dir_);
    ensure_dir_exists(l10n_dir_);
    ensure_dir_exists(docs_dir_);
    ensure_dir_exists(hooks_dir_);
    ensure_dir_exists(lock_dir_);
    ensure_file_exists(pkgs_file_);
    ensure_file_exists(holdpkgs_file_);
    ensure_file_exists(essential_file_);
    ensure_file_exists(files_db_);
    ensure_file_exists(provides_db_);
    ensure_file_exists(provides_soname_db_);
    // 与兄弟库同一口径：DB
    // 一族（pkgs/holdpkgs/files.db/provides.db/provides_soname.db/confhashes.db/xattrkeys.db）
    // 要么都预建、要么都不建。两个具体理由：
    //   · 备份链：`Cache::write(milestone)` 对一族逐个"备份原文件 + 全量重写"，而
    //     write_db_file_wal 对**不存在**的文件走 DBNEW、**不产生** :batch-start 备份 ——
    //     不预建就等于这一族的"每里程碑一份备份"对这一个库不成立；
    //   · 崩溃恢复判据：`batch_start_db_still_in_place` 把"存在但 0 字节"读作"内容丢了"
    //     （备份非空 → 从 :batch-start 备份还原），而"文件缺失"是另一支 —— 预建后它与兄弟库
    //     走同一条还原路径，"恢复行为取决于这是第几个加进来的库"这件事就不存在了。
    //     （main 的启动顺序是 init_filesystem() → recover_packages()，所以这才是真实形态。）
    ensure_file_exists(conf_hashes_db_);
    ensure_file_exists(xattr_keys_db_);
}

/**
 * 覆盖系统的架构检测结果，强制使用指定架构
 */
void Config::set_architecture(const std::string& arch)
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    architecture_override_ = arch;
}

/**
 * 获取当前系统的 CPU 架构
 * 如果未设置架构覆盖，通过 uname 系统调用获取
 */
std::string Config::get_architecture()
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    if (!architecture_override_.empty()) {
        return architecture_override_;
    }

    struct utsname buf;
    if (uname(&buf) != 0) {
        throw LpkgException(get_string("error.get_arch_failed"));
    }
    return std::string(buf.machine);
}

/**
 * 从镜像配置文件中读取镜像源 URL
 * 确保 URL 末尾包含斜杠
 */
std::string Config::get_mirror_url()
{
    std::ifstream mirror_file(mirror_conf_);
    if (!mirror_file.is_open()) {
        throw LpkgException(string_format("error.open_file_failed", mirror_conf_.string()));
    }
    std::string mirror_url;
    if (!std::getline(mirror_file, mirror_url) || mirror_url.empty()) {
        throw LpkgException(get_string("error.invalid_mirror_config"));
    }
    // 首行是**原样使用**的：注释、CRLF 的 \r、首尾空白都会成为 URL 的一部分 →
    // 索引下载必然失败，而失败又会落进"空索引"路径（静默说"已是最新"）。此处统一归一。
    if (const auto hash = mirror_url.find('#'); hash != std::string::npos) mirror_url.resize(hash);
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!mirror_url.empty() && is_space(mirror_url.back())) mirror_url.pop_back();
    while (!mirror_url.empty() && is_space(mirror_url.front()))
        mirror_url.erase(mirror_url.begin());
    if (mirror_url.empty()) throw LpkgException(get_string("error.invalid_mirror_config"));
    if (mirror_url.back() != '/') {
        mirror_url += '/';
    }
    return mirror_url;
}
