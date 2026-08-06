#include "dep_parser.hpp"

#include <string>
#include <vector>

#include "base/constants.hpp"

namespace detail
{

/**
 * 解析依赖字符串列表为 DependencyInfo 结构体，支持复合约束
 */
std::vector<DependencyInfo> parse_dep_strings(const std::vector<std::string>& dep_strs)
{
    std::vector<DependencyInfo> deps;
    static const std::vector<std::string> ops = {">=", "<=", "!=", "==", ">", "<", "="};
    for (const auto& d_str : dep_strs) {
        DependencyInfo dep;
        const std::string& d = d_str;

        // 找到在字符串中最早出现的合法操作符，分割包名和约束序列
        size_t op_pos = std::string::npos;
        for (size_t pos = 0; pos < d.size(); ++pos) {
            for (const auto& op : ops) {
                if (d.compare(pos, op.size(), op) == 0) {
                    op_pos = pos;
                    break;
                }
            }
            if (op_pos != std::string::npos) break;
        }

        if (op_pos != std::string::npos) {
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
                    if (remaining.compare(pos, o.size(), o) == 0) {
                        cur_op = o;
                        pos += o.size();
                        break;
                    }
                }
                if (cur_op.empty()) break;

                while (pos < remaining.size() && remaining[pos] == ' ') ++pos;

                size_t ver_end = remaining.size();
                for (size_t p = pos; p < remaining.size(); ++p) {
                    bool hit = false;
                    for (const auto& o : ops) {
                        if (remaining.compare(p, o.size(), o) == 0) {
                            ver_end = p;
                            hit = true;
                            break;
                        }
                    }
                    if (hit) break;
                }

                std::string ver_str = remaining.substr(pos, ver_end - pos);
                while (!ver_str.empty() && ver_str.back() == ' ') ver_str.pop_back();

                dep.constraints.push_back({cur_op, ver_str});
                pos = ver_end;
            }
        } else {
            dep.name = d_str;
        }
        deps.push_back(std::move(dep));
    }
    return deps;
}

}  // namespace detail
