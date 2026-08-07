#include "version.hpp"

#include <solv/evr.h>

#include <algorithm>
#include <string>
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

/**
 * libsolv EVRCMP（rpm 语义），返回 <0 / 0 / >0。
 * 两端都先归一化：lpkg 格式里 `-` 只出现在预发布位置，替换成 `~` 后 rpm 排序即与 lpkg 语义一致。
 */
int evr_cmp(const std::string& a, const std::string& b)
{
    const std::string na = normalize(a);
    const std::string nb = normalize(b);
    return solv_vercmp(na.c_str(), na.c_str() + na.size(), nb.c_str(), nb.c_str() + nb.size());
}

}  // namespace

std::string to_libsolv_evr(const std::string& v)
{
    return normalize(v);
}

std::string from_libsolv_evr(const std::string& v)
{
    std::string s = v;
    std::replace(s.begin(), s.end(), '~', '-');
    return s;
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
