#include "so_spec.hpp"

#include <algorithm>
#include <cstddef>

#include "constants.hpp"
#include "strings.hpp"

namespace
{

/// 符号版本名的字符集：`[A-Za-z0-9_.+-]`（GLIBC_2.40 / GLIBCXX_3.4.30 / CXXABI_1.3.11 / QT_6 …）。
/// `-` 与 `+` 也允许（见下）；其余字符一律拒，把拼错的规格挡在读入处。
/// ⚠️ **订正 2026-10-05**：原文写字符集是 `[A-Za-z0-9_.]`（漏了 `-` / `+`），与实现不符。
bool symbol_ok(std::string_view s)
{
    if (s.empty()) return false;
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f) return false;  // 空白/控制字符：同上，为了"零空白"不变量
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-' || c == '+';
        if (!ok) return false;
        // 结构字符一律不许进符号名（逗号是花括号块里的分隔符、其余是索引分帧字符）
        if (c == constants::SO_SYMBOL_SEP || c == constants::SO_BRACE_OPEN ||
            c == constants::SO_BRACE_CLOSE || c == constants::COMMA_CHAR ||
            c == constants::PIPE_CHAR || c == constants::SEMICOLON_CHAR ||
            c == constants::COLON_CHAR)
            return false;
    }
    return true;
}

/**
 * 裸 SONAME 是否合法（严格）。拒两类字符：
 *
 * · **结构字符**（`@ { } , | ; :`）—— 前四个是这套语法的元字符；后三个是索引行/版本块的
 *   分帧字符（`|` 分行、`;` 分版本块、`:` 分字段）。`:` 尤其要紧：它已经在版本域被判过死刑
 *   （`is_safe_path_component`），这里对 SONAME 是同一个理由。
 * · **空白与控制字符**（`<= 0x20`、`0x7f`）—— 见头文件：这两个字段靠"不许空白"保住
 *   "索引行零空白"这个可执行不变量。
 *
 * ⚠️ 这条严格判据**只用于读入处**。判定路径走 `parse_so_spec` 的宽容分支（整串当裸名），
 * 所以历史数据里万一有带空格的怪 SONAME，也不会因为这条而从"整串相等"变成"谁都不匹配"。
 */
bool soname_ok(std::string_view s)
{
    if (s.empty()) return false;
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f) return false;
        if (c == constants::SO_SYMBOL_SEP || c == constants::SO_BRACE_OPEN ||
            c == constants::SO_BRACE_CLOSE || c == constants::COMMA_CHAR ||
            c == constants::PIPE_CHAR || c == constants::SEMICOLON_CHAR ||
            c == constants::COLON_CHAR)
            return false;
    }
    return true;
}

/// 严格解析：合法才写出参并返回 true（`wellformed` 与 `parse` 共用这一份判据）。
bool strict_parse(std::string_view raw, std::string& soname, std::vector<std::string>& symbols)
{
    const auto at = raw.find(constants::SO_SYMBOL_SEP);
    if (at == std::string_view::npos) {
        if (!soname_ok(raw)) return false;
        soname.assign(raw);
        symbols.clear();
        return true;
    }
    // `@` 至多一个：`X@A@B` 这种形态没法解释，直接判畸形（别猜哪个 `@` 是分隔符）。
    if (raw.find(constants::SO_SYMBOL_SEP, at + 1) != std::string_view::npos) return false;

    const std::string_view name = raw.substr(0, at);
    if (!soname_ok(name)) return false;

    const std::string_view ver = raw.substr(at + 1);
    if (ver.empty()) return false;  // `X@` —— 退化的"空版本"

    std::vector<std::string> found;
    if (ver.front() == constants::SO_BRACE_OPEN) {
        // 花括号块：`{V1,V2}`，必须在**末尾**（`back()` 即校验它）、不许嵌套、不许有空项。
        // ⚠️ 每一段**不 trim**：`{A, B}` 里的空格必须判畸形 —— 规格里出现空白就把"索引行
        // 零空白"这个不变量破了（见头文件）。这里的严格是**为了格式**，不是为了好看。
        if (ver.size() < 3 || ver.back() != constants::SO_BRACE_CLOSE) return false;
        const std::string_view inner = ver.substr(1, ver.size() - 2);
        if (inner.find(constants::SO_BRACE_OPEN) != std::string_view::npos ||
            inner.find(constants::SO_BRACE_CLOSE) != std::string_view::npos)
            return false;
        for (const auto piece : split_string_view(inner, constants::COMMA_CHAR)) {
            if (!symbol_ok(piece)) return false;
            found.emplace_back(piece);
        }
    } else {
        if (ver.find(constants::SO_BRACE_OPEN) != std::string_view::npos ||
            ver.find(constants::SO_BRACE_CLOSE) != std::string_view::npos)
            return false;  // 花括号出现在非块形态里（`X@A{B}`、`X@A}B`）
        if (!symbol_ok(ver)) return false;
        found.emplace_back(ver);
    }

    // **去重 + 排序**（规范化）：`X@{A,B}` 与 `X@{B,A}` 是同一个规格，必须归一成同一个串 ——
    // 否则归档写一种、索引写另一种就会被 `verify_package_metadata` 判成不一致；DB 键、
    // 反向依赖键同样会一分为二。判定语义本来就是集合语义，排序只是把它**变成字面相等**。
    symbols.clear();
    for (auto& s : found) {
        bool seen = false;
        for (const auto& prev : symbols) seen = seen || prev == s;
        if (!seen) symbols.push_back(std::move(s));
    }
    std::sort(symbols.begin(), symbols.end());
    soname.assign(name);
    return true;
}

}  // namespace

SoSpec parse_so_spec(std::string_view raw)
{
    SoSpec spec;
    if (strict_parse(raw, spec.soname, spec.symbols)) return spec;
    // 宽容分支：整个规格不合法 ⇒ 整串当**裸** SONAME（落回 8.0.0 之前"整串相等"的语义）。
    spec.soname.assign(raw);
    spec.symbols.clear();
    return spec;
}

std::string so_spec_key(std::string_view raw)
{
    return parse_so_spec(raw).soname;
}

std::string format_so_spec(const SoSpec& spec)
{
    if (spec.symbols.empty()) return spec.soname;
    std::string out = spec.soname;
    out.push_back(constants::SO_SYMBOL_SEP);
    if (spec.symbols.size() == 1) {
        out += spec.symbols.front();
        return out;
    }
    out.push_back(constants::SO_BRACE_OPEN);
    for (std::size_t i = 0; i < spec.symbols.size(); ++i) {
        if (i != 0) out.push_back(constants::COMMA_CHAR);
        out += spec.symbols[i];
    }
    out.push_back(constants::SO_BRACE_CLOSE);
    return out;
}

bool so_spec_wellformed(std::string_view raw)
{
    std::string name;
    std::vector<std::string> symbols;
    return strict_parse(raw, name, symbols);
}

std::vector<std::string> split_so_list(std::string_view field)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    int depth = 0;  // 只在 `{`…`}` 内 > 0（不许嵌套；这里用深度计数只是为了让"多一个 `}`"
                    // 这类畸形输入退化得更温和）
    for (std::size_t i = 0; i < field.size(); ++i) {
        const char c = field[i];
        if (c == constants::SO_BRACE_OPEN) {
            ++depth;
        } else if (c == constants::SO_BRACE_CLOSE) {
            if (depth > 0) --depth;
        } else if (c == constants::COMMA_CHAR && depth == 0) {
            std::string piece = trim_copy(field.substr(start, i - start));
            if (!piece.empty()) out.push_back(std::move(piece));
            start = i + 1;
        }
    }
    std::string tail = trim_copy(field.substr(start));
    if (!tail.empty()) out.push_back(std::move(tail));
    return out;
}

bool so_spec_satisfies(std::string_view provided, std::string_view needed)
{
    const SoSpec p = parse_so_spec(provided);
    const SoSpec n = parse_so_spec(needed);
    if (p.soname != n.soname) return false;
    if (n.bare()) return true;   // need 不带符号版本：库在就算满足
    if (p.bare()) return false;  // 保守：provider 没声明 ⇒ 拿不出任何符号版本
    for (const auto& want : n.symbols) {
        bool found = false;
        for (const auto& have : p.symbols) found = found || have == want;
        if (!found) return false;
    }
    return true;
}

std::vector<std::string> expand_so_spec(std::string_view raw, bool providing)
{
    const SoSpec spec = parse_so_spec(raw);
    std::vector<std::string> out;
    if (spec.symbols.empty()) {
        if (!spec.soname.empty()) out.push_back(spec.soname);
        return out;
    }
    if (providing) out.push_back(spec.soname);  // 裸条目必须有：否则裸 need 会被版本化声明打破
    for (const auto& v : spec.symbols) {
        out.push_back(spec.soname + std::string(1, constants::SO_SYMBOL_SEP) + v);
    }
    return out;
}
