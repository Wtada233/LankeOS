// harness #8：lpkg 状态文件的两个**行式解析器** ——
//   · `Cache::read_db_uncached()`（`db/cache.cpp`，Tab 分帧：`key\tv1,v2\n`）
//   · `read_set_from_file()`（`base/io_set_file.hpp`，逐行一个元素）
//
// ① 为什么 fuzz 它们（有什么真实后果）
//   这两个函数读的是 /var/lib/lpkg 下的**归属数据库**（files.db / provides.db / confhashes.db /
//   xattrkeys.db）与集合文件（pkgs / holdpkgs / essential）。它们的输入是**磁盘上可能是半损 / 被
//   篡改 / 旧版本写的**文件。后果是实打实的：
//     · Tab 分帧：key 里含 TAB 会让重载时被截断、属主串错位 —— 仓库为此专门在归档侧拒过含 TAB
//       的成员名（`archive.cpp` 的成员名判据），这里守的是**读侧**不制造这种键；
//     · 空值 token（`a\t,,\n`）绝不能变成一个"无人提供的 capability" / 空属主；
//     · "文件存在却不可读"绝不能静默当空库（`read_db_uncached` / `read_set_from_file` 都明文
//       杜绝 —— 那会让已装包的归属凭空归零）。
//
// ② oracle（断言什么）
//   ① 不崩（ASan/UBSan）；
//   ② **两个解析器对"恒可读的同一个文件"都不许抛**（本 harness 每轮都写文件 ⇒ 文件恒可读；
//      "文件不存在"那条退化成空表的常态路径走不到）。抛了就把异常类型与消息打到 stderr 再 trap；
//   ③ **逐字段等于独立参考实现**：参考实现按 `\n` / `\t` / `,` 逐字复刻格式（含"跳过空 token"、
//      "只剥行尾一个 \r"），两个返回结构都必须与它**完全相等**。这条比任何零散不变量都强 ——
//      格式一旦漂移，返回的键/值集合就会与参考分叉；
//   ④ **纯函数 / 无状态**：同一文件解析两次结果相同。
//
// ③ 已知边界 / 有意不报的东西
//   · **`read_db_uncached` 对"文件不存在"返回空表是常态**（首次运行 / 老 DB 没这个文件）；本
//     harness 每轮都写文件，走不到那条路径，所以**不**把它写成缺陷（也不去构造它）。
//   · **有意不断言"返回的 key 不含 TAB/换行"**：在现行实现下它是**恒真**的 —— key 是
//     `line.find('\t')` **之前**的子串（结构上不可能含 TAB），而 `std::getline` 已按 `\n` 切行
//     （结构上不可能含 `\n`）。恒真断言没有区分力（仓库反复踩过这类"看着有牙其实没有"的断言），
//     故省略。真正要防的"含 TAB 的 key"发生在**写入侧**（base64 编码），不是这里能观测的。
//   · **空 key**（输入 `\tvalue` → `db[""]={"value"}`）是可达的现状，不报：解析器不做 key 过滤，
//     断言它会在合法可达输入上误报。是否收紧（像空版本那样跳过）由维护者定。
//   · 值里含任意字节（含 NUL / `\r`）都合法 —— 参考实现逐字复刻，不做更强要求。
//
// ④ 输入格式：**整个文件的原始字节**（≤ 8192），每轮写到固定路径 `<沙箱>/db.in` 再解析。

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <typeinfo>
#include <unordered_set>
#include <vector>

#include "base/constants.hpp"
#include "base/io_set_file.hpp"
#include "config/config.hpp"
#include "db/cache.hpp"
#include "fuzz_common.hpp"  // silence_stdout()
#include "i18n/localization.hpp"

namespace fs = std::filesystem;

namespace
{

/// 沙箱：Config 的 root 指到这里，避免 `Cache::instance()` 去读**真实**的 /var/lib/lpkg。
/// （与 archive_name harness 同址，但两者在 run.sh 里**串行**跑，各自的 Initialize 都会先
/// 清空，不会互相污染。）
constexpr const char* kBase = "/lpkgfuzz";
constexpr const char* kIn = "/lpkgfuzz/db.in";
constexpr std::size_t kMaxBytes = 8192;

/// 两类结局各计一次：证明"解析出条目"与"空结果"两条路都真的被走到。
std::size_t g_entries = 0;
std::size_t g_empty = 0;

void report_outcomes()
{
    std::fprintf(stderr, "[fuzz] db_line: 解析出条目 %zu 次 / 空结果 %zu 次\n", g_entries, g_empty);
}

[[noreturn]] void oracle_violation(const std::string& why)
{
    std::fprintf(stderr, "[fuzz] db_line oracle 违反: %s\n", why.c_str());
    __builtin_trap();
}

using DbMap = std::map<std::string, std::unordered_set<std::string>, std::less<>>;

/// 按 `std::getline` 的语义切行（`\n` 为界，不含 `\n`；末尾无 `\n` 的最后一段也算一行）。
std::vector<std::string> split_lines(const std::string& bytes)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t nl = bytes.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(bytes.substr(start));
            break;
        }
        out.push_back(bytes.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

/// 参考实现：`read_db_uncached` 的格式（逐字复刻）。
DbMap ref_db(const std::string& bytes)
{
    DbMap db;
    for (const auto& line : split_lines(bytes)) {
        if (line.empty()) continue;
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string key = line.substr(0, tab);
        std::string values = line.substr(tab + 1);
        if (!values.empty() && values.back() == '\r') values.pop_back();
        std::size_t start = 0, end;
        while ((end = values.find(',', start)) != std::string::npos) {
            if (end > start) db[key].insert(values.substr(start, end - start));
            start = end + 1;
        }
        if (start < values.size()) db[key].insert(values.substr(start));
    }
    return db;
}

/// 参考实现：`read_set_from_file` 的格式（逐行，自动去除行尾 \r，跳过空行）。
std::unordered_set<std::string> ref_set(const std::string& bytes)
{
    std::unordered_set<std::string> set;
    for (std::string line : split_lines(bytes)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) set.insert(line);
    }
    return set;
}

std::string dump_where(std::string_view key)
{
    return "key='" + std::string(key) + "'";
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    // 先设好 Config 沙箱，再碰 `Cache::instance()`（它的构造函数会 `load()`，否则会去读真实
    // /var/lib/lpkg）。顺序照 tests/integration/test_depend_scanner_index_parity.cpp 的 SetUp()。
    Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
    Config::instance().set_testing_mode(true);
    init_localization();

    std::error_code ec;
    fs::remove_all(kBase, ec);
    fs::create_directories(kBase, ec);

    Config::instance().set_root_path(kBase);
    Config::instance().init_filesystem();  // 预建 pkgs/holdpkgs/…（load() 要求它们存在）
    Cache::instance().load();              // 让构造/首轮 load 的任何异常在**初始化期**就炸出来

    silence_stdout();  // 进度行等走 stdout；stderr 留给 sanitizer 与警告
    std::atexit(report_outcomes);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > kMaxBytes) return 0;
    const std::string bytes(reinterpret_cast<const char*>(data), size);

    // 每轮写到固定路径：两个解析器读同一个文件。
    {
        std::ofstream f(kIn, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return 0;
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!f) return 0;
    }

    DbMap db;
    std::unordered_set<std::string> set;
    try {
        db = Cache::instance().read_db_uncached(kIn);
        set = read_set_from_file(kIn, MissingSetFilePolicy::Throw);
    } catch (const std::exception& e) {
        oracle_violation("解析器对**恒可读**文件抛异常: " + std::string(typeid(e).name()) + ": " +
                         e.what());
    } catch (...) {
        oracle_violation("解析器抛了非 std::exception 的异常");
    }

    // ②′ 值集合里不许含空串：空 token（`a\t,,\n`）会变成一个"无人提供的 capability" /
    // 空属主。有区分力 —— 去掉实现里 `if (end > start)` 那道跳过空 token 的守卫就会红。
    for (const auto& [k, values] : db) {
        if (values.count("") != 0) {
            oracle_violation("read_db_uncached 的值集合含空串（无人提供的 capability）: key='" + k +
                             "'");
        }
    }

    // ③ 逐字段等于参考实现。
    const DbMap want_db = ref_db(bytes);
    if (db != want_db) {
        // 只报第一条差异，避免输出淹没（键集合可能很大）。
        for (const auto& [k, v] : want_db) {
            auto it = db.find(k);
            if (it == db.end()) oracle_violation("read_db_uncached 少了键 " + dump_where(k));
            if (it->second != v)
                oracle_violation("read_db_uncached 键 " + dump_where(k) + " 的值集合不符");
        }
        for (const auto& [k, v] : db) {
            if (want_db.count(k) == 0) oracle_violation("read_db_uncached 多了键 " + dump_where(k));
        }
        oracle_violation("read_db_uncached 与参考实现不符（值集合差异）");
    }

    const std::unordered_set<std::string> want_set = ref_set(bytes);
    if (set != want_set) {
        for (const auto& e : want_set) {
            if (set.count(e) == 0) oracle_violation("read_set_from_file 少了元素");
        }
        for (const auto& e : set) {
            if (want_set.count(e) == 0) oracle_violation("read_set_from_file 多了元素");
        }
        oracle_violation("read_set_from_file 与参考实现不符");
    }

    // ④ 纯函数 / 无状态：同一文件解析两次结果相同。
    if (Cache::instance().read_db_uncached(kIn) != db) {
        oracle_violation("read_db_uncached 两次解析结果不同（有状态泄漏）");
    }
    if (read_set_from_file(kIn, MissingSetFilePolicy::Throw) != set) {
        oracle_violation("read_set_from_file 两次解析结果不同（有状态泄漏）");
    }

    if (db.empty() && set.empty()) {
        ++g_empty;
    } else {
        ++g_entries;
    }
    return 0;
}
