#include "dep_parser.hpp"
#include "base/constants.hpp"
#include <string>
#include <vector>

namespace detail {

/**
 * 解析依赖字符串列表为 DependencyInfo 结构体，支持复合约束
 */
std::vector<DependencyInfo> parse_dep_strings(const std::vector<std::string>& dep_strs) {
    std::vector<DependencyInfo> deps;
    static const std::vector<std::string> ops = {">=", "<=", "!=", "==", ">", "<", "="};
    for (const auto& d_str : dep_strs) {
        DependencyInfo dep;
        const std::string& d = d_str;

        // 找到第一个操作符，分割包名和约束序列
        size_t op_pos = std::string::npos;
        for (const auto& op : ops) {
            if ((op_pos = d.find(op)) != std::string::npos) {
                std::string name = d.substr(0, op_pos);
                while (!name.empty() && name.back() == ' ') name.pop_back();
                dep.name = name;

                // 解析后续所有 (op, version) 对
                std::string remaining = d.substr(op_pos);
                size_t pos = 0;
                while (pos < remaining.size()) {
                    while (pos < remaining.size() && remaining[pos] == ' ') ++pos;
                    if (pos >= remaining.size()) break;

                    std::string cur_op;
                    for (const auto& o : ops) {
                        if (remaining.substr(pos, o.size()) == o) {
                            cur_op = o;
                            pos += o.size();
                            break;
                        }
                    }
                    if (cur_op.empty()) break;

                    while (pos < remaining.size() && remaining[pos] == ' ') ++pos;

                    size_t ver_end = remaining.size();
                    for (const auto& o : ops) {
                        size_t np = remaining.find(o, pos);
                        if (np < ver_end) ver_end = np;
                    }

                    std::string ver_str = remaining.substr(pos, ver_end - pos);
                    while (!ver_str.empty() && ver_str.back() == ' ') ver_str.pop_back();

                    dep.constraints.push_back({cur_op, ver_str});
                    pos = ver_end;
                }
                break;
            }
        }
        if (op_pos == std::string::npos) {
            dep.name = d_str;
        }
        deps.push_back(std::move(dep));
    }
    return deps;
}

} // namespace detail
