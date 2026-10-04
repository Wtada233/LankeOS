// harness：WAL 行解析 —— **崩溃恢复路径的唯一输入**。
//
// 为什么是它：`transaction.log` 是回滚与崩溃恢复的**唯一依据**（ARCH.md 的 I-9 / §11），
// 而它按行解析（33 个行前缀 + 箭头分帧 + "尾部固定字段从右往左切、剩余整段归 arg1"）。
// 损坏行、半写行、"未来格式"行都会进 `parse_op`，而**每次启动**都会读它
// （`lpkg rec` 与 `init_database_for` 的自动调用）⇒ **这里抛异常 = 所有 lpkg 命令起不来**。
//
// 它已经真出过事：CRLF 的 WAL 让"未提交批次"守卫**反转成删备份**（危险的那一侧）；
// 归档成员名里的 `\n` 能伪造出**语法完全合法**的 WAL 行（归档侧为此加了整包拒绝）。
// 这两件都发生在"行"这一层 —— 正是本 harness 喂的东西。
//
// ── oracle（四条，每一条在它守的东西坏掉时都会红）────────────────────────────
//  1. **不抛**。`parse_op` 是纯字符串切分（无数字转换、不碰文件系统），唯一的抛点是
//     `walop_type_from_name`，而它已被内部 catch 成 INVALID —— "不抛"是可断言的契约。
//  2. `op.raw` 必须**逐字节等于**输入行（`parse_op` 第一句就是 `op.raw = line`）。
//     破了它 = 回滚审计行印出来的东西与真实行不符。
//  3. 有效类型 ⇒ `walop_type_name()` 不得是 `UNKNOWN`（TYPE_MAP 完整性：加了枚举却没登记
//     名字，回滚审计行与 `rec` 的日志就会印成 UNKNOWN）。
//  4. `skip_in_reverse()` 与 `wal_type_is_reversible()` **必须互补**（`!=`）—— 即
//     "每个类型要么在撤销表里、要么被显式跳过"。**相等 = 该类型既不可逆又不会被跳过 ⇒
//     回滚静默少做一步**（数据不一致，且没有任何痕迹）。这条与
//     `tests/unit/test_undo_table.cpp` 的不变量同源，这里把它搬到**任意畸形行**上。
//     另加一条幂等：同一行解析两遍必须同结果（防解析器引入状态）。
//
// ── 关于输出噪音（不处理的话这个 harness 是没法跑的）──────────────────────────
// `parse_op` 对**未知类型**会 `log_warning` 一次，而它走 `std::cerr`（`base/utils.cpp:120`）。
// fuzzer 每轮都在造未知类型 ⇒ 60 s 能写出几百 MB 日志、并把吞吐拖垮。处理办法是**只把 C++
// 的 `std::cerr` 接到已被静音的 stdout**：
//   · ASan/UBSan 的崩溃报告与 libFuzzer 自己的统计走**裸 fd 2**（C 层），**不受影响**；
//   · 本文件的 `oracle_violation` 用 `std::fprintf(stderr, …)`（也是 C 层）—— 照样报得出来；
//   · 代价：lpkg 自己的告警在 fuzz 输出里看不见了 ⇒ "INVALID 这条路径真的被走到了"改由
//     **计数器**证明（见 `report_outcomes`），而不是靠翻日志。

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "db/wal_op.hpp"
#include "fuzz_common.hpp"
#include "i18n/localization.hpp"

namespace
{

/// 每轮最多解析几行：够表达"一批行里的上下文"，又不让单轮变成 I/O 压力测试。
constexpr std::size_t kMaxLines = 8;

std::size_t g_valid = 0;
std::size_t g_invalid = 0;

void report_outcomes()
{
    std::fprintf(stderr, "[fuzz] wal_line: 有效行 %zu / INVALID %zu\n", g_valid, g_invalid);
}

/// 把行渲染成可见形式（控制字符转义）—— 行里的 `\n`/`\r` 是**本 harness 的常态输入**，
/// 原样打进 stderr 会把报告自己切成多行，反而看不出是哪一条触发的。
std::string visible(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
        if (c >= 0x20 && c < 0x7f) {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        }
    }
    return out;
}

[[noreturn]] void oracle_violation(const char* why, const std::string& line)
{
    std::fprintf(stderr, "[fuzz] oracle 违反: %s\n  行(%zu 字节) = [%s]\n", why, line.size(),
                 visible(line).c_str());
    __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    init_localization();
    silence_stdout();  // 进度行等走 stdout；stderr 的裸 fd 留着给 sanitizer
    // 见顶部"关于输出噪音"：只静音 C++ 的 std::cerr（lpkg 的告警），不碰裸 fd 2。
    std::cerr.rdbuf(std::cout.rdbuf());
    std::atexit(report_outcomes);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 4096) return 0;

    // 换行分帧。**空行保留**：空行是真实存在于 WAL 里的输入（也不该被解析成任何真实操作）。
    std::vector<std::string> lines;
    std::string cur;
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] == '\n') {
            lines.push_back(cur);
            cur.clear();
            if (lines.size() >= kMaxLines) break;
        } else {
            cur.push_back(static_cast<char>(data[i]));
        }
    }
    if (lines.size() < kMaxLines) lines.push_back(cur);

    for (const auto& line : lines) {
        wal::WALOp op;
        try {
            op = wal::parse_op(line);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[fuzz] 异常原文: %s\n", e.what());
            oracle_violation("parse_op 抛了 —— 恢复路径抛异常会让每次启动都失败", line);
        } catch (...) {
            oracle_violation("parse_op 抛了非 std::exception", line);
        }

        // 2. 原样保留
        if (op.raw != line) oracle_violation("op.raw 与输入行不逐字节相等", line);

        // 3. TYPE_MAP 完整性 + 计数（两条路径都要被走到，见顶部）
        if (op.type != wal::WALOpType::INVALID) {
            ++g_valid;
            if (wal::walop_type_name(op.type) == "UNKNOWN") {
                oracle_violation("解析出了有效类型，却没在 TYPE_MAP 里登记名字", line);
            }
        } else {
            ++g_invalid;
        }

        // 4. "要么可逆、要么被显式跳过" —— 相等意味着回滚会静默漏掉它
        if (op.skip_in_reverse() == wal::wal_type_is_reversible(op.type)) {
            oracle_violation(
                "该类型既不在撤销表里、也没被跳过（回滚会静默少做一步，且没有任何痕迹）", line);
        }

        // 幂等：解析器不得有状态
        const wal::WALOp again = wal::parse_op(line);
        if (again.type != op.type || again.raw != op.raw) {
            oracle_violation("同一行解析两次结果不同（解析器有状态）", line);
        }
    }
    return 0;
}
