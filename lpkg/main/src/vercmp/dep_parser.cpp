#include "dep_parser.hpp"

#include <string>
#include <vector>

#include "base/constants.hpp"
#include "base/utils.hpp"

namespace detail
{

/**
 * 解析依赖字符串列表为 DependencyInfo 结构体，支持复合约束
 */
std::vector<DependencyInfo> parse_dep_strings(const std::vector<std::string>& dep_strs)
{
    std::vector<DependencyInfo> deps;
    static const std::vector<std::string> ops = {">=", "<=", "!=", "==", ">", "<", "="};
    for (const auto& raw : dep_strs) {
        // 统一 trim：手写 build_deps 里的 " cmake" / "cmake " 必须归一到同一个包名，
        // 否则会去找名叫 " cmake" 的包（仓库里当然没有）。
        const std::string d = trim_copy(raw);
        // 空片段（索引里 "a,,b" 拆出的空元素、纯空白）不成依赖。**空名依赖必须在
        // 这里就被挡掉**：它在 libsolv 里是 ID_EMPTY，既不解析也不报错，求解会"成功"
        // 却产出空事务 → 上层打印"所有包都已安装"并 exit 0（TODO.md D1/D3）。
        if (d.empty()) continue;

        DependencyInfo dep;
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
                // 跳过约束之间的分隔（空格与 ','）："cmake >= 3.20, < 4.0" 里的逗号
                // 属于复合约束语法，不能被当成版本号的一部分（否则 version 解析成
                // "3.20,"，版本比较必然失败 → 依赖被误判为不满足）
                while (pos < remaining.size() &&
                       (remaining[pos] == ' ' || remaining[pos] == ','))
                    ++pos;
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
                    if (!hit && remaining[p] == ',') {
                        ver_end = p;  // 复合约束的 ',' 分隔符：版本到此为止
                        hit = true;
                    }
                    if (hit) break;
                }

                std::string ver_str = remaining.substr(pos, ver_end - pos);
                while (!ver_str.empty() && ver_str.back() == ' ') ver_str.pop_back();

                dep.constraints.push_back({cur_op, ver_str});
                pos = ver_end;
            }
        } else {
            dep.name = d;
        }

        // 只由操作符构成的片段（如复合约束被拆开后剩下的 "< 4.0"）不产生依赖项
        if (dep.name.empty()) continue;
        deps.push_back(std::move(dep));
    }
    return deps;
}

}  // namespace detail
