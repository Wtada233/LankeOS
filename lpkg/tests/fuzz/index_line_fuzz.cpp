// harness #7：仓库索引行解析 —— `parse_repo_index_line()`（`base/repo_index.hpp`，唯一实现，
// `base/utils.cpp` 定义）。
//
// ① 为什么 fuzz 它（有什么真实后果）
//   索引（`constants::REPO_INDEX_FILE`）的行格式是
//   `包名|版本:哈希:依赖:provides:provides_soname:needed_so;版本2:…|`。
//   两个消费者**必须**共用这一个解析器：`repo/repository.cpp`（建包表）与
//   `pkg/depend_scanner.cpp`（建 needed_so 反图）。它们曾各写一份，字段数判据不一致 ——
//   于是旧格式行在其中一侧被整行丢掉 → 反向依赖图缺一整类边 → **静默打印"无受影响包"**
//   （给出错误答案而不是报错，用户据此删包/判断重建面）。少读一条不是显示问题，是数据丢失。
//
//   ⚠️ **订正 2026-10-04（8.0.0，破坏性）**：版本块从"容忍 4/5 字段 + 行级 provides 回退"收紧
//   为**恰好 6 字段**、**没有**行级第 3 段。非 6 字段的版本块**整块跳过**（不是被误读）。
//
// ② oracle（断言什么）
//   ① 不崩（ASan/UBSan）；
//   ② 每个返回块的 `version` **非空** —— 解析器明文"版本号为空的畸形块跳过"，与 farm 的
//      `graph.rs` 同判据。这条会红：若有人去掉那个跳过守卫，畸形块就会带着空版本漏出去；
//   ③ **纯函数 / 无状态**：同一行解析两次结果**逐字段相等**（防"解析器里有静态状态"这类泄漏）；
//   ④ **6 字段块回归钉**：`a|1.0:h:dep:cap:libA.so.1:libc.so.6` 必须解析出 1 块，且三个
//      字段各就各位（provides / provides_soname / needed_so **不许串味**）；
//   ⑤ **非 6 字段整块跳过回归钉**：4/5 字段的旧格式行必须**解析出空表**（不再被误读）；
//   ⑥ **聚合版本回归钉**：`;` 分隔的第二块照常解析（与第一块共享包名）。④⑤⑥ 都是**已知语义的
//      回归钉**，每轮无条件跑一次，不靠 fuzzer 碰运气。
//
// ③ 已知边界 / 有意不报的东西
//   · 字段里含 `;` 是版本块分隔符、含 `|` 是行内段分隔符 —— 这是格式本身，不算缺陷；
//   · **包名可以为空**：输入形如 `|<版本>:…` 时 `parts[0]==""`，解析器不做过滤 →
//     返回 `name==""` 的块（`repository.cpp` 也不过滤）。这属于实现的现状，**不是本 harness
//     要报的契约**，所以**有意不断言 `name` 非空** —— 断言它会在 `|1.0:h:p` 这类合法可达的
//     畸形输入上误报（见交付报告）。真要收紧，应在解析器里像空版本一样跳过空名块，而不是
//     在 fuzzer 里 trap。
//   · 行尾只剥**一个** `\r`（`line.remove_suffix(1)`）；输入里嵌的其它 `\r`/`\n`/NUL 都是普通
//     字节，不作额外要求。
//   · `#` 开头（剥 `\r` 后）是注释行 → 空表；无 `|` 的行 → 空表。都是设计。
//
// ④ 输入格式：一行索引文本，总长 ≤ 4096 字节。

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "base/repo_index.hpp"
#include "fuzz_common.hpp"  // silence_stdout()

namespace
{

constexpr std::size_t kMaxBytes = 4096;

/// 两类结局各计一次：证明"解析出块"与"空表（拒绝/跳过）"两条路都真的被走到。
std::size_t g_blocks = 0;
std::size_t g_empty = 0;

void report_outcomes()
{
    std::fprintf(stderr, "[fuzz] index_line: 解析出块 %zu 次 / 空表 %zu 次\n", g_blocks, g_empty);
}

[[noreturn]] void oracle_violation(const std::string& why)
{
    std::fprintf(stderr, "[fuzz] 索引行解析 oracle 违反: %s\n", why.c_str());
    __builtin_trap();
}

bool same_block(const RepoIndexVersionBlock& a, const RepoIndexVersionBlock& b)
{
    return a.name == b.name && a.version == b.version && a.hash == b.hash && a.deps == b.deps &&
           a.provides == b.provides && a.provides_soname == b.provides_soname &&
           a.needed_so == b.needed_so;
}

/// ④⑤⑥ 三条回归钉：都是**已知语义**，每轮无条件跑（不靠 fuzzer 碰运气）。
void check_regression_pins()
{
    // ④ 6 字段块：版本:哈希:依赖:provides:provides_soname:needed_so —— 三个字段各就各位
    const auto full = parse_repo_index_line("a|1.0:h:dep1,dep2:capA:libA.so.1:libc.so.6");
    if (full.size() != 1 || full[0].provides != "capA" || full[0].provides_soname != "libA.so.1" ||
        full[0].needed_so != "libc.so.6") {
        oracle_violation(
            "6 字段块回归: 'a|1.0:h:dep1,dep2:capA:libA.so.1:libc.so.6' 字段错位，实际块数=" +
            std::to_string(full.size()) +
            (full.empty()
                 ? std::string{}
                 : " provides='" + full[0].provides + "' provides_soname='" +
                       full[0].provides_soname + "' needed_so='" + full[0].needed_so + "'"));
    }

    // ⑤ 非 6 字段整块跳过：4 字段（旧写入器把 provides 放 vh[3]）、5 字段、1 字段都必须空表
    if (!parse_repo_index_line("a|1.0:h::p").empty()) {
        oracle_violation("4 字段旧块必须整块跳过（不再被误读成 provides）");
    }
    if (!parse_repo_index_line("a|1.0:h::p:q").empty()) {
        oracle_violation("5 字段旧块必须整块跳过");
    }
    if (!parse_repo_index_line("baz|3.0").empty()) {
        oracle_violation("1 字段版本块必须整块跳过（8.0.0 起要求恰好 6 字段）");
    }

    // ⑥ 聚合版本：`;` 分隔的第二块照常解析
    const auto multi = parse_repo_index_line("m|1.0:h:::libm.so.1:;2.0:h::::libm.so.2");
    if (multi.size() != 2 || multi[0].provides_soname != "libm.so.1" ||
        multi[1].needed_so != "libm.so.2") {
        oracle_violation("聚合版本块回归: ';' 分隔的两块字段不对，实际块数=" +
                         std::to_string(multi.size()));
    }
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    silence_stdout();  // 本 harness 不打 stdout；stderr 留给 sanitizer 与 oracle 报告
    // 注册结局计数（证明"解析出块"与"空表"两条路都真的被走到）。
    std::atexit(report_outcomes);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > kMaxBytes) return 0;

    // 与所有模板一致：无条件跑回归钉（它们不依赖本轮输入）。
    check_regression_pins();

    const std::string line(reinterpret_cast<const char*>(data), size);
    const std::vector<RepoIndexVersionBlock> blocks = parse_repo_index_line(line);

    // ② 版本非空（解析器明文跳过空版本块）
    for (const auto& b : blocks) {
        if (b.version.empty()) {
            oracle_violation("版本为空的畸形块未被跳过: 行='" + line + "' name='" + b.name + "'");
        }
    }

    // ③ 纯函数：同一行两次解析必须逐字段相等
    const std::vector<RepoIndexVersionBlock> again = parse_repo_index_line(line);
    if (blocks.size() != again.size()) {
        oracle_violation("同一行两次解析块数不同（解析器有状态泄漏）: 行='" + line + "' " +
                         std::to_string(blocks.size()) + " vs " + std::to_string(again.size()));
    }
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (!same_block(blocks[i], again[i])) {
            oracle_violation("同一行两次解析结果不同（解析器有状态泄漏）: 行='" + line + "' 索引 " +
                             std::to_string(i));
        }
    }

    if (blocks.empty()) {
        ++g_empty;
    } else {
        ++g_blocks;
    }
    return 0;
}
