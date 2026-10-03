#include "version.hpp"

#include <solv/evr.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../base/constants.hpp"
#include "../base/exception.hpp"
#include "../i18n/localization.hpp"

namespace
{

/** 归一化：lpkg `-预发布` → rpm `~`（rpm 里 `~` 是"预发布、小于基础版"）。 */
std::string normalize(const std::string& v)
{
    std::string s = v;
    std::replace(s.begin(), s.end(), '-', '~');
    return s;
}

/** 拆发行修订号：lpkg 版本格式为 `version[+release]`（如 `261.2+3`）。`+` 是版本与
 *  release 的分隔符，版本本身不含 `+`。 */
std::pair<std::string, std::string> split_release(const std::string& v)
{
    const auto pos = v.find('+');
    if (pos == std::string::npos) return {v, ""};
    return {v.substr(0, pos), v.substr(pos + 1)};
}

/**
 * libsolv EVRCMP（rpm 语义），返回 <0 / 0 / >0。
 * 两端都先归一化：lpkg 格式里 `-` 只出现在预发布位置，替换成 `~` 后 rpm 排序即与 lpkg 语义一致。
 *
 * `+N` 是发行修订号，必须与版本分离后再比：rpm 段比较会把 `261.2+3` 压平成
 * [261,2,3]、`261+3` 压平成 [261,3]，第二段 `2` 与 `3` 竞争导致
 * `261.2+3 < 261+3`（错——261.2 是版本升级，release 应排在其后）。故先比版本、
 * 版本相同再比 release；无 release 视为小于有 release。
 */
int evr_cmp(const std::string& a, const std::string& b)
{
    const std::string na = normalize(a);
    const std::string nb = normalize(b);
    const auto [va, ra] = split_release(na);
    const auto [vb, rb] = split_release(nb);

    const int vcmp =
        solv_vercmp(va.c_str(), va.c_str() + va.size(), vb.c_str(), vb.c_str() + vb.size());
    if (vcmp != 0) return vcmp;

    if (ra.empty() && rb.empty()) return 0;
    if (ra.empty()) return -1;
    if (rb.empty()) return 1;
    return solv_vercmp(ra.c_str(), ra.c_str() + ra.size(), rb.c_str(), rb.c_str() + rb.size());
}

}  // namespace

std::string to_libsolv_evr(const std::string& v)
{
    if (const auto bad = v.find_first_of(constants::EVR_RESERVED_CHARS); bad != std::string::npos) {
        // 见头文件：这三个字符对 libsolv 有特殊含义，是桥接的保留字符。**绝不静默** ——
        // 让它们混进去的后果是依赖匹配悄悄错序/编解码不再一一对应，比一次显式失败糟得多。
        throw LpkgException(
            string_format("error.version_reserved_char", v, std::string(1, v[bad])));
    }
    std::string s =
        normalize(v);  // 非 const：下面 `return s` 要能自动 move（tidy: no-automatic-move）
    const auto pos = s.find('+');
    if (pos == std::string::npos) return s;  // 无 release：版本部分本身就是完整 EVR
    // 退化写法 `1.0+`（`+` 后为空）：**与"没有 release"同义** —— `evr_cmp` 就是这么判的
    // （`split_release` 给出空 release ⇒ `1.0+ == 1.0`）。不特判会编出 `1.0^^`，而它在
    // libsolv 里**大于** `1.0` ⇒ 桥接在这个边界上不再保序（实测 6 对退化串分叉）。
    // 真实版本不以 `+` 结尾（索引 678 个里 0 个），但把边界钉平比留个静默分叉便宜。
    if (pos + 1 == s.size()) return s.substr(0, pos);
    // `^` 的位置承重（见头文件）：libsolv 的 rpm 比较器把 `^` 当"比基础版新、比任何真实
    // 下一段旧"，正是发行修订号的语义；而且全串不出现 `-` ⇒ libsolv 的 version/release
    // 切分永远是"没有 release"，EVRCMP_MATCH_RELEASE 那两个 ±2 特例分支不可能触发。
    return s.substr(0, pos) + std::string(constants::EVR_RELEASE_SEP) + s.substr(pos + 1);
}

std::string from_libsolv_evr(const std::string& v)
{
    // `~` = 预发布（原 `-`）；**唯一的** `^^` 是 release 分隔符（原 `+`）。
    // 切分是**无损**的：版本部分里不允许出现 `^`（`to_libsolv_evr` 会拒绝），所以第一个
    // `^^` 一定是分隔符 —— 不再有"按最后一个 `-` 猜"那种有损还原（旧实现在版本/release
    // 里含 `~` 时会还原错）。
    const auto unnormalize = [](std::string_view s) {
        std::string out(s);
        std::replace(out.begin(), out.end(), '~', '-');
        return out;
    };
    const auto pos = v.find(constants::EVR_RELEASE_SEP);
    if (pos == std::string::npos) return unnormalize(v);
    return unnormalize(std::string_view(v).substr(0, pos)) + "+" +
           unnormalize(std::string_view(v).substr(pos + constants::EVR_RELEASE_SEP.size()));
}

bool version_compare(const std::string& v1_str, const std::string& v2_str)
{
    return evr_cmp(v1_str, v2_str) < 0;
}

bool version_satisfies(const std::string& current_version, const std::string& op,
                       const std::string& required_version)
{
    const int c = evr_cmp(current_version, required_version);
    if (op == "=" || op == "==") return c == 0;
    if (op == "!=") return c != 0;
    if (op == "<") return c < 0;
    if (op == "<=") return c <= 0;
    if (op == ">") return c > 0;
    if (op == ">=") return c >= 0;
    throw LpkgException(string_format("error.invalid_version_format", op));
}

bool version_satisfies_all(const std::string& current_version,
                           const std::vector<Constraint>& constraints)
{
    for (const auto& c : constraints)
        if (!version_satisfies(current_version, c.op, c.version)) return false;
    return true;
}
