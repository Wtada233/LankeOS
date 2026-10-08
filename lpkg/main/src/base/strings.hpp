#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "constants.hpp"

/**
 * 去除首尾空白（空格与制表符）。依赖串/索引字段/配置值都要按同一套规则归一，
 * 否则 " glibc"、"glibc " 会被当成两个不同的包名去找。
 */
std::string trim_copy(std::string_view s);

// ============ 包路径 / 备份路径工具 ============

/** 生成随机小写字母+数字后缀（用于 .lpkg_bak / stash 文件名防冲突） */
std::string random_suffix(size_t len = constants::RANDOM_SUFFIX_LEN);

// ============ 字符串工具 ============

/** 替换字符串中的所有匹配子串 */
void string_replace_all(std::string& str, const std::string& from, const std::string& to);

/**
 * 用单引号把一段文本包成安全的 shell 参数；文本内的单引号按 POSIX 方式转义为 `'\''`。
 * 构建阶段命令行与 hook 执行路径共用它（此前两处各写一份）。
 */
std::string shell_quote(std::string_view s);

// ============ base64（WAL 行里的键与值）============
//
// 用途**只有一个**：把 xattr 的**键与值**编进 WAL 行。它们都是任意字节串，而 WAL 是
// **行式、空格分帧、`" → "` 是箭头分界**的文本协议（见 wal_op.cpp 的 parse_op）——
// 裸放一个含 ` → `（或含换行、含尾部空格）的键，就会把一条行**重新分帧**成另一条合法行，
// 回滚侧照着重构出来的路径去 chmod/chown/lsetxattr。这与归档成员名那套消毒是**同一类**
// 问题（`archive.cpp` 的 reject 逻辑），但那里能拒绝整包，这里拒绝不了（键是模板/上游给的），
// 所以选择**编码**而不是拒绝。
//
// base64 的字母表（`A-Za-z0-9+/=`）不含空格、不含换行、不含 `→` —— 编码之后这些字符在
// 协议层不可能出现。**空值**编码成空串，故调用方要用一个哨兵把"空值"与"没有这一侧"
// 区分开（见 wal_op.cpp 的 XATTR_* 行）。

/** 标准 base64（带 `=` 填充；空输入 → 空串） */
std::string base64_encode(const std::vector<char>& data);

/** 解码；输入含非 base64 字符 / 长度非法 → nullopt（**不抛**：这是回滚路径上的输入） */
std::optional<std::vector<char>> base64_decode(std::string_view s);

/**
 * 按分隔符切分 string_view，返回子串列表（零拷贝，仅分配 vector）
 *
 * **契约（勿改行为）**：切分**总会产出尾段**，并且**保留空段**：
 *   `""`    → `[""]`
 *   `"a,"`  → `["a", ""]`
 *   `"a,,b"`→ `["a", "", "b"]`
 * 也就是说，产出里**包含空串**是**正常结果**，不是错误。调用方**必须自行跳过空段**
 * （`if (x.empty()) continue;`）—— 否则空串会被当成一个真实的 token（例如被收进 libsolv
 * 的 pool 成为 `STRID_EMPTY`，或变成一个空名依赖），后果参见 repository.cpp 的
 * `split_dep_field` / `split_comma_list` 的说明。**不要**为了"省掉一次 `if`"而修改本函数
 * 去吞掉空段：那会波及全部调用点，且会改变 `"a,"` 这类输入对"尾段存在性"的语义。
 *
 * @return 切分后的子串列表（含空段，含尾段）
 */
inline std::vector<std::string_view> split_string_view(std::string_view s, char d)
{
    std::vector<std::string_view> r;
    size_t start = 0, end;
    while ((end = s.find(d, start)) != std::string_view::npos) {
        r.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    r.push_back(s.substr(start));
    return r;
}
