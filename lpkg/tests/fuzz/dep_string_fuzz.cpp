// harness #6：依赖串解析 —— `detail::parse_dep_strings()` 与 `detail::dependency_name_of()`。
//
// ① 为什么 fuzz 它们（有什么真实后果）
//   · `parse_dep_strings()` 把 `deps/` / metadata.json / 索引里的依赖串变成 libsolv 的依赖。
//     **空名依赖**在 libsolv 里是 ID_EMPTY：既不解析也不报错，求解会"成功"却产出空事务 →
//     上层打印"所有包都已安装"并 exit 0。所以"空名依赖"是本 harness
//     第一要防的东西。
//   · `dependency_name_of()` 是"一行 deps 元数据 → 依赖**包名**"的**唯一**实现，存在的理由
//     就是消灭第二份语法：约束紧贴包名（`provb>=2.0`）与空格分隔（`provb >= 2.0`）必须算出
//     **同一个**包名。规则不一致的后果是同一个包在不同路径上算出不同的键 —— 最要命的一条在
//     `Cache::ensure_reverse_deps`：用纯空白切名字时 `provb>=2.0` 整串成了键，于是"按包名查
//     反向依赖"永远查不到（`autoremove` 正是这么查的）。
//
// ② oracle（断言什么）
//   ① 不崩（ASan/UBSan）；
//   ② `parse_dep_strings` 返回的每个 `DependencyInfo.name` **非空**（空名 = ID_EMPTY = 空事务）；
//   ③ `dependency_name_of(line)` 必须等于一个**回归钉子** `ref_direct`（**照实现转写**的逐位置
//      取名字，见其定义处说明）—— 它只保证"实现没被改"（改回空白切分 / 第二种语法会红），
//      **不构成**独立语义判据；**真正有区分力的是下面 ④⑤ 两条自洽性断言**；
//   ④ **约束紧贴包名 vs 空格分隔**：对同一行取回归钉子给出的名字 `nm` 与最早运算符之后的 `rest`，
//      `nm+rest`（紧贴）与 `nm+" "+rest`（空格分隔）必须解析出同一个包名（且都等于 `nm`）。
//      这正是 `dependency_name_of` 存在的理由，也是当年 bug 的形态 —— **本条最有价值**。
//      ⚠️ 紧贴这半有一个**伪影格**会被有意跳过：去掉空格若让 `nm` 末尾与运算符首字符并置成
//      **更早**的二元运算符（`!`+`=`→`!=`），`nm+rest` 的包名会合法地短一位（`9!! =Ya`
//      的紧贴形 `9!!=Ya` 里 `!=` 命中更早 → 包名 `9!`）。判据是紧贴形里最早运算符仍在 `nm.size()`
//      处才断言；这不是 `dependency_name_of` 的缺陷，跳过它避免误报；
//   ⑤ **分组无关**：把输入逐行单独解析再拼接，必须与一次性解析全部行**逐字段相等**（说明解析
//      没有跨行状态；`,` 切分与算子续接（`>= 2.0.0 < 3.0.0` 合成一个依赖）的自洽性一坏，
//      两条路就会分叉）。
//
// ③ 已知边界 / 有意不报的东西
//   · **没有版本约束的裸名**（`glibc`）、**空串/纯空白**、**超长串**都是合法输入：裸名是常态、
//     空片段被丢弃是设计（`if (d.empty()) continue;`）、超长不影响判据。都不算缺陷。
//   · 约束里出现任意字节（含 `\r`/TAB）都合法：`dependency_name_of` 只剥行尾**一个** `\r`
//     且 `trim_copy` 只碰空格/TAB —— 回归钉子逐字复刻这一点，不做更强的要求（否则会误报）。
//   · **不断言"版本串非空"**：`foo >=` 解析出空版本是实现的现状，不是要在这里守的契约。
//   · 不断言 `parse_dep_strings` 会把一个字符串按 `,` 拆成多个依赖：**它不拆**。`a>=1,b<=2`
//     是**一个**依赖带两条约束（`,` 只在约束区内被跳过）。真按 `,` 拆开的那一步在调用方
//     （`repository.cpp` 的 `split_dep_field`），不在本函数职责内。
//
// ④ 输入格式：换行分隔的依赖串，最多 8 条（空行丢弃），总长 ≤ 4096 字节。

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "base/strings.hpp"       // trim_copy（依赖串统一 trim 用的就是它，回归钉子也复用）
#include "fuzz_common.hpp"        // silence_stdout()
#include "vercmp/dep_parser.hpp"  // detail::parse_dep_strings / detail::dependency_name_of

namespace
{

/// 与 `dep_parser.cpp` 的 `DEP_OPS` **同序**：同一位置上 `>=` 必须胜过 `>`。
constexpr std::string_view kOps[] = {">=", "<=", "!=", "==", ">", "<", "="};

constexpr std::size_t kMaxLines = 8;
constexpr std::size_t kMaxBytes = 4096;

/// 两类结局各计一次：证明**接受路径**（解析出依赖）与**空结果路径**都真的被走到 ——
/// 否则一个只跑一条路的 harness 看起来也是绿的。
std::size_t g_nonempty = 0;
std::size_t g_empty = 0;

void report_outcomes()
{
    std::fprintf(stderr, "[fuzz] dep_string: 解析出依赖 %zu 次 / 空结果 %zu 次\n", g_nonempty,
                 g_empty);
}

/// oracle 违反 —— **先把原因打到 stderr 再崩**（SIGILL 的栈会被 ASan 丢掉，不打这行两处
/// 断点没法区分）。
[[noreturn]] void oracle_violation(const std::string& why)
{
    std::fprintf(stderr, "[fuzz] 依赖串解析 oracle 违反: %s\n", why.c_str());
    __builtin_trap();
}

/// **回归钉子（不是独立语义判据）**：一行 → 包名，**照 `dependency_name_of` 的判据转写** ——
/// 逐位置找最早运算符、只剥行尾一个 `\r`、只去空格/TAB、名字段只去尾空格；连
/// "先 `trim_copy` 再交给取名函数"的顺序都刻意复刻（否则 `foo\r ` 这类输入会与实现分叉）。
/// 所以它**只能抓"实现被改了"**（两侧之一漂移），**抓不到"实现本来就错"的语义缺陷** ——
/// 它是与实现一起调到一致的，不构成独立判据。
/// 真正有区分力的**语义**判据是本文件下面的两条**自洽性**断言（见 ④⑤）：
///   · 约束**紧贴包名**与**空格分隔**必须解析出同一个包名 —— 正是 `provb>=2.0` 那个 bug 的形态；
///   · 一次性解析整份输入与逐行解析再拼接必须逐字段相同。
std::string ref_direct(std::string_view line)
{
    std::string s(line);
    if (!s.empty() && s.back() == '\r') s.pop_back();  // 只剥行尾一个 \r
    std::string d = trim_copy(s);                      // 只去空格/TAB
    if (d.empty()) return {};
    for (std::size_t pos = 0; pos < d.size(); ++pos) {
        for (std::string_view op : kOps) {
            if (d.compare(pos, op.size(), op) != 0) continue;
            std::string name = d.substr(0, pos);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            return name;
        }
    }
    return d;  // 没有运算符 ⇒ 整串就是包名
}

/// `parse_dep_strings` 内部的取名路径：先 `trim_copy(raw)` 再交给取名函数。
/// **同上：照实现转写的回归钉子**，复刻这个"先 trim、后剥 `\r`"的顺序只为与实现一致。
std::string ref_parsed_name(std::string_view raw)
{
    const std::string d = trim_copy(raw);
    if (d.empty()) return {};
    return ref_direct(d);
}

/// 最早出现的合法运算符的位置（`std::string::npos` = 没有）。只做位置探测，供"紧贴写法是否
/// 被并置出一个更早的运算符"这一形状判断复用。
std::size_t earliest_op_pos(const std::string& d)
{
    for (std::size_t i = 0; i < d.size(); ++i) {
        for (std::string_view op : kOps) {
            if (d.compare(i, op.size(), op) == 0) return i;
        }
    }
    return std::string::npos;
}

bool same_dep(const DependencyInfo& a, const DependencyInfo& b)
{
    return a.name == b.name && a.constraints == b.constraints;
}

std::string dump_dep(const DependencyInfo& d)
{
    std::string out = "'" + d.name + "'";
    for (const auto& c : d.constraints) out += " " + c.op + " " + c.version;
    return out;
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    silence_stdout();  // 本 harness 不打 stdout；stderr 留给 sanitizer 与 oracle 报告
    // 注册结局计数（证明"解析出依赖"与"空结果"两条路都真的被走到）。
    std::atexit(report_outcomes);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > kMaxBytes) return 0;

    // 换行分隔的依赖串；空行丢弃（与 `parse_dep_strings` 对空片段的处置一致）。
    std::vector<std::string> lines;
    std::string cur;
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] == '\n') {
            if (!cur.empty()) lines.push_back(cur);
            cur.clear();
            if (lines.size() >= kMaxLines) break;
        } else {
            cur.push_back(static_cast<char>(data[i]));
        }
    }
    if (!cur.empty() && lines.size() < kMaxLines) lines.push_back(cur);
    if (lines.empty()) return 0;

    // ⑤ 分组无关：一次性解析全部行 vs 逐行解析再拼接。
    const std::vector<DependencyInfo> all = detail::parse_dep_strings(lines);
    std::vector<DependencyInfo> concat;
    for (const auto& line : lines) {
        const std::vector<DependencyInfo> one = detail::parse_dep_strings({line});
        if (one.size() > 1) {
            oracle_violation("单条依赖串解析出 " + std::to_string(one.size()) +
                             " 个 DependencyInfo（应为 0 或 1）: 行='" + line + "'");
        }
        if (!one.empty()) {
            // ② 非空名
            if (one[0].name.empty()) {
                oracle_violation("空名依赖（libsolv 里是 STRID_EMPTY ⇒ 求出空事务）: 行='" + line +
                                 "'");
            }
            // ③ 与参考实现一致
            const std::string ref = ref_parsed_name(line);
            if (one[0].name != ref) {
                oracle_violation("包名与参考实现不符: 行='" + line + "' impl='" + one[0].name +
                                 "' ref='" + ref + "'");
            }
            concat.push_back(one[0]);
        }
    }
    if (all.size() != concat.size()) {
        oracle_violation("分组改变了解析结果条数: 一次性 " + std::to_string(all.size()) +
                         " 条 vs 逐行拼接 " + std::to_string(concat.size()) + " 条");
    }
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (!same_dep(all[i], concat[i])) {
            oracle_violation("分组改变了解析结果: 索引 " + std::to_string(i) + " 一次性 " +
                             dump_dep(all[i]) + " vs 逐行 " + dump_dep(concat[i]));
        }
    }
    // ② 全局非空名（分组路径已查过，这里兜住 all 里可能有的别的入口）
    for (const auto& d : all) {
        if (d.name.empty()) oracle_violation("空名依赖（全量结果）: " + dump_dep(d));
    }

    // ③④ 取名函数：参考名 + 紧贴/空格两写法同解。
    for (const auto& line : lines) {
        const std::string ref = ref_direct(line);
        const std::string got = detail::dependency_name_of(line);
        if (got != ref) {
            oracle_violation("dependency_name_of 与参考实现不符: 行='" + line + "' impl='" + got +
                             "' ref='" + ref + "'");
        }

        // 取参考名对齐后的"trim 版"上最早运算符的位置，构造紧贴 / 空格两种写法。
        std::string s(line);
        if (!s.empty() && s.back() == '\r') s.pop_back();
        const std::string d = trim_copy(s);
        const std::size_t op_pos = earliest_op_pos(d);
        if (op_pos != std::string::npos && op_pos > 0) {
            const std::string rest = d.substr(op_pos);  // 运算符 + 其后所有内容
            const std::string spaced = ref + " " + rest;
            // 空格分隔这半永远干净：ref 里没有运算符，`ref + " "` 把运算符推到 `ref.size()+1`，
            // 包名截到 `ref` 再去掉尾空格。
            const std::string got_spaced = detail::dependency_name_of(spaced);
            if (got_spaced != ref) {
                oracle_violation("空格分隔写法给出错误包名: ref='" + ref + "' 输入='" + spaced +
                                 "' → '" + got_spaced + "'");
            }
            // 紧贴这半：去掉空格会把 `ref` 末尾与运算符首字符**并置**。若这个并置恰好构成一个
            // **更早**的二元运算符（`!` + `=` → `!=`），`attached` 的包名会**合法地**比 `ref`
            // 短一位 —— 那是"harness 改写了输入"造成的伪影，不是 `dependency_name_of` 的缺陷
            // （`9!! =Ya` 的紧贴形 `9!!=Ya` 里 `!=` 命中更早，包名成了 `9!`）。
            // 判据：紧贴形里最早运算符的位置是否仍等于 `ref.size()`；不是就跳过这一格。
            const std::string attached = ref + rest;
            if (earliest_op_pos(attached) == ref.size()) {
                const std::string got_attached = detail::dependency_name_of(attached);
                if (got_attached != ref) {
                    oracle_violation(
                        "紧贴写法给出错误包名（约束紧贴包名与空格分隔必须同解）: ref='" + ref +
                        "' 输入='" + attached + "' → '" + got_attached + "'");
                }
            }
        }
    }

    if (all.empty()) {
        ++g_empty;
    } else {
        ++g_nonempty;
    }
    return 0;
}
