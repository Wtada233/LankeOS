#include "version.hpp"

#include <solv/evr.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

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
    std::string s = normalize(v);
    // `+N` 是发行修订号。libsolv 的 pool_evrcmp 按**最后一个 `-`** 切 version/release，
    // 把 `+` 转成 `-` 让 release 被正确识别——否则整串平铺比较会把 `261.2+3` 判成
    // `< 261+3`，依赖要求新版本时 libsolv 误判为"降级"、只能升级满足依赖 → 事务无解。
    std::replace(s.begin(), s.end(), '+', '-');
    return s;
}

std::string from_libsolv_evr(const std::string& v)
{
    // EVR 里 `~` = 预发布（原 `-`）；唯一的 `-` 是 release 分隔符（原 `+`）。
    // 按最后一个 `-` 切分：其前 `~`→`-` 还原版本，其后用 `+` 接回 release。
    const auto pos = v.rfind('-');
    if (pos == std::string::npos) {
        std::string s = v;
        std::replace(s.begin(), s.end(), '~', '-');
        return s;
    }
    std::string ver = v.substr(0, pos);
    std::replace(ver.begin(), ver.end(), '~', '-');
    return ver + "+" + v.substr(pos + 1);
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
