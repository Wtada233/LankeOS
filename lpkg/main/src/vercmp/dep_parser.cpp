#include "dep_parser.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "base/constants.hpp"
#include "base/utils.hpp"

namespace detail
{

namespace
{
/**
 * 版本约束的运算符表 —— **本文件唯一一份**，`parse_dep_strings` 的三处循环与
 * `dependency_name_of` 共用。顺序 = 匹配优先级（同一位置上 `>=` 必须胜过 `>`）。
 */
constexpr std::array<std::string_view, 7> DEP_OPS = {">=", "<=", "!=", "==", ">", "<", "="};
}  // namespace

std::string dependency_name_of(std::string_view line)
{
    // `trim_copy` **只去空格与制表符**（见 base/utils.cpp），不碰 `\r` ——
    // 而 CRLF 的 `deps/` 文件读出来每行都带 `\r`，不剥就会进包名（`provb\r` 这种键
    // 谁都查不到）。原先只有 `ensure_reverse_deps` 那一处记得剥，其余三处都漏。
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    std::string d = trim_copy(
        line);  // 非 const：下面的 `return d;` 才能移动（`-Wno-automatic-move` 拦的是 const）
    if (d.empty()) return {};
    // 与 parse_dep_strings 同一判据：**最早出现**的合法运算符之前就是包名。
    // 逐位置扫描（而不是逐个找运算符）是为了保证"物理位置最早"优先 —— 见
    // tests/unit/test_version.cpp 的 DepParser.OperatorAppearanceOrder。
    for (std::size_t pos = 0; pos < d.size(); ++pos) {
        for (const auto& op : DEP_OPS) {
            if (d.compare(pos, op.size(), op) != 0) continue;
            std::string name = d.substr(0, pos);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            return name;
        }
    }
    return d;  // 没有运算符 ⇒ 整串就是包名
}

/**
 * 解析依赖字符串列表为 DependencyInfo 结构体，支持复合约束
 */
std::vector<DependencyInfo> parse_dep_strings(const std::vector<std::string>& dep_strs)
{
    std::vector<DependencyInfo> deps;
    constexpr auto& ops = DEP_OPS;
    for (const auto& raw : dep_strs) {
        // 统一 trim：手写 build_deps 里的 " cmake" / "cmake " 必须归一到同一个包名，
        // 否则会去找名叫 " cmake" 的包（仓库里当然没有）。
        const std::string d = trim_copy(raw);
        // 空片段（索引里 "a,,b" 拆出的空元素、纯空白）不成依赖。**空名依赖必须在
        // 这里就被挡掉**：它在 libsolv 里是 ID_EMPTY，既不解析也不报错，求解会"成功"
        // 却产出空事务 → 上层打印"所有包都已安装"并 exit 0（历史 TODO.md D1/D3）。
        if (d.empty()) continue;

        DependencyInfo dep;
        // 包名 = 最早出现的合法运算符之前那段 —— **调 `dependency_name_of`，不在这里重写
        // 一遍**（那段循环曾是第二份实现；"取名字"与"解析约束"必须用同一套判据，
        // 否则同一行在不同路径上会算出不同的键，见 dep_parser.hpp 的说明）。
        dep.name = dependency_name_of(d);

        // 名字之后还有内容 ⇒ 那部分就是约束序列（可能先有几个空格，下面的循环会跳过）
        if (dep.name.size() < d.size()) {
            // 解析后续所有 (op, version) 对
            std::string remaining = d.substr(dep.name.size());
            size_t pos = 0;
            while (pos < remaining.size()) {
                // 跳过约束之间的分隔（空格与 ','）："cmake >= 3.20, < 4.0" 里的逗号
                // 属于复合约束语法，不能被当成版本号的一部分（否则 version 解析成
                // "3.20,"，版本比较必然失败 → 依赖被误判为不满足）
                while (pos < remaining.size() && (remaining[pos] == ' ' || remaining[pos] == ','))
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
        }
        // 无运算符时 `dependency_name_of` 返回整串，这里**不需要** else 分支
        // （原先那句 `dep.name = d;` 是它那份重复实现的一部分）。

        // 只由操作符构成的片段（如复合约束被拆开后剩下的 "< 4.0"）不产生依赖项
        if (dep.name.empty()) continue;
        deps.push_back(std::move(dep));
    }
    return deps;
}

}  // namespace detail
