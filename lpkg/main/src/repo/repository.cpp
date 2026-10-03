#include "repository.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <ranges>
#include <set>
#include <sstream>
#include <string_view>

#include "archive/downloader.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "config/config.hpp"
#include "i18n/localization.hpp"
#include "vercmp/dep_parser.hpp"
#include "vercmp/version.hpp"

/**
 * 拆索引里的 deps 字段为依赖字符串列表。
 *
 * 索引用 ',' 连接各依赖，而**依赖语法本身也用 ',' 表达复合约束**
 * （`"cmake >= 3.20, < 4.0"` 是一个依赖，见 tests/integration/test_build_deps.cpp）。
 * 所以拆开后，以操作符开头的片段是上一条的续接（"< 4.0"），必须合回上一条——
 * 否则它会变成空名依赖，在 libsolv 里是 ID_EMPTY：不解析也不报错，求解静默产出
 * 空事务，用户看到"所有包都已安装"却没装任何东西（历史 TODO.md D2）。
 *
 * 判断依据是"片段是否以比较操作符开头"——依赖名不可能这样开头。
 */
static std::vector<std::string> split_dep_field(std::string_view deps_sv)
{
    const auto starts_with_operator = [](const std::string& s) {
        return !s.empty() && (s[0] == '<' || s[0] == '>' || s[0] == '=' || s[0] == '!');
    };
    std::vector<std::string> dep_strs;
    for (auto piece_sv : split_string_view(deps_sv, constants::COMMA_CHAR)) {
        const std::string piece = trim_copy(piece_sv);
        if (piece.empty()) continue;  // 空片段（尾随/连续逗号）不成依赖
        if (starts_with_operator(piece) && !dep_strs.empty())
            dep_strs.back() += ", " + piece;  // 复合约束的续接
        else
            dep_strs.push_back(piece);
    }
    return dep_strs;
}

/**
 * 定位仓库索引文件：本地镜像（file:// 或裸路径）直接拼路径，远程镜像下载到临时目录。
 *
 * 三种失败都发**各自**的告警并返回 nullopt —— 它们的文案不同（配置/下载/不存在），但都只
 * 意味着"本次没有索引可用"，与"索引文件存在却打不开"（硬失败，见 load_index）不同类。
 */
static std::optional<std::filesystem::path> resolve_index_path()
{
    // 读取镜像地址（可能为本地路径或 http URL）
    std::string mirror;
    try {
        mirror = Config::instance().get_mirror_url();
    } catch (const std::exception& e) {
        log_warning(string_format("warning.repo_mirror_config", e.what()));
        return std::nullopt;
    }
    std::string arch = Config::instance().get_architecture();

    bool is_local = mirror.find(constants::PROTOCOL_FILE) == 0 || mirror.find('/') == 0;

    try {
        std::filesystem::path index_path;
        if (is_local) {
            std::string path_str =
                (mirror.find(constants::PROTOCOL_FILE) == 0) ? mirror.substr(7) : mirror;
            index_path = std::filesystem::path(path_str) / arch / constants::REPO_INDEX_FILE;
        } else {
            std::string url = mirror + arch + "/" + std::string(constants::REPO_INDEX_FILE);
            index_path = Config::get_tmp_dir() / constants::REPO_INDEX_TMP;
            download_file(url, index_path, false);
        }
        // 判定用不抛的 exists_follow：索引路径若被一个符号链接环占着，`fs::exists` 会抛
        // filesystem_error 而不是判"没有索引"（见 base/path_predicates.hpp 的谓词说明）。
        if (!exists_follow(index_path)) {
            log_warning(string_format("warning.repo_index_missing", index_path.string()));
            return std::nullopt;
        }
        return index_path;
    } catch (const std::exception& e) {
        log_warning(string_format("warning.repo_index_download", e.what()));
        return std::nullopt;
    }
}

/**
 * 逗号分隔字段 → **非空** token 列表（去首尾空白、丢空片段）。
 *
 * `split_string_view` **总是**产出尾随一段（`"a,"` → `["a", ""]`），而索引里的
 * `provides`/`needed_so` 用 `,` 连接。不处理空 token / 前导空白会把**空串**当成一个
 * capability 收进 libsolv 的 pool：它是 `STRID_EMPTY`，无人提供，而 `collect_problems`
 * 又因为 `dep_name` 为空而跳过它 —— 用户只看到一条无从定位的 "solve failed"
 * （2026-10-02 修）。`deps` 字段早有 `split_dep_field` 做同样的清洗，这里补齐另两个字段。
 */
static std::vector<std::string> split_comma_list(std::string_view sv)
{
    std::vector<std::string> out;
    for (auto piece : split_string_view(sv, constants::COMMA_CHAR)) {
        std::string s = trim_copy(piece);
        if (!s.empty()) out.push_back(std::move(s));
    }
    return out;
}

/** 索引里的一个版本块 → PackageInfo（deps 串含复合约束，交给 split_dep_field 合并） */
static PackageInfo make_package_info(const RepoIndexVersionBlock& b)
{
    PackageInfo pkg;
    pkg.name = b.name;
    pkg.version = b.version;
    pkg.sha256 = b.hash;
    // b.deps 为空时 split_dep_field 会切出空片段，必须在调用前挡住（同 provides/needed_so）
    if (!b.deps.empty()) pkg.dependencies = detail::parse_dep_strings(split_dep_field(b.deps));
    pkg.provides = split_comma_list(b.provides);
    pkg.needed_so = split_comma_list(b.needed_so);
    return pkg;
}

/**
 * 吸收索引里的一行（该行的**全部**版本块）。
 *
 * 每块先记 providers_（provides —— 版本级优先，解析器已回退到包级），再把 PackageInfo
 * 追加进 packages_ 的该包版本列表。两处顺序与逐行内联时一致，不要调换。
 */
void Repository::absorb_index_line(std::string_view line)
{
    for (const auto& b : parse_repo_index_line(line)) {
        // 记录提供者（provides）——版本级优先，解析器已回退到包级。
        // 走 `split_comma_list`（与 make_package_info 同一清洗）：空 token 不能进 providers_，
        // 否则 `find_provider("")` / 依赖判定会拿到脏结果。
        for (const auto& prov : split_comma_list(b.provides)) {
            auto& pv = providers_[prov];
            if (pv.empty() || pv.back() != b.name) {
                pv.push_back(b.name);
            }
        }
        // 先构造再索引（两步分开写，避免"索引表已被插入空壳、构造却抛了"这种副作用顺序差）
        PackageInfo pkg = make_package_info(b);
        packages_[pkg.name].push_back(std::move(pkg));
    }
}

void Repository::sort_package_versions()
{
    // 每个包的版本列表按版本号升序排列（最后一个就是最新版）
    for (auto& versions : packages_ | std::views::values) {
        std::ranges::sort(versions, [](const PackageInfo& a, const PackageInfo& b) {
            return version_compare(a.version, b.version);
        });
    }
}

/**
 * 按**路径**解析索引文件 —— 与 `load_index()` 共用同一套解析与排序，只是不负责"去哪拿"。
 *
 * 逐个解析索引行，格式: 包名|版本:哈希:依赖:提供:needed_so;版本2:...|包级提供
 *
 * **字段切分走 base/utils.cpp 的 parse_repo_index_line（唯一实现）**：本函数与
 * pkg/depend_scanner.cpp 曾各写一份，而那份要求版本块 ≥5 字段 —— 4 字段的行
 * （provides 在 vh[3]、无 needed_so）在 `depend remove` / `depend abibreak` 里被整行
 * 丢掉，静默报"无受影响包"。切分逻辑不再有任何第二份。
 *
 * 失败语义：**打不开**抛（`error.open_file_failed`）；**读中途出错**（`badbit`，包表可能
 * 残缺）返回 `false`，由调用方按自己的策略处理 —— 本函数**不发任何告警、也不决定**读失败
 * 算不算致命（两个消费者的策略有意不同，见头文件）。
 */
bool Repository::load_index_from_file(const std::filesystem::path& index_path)
{
    packages_.clear();
    providers_.clear();

    std::ifstream file(index_path);
    if (!file.is_open()) {
        throw LpkgException(string_format("error.open_file_failed", index_path.string()));
    }
    std::string line;
    while (std::getline(file, line)) {
        absorb_index_line(line);
    }
    // 非 EOF 收尾且 `bad`（实测：目录 = open 成功 + badbit；空文件是干净的 eof）⇒
    // 读中途失败。**不抛**：调用方要先决定"这算不算致命"（见头文件的两条策略）。
    const bool clean = !file.bad();
    sort_package_versions();
    return clean;
}

/**
 * 加载仓库索引文件
 * 支持远程（http/https）和本地（file://）两种方式。
 * 远程索引会被下载到临时目录后解析。
 * 所有异常会被捕获并输出警告，不会影响程序运行。
 */
void Repository::load_index()
{
    packages_.clear();
    providers_.clear();

    const auto index_path = resolve_index_path();
    if (!index_path) return;  // 告警已由 resolve_index_path 按各自的失败原因发出

    try {
        // 返回值（读中途出错 = 包表可能残缺）在这条路径上**有意忽略**：本函数无论读没读全，
        // 结局都是"空/残缺仓库 + 告警"，由下面那条 packages_.empty() 兜住用户可见性。
        // 实测契约：索引用**目录**占住时（open 成功、读即失败 ⇒ badbit）这里落的是
        // `warning.repo_index_empty`，**不是** `repo_index_unreadable`
        // —— `AggregatedIndexTest.DirectoryIndexIsReportedNotEmptyRepo` 钉着它。
        (void)load_index_from_file(*index_path);
    } catch (const std::exception&) {
        // 文件"存在"但打不开/读不出来此前完全静默：解析出 0 个包 → 上层会报告"所有包
        // 都已是最新版本"，用户以为没事（历史 TODO D4）。
        //
        // ⚠️ **实测订正 2026-09-26：这条分支几乎不可达，别指望它兜住"索引是目录/损坏"**。
        //    · "**竟是个目录**"不成立 —— Linux 上 `std::ifstream` **打开目录是成功的**（失败的
        //      是随后的读），于是目录索引会落到下面"解析出 0 个包"那条告警（`repo_index_empty`）
        //      上，而不是这里（实测：造一个目录索引，捕获到的是 `repo_index_empty`）。
        //    · "权限"也挡不住 —— lpkg 永远以 root 跑。
        //    真正兜住"索引损坏/是目录/被截断"的是下面那条 **`warning.repo_index_empty`**：
        //    它可达、且是用户可见的那个"不静默"。本分支保留是纵深防御（FIFO/设备之类的怪场景）。
        //
        // ⚠️ **这里的处置与 `resolve_index_path()` 的几种失败是同一形状：告警 + 返回（仓库留空、
        //    不抛）**，不是"中止命令"。（订正 2026-09-26：本行原写"**这是硬失败，不是当空仓库**"
        //    —— 那句话描述的是**意图**，而实现就是"当空仓库 + 告警"，两者不可区分；这种措辞会让人
        //    以为改这里会挡住安装。）
        //    **为什么不抛**：仓库只用于**依赖解析**，本地 `.lpkg` 走 `local_candidates` 那条路 ——
        //    "仓库连不上/索引坏掉"不该让 `lpkg install ./x.lpkg` 失败（离线装包是常见用法）。
        //    所以真正的要求不是"硬失败"，而是**绝不静默**：四种取不到索引的情形
        //    （配置缺失 / 索引不存在 / 下载失败 / 打不开）各发自己的告警键，用户能区分原因。
        log_warning(string_format("warning.repo_index_unreadable", index_path->string()));
        return;
    }

    // 解析出 0 个包（空文件/半截下载/全是被跳过的坏行）必须告警：否则上游会把
    // "仓库为空"读成"一切正常"，`lpkg upgrade` 直接打印"所有包都已是最新版本"（历史 TODO D4）
    if (packages_.empty()) {
        log_warning(string_format("warning.repo_index_empty", index_path->string()));
    }
}

/**
 * 根据 capability 查找提供该能力的包。
 *
 * 返回的版本**必须确实提供该 capability**：providers_ 只在索引解析时记录
 * 包名（含所有版本），而 find_package 只回最新版——若最新版已不再提供该
 * capability（旧版提供、新版丢弃），直接返回最新版会导致"明明有提供者却报
 * 未解析"的假失败。因此从每个候选包的最新版本向下找第一个提供该
 * capability 的版本。
 */
std::optional<PackageInfo> Repository::find_provider(const std::string& capability) const
{
    auto it = providers_.find(capability);
    if (it == providers_.end() || it->second.empty()) return std::nullopt;
    for (const auto& pkg_name : it->second) {
        auto pit = packages_.find(pkg_name);
        if (pit == packages_.end() || pit->second.empty()) continue;
        for (auto rit = pit->second.rbegin(); rit != pit->second.rend(); ++rit) {
            for (const auto& prov : rit->provides) {
                if (prov == capability) return *rit;
            }
        }
    }
    return std::nullopt;
}

/** 更新（或新增）某包某版本的元数据 */
void Repository::update_package_info(const std::string& name, const std::string& version,
                                     const std::vector<DependencyInfo>& deps,
                                     const std::vector<std::string>& provides,
                                     const std::vector<std::string>& needed_so)
{
    auto& versions = packages_[name];

    // 记录更新前该包（跨所有版本）提供的 capability，用于计算受影响集合
    std::set<std::string> old_provs;
    for (const auto& pkg : versions) {
        for (const auto& p : pkg.provides) old_provs.insert(p);
    }

    bool found = false;
    for (auto& pkg : versions) {
        if (pkg.version == version) {
            pkg.dependencies = deps;
            pkg.provides = provides;
            pkg.needed_so = needed_so;
            found = true;
            break;
        }
    }
    if (!found) {
        PackageInfo pkg;
        pkg.name = name;
        pkg.version = version;
        pkg.dependencies = deps;
        pkg.provides = provides;
        pkg.needed_so = needed_so;
        versions.push_back(std::move(pkg));
        std::ranges::sort(versions, [](const PackageInfo& a, const PackageInfo& b) {
            return version_compare(a.version, b.version);
        });
    }

    // 增量更新 providers_：只处理本包，不再整表重建。整表重建遍历 unordered_map
    // 的 packages_（迭代顺序不确定），会破坏 find_provider 返回的"第一个提供者"
    // 的确定性。这里只移除本包名、按当前 provides 重新加入，并只对受影响
    // capability 的候选列表按包名字典序排序，保证结果确定。
    std::set<std::string> affected = old_provs;
    for (const auto& pkg : versions) {
        for (const auto& p : pkg.provides) affected.insert(p);
    }

    // a) 从所有 capability 的候选列表中移除本包名
    for (auto it = providers_.begin(); it != providers_.end();) {
        auto& vec = it->second;
        vec.erase(std::remove(vec.begin(), vec.end(), name), vec.end());
        if (vec.empty()) {
            it = providers_.erase(it);
        } else {
            ++it;
        }
    }
    // b) 重新加入本包当前提供的 capability
    for (const auto& pkg : versions) {
        for (const auto& prov : pkg.provides) {
            auto& pv = providers_[prov];
            if (std::find(pv.begin(), pv.end(), name) == pv.end()) {
                pv.push_back(name);
            }
        }
    }
    // c) 受影响 capability 的候选按包名字典序排序 → find_provider 结果确定
    for (const auto& cap : affected) {
        auto it = providers_.find(cap);
        if (it != providers_.end()) std::ranges::sort(it->second);
    }
}

/** 按包名查找最新版本 */
std::optional<PackageInfo> Repository::find_package(const std::string& name)
{
    auto it = packages_.find(name);
    if (it == packages_.end() || it->second.empty()) return std::nullopt;
    return it->second.back();
}

/** 按包名+版本精确查找 */
std::optional<PackageInfo> Repository::find_package(const std::string& name,
                                                    const std::string& version)
{
    auto it = packages_.find(name);
    if (it == packages_.end()) return std::nullopt;
    for (const auto& pkg : it->second) {
        if (pkg.version == version) return pkg;
    }
    return std::nullopt;
}

/** 按复合版本约束查找最匹配的版本（从高到低遍历，返回第一个满足全部约束的） */
std::optional<PackageInfo> Repository::find_best_matching_version(
    const std::string& name, const std::vector<Constraint>& constraints)
{
    auto it = packages_.find(name);
    if (it == packages_.end() || it->second.empty()) return std::nullopt;
    for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
        if (version_satisfies_all(rit->version, constraints)) {
            return *rit;
        }
    }
    return std::nullopt;
}

/** 按单一版本约束查找最匹配的版本（从高到低遍历，返回第一个满足条件的） */
std::optional<PackageInfo> Repository::find_best_matching_version(const std::string& name,
                                                                  const std::string& op,
                                                                  const std::string& version_req)
{
    auto it = packages_.find(name);
    if (it == packages_.end() || it->second.empty()) return std::nullopt;
    for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
        if (version_satisfies(rit->version, op, version_req)) {
            return *rit;
        }
    }
    return std::nullopt;
}

/** 见 repository.hpp 的说明：`load_index()` 失败时"只告警不抛"的**唯一**实现。 */
bool load_index_or_warn(Repository& repo)
{
    try {
        repo.load_index();
        return true;
    } catch (const std::exception& e) {
        log_warning(string_format("warning.repo_index_load_failed", e.what()));
        return false;
    }
}
