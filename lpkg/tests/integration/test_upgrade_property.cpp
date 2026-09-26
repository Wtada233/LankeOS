/**
 * test_upgrade_property.cpp — 升级路径的**随机化属性测试**（重构验收闸门）
 *
 * ── 这是什么 ────────────────────────────────────────────────────────────────
 * 同一个包的两个版本之间，**同一个逻辑路径**可能变类型（文件 / 目录 / 符号链接）、
 * 可能被新版丢弃（废弃）、也可能被别的包共享。今天这些组合的处理散在三趟代码里
 * （② `backup_existing_files` 让开挡路物 / ③ `copy_package_files` 写新文件 /
 *   ⑤ `commit_without_file_ops` 清废弃），**两趟之间靠注释维持一致** —— 已经发生过
 * "第②步以为第⑤步会清、第⑤步以为轮不到自己"的漏格（DirToFile / DirToSymlink，
 * 见 test_type_transition_matrix.cpp）。该漏格已修：归档非目录撞盘上真目录时，只要
 * `dir_tree_entirely_ours`（pacman 的 `dir_belongsto_pkgs`）判"整棵树都归本包/本批次升级的
 * 包"，`backup_existing_files` 就整树搬进 stash 让路、随后写文件/链接，可回滚。
 *
 * test_type_transition_matrix.cpp 把 9 格**手写枚举**（每格一份断言）；本文件走另一条路：
 * **固定种子 → 随机造一套 v1/v2 目录树 → 对每个种子跑两条属性**，把"漏格"从"人想不想得到"
 * 变成"生成器能不能造出来"。它是后续"升级路径重构"的验收闸门。
 *
 * ── 两条属性（每个种子都跑）────────────────────────────────────────────────
 *   属性 A（干净升级）：装 v1 → 断言盘面 == v1 形态 → 装 v2 → 断言**成功**且盘面 == v2 形态。
 *   属性 B（注入失败后回滚）：装 v1 → 随机造"用户改动" → 注入中途失败 → 装 v2 → 断言
 *     抛异常 **且** 盘面**逐项回到改动后的 v1 形态**（类型/内容/链接目标全比）+ DB 版本仍是 v1。
 *
 * ── 确定性（不是"随机跑，红了算运气"）──────────────────────────────────────
 * 种子来自**固定列表**（0..N-1，N 默认 200），每个种子一棵独立的树；随机数只有
 * `std::mt19937_64`（种子由 seed 派生），**没有 `std::random_device`**、不看时间、不看环境。
 * 失败信息里带种子号，能单独复现：
 *   · `LPKG_PROP_SEED=<n>`  → 全量用例只跑这一个种子（用于最小化复现）；
 *                              另有 SingleSeedReplay 用例把该种子的两棵模型树全打出来
 *   · `LPKG_PROP_SEEDS=<n>` → 只跑前 n 个种子（调试/计时用）
 * 两个变量被用到时会在 stderr 打一行醒目提示（免得"少跑了"被当成"全过了"）。
 *
 * ── 生成器：为什么这么造 ────────────────────────────────────────────────────
 * 路径集合随机，但**每格形态**从 {普通文件（带内容）, 目录（含 1-3 个内部条目，可再嵌一层）,
 * 符号链接（指向随机目标）} 里挑；v1/v2 的路径集合**大部分重叠**（v2 由 v1 的路径集合派生，
 * 逐个挑新形态），另掺少量"仅 v1 有"（废弃）与"仅 v2 有"（新增）—— 类型变更因此是常态而
 * 不是边角。每个树**恒有一个普通文件 `companion.txt`**（v1/v2 都有）：断点
 * `copy_after_wal_<pkg>` 只挂在普通文件的 COPY 分支上，没有它就可能"注入没生效、断言恒真"
 * （见下面「断点注入不许空转」）。
 *
 * ── 断点注入不许空转（本文件最容易变成假绿的地方）────────────────────────────
 * 属性 B 分三类结果，**只有第一类算通过**：
 *   ① `bp_hit == true` → 注入真的生效，回滚保真被考到了 → 断言盘面逐项回到改动后的 v1；
 *   ② `bp_hit == false` 且批次在预检就被拒 → **注入未生效**（拷贝阶段压根没进去）：
 *      单独计数、**不计入通过**；要求"盘面一个字节都没动"。生成器已不再制造"正确拒绝"的
 *      来源（见下），故**两个变体都要求此数恒为 0**；不为 0 = 批次在拷贝阶段之前被**别的原因**
 *      拒了，那是新问题，不能靠"计入 not_exercised"就不了了之。
 *   ③ `bp_hit == false` 而 v2 竟然装成功 → 断点没接上，直接判失败。
 *
 * ── "用户改动"不许制造出**正确拒绝**（2026-09-25）──────────────────────────────
 * 用户改动里"往目录里加一个文件"那一支，只有在**该目录不参与 dir→非目录 转换**时才用：
 * 目录被 v2 换成文件/链接时，往里塞一个**无人持有**的文件会让整批被 `dir_tree_entirely_ours`
 * 判否而**正确拒绝**（pacman 语义：整树搬走会毁掉那个文件）—— 那种种子考不到"接管 + 回滚"，
 * 只会让属性 B 的注入空转。故那里改成改写**我们自己**某个子条目的内容（归属不变 ⇒ 整树仍是
 * 我们的 ⇒ 接管照常放行，而"用户改过的那份"照样是回滚保真的锚点）。
 * "目录含无主内容 + 该目录要被非目录替换 ⇒ 必须拒绝且盘面一字未动"由**固定用例**
 * `DirReplacedByNonDirWithUnownedContentIsRefused` 单独钉（随机组不再覆盖它，避免把
 * "正确拒绝"与"注入未生效"混在一起）。
 *
 * ── 两个变体（两轴：属性 × 是否允许 dir→非目录）──────────────────────────────
 *   CleanUpgrade_AllTransitions   / RollbackFidelity_AllTransitions
 *      完整随机（含 dir→文件 / dir→符号链接）。这些迁移现在**走接管**（整树归本包即放行）
 *      ⇒ **应当是绿的**；红了 = 接管路径本身有漏。
 *   CleanUpgrade_NoDirReplaced    / RollbackFidelity_NoDirReplaced
 *      生成器禁止 dir→非目录（v1 是目录则 v2 也必须是目录）。绕开该迁移后只剩类型变更、
 *      废弃、新增、共享目录、回滚保真 —— 它红了 = 那些通用路径上有新问题。
 *
 * ── 两片树：`usr/share/prop/<ns><tag><seed>/` 与 **`/etc`** ──────────────────────
 * 一般形态的路径基准是 `usr/share/prop/...`（`m.base`）；**`/etc` 是本轮的第二个生成维度**
 * （`m.etc_base = etc/prop/<ns><tag><seed>`，见下面「/etc 维度」）。两片树各自有模型与断言，
 * **不共用** `/etc` 与其余路径的判据 —— 因为 `/etc` 的**终态不等于包内形态**（用户改过的
 * 配置留在原位、`.lpkgnew` 等审阅、`.lpkgsave` 整树保留、废弃配置永不物理删除），
 * 把它的条目混进 `m.v1/m.v2` 会让"盘面 == 新版本形态"这条一般属性在 `/etc` 上恒假
 * （`ARCH.md` §5.4 不变量 2 的 `/etc` 例外）。
 *
 * ── `/etc` 维度：生成什么、断言什么 ──────────────────────────────────────────
 * **生成**（`gen_etc_model`，与 usr 侧同一套 `gen_subtree`/`next_kind` 判据）：
 *   · 条目形态：普通文件 / 目录 / 符号链接；
 *   · v1 → v2 迁移：同类型、类型互转（含 **dir → 非目录**、**file → 目录**）、废弃、新增；
 *   · 三哈希三条分支的原料**尽量结构性保证**（不靠种子碰运气）：恒一格 `file→file` 且内容
 *     **不同**（`companion.conf`，与种子无关）；另**显式钉一格** `file→file` 且内容**逐字相同**
 *     （"包没改这个配置"）—— 它只在该种子确实有 `file → file` 槽位时存在（生成器不凭空造格）；
 *   · 用户改动（属性 B）：`plan_etc_user_edits` 对上面两格**必改**（+ 随机改其余格子）——
 *     于是 `SaveLpkgnew` 的原料每个种子都有，`KeepLocal` 的原料在"有那格"的种子上有；
 *     **覆盖非空**由文件末尾的 `EXPECT_GT(...)` 与 `run_group` 的形状兜底保证（加种子不是
 *     正道，见上一条结论）；
 *   · 至少覆盖"**盘上是真目录**、新版本是文件/符号链接"（随机 + `run_group` 的覆盖性兜底）
 *     —— 那正是本仓库最近最大的缺陷形状（`/etc` 的 dir → 非目录 两条腿都装不上）。
 * **断言**（`/etc` 的例外写进**模型**，不在断言处打补丁）：
 *   · 终态 == `model_etc_after()` 算出来的期望（逐路径逐形态逐字节），包括
 *     `<路径>.lpkgnew` / `<路径>.lpkgsave` / 废弃配置留在原位；
 *   · **安全不变量**（都能从代码与 `ARCH.md` §5.4 推出，且是 `/etc` 存在的意义）：
 *     a. **用户改过的配置永不静默丢失** —— 原位 / `.lpkgsave` / `.lpkgnew` 三处之一必能
 *        逐字节找回（`etc_node_findable`，独立于模型的第二道网）；
 *     b. 盘上那份被保留时，**记录里记的仍是 `hash_pkg`（包内内容）**，绝不追认盘上那份
 *        （`etc_conf_hash_problems`，正反两侧都查）；
 *     c. 回滚保真照旧成立（注入失败 → `/etc` 子树逐字节回到批次前）；
 *     d. 安装/升级**必须成功**（不允许"这条路径永远装不上"这类静默失败）。
 *   `/etc` 的**固定组合**另有 `EtcPrefixCombinations_Recorded`（文件末尾）**记录** 9 格的
 *   盘面，与本维度的随机抽样互补：前者读得出单格语义，后者抓得到没想到的组合。
 *
 * ── 与 test_type_transition_matrix.cpp 的分工 ───────────────────────────────
 * 那个文件是 9 格的**手工样本**（每格的语义、断点是否可达都写死在注释里，读得懂）；
 * 本文件是同一片语义的**广谱抽样**（种子 × 形态组合），读不出单格语义，但能自动撞见
 * "手工枚举没想到的组合"。两者都要留着：前者定位，后者抓漏。
 */

#include <gtest/gtest.h>
#include <sys/xattr.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/crypto/hash.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

namespace
{

/// `/etc` 的两种落点后缀。取真常量（而非就地写字符串），免得实现改了后缀、测试还在自我印证。
const std::string K_LPKGNEW{constants::SUFFIX_LPKG_NEW};
const std::string K_LPKGSAVE{constants::SUFFIX_LPKG_SAVE};

// ============================================================================
// 形态模型与摘要
// ============================================================================

/// 一格形态：普通文件 / 目录 / 符号链接。payload：文件内容 或 链接目标（目录为空）
struct Node {
    std::string kind;  // "file" | "dir" | "symlink"
    std::string payload;
};

/// 扁平条目表：key = 相对 seed 根的路径（如 "S3/c0"）。std::map 有序 ⇒ 父目录恒在子条目前
using Tree = std::map<std::string, Node>;

/// 摘要里把换行转义（摘要行是"一行一条记录"，用户改动写进去的内容带 \n）
std::string escape_payload(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else
            out += c;
    }
    return out;
}

// ============================================================================
// xattr 维度的模型（本轮新增；见文件头「xattr 维度」）
//
// 为什么**不复用** `Tree`/`Node`（把 xattr 塞进 Node）：
//   · `Node` 被摘要（`summary_line`）、差异（`diff_summaries`）、模型（`model_etc_after`）、
//     用户改动全都用着，"每个 Node 多带一个 map"会让**所有**既有比对口径跟着变
//     （既要处理"xattr 不同但形态相同"的差异渲染，又要防止既有断言被 xattr 噪声带红）。
//   · 独立一张 `XMap`（路径 → 键 → 值）能把新维度**加在既有断言旁边**，不动既有口径 ——
//     这也正是 `/etc` 维度当初独立成一片树的同一个理由。
// ============================================================================

/// 路径 → 键 → 值。路径与 `Tree` **同一口径**（相对 test_root，目录不带尾斜杠）。
using XMap = std::map<std::string, std::map<std::string, std::string>>;

/** 给一个目录设一个 xattr（测试侧写盘用）。用 `ADD_FAILURE` 而非 `ASSERT_*`：本辅助要被
 *  返回值非 void 的函数（`pack_tree`）调用，`ASSERT_*` 在那里编不过。 */
void set_xattr_or_fail(const fs::path& p, const std::string& key, const std::string& val)
{
    if (::lsetxattr(p.c_str(), key.c_str(), val.data(), val.size(), 0) != 0)
        ADD_FAILURE() << "测试脚手架：lsetxattr(" << p << ", " << key << ") 失败："
                      << std::strerror(errno);
}

/** 读一个路径上的全部 xattr。判据是 **lstat 语义**（不跟随末段链接，与实现同口径）。 */
std::map<std::string, std::string> read_xattrs(const fs::path& p)
{
    std::map<std::string, std::string> out;
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(p, ec))) return out;
    ssize_t len = ::llistxattr(p.c_str(), nullptr, 0);
    if (len <= 0) return out;
    std::vector<char> names(static_cast<size_t>(len));
    len = ::llistxattr(p.c_str(), names.data(), names.size());
    if (len <= 0) return out;
    for (const char* n = names.data(); n < names.data() + len; n += std::strlen(n) + 1) {
        if (*n == '\0') continue;
        const ssize_t vlen = ::lgetxattr(p.c_str(), n, nullptr, 0);
        if (vlen < 0) continue;
        std::string val(static_cast<size_t>(vlen), '\0');
        if (::lgetxattr(p.c_str(), n, val.data(), val.size()) != vlen) continue;
        out[n] = val;
    }
    return out;
}

/**
 * 扫盘上**目录**的 xattr（键 = `key_prefix + "/" + 相对 root 的路径`，与 `Tree` 同口径）。
 *
 * 只收目录 —— xattr 在这套实现里**只作用于目录**（`write_dir_entry` / `let_go_make_dir` 的
 * 逐键写入，`revoke_undeclared_xattrs` 的撤销都只针对目录）：普通文件的 xattr 随
 * "`.lpkgtmp` + rename"进一个**新 inode**，不存在"上一版留下的陈旧键"这回事。
 * 收别的形态只会让断言去比对实现从不构造的状态。
 */
void collect_disk_xattrs(const fs::path& root, const std::string& key_prefix, XMap& out)
{
    std::error_code iter_ec;
    if (!fs::exists(root, iter_ec)) return;
    const fs::path abs_root = fs::absolute(root, iter_ec);
    if (iter_ec) return;
    for (fs::recursive_directory_iterator it(abs_root, fs::directory_options::none, iter_ec), end;
         !iter_ec && it != end; it.increment(iter_ec)) {
        std::error_code ec;
        if (!it->is_directory(ec) || it->is_symlink(ec)) continue;  // 真目录才算
        const fs::path rel = fs::relative(it->path(), abs_root, ec);
        if (ec) continue;
        auto x = read_xattrs(it->path());
        if (x.empty()) continue;
        out[key_prefix + "/" + rel.generic_string()] = std::move(x);
    }
}

/**
 * 期望 xattr 状态 vs 盘面：逐**目录**比键值集合（缺/多/值不同都算问题）。
 * 返回问题清单（空 = 通过）。**只比 `want` 里出现的目录** + 盘上多出来的目录
 * （后者由调用方决定是否要报，见 `extra`）。
 */
std::string xmap_problems(const XMap& want, const XMap& got, bool report_extra)
{
    std::ostringstream os;
    for (const auto& [path, kv] : want) {
        const auto it = got.find(path);
        if (it == got.end()) {
            if (!kv.empty()) {
                os << "      · " << path << "：期望有键，盘上一条都没有（期望：";
                for (const auto& [k, v] : kv) os << " " << k << "=" << escape_payload(v);
                os << "）\n";
            }
            continue;
        }
        for (const auto& [k, v] : kv) {
            const auto kit = it->second.find(k);
            if (kit == it->second.end())
                os << "      · " << path << "：缺键 " << k << "（期望值=" << escape_payload(v)
                   << "）\n";
            else if (kit->second != v)
                os << "      · " << path << "：键 " << k << " 的值不同：期望=" << escape_payload(v)
                   << " 实际=" << escape_payload(kit->second) << "\n";
        }
        for (const auto& [k, v] : it->second) {
            if (!kv.contains(k))
                os << "      · " << path << "：多出一个不该有的键 " << k << "=" << escape_payload(v)
                   << "\n";
        }
    }
    if (report_extra) {
        for (const auto& [path, kv] : got) {
            if (want.contains(path) || kv.empty()) continue;
            os << "      · " << path << "：盘上有键但模型里没有这个目录的记录（";
            for (const auto& [k, v] : kv) os << " " << k << "=" << escape_payload(v);
            os << "）\n";
        }
    }
    return os.str();
}

/**
 * 摘要行的**唯一格式**：`<类型> <相对路径> | <载荷>`。
 * 路径由生成器保证不含空格（第 2 段恒为路径，便于从差异里反解出"是哪一格"）。
 * 两侧（模型 / 盘面）必须用同一个格式，否则比对就成了"格式差异"而不是"形态差异"。
 */
std::string summary_line(const std::string& kind, const std::string& rel,
                         const std::string& payload)
{
    return kind + " " + rel + " | " + escape_payload(payload);
}

/**
 * 模型摘要：与 `summary_of_disk` **同格式同口径**。
 *
 * Tree 的 key 是"相对 test_root 的完整路径"（打包/落盘要用它），而盘面摘要的路径是
 * **相对 seed 根**的 —— 两边必须落在同一个坐标系里才能逐项比，故这里把 `base + "/"`
 * 前缀剥掉（曾因为没剥，10 个种子全报"v1 盘面 != v1 模型"，是脚手架 bug 不是产品缺陷）。
 */
std::vector<std::string> summary_of_tree(const Tree& t, const std::string& base)
{
    const std::string prefix = base + "/";
    std::vector<std::string> out;
    out.reserve(t.size());
    for (const auto& [path, node] : t) {
        const std::string rel = path.starts_with(prefix) ? path.substr(prefix.size()) : path;
        out.push_back(summary_line(node.kind, rel, node.payload));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string read_text(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void write_text(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

/**
 * 盘面摘要：递归遍历 root，产出与 `summary_of_tree` **同格式**的排序行。
 * 类型判定一律用 lstat 语义（先判 symlink，再判 directory），否则 symlink→目录
 * 会被当成目录 —— 那正是"两侧对同一个对象理解不一致"的经典来源。
 */
std::vector<std::string> summary_of_disk(const fs::path& root)
{
    std::vector<std::string> out;
    std::error_code iter_ec;
    if (!fs::exists(root, iter_ec)) return {"<根不存在> " + root.string()};

    for (fs::recursive_directory_iterator it(root, fs::directory_options::none, iter_ec), end;
         !iter_ec && it != end; it.increment(iter_ec)) {
        const fs::path p = it->path();
        std::error_code ec;
        const fs::path rel = fs::relative(p, root, ec);
        const std::string rel_str = ec ? p.filename().string() : rel.generic_string();
        std::string kind = "other";
        std::string payload;
        if (it->is_symlink()) {
            kind = "symlink";
            const fs::path target = fs::read_symlink(p, ec);
            payload = ec ? "<读链接失败>" : target.string();
        } else if (it->is_directory()) {
            kind = "dir";
        } else if (it->is_regular_file()) {
            kind = "file";
            payload = read_text(p);
        }
        out.push_back(summary_line(kind, rel_str, payload));
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// 两个排序摘要的差集（用于把"哪一格变了"直接打进断言消息）
struct SummaryDiff {
    std::vector<std::string> only_expected;  // 期望有、盘上无（或形态不同）
    std::vector<std::string> only_actual;    // 盘上有、期望无
    bool empty() const
    {
        return only_expected.empty() && only_actual.empty();
    }
};

SummaryDiff diff_summaries(const std::vector<std::string>& expected,
                           const std::vector<std::string>& actual)
{
    SummaryDiff d;
    std::set_difference(expected.begin(), expected.end(), actual.begin(), actual.end(),
                        std::back_inserter(d.only_expected));
    std::set_difference(actual.begin(), actual.end(), expected.begin(), expected.end(),
                        std::back_inserter(d.only_actual));
    return d;
}

/// 摘要行 → 路径（第 2 段）。生成器保证路径不含空格，故这个反解是稳的。
std::string path_of_line(const std::string& line)
{
    const auto first = line.find(' ');
    if (first == std::string::npos) return line;
    const auto second = line.find(' ', first + 1);
    if (second == std::string::npos) return line.substr(first + 1);
    return line.substr(first + 1, second - first - 1);
}

std::set<std::string> paths_of_lines(const std::vector<std::string>& lines)
{
    std::set<std::string> out;
    for (const auto& l : lines) out.insert(path_of_line(l));
    return out;
}

std::string render_diff(const SummaryDiff& d, size_t max_per_side = 12)
{
    std::ostringstream os;
    const auto dump = [&os](const char* tag, const std::vector<std::string>& v) {
        for (size_t i = 0; i < v.size(); ++i) {
            if (i == 0) os << "    " << tag << "\n";
            os << "      " << v[i] << "\n";
        }
    };
    std::vector<std::string> e(
        d.only_expected.begin(),
        d.only_expected.begin() +
            static_cast<long>(std::min(max_per_side, d.only_expected.size())));
    std::vector<std::string> a(
        d.only_actual.begin(),
        d.only_actual.begin() + static_cast<long>(std::min(max_per_side, d.only_actual.size())));
    dump("- 期望有而盘上没有（或形态不同）:", e);
    dump("+ 盘上有而期望没有:", a);
    if (d.only_expected.size() > max_per_side)
        os << "    ...（期望侧还有 " << (d.only_expected.size() - max_per_side) << " 条未列）\n";
    if (d.only_actual.size() > max_per_side)
        os << "    ...（盘面侧还有 " << (d.only_actual.size() - max_per_side) << " 条未列）\n";
    return os.str();
}

std::string render_lines(const std::vector<std::string>& v)
{
    std::ostringstream os;
    for (const auto& l : v) os << "      " << l << "\n";
    return os.str();
}

// ============================================================================
// `/etc` 的模型与安全不变量（见文件头「/etc 维度」）
//
// 为什么 `/etc` 不能复用"盘面 == 新版本形态"这条一般属性：`/etc` 的落点由**配置保护**决定
// （三哈希分流 / `.lpkgnew` / `.lpkgsave` / 废弃配置留在原位），终态**本来就不等于**包内形态。
// 所以这里给 `/etc` 单独写一份模型 —— 把例外写进模型，而不是在断言处打补丁。
// ============================================================================

/// `subtree_keys` 的结果：`root` 自己 + 它下面所有条目（整树改名要用）
std::vector<std::string> subtree_keys(const Tree& t, const std::string& root)
{
    std::vector<std::string> out;
    const std::string prefix = root + "/";
    for (const auto& [path, node] : t)
        if (path == root || path.starts_with(prefix)) out.push_back(path);
    return out;
}

/// `OpSink::save_config` 的模型：整棵树改名成 `<root>.lpkgsave`（内容一个不丢）。
/// 目标名被占时的 `<dst>.<N>` 移位在本测试的路径空间里不会发生（每个种子的前缀唯一）。
void model_save_config(Tree& t, const std::string& root)
{
    for (const auto& k : subtree_keys(t, root)) {
        t[root + K_LPKGSAVE + k.substr(root.size())] = t[k];
        t.erase(k);
    }
}

/**
 * 升级到 v2 之后，**目录**上的期望 xattr（路径 → 键 → 值）。终态形状由调用方给
 * （属性 A = v2 ∪ 合租；属性 B 重试 = 同一棵树，多出来的目录由调用方决定是否报）。
 *
 * 规则与实现同一套（`installation_task_copy.cpp` 的 dir 分支 + `revoke_undeclared_xattrs`）：
 *   · **v2 声明的键** → 写入侧逐键 `lsetxattr` 落上去（`write_dir_entry` 刷新既有目录、
 *     `let_go_make_dir` 建新目录，两处都写）；
 *   · **本包 v1 声明过、v2 不再声明、且没有别的属主**的键 → 撤销趟 `lremovexattr` 撤掉；
 *   · **别的包仍持有**的键 → **不动盘**（判据是"这个**键**还有没有别的属主"，不是"这个目录
 *     还有没有别的属主"—— xattr 按目录共用，一个目录被多包持有是常态）；
 *   · 终态不是目录的路径（被换型 / 被删）⇒ 盘上那份连同 xattr 一起没了 ⇒ 模型里也不出现。
 *
 * 注意：**只列"至少有一个键"的目录**（与 `collect_disk_xattrs` 同口径），空 map 不参与比对。
 */
XMap model_x_upgrade(const Tree& terminal, const XMap& x2, const XMap& x_other)
{
    XMap out;
    for (const auto& [path, node] : terminal) {
        if (node.kind != "dir") continue;
        std::map<std::string, std::string> kv;
        const auto it2 = x2.find(path);
        if (it2 != x2.end()) kv = it2->second;  // v2 声明的（含"改值"后的新值）
        const auto io = x_other.find(path);     // 别的包持有的（本包撤不动它们）
        if (io != x_other.end())
            for (const auto& [k, v] : io->second) kv[k] = v;
        if (!kv.empty()) out[path] = std::move(kv);
    }
    return out;
}

/**
 * 卸载之后的期望 xattr：本包声明过的键**全部**被撤（不是"新版本不再声明"—— 卸载没有
 * "新版本"，`do_remove_package` 遍历 `get_package_xattr_keys(pkg)` 逐键撤），
 * 但**别的包仍持有**的键留下（合租共用键的安全生产）。
 * 目录本身若被 rmdir 掉，它的键自然消失 —— 由调用方给的 `terminal` 形状决定是否列它。
 */
XMap model_x_remove(const Tree& terminal, const XMap& x_pkg, const XMap& x_other)
{
    XMap out;
    for (const auto& [path, node] : terminal) {
        if (node.kind != "dir") continue;
        std::map<std::string, std::string> kv;
        const auto io = x_other.find(path);
        if (io != x_other.end()) kv = io->second;  // 只有别人的键能活下来
        if (!kv.empty()) out[path] = std::move(kv);
    }
    (void)x_pkg;  // 本包的键一律撤 ⇒ 不参与期望（留着这个参数是为了把判据写在签名上）
    return out;
}

/// `model_etc_after` 的读数：终态 + 三哈希分流/接管的**分类计数**（供反假绿覆盖断言）
struct EtcOutcome {
    Tree tree;             ///< `/etc` 子树的**期望终态**（键 = 相对 test_root 的路径）
    int install_new = 0;   ///< 三哈希 ①：盘上 == 上次装进去的（用户没改过）→ 静默换新版
    int keep_local = 0;    ///< 三哈希 ②：包本身没改这个配置 → 盘上那份留原位、**无** `.lpkgnew`
    int save_lpkgnew = 0;  ///< 三哈希 ③ / 符号链接条目 → 新版落 `.lpkgnew`，盘上那份留原位
    int obsolete_to_lpkgsave = 0;      ///< 废弃的 /etc 文件/链接 → 改名 `.lpkgsave`（2026-09-26）
    int type_changed_to_lpkgsave = 0;  ///< 类型变化（文件↔链接）→ 原物 `.lpkgsave` + 新物就地

    int dir_on_disk_replaced = 0;  ///< 盘上是**真目录**、归档是非目录 → 整树 `.lpkgsave` + 就地落位
    int dir_entry_over_nondir =
        0;  ///< 归档是**目录**、盘上是非目录（file/链接 → 目录）→ 整树 `.lpkgsave` + 建目录
    int write_in_place = 0;  ///< 盘上没被占（或让开趟已清空）→ 就地落位
    int kept_on_disk = 0;    ///< 盘上那份被保留（= keep_local + save_lpkgnew，读起来直观）
};

/**
 * `/etc` 子树"装完 `v2`"之后的**期望终态**（模型 = `decide_path` + `classify_config_update`
 * + `OpSink` 三个原语在 `/etc` 上的合成，逐格对照见下面每条注释）。
 *
 * 输入：
 *   · `v1`          —— v1 的**包内容**（用来算"上次我们装进去过什么"：confhashes 的旧值）；
 *   · `disk_before` —— 本次安装**之前**的盘面（属性 A = v1 形态；属性 B = v1 + 用户改动）；
 *   · `v2`          —— 本次要装的包内容（归档条目）。
 *
 * 判据（与实现同一套，缺一不可）：
 *   1. **confhashes 只记 `/etc` 的普通文件条目**：符号链接与目录条目在 `copy_package_files`
 *      里各自 `continue`（`is_symlink(src)` / `is_directory_follow(src)` 两个分支），够不到
 *      `set_conf_hash`。所以"有旧记录"当且仅当 v1 在该路径上是**普通文件**。
 *   2. ~~**废弃的 `/etc` 条目不碰盘**~~（2026-09-26 改）：v1 提供、v2 **不再提供**的
 *      `/etc` **文件/符号链接** → 改名 `<路径>.lpkgsave`（实现里的 `SaveConfigObsolete`），
 *      内容一个不丢；**目录**仍维持"只撤所有权、不碰盘"（有意例外）。
 *      ⇒ 模型对这些路径做一次 `model_save_config`，而不是"什么都不做"。
 *   3. 归档是**目录**条目：盘上已是真目录 → 不动；盘上被非目录占住 → `save_config` 整树改名
 *      再建目录；盘上没有 → 直接建。
 *   4. 归档是**非目录**条目而盘上是**真目录** → `SaveConfig`（整树改名成 `<路径>.lpkgsave/`）
 *      + `WriteInPlace`（就地落位、**不落** `.lpkgnew`）。这一格**先于**三哈希返回：
 *      目录不是「用户可能改过的那份配置文件」（`ARCH.md` §6.3 的类型变化格）。
 *   5. 其余 `/etc` 非目录条目：盘上被**非目录**占住 → 三哈希；否则就地落位。
 *      · 归档是**符号链接** → 一律按配置冲突处理：不接管，新版退 `.lpkgnew`；
 *      · 归档是普通文件 → `classify_config_update`（三哈希）三条分支，逐字复刻：
 *        ① 盘上 == 上次记录（且盘上是普通文件）→ `InstallNew`（就地换新版）
 *        ② 上次记录 == 本次包内         → `KeepLocal`（盘上那份留原位、无 `.lpkgnew`）
 *        ③ 其余（含"无从判定"）          → `SaveLpkgnew`（盘上那份留原位 + 落 `.lpkgnew`）
 */
EtcOutcome model_etc_after(const Tree& v1, const Tree& disk_before, const Tree& v2)
{
    EtcOutcome o;
    o.tree = disk_before;

    // 判据 1：confhashes 的旧值 = v1 在那个路径上的**普通文件**内容（其余形态没有记录）
    const auto recorded_of = [&](const std::string& p) -> std::string {
        const auto it = v1.find(p);
        return (it != v1.end() && it->second.kind == "file") ? it->second.payload : std::string{};
    };

    for (const auto& [path, node] : v2) {  // map 有序 ⇒ 父目录恒先于子条目
        const auto it = o.tree.find(path);
        const bool exists = it != o.tree.end();

        if (node.kind == "dir") {                              // 判据 3
            if (exists && it->second.kind == "dir") continue;  // 已是真目录（Noop）
            if (exists) {
                model_save_config(o.tree, path);  // SaveConfigAndMkDir
                ++o.dir_entry_over_nondir;
            } else {
                ++o.write_in_place;
            }
            o.tree[path] = Node{"dir", ""};
            continue;
        }

        if (exists && it->second.kind == "dir") {  // 判据 4
            model_save_config(o.tree, path);
            o.tree[path] = node;
            ++o.dir_on_disk_replaced;
            continue;
        }

        // 判据 4b（2026-09-26 统一）：盘上与归档**都是非目录**、但**类型不同**
        // （文件 ↔ 符号链接）→ 原物改名 `.lpkgsave` + 新物**就地**落位。
        // 改前这两格是分裂的：file→symlink 走"退 `.lpkgnew`、原文件留原样"、
        // symlink→file 走**三哈希**（拿链接目标的内容当 hash_local）—— 后者尤其没道理：
        // 类型都换了还谈"用户改没改过这份配置"。
        if (exists && (node.kind == "symlink") != (it->second.kind == "symlink")) {
            model_save_config(o.tree, path);
            o.tree[path] = node;
            ++o.type_changed_to_lpkgsave;
            continue;
        }

        if (!exists) {  // 盘上没被占 → 就地落位
            o.tree[path] = node;
            ++o.write_in_place;
            continue;
        }

        // ── 判据 5：盘上被**非目录**占住 ──────────────────────────────────────
        if (node.kind == "symlink") {
            o.tree[path + K_LPKGNEW] = node;  // 符号链接条目一律按配置冲突处理
            ++o.save_lpkgnew;
            continue;
        }
        const std::string recorded = recorded_of(path);
        bool install_new = false;
        bool keep_local = false;
        if (!recorded.empty()) {
            if (it->second.kind == "file" && it->second.payload == recorded)
                install_new = true;  // ① 盘上 == 上次装进去的
            else if (recorded == node.payload)
                keep_local = true;  // ② 包本身没改这个配置
        } else if (it->second.kind == "file" && it->second.payload == node.payload) {
            keep_local = true;  // 无旧记录（v1 给的是符号链接 / 盘上那份无主）→ 比内容
        }
        if (install_new) {
            o.tree[path] = node;
            ++o.install_new;
        } else if (keep_local) {
            ++o.keep_local;  // 盘上那份**原样留着**
        } else {
            o.tree[path + K_LPKGNEW] = node;  // ③ 三者互异 → 留原位 + 落 `.lpkgnew`
            ++o.save_lpkgnew;
        }
    }
    // 判据 2（2026-09-26 改）：废弃的 `/etc` **文件/符号链接** → 改名 `.lpkgsave`。
    // `v2.count(path)` 同时覆盖了"新版本以**目录**形态提供同一个裸键"的情形 —— 那种情况下
    // 让开趟已按"非目录 → 目录"那一格处理过（改名 + 建目录），这里绝不能再来一次
    // （实测：会搬走刚建好的目录、留下空的 `<路径>.lpkgsave/` 空壳）。
    for (const auto& [path, node] : v1) {
        if (v2.count(path)) continue;       // 新版本仍提供（含"变成了目录"）
        if (node.kind == "dir") continue;   // 目录：只撤所有权，不碰盘（有意例外）
        if (!o.tree.count(path)) continue;  // 盘上本来就没有（用户删了）
        model_save_config(o.tree, path);
        ++o.obsolete_to_lpkgsave;
    }

    o.kept_on_disk = o.keep_local + o.save_lpkgnew;
    return o;
}

/**
 * 安全不变量 a（`ARCH.md` §5.4 不变量 4）：**用户改过的那份配置永不静默丢失** ——
 * 它必须能在下面四处之一逐字节找回（形态与内容都比，不是"文件还在"就算数）：
 *   · 原位 `<p>`                       —— `KeepLocal` / 三哈希 ② 的落点
 *   · `<p>.lpkgnew`                    —— `SaveLpkgnew` / 符号链接条目的落点
 *   · `<p>.lpkgsave`                   —— `save_config`（挡路的是普通文件/符号链接）
 *   · `<祖先>.lpkgsave/<剩余路径>`      —— `save_config` **整树**改名（挡路的是目录）
 *
 * 这一条**独立于模型**：模型断言（终态 == 期望）先跑，但即便模型与实现"一起错"，
 * 这条网仍会在"用户那份没了"时炸出来。
 */
bool etc_node_findable(const Tree& post, const std::string& p, const Node& want)
{
    const auto same = [&](const std::string& k) {
        const auto it = post.find(k);
        return it != post.end() && it->second.kind == want.kind &&
               it->second.payload == want.payload;
    };
    if (same(p) || same(p + K_LPKGNEW) || same(p + K_LPKGSAVE)) return true;
    std::string acc;  // 逐级祖先：整树改名后内容落在 `<祖先>.lpkgsave/<剩余路径>`
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '/') {
            const std::string renamed = acc + K_LPKGSAVE + p.substr(acc.size());
            if (same(renamed)) return true;
        }
        acc += p[i];
    }
    return false;
}

/**
 * 把盘面读成一棵树（键与模型**同一口径**：`key_prefix` + 相对 `root` 的路径）。
 * 只服务于不变量 a 的"那份内容还在不在"查找 —— 类型判定一律 lstat 语义
 * （先判 symlink 再判 directory），与 `summary_of_disk` 同口径。
 */
void collect_disk_tree(const fs::path& root, const std::string& key_prefix, Tree& out)
{
    std::error_code iter_ec;
    if (!fs::exists(root, iter_ec)) return;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::none, iter_ec), end;
         !iter_ec && it != end; it.increment(iter_ec)) {
        std::error_code ec;
        const fs::path rel = fs::relative(it->path(), root, ec);
        if (ec) continue;
        const std::string key = key_prefix + "/" + rel.generic_string();
        if (it->is_symlink()) {
            const fs::path target = fs::read_symlink(it->path(), ec);
            out[key] = Node{"symlink", ec ? "<读链接失败>" : target.string()};
        } else if (it->is_directory()) {
            out[key] = Node{"dir", ""};
        } else if (it->is_regular_file()) {
            out[key] = Node{"file", read_text(it->path())};
        }
    }
}

// ============================================================================
// 生成器（固定种子 → 确定性的一棵 v1 树与一棵 v2 树）
// ============================================================================

const char* const KINDS[3] = {"file", "dir", "symlink"};

struct SlotGen {
    std::mt19937_64 rng;

    int rnd(int lo, int hi)
    {
        std::uniform_int_distribution<int> d(lo, hi);
        return d(rng);
    }
    bool chance(int pct)
    {
        return rnd(1, 100) <= pct;
    }
    /// 链接目标：指向 seed 根**之外**的散落文件（不归任何包）——目标是相对路径，
    /// 且指向的目录在包内容树里不存在，故打包/解包侧都不会把它误判成"目录条目"。
    std::string link_target()
    {
        return "../scatter/t" + std::to_string(rnd(0, 2)) + ".txt";
    }
};

std::string file_content(const std::string& rel, const std::string& ver)
{
    return "content(" + ver + ") " + rel + "\n";
}

/**
 * 往树里放一格：文件/链接是一格；目录是"目录格 + 1-3 个内部条目（可再嵌一层子目录）"。
 * 内部条目名一律 `c<i>`（v2 新增的条目用 `n<i>`，两套名字不交叉 ⇒ 不可能因为命名撞车
 * 把"废弃"误造出来）。
 */
void gen_subtree(SlotGen& g, Tree& t, const std::string& path, const std::string& kind,
                 const std::string& ver, int depth)
{
    if (kind == "file") {
        t[path] = Node{"file", file_content(path, ver)};
        return;
    }
    if (kind == "symlink") {
        t[path] = Node{"symlink", g.link_target()};
        return;
    }
    t[path] = Node{"dir", ""};
    const int n = g.rnd(1, 3);
    for (int i = 0; i < n; ++i) {
        const std::string child = path + "/c" + std::to_string(i);
        if (depth < 1 && g.chance(25))
            gen_subtree(g, t, child, "dir", ver, depth + 1);
        else
            t[child] = Node{"file", file_content(child, ver)};
    }
}

// ============================================================================
// xattr 维度的生成器（本轮新增；见文件头「xattr 维度」）
//
// 只作用于**目录**（实现如此：`write_dir_entry`/`let_go_make_dir` 的逐键写入与
// `revoke_undeclared_xattrs` 的撤销都只针对目录；普通文件的 xattr 随 `.lpkgtmp` + rename
// 进新 inode，不存在"陈旧键"）。键名固定集合、值随版本变 —— 断言比的是**键值对**，
// 值必须能区分"保留 / 改值"，否则"改值"这条迁移会退化成"保留"而测不出来。
// ============================================================================

/// 四种迁移 + 维度自带的随机键 + 合租共用键。`user.*` 命名空间：非特权可设，测试可跑。
const char* const XK_KEEP = "user.keep";
const char* const XK_CHG = "user.chg";
const char* const XK_DROP = "user.drop";
const char* const XK_ADD = "user.add";
const char* const XK_RAND = "user.rand";
const char* const XK_SHARED = "user.shared";

/**
 * **结构性**保证四种迁移每个种子都造出来（不消耗 rng）：取 map 序第一个"v1 与 v2 都是目录"
 * 的路径，在它上面钉 `keep`（值不变）/ `chg`（改值）/ `drop`（只在 v1 ⇒ 升级时必须撤）/
 * `add`（只在 v2 ⇒ 升级时必须写）。
 *
 * 为什么不能靠随机：四种迁移要同时命中"两版都是目录" + 各自的方向，概率与 `/etc` 那格
 * （`KeepLocal` 的原料）一样低；而"新版本不再声明的键必须撤"是**安全性质**（陈旧的
 * `posix_acl_default` 会继续决定该目录下新建文件的继承权限），覆盖不该靠种子碰运气
 * —— 加种子不是正道（见文件头那条结论）。
 *
 * @return 是否钉成功（没有"两版都是目录"的路径时为 false，该种子不参与四迁移覆盖读数）
 */
bool pin_xattr_transitions(const Tree& v1, const Tree& v2, XMap& x1, XMap& x2)
{
    for (const auto& [path, node] : v1) {  // map 有序 ⇒ 选择确定
        if (node.kind != "dir") continue;
        const auto it2 = v2.find(path);
        if (it2 == v2.end() || it2->second.kind != "dir") continue;
        x1[path][XK_KEEP] = "same";
        x2[path][XK_KEEP] = "same";  // ① 保留（值逐字不变）
        x1[path][XK_CHG] = "chg-v1";
        x2[path][XK_CHG] = "chg-v2";    // ② 改值
        x1[path][XK_DROP] = "drop-v1";  // ③ 丢弃：v2 不再声明 ⇒ 升级时该键必须被撤
        x2[path][XK_ADD] = "add-v2";    // ④ 新增：v1 没有 ⇒ 升级时写入
        return true;
    }
    return false;
}

/// v1 侧的**广度**：给其它目录随机挂 0..2 个键（四迁移那格由 pin 负责，这里只管铺开）。
void assign_v1_xattrs(const Tree& t, XMap& x, SlotGen& g)
{
    for (const auto& [path, node] : t) {
        if (node.kind != "dir") continue;
        std::map<std::string, std::string>& kv = x[path];
        if (kv.contains(XK_KEEP) || kv.contains(XK_DROP)) continue;  // pin 钉过的不动
        for (int i = 0, n = g.rnd(0, 2); i < n; ++i)
            kv[std::string(XK_RAND) + std::to_string(i)] = "v1:" + path;
    }
}

/// v2 侧：由 v1 的声明集**派生**（同键保留/改值/丢弃），另掺"新版才加的键"。
/// 目录在 v2 里不再是目录（类型变化 / 整条消失）⇒ 该路径的声明集为空 ⇒ 本包那些键全撤。
void derive_v2_xattrs(const Tree& v2, const XMap& x1, XMap& x2, SlotGen& g)
{
    for (const auto& [path, kv] : x1) {
        const auto it2 = v2.find(path);
        if (it2 == v2.end() || it2->second.kind != "dir") continue;  // 目录没了/换型
        std::map<std::string, std::string>& out = x2[path];
        for (const auto& [k, v] : kv) {
            if (out.contains(k)) continue;  // pin 钉过的
            const int roll = g.rnd(1, 100);
            if (roll <= 45)
                out[k] = v;  // 保留
            else if (roll <= 75)
                out[k] = "v2:" + path;  // 改值
            // 其余：丢弃（本包不再声明）
        }
        if (g.chance(35)) out[std::string(XK_ADD) + "r"] = "v2-new:" + path;
    }
}

/// 一个种子的全套模型
struct SeedModel {
    std::uint64_t seed = 0;
    std::string pkg;     // 主包名（种子间唯一：同一个测试里根目录是共用的）
    std::string co_pkg;  // 合租包名（只有 share_dir 非空时用得上）
    std::string base;    // seed 根（相对 test_root）：usr/share/prop/<ns><tag><seed>
    Tree v1;
    Tree v2;
    Tree co;                // 合租方持有的条目（与 v1/v2 重叠：同一个共享目录）
    std::string share_dir;  // 合租槽位路径；空 = 本种子不合租
    bool allow_dir_replaced = true;
    bool dir_replaced = false;                    // v2 里存在 "v1 是目录、v2 是非目录" 的共享路径
    std::vector<std::string> dir_replaced_paths;  // 上述路径（用于把失败归因到接管路径）

    // ── `/etc` 维度（本轮新增；见文件头「/etc 维度」与 gen_etc_model）────────────
    // 单独一片树：`/etc` 的终态**不**等于包内形态（配置保护），断言与模型都自成一套。
    std::string etc_base;           // /etc 侧的 seed 根：etc/prop/<ns><tag><seed>
    Tree etc1;                      // v1 的 /etc 包内容
    Tree etc2;                      // v2 的 /etc 包内容
    bool etc_dir_replaced = false;  // v2 里存在 "v1 是目录、v2 是非目录" 的 /etc 路径
    std::vector<std::string> etc_dir_replaced_paths;

    // ── xattr 维度（本轮新增；见文件头「xattr 维度」与 XMap 的说明）──────────────
    // 只作用于**目录**。`x_v1`/`x_etc1` = 装完 v1 后本包的**归属集合**（也是撤销趟遍历的对象）；
    // `x_v2`/`x_etc2` = 新版本声明集；`x_co` = 合租包声明集（多包共用 ⇒ 撤销不许碰）。
    XMap x_v1;
    XMap x_v2;
    XMap x_co;
    XMap x_etc1;
    XMap x_etc2;
    /// 本种子是否钉到了"四迁移"那一格（没钉到的种子不参与四迁移覆盖读数）
    bool xattr_transitions_pinned = false;
    bool etc_xattr_transitions_pinned = false;
    /// v2 里 xattr 声明集**变小**的路径（= 撤销趟真的有事可做的那些目录）
    std::vector<std::string> xattr_revoked_paths;

    /** 别的包（合租方）是否持有该 (路径, 键) —— 撤销判据里的第二道闸 */
    bool other_owns_xattr(const std::string& path, const std::string& key) const
    {
        const auto it = x_co.find(path);
        return it != x_co.end() && it->second.contains(key);
    }

    /// 本包在 usr 侧声明的全部 (路径, 键)（= 升级时"可能要撤"的候选集）
    std::vector<std::pair<std::string, std::string>> owned_usr_xattrs() const
    {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& [p, kv] : x_v1)
            for (const auto& [k, v] : kv) out.emplace_back(p, k);
        return out;
    }

    fs::path root(const fs::path& test_root) const
    {
        return test_root / base;
    }
    /// `/etc` 侧的 seed 根（盘面摘要与模型都相对它取值）
    fs::path etc_root(const fs::path& test_root) const
    {
        return test_root / etc_base;
    }
};

/// v1 的某一格在 v2 里变成什么形态。与 usr / `/etc` 两片树**共用同一套判据** ——
/// "类型变更是常态而不是边角"这条生成器意图只写在这里一处（两个调用点不会再漂移）。
/// `allow_dir_replaced == false` 时**一次随机数都不消耗**（目录必须还是目录）：
/// 这个性质是种子可复现性的一部分，别改成"先抽再判"。
std::string next_kind(SlotGen& g, const std::string& k1, bool allow_dir_replaced)
{
    if (k1 == "dir") {
        if (!allow_dir_replaced) return "dir";
        if (g.chance(65)) return "dir";
        return g.chance(50) ? "file" : "symlink";
    }
    if (g.chance(40)) return k1;  // 形态不变、只换内容/目标
    if (k1 == "file") return g.chance(50) ? "dir" : "symlink";
    return g.chance(50) ? "dir" : "file";
}

/// path 的某个**祖先**在 v2 里已经变成非目录 ⇒ 该路径在 v2 里不存在
/// （父格被文件/链接取代，子格连同一起消失 —— 正是"旧路径废弃"的一种）
bool under_nondir(const Tree& v2, const std::string& path)
{
    std::string acc;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/') {
            const auto it = v2.find(acc);
            if (it != v2.end() && it->second.kind != "dir") return true;
        }
        acc += path[i];
    }
    return false;
}

Tree merge_trees(const Tree& a, const Tree& b)
{
    Tree out = a;
    for (const auto& [path, node] : b) out.emplace(path, node);
    return out;
}

/// 两片树的 xattr 声明集合起来（打包时要按合并后的树一次性铺到 content/ 上）
XMap merge_xmaps(const XMap& a, const XMap& b)
{
    XMap out = a;
    for (const auto& [path, kv] : b)
        for (const auto& [k, v] : kv) out[path][k] = v;
    return out;
}

/**
 * `/etc` 维度的生成器（本轮新增）。**独立 RNG 流**：加这一维不能改变 usr 侧随机树
 * （否则今天 32 个种子的形态全变，"绿/红"就没法与上一版对照了）。
 *
 * 覆盖（逐条对任务清单）：
 *   · 条目形态：普通文件 / 目录 / 符号链接（`gen_subtree`，与 usr 侧同一套）；
 *   · 迁移：同类型、类型互转（含 **dir → 非目录** 与 **file → 目录**）、废弃、新增；
 *   · 用户改动的原料（属性 B 的 `plan_etc_user_edits` 靠它们把三哈希三条分支都走到）：
 *       – `companion.conf` 恒为 `file → file` 且内容**不同** ⇒ `SaveLpkgnew` 的原料；
 *       – 另**显式钉一格** `file → file` 且内容**逐字相同**（"包没改这个配置"）
 *         ⇒ `KeepLocal` 的原料。它不消耗随机数（取 map 序第一个满足条件的格），
 *         因此"三哈希三条分支都被覆盖"是**结构性**的，不靠种子碰运气。
 *   · "盘上是**真目录**、新版本是文件/符号链接"（本仓库最近最大的缺陷形状）：随机 + 兜底。
 */
void gen_etc_model(SlotGen& g, SeedModel& m, bool allow_dir_replaced)
{
    const std::string rel_prefix = m.etc_base + "/";

    // 恒在的 `/etc` 普通文件：三哈希的 ①（InstallNew）与 ③（SaveLpkgnew）都靠它必然可达，
    // 也让属性 B 的断点时刻 `/etc` 必然"已被动过"（回滚有 /etc 的活要干，不是空转）。
    m.etc1[m.etc_base + "/companion.conf"] = Node{"file", "etc companion v1\n"};
    m.etc2[m.etc_base + "/companion.conf"] = Node{"file", "etc companion v2\n"};

    // 共享槽位（v1/v2 都有该路径，类型可能变）
    for (int i = 0, n = g.rnd(3, 5); i < n; ++i)
        gen_subtree(g, m.etc1, m.etc_base + "/S" + std::to_string(i), KINDS[g.rnd(0, 2)], "v1", 0);
    // 仅 v1（废弃）—— `/etc` 的废弃条目**只撤所有权、留在原位**，是那个例外的一手证据
    for (int i = 0, n = g.rnd(0, 2); i < n; ++i)
        gen_subtree(g, m.etc1, m.etc_base + "/D" + std::to_string(i), KINDS[g.rnd(0, 2)], "v1", 0);
    // 仅 v2（新增）
    for (int i = 0, n = g.rnd(0, 1); i < n; ++i)
        gen_subtree(g, m.etc2, m.etc_base + "/N" + std::to_string(i), KINDS[g.rnd(0, 2)], "v2", 0);

    // ── v2：由 v1 的路径集合逐个派生（路径重叠最大化 ⇒ 类型变更最大化）──────────
    // `companion.conf` 例外：它在 v2 里**必须仍是普通文件、且内容与 v1 不同** ——
    // 三哈希 ③（`SaveLpkgnew`）的原料，也是"用户改过 → 落 `.lpkgnew`"这条路必然可达的保证。
    std::vector<std::string> kept_dirs;
    const std::string companion_path = m.etc_base + "/companion.conf";
    for (const auto& [path, node] : m.etc1) {
        if (path == companion_path) continue;
        if (under_nondir(m.etc2, path)) continue;  // 祖先在 v2 里已不是目录
        const std::string k2 = next_kind(g, node.kind, allow_dir_replaced);
        if (k2 == "dir") {
            m.etc2[path] = Node{"dir", ""};
            kept_dirs.push_back(path);
        } else if (k2 == "file") {
            m.etc2[path] = Node{"file", file_content(path, "v2")};
        } else {
            m.etc2[path] = Node{"symlink", g.link_target()};
        }
    }
    // 保留为目录的那些：掺 0-2 个"新版才发的条目"（与 usr 侧同构）
    for (const auto& d : kept_dirs) {
        if (g.chance(60)) {
            const std::string p = d + "/n0.conf";
            m.etc2[p] = Node{"file", file_content(p, "v2")};
        }
        if (g.chance(25)) {
            const std::string p = d + "/sub0";
            m.etc2[p] = Node{"dir", ""};
            m.etc2[p + "/n0.conf"] = Node{"file", file_content(p + "/n0.conf", "v2")};
        }
    }

    // ── 结构性覆盖：**保证**至少一格 `file → file` 且内容与 v1 逐字相同 ──────────
    // 它是三哈希 ②（"包本身没改这个配置" → `KeepLocal`）的**唯一原料**。靠随机撞不稳
    // （要同时命中"同类型 + 内容相同 + 用户改过"三件事），所以显式钉一格：
    // 取 map 序第一个满足条件的格（**确定性**，不消耗 rng），跳过 companion.conf
    // （它必须保持"内容不同"，否则 `SaveLpkgnew` 的原料就没了）。
    const std::string companion = m.etc_base + "/companion.conf";
    const auto same_content_candidate = [&]() -> std::string {
        for (const auto& [path, node] : m.etc1) {
            if (path == companion || node.kind != "file") continue;
            const auto it2 = m.etc2.find(path);
            if (it2 != m.etc2.end() && it2->second.kind == "file" &&
                it2->second.payload == node.payload)
                return path;
        }
        return {};
    };
    if (same_content_candidate().empty()) {
        for (const auto& [path, node] : m.etc1) {
            if (path == companion || node.kind != "file") continue;
            const auto it2 = m.etc2.find(path);
            if (it2 == m.etc2.end() || it2->second.kind != "file") continue;
            m.etc2[path] = Node{"file", node.payload};  // 包"没改"这个配置
            break;
        }
    }

    // ── 归因用：v2 里有哪些"v1 是目录、v2 是非目录"的 /etc 路径（缺陷形状的触发点）──
    for (const auto& [path, node] : m.etc1) {
        if (node.kind != "dir") continue;
        const auto it = m.etc2.find(path);
        if (it != m.etc2.end() && it->second.kind != "dir")
            m.etc_dir_replaced_paths.push_back(
                path.starts_with(rel_prefix) ? path.substr(rel_prefix.size()) : path);
    }
    m.etc_dir_replaced = !m.etc_dir_replaced_paths.empty();
}

/**
 * 造一个种子的模型。
 *
 * `allow_dir_replaced == false`：v1 是目录的路径在 v2 里**必须还是目录** —— 绕开
 * "dir→非目录"这条自成一格的迁移路径，让这一组能报**通用路径**（类型变更/废弃/新增/共享
 * 目录/回滚保真）上的新问题。今天 dir→非目录 已由 `dir_tree_entirely_ours` 放行（接管），
 * 不再是被拒的"缺口"，但两组仍各留一把尺：AllTransitions 盯接管路径，本组盯其余。
 *
 * `ns`：命名空间（属性 A 用 "a"、属性 B 用 "b"）。**两条属性必须各占一套包名与路径前缀**：
 * 同一个测试里 test_root 是共用的，而属性 A 会把包升到 2.0、把路径弄成"已被本包持有"，
 * 属性 B 再用同一个包名重装 v1 就变成"2.0 降级回 1.0"，撞上的是**另一套**语义
 * （实测：整批预检直接判 file conflict，B 的前置全部失败 —— 那是测试自己制造的假红）。
 */
SeedModel make_model(std::uint64_t seed, bool allow_dir_replaced, const std::string& ns)
{
    SlotGen g;
    g.rng.seed(seed * 1000003ULL + 17ULL);

    SeedModel m;
    m.seed = seed;
    m.allow_dir_replaced = allow_dir_replaced;
    const std::string tag = allow_dir_replaced ? "all" : "nrd";
    m.pkg = "p" + ns + tag + std::to_string(seed);
    m.co_pkg = "c" + ns + tag + std::to_string(seed);
    // 路径前缀也带 tag：单种子复现用例会把两个变体跑在**同一个** test_root 里，
    // 前缀不带 tag 的话第二个变体会撞上第一个变体装出来的文件（实测：整批预检
    // 报 "File /usr/share/prop/b0/S0 is owned by package pball0"）
    m.base = "usr/share/prop/" + ns + tag + std::to_string(seed);

    // 恒在的普通文件：断点 copy_after_wal 只挂在普通文件的 COPY 分支上，有它才保证
    // "批次进得了拷贝阶段就必然命中注入"（见文件头「断点注入不许空转」）。
    m.v1[m.base + "/companion.txt"] = Node{"file", file_content(m.base + "/companion.txt", "v1")};
    m.v2[m.base + "/companion.txt"] = Node{"file", file_content(m.base + "/companion.txt", "v2")};

    // 共享槽位（v1 与 v2 都有该路径，类型可能变）——"大部分重叠"就落在这里
    const int shared = g.rnd(4, 8);
    for (int i = 0; i < shared; ++i) {
        const std::string slot = m.base + "/S" + std::to_string(i);
        gen_subtree(g, m.v1, slot, KINDS[g.rnd(0, 2)], "v1", 0);
    }
    // 仅 v1（废弃）
    for (int i = 0, n = g.rnd(0, 2); i < n; ++i) {
        const std::string slot = m.base + "/D" + std::to_string(i);
        gen_subtree(g, m.v1, slot, KINDS[g.rnd(0, 2)], "v1", 0);
    }
    // 仅 v2（新增）
    for (int i = 0, n = g.rnd(0, 2); i < n; ++i) {
        const std::string slot = m.base + "/N" + std::to_string(i);
        gen_subtree(g, m.v2, slot, KINDS[g.rnd(0, 2)], "v2", 0);
    }
    // 合租（1/3 的种子）：v1 有一个目录 + 内容，v2 整个丢弃；**另一个包持有同一个目录**
    // —— 升级后这个目录必须保留（引用计数"最后持有者才删"的判定对象；否则 v2 装完目录消失）。
    if (g.chance(33)) {
        m.share_dir = m.base + "/C0";
        gen_subtree(g, m.v1, m.share_dir, "dir", "v1", 0);
        m.co[m.share_dir] = Node{"dir", ""};
    }

    // ── v2：由 v1 的路径集合**逐个派生**（路径重叠最大化 ⇒ 类型变更最大化）──
    // 伴生文件在 v2 里**必须仍是普通文件**（它是"COPY 分支必然可达"的保证，不能随机变形态）
    const std::string companion = m.base + "/companion.txt";
    std::vector<std::string> kept_dirs;
    for (const auto& [path, node] : m.v1) {
        if (path == companion) continue;
        if (!m.share_dir.empty() && (path == m.share_dir || path.starts_with(m.share_dir + "/")))
            continue;                            // 合租槽位：v2 整个丢弃（由合租方持有 → 目录保留）
        if (under_nondir(m.v2, path)) continue;  // 祖先在 v2 里已不是目录

        const std::string k2 = next_kind(g, node.kind, allow_dir_replaced);

        if (k2 == "dir") {
            m.v2[path] = Node{"dir", ""};
            kept_dirs.push_back(path);
        } else if (k2 == "file") {
            m.v2[path] = Node{"file", file_content(path, "v2")};
        } else {
            m.v2[path] = Node{"symlink", g.link_target()};
        }
    }
    // 保留为目录的那些：掺 0-2 个"新版才发的条目"（新增文件 / 新增子目录）
    for (const auto& d : kept_dirs) {
        if (g.chance(60)) {
            const std::string p = d + "/n0.txt";
            m.v2[p] = Node{"file", file_content(p, "v2")};
        }
        if (g.chance(25)) {
            const std::string p = d + "/sub0";
            m.v2[p] = Node{"dir", ""};
            m.v2[p + "/n0.txt"] = Node{"file", file_content(p + "/n0.txt", "v2")};
        }
    }

    // ── 归因用：v2 里有哪些"v1 是目录、v2 是非目录"的共享路径（= 接管路径的触发点）──
    // 存**相对 seed 根**的形态：断言里的差异路径也是相对形式，两边同口径才能判定"差异是否
    // 落在接管路径上"。
    const std::string rel_prefix = m.base + "/";
    for (const auto& [path, node] : m.v1) {
        if (node.kind != "dir") continue;
        const auto it = m.v2.find(path);
        if (it != m.v2.end() && it->second.kind != "dir")
            m.dir_replaced_paths.push_back(
                path.starts_with(rel_prefix) ? path.substr(rel_prefix.size()) : path);
    }
    m.dir_replaced = !m.dir_replaced_paths.empty();

    // ── `/etc` 维度（**独立 RNG 流**：加这一维不改变上面那片 usr 树，种子可复现性保持）──
    m.etc_base = "etc/prop/" + ns + tag + std::to_string(seed);
    SlotGen g_etc;
    g_etc.rng.seed(seed * 2654435761ULL + 91ULL);
    gen_etc_model(g_etc, m, allow_dir_replaced);

    // ── xattr 维度（本轮新增；**又一条独立 RNG 流**）──────────────────────────────
    // 同一条纪律：加维度不能改变既有的树与既有种子的行为，否则"今天的绿/红"就没法与
    // 上一版对照（这也是 `/etc` 维度当初独立成流的原因）。
    SlotGen g_x;
    g_x.rng.seed(seed * 6364136223846793005ULL + 29ULL);
    m.xattr_transitions_pinned = pin_xattr_transitions(m.v1, m.v2, m.x_v1, m.x_v2);
    assign_v1_xattrs(m.v1, m.x_v1, g_x);
    derive_v2_xattrs(m.v2, m.x_v1, m.x_v2, g_x);
    m.etc_xattr_transitions_pinned = pin_xattr_transitions(m.etc1, m.etc2, m.x_etc1, m.x_etc2);
    assign_v1_xattrs(m.etc1, m.x_etc1, g_x);
    derive_v2_xattrs(m.etc2, m.x_etc1, m.x_etc2, g_x);

    // 合租共用的键：**两个包都声明同一个键**（值也相同）⇒ 升级时本包不再声明那个目录、
    // 而合租方仍持有 ⇒ 撤销趟**必须不动盘**。这是按**键**记归属的意义所在：按目录判会
    // 撤掉别人的键，按"本包不再声明"判也一样（合租方还声明着）。
    if (!m.share_dir.empty()) {
        m.x_co[m.share_dir][XK_SHARED] = "shared";
        m.x_v1[m.share_dir][XK_SHARED] = "shared";
    }

    // 归因用：v2 里**声明集变小**的路径（撤销趟真的有事可做的那些目录）
    for (const auto& [path, kv] : m.x_v1) {
        const auto it2 = m.x_v2.find(path);
        for (const auto& [k, v] : kv) {
            if (it2 == m.x_v2.end() || !it2->second.contains(k)) {
                m.xattr_revoked_paths.push_back(path);
                break;
            }
        }
    }
    return m;
}

// ============================================================================
// 用户改动（属性 B 的"改动后 v1"锚点）
// ============================================================================

/**
 * 对**我方**条目随机做用户改动：文件改内容、链接换目标、目录里加一个用户文件。
 *
 * 刻意对"模型"与"盘面"**各做一遍**（`apply_edits_to_tree` / `apply_edits_to_disk`），
 * 再用"改动后盘面 == 改动后模型"这条断言把两者绑死 —— 只写一遍（例如"直接扫盘当基线"）
 * 会让"改动本身就没生效"被读成"回滚保真"，那是另一种假绿。
 */
struct UserEdit {
    std::string path;
    std::string kind;     // v1 里的形态
    std::string payload;  // 文件新内容 / 链接新目标 / 目录里新加的文件相对路径
};

/** `dir` 子树里**我们自己**的第一个普通文件（整棵子树只有目录/链接时返回空串）。 */
std::string first_owned_file_under(const Tree& t, const std::string& dir)
{
    const std::string prefix = dir + "/";
    for (const auto& [path, node] : t) {  // map 有序 ⇒ 结果确定
        if (node.kind == "file" && path.starts_with(prefix)) return path;
    }
    return {};
}

/**
 * 用户改动的**唯一实现**（usr 与 `/etc` 两片树共用一份）。`v1` = "改之前"的树、`v2` = 新版要发的
 * 树、`skip` = 本片树不碰的格子（合租槽位）、`note_name` = "往保留的目录里塞用户文件"用的文件名。
 *
 * `note_name` 两片树不同（`user-note.txt` / `user-note.conf`）：名字撞上 v2 的新条目会让"用户
 * 那个无主文件"与"包的新文件"重合，冲突语义就变味了 —— 两片树的 v2 新条目名也不一样。
 */
std::vector<UserEdit> plan_edits_impl(const Tree& v1, const Tree& v2,
                                      const std::function<bool(const std::string&)>& skip,
                                      const std::string& note_name, SlotGen& g)
{
    std::vector<UserEdit> edits;
    for (const auto& [path, node] : v1) {
        if (skip(path)) continue;
        if (!g.chance(45)) continue;
        UserEdit e;
        e.path = path;
        e.kind = node.kind;
        if (node.kind == "file") {
            e.payload = "user edited: " + path + "\n";
        } else if (node.kind == "symlink") {
            e.payload = "../scatter/user-t" + std::to_string(g.rnd(0, 1)) + ".txt";
        } else {
            // 目录被 v2 换成**非目录**时，**不能**往里加无人持有的文件：整批会被
            // `dir_tree_entirely_ours` 判否而**正确拒绝**（pacman 的 dir_belongsto_pkgs 语义
            // —— 整树搬走会毁掉那个文件），本种子就考不到"接管 + 回滚"了。
            // "被换成非目录"要**连祖先一起看**：接管的是**整棵子树**，所以只要某个祖先在 v2 里
            // 成了非目录，本目录也跟着被接管（它连同整树一起让开）——只判本路径会漏掉
            // `S0` 变文件、`S0/c0` 里被塞进 user-note.txt 这一类，同样会触发拒绝。
            // 改写成**我们自己**某个条目的内容，归属不变 ⇒ 整树仍是我们的 ⇒ 接管照常放行，
            // "用户改过的那份"照样是回滚保真的锚点（内容与 v1 原始、与 v2 形态都不同）。
            // "目录里有无主内容 + 该目录被非目录替换 ⇒ 必须拒绝"由文件末尾的固定用例
            // DirReplacedByNonDirWithUnownedContentIsRefused 单独钉。
            const auto v2_it = v2.find(path);
            const bool replaced_by_nondir =
                under_nondir(v2, path) || (v2_it != v2.end() && v2_it->second.kind != "dir");
            if (replaced_by_nondir) {
                const std::string owned = first_owned_file_under(v1, path);
                if (owned.empty()) continue;  // 整棵子树没有普通文件可改 → 本格不造用户改动
                e.path = owned;
                e.kind = "file";
                e.payload = "user edited: " + owned + "\n";
            } else {
                e.payload = path + "/" + note_name;  // 目录保留/废弃：内部加一个用户文件
            }
        }
        edits.push_back(std::move(e));
    }
    return edits;
}

std::vector<UserEdit> plan_user_edits(const SeedModel& m, SlotGen& g)
{
    return plan_edits_impl(
        m.v1, m.v2,
        [&](const std::string& p) {  // 合租槽位不碰（它的保留语义另有专断）
            return !m.share_dir.empty() && (p == m.share_dir || p.starts_with(m.share_dir + "/"));
        },
        "user-note.txt", g);
}

/**
 * **结构性覆盖**：让三哈希的 ②（`KeepLocal`）与 ③（`SaveLpkgnew`）两格有**确定的原料**。
 *
 * 为什么不能只靠 `g.chance(45)`：要同时命中"该格是 `file → file`" + "内容相同/不同" +
 * "恰好被改过"三件事，概率很低；而"用户改过的配置要么留原位、要么 `.lpkgnew`"正是 `/etc`
 * 配置保护的**全部意义**，覆盖不该靠种子碰运气（加种子不是正道 —— 见文件头）。
 * 兜底只**追加**两格改动（不动随机那一批），因此每个种子至多多出两条改动。
 *
 * 覆盖**非空**的保证分两层：本函数在**每个种子**内钉住"内容相同 / 内容不同"两格的用户改动
 * （前者需要该种子确实有一格 `file → file`，生成器已显式钉了一格 —— 见 `gen_etc_model`）；
 * 跨种子的 `EXPECT_GT(b.etc_keep_local, 0)` / `EXPECT_GT(b.etc_save_lpkgnew, 0)` 由文件末尾
 * 的用例断言兜底（`SaveLpkgnew` 还有 `companion.conf` 这一格恒在的原料，与种子无关）。
 */
void ensure_etc_edit_coverage(const SeedModel& m, std::vector<UserEdit>& edits)
{
    const auto already = [&](const std::string& p) {
        for (const auto& e : edits)
            if (e.path == p) return true;
        return false;
    };
    std::string same_content;  // v1 == v2 内容 ⇒ 包没改这个配置 ⇒ 用户改过则 KeepLocal
    std::string diff_content;  // v1 != v2 内容 ⇒ 用户改过则 SaveLpkgnew（落 .lpkgnew）
    for (const auto& [path, node] : m.etc1) {
        if (node.kind != "file") continue;
        const auto it2 = m.etc2.find(path);
        if (it2 == m.etc2.end() || it2->second.kind != "file") continue;
        if (it2->second.payload == node.payload) {
            if (same_content.empty()) same_content = path;
        } else if (diff_content.empty()) {
            diff_content = path;
        }
    }
    for (const std::string& p : {same_content, diff_content}) {
        if (p.empty() || already(p)) continue;
        edits.push_back(UserEdit{p, "file", "user edited: " + p + "\n"});
    }
}

/// `/etc` 的用户改动：与 usr 侧**同一套判据**（同一份实现）+ 上面那条覆盖兜底。
std::vector<UserEdit> plan_etc_user_edits(const SeedModel& m, SlotGen& g)
{
    auto edits = plan_edits_impl(
        m.etc1, m.etc2, [](const std::string&) { return false; }, "user-note.conf", g);
    ensure_etc_edit_coverage(m, edits);
    return edits;
}

void apply_edits_to_tree(Tree& t, const std::vector<UserEdit>& edits)
{
    for (const auto& e : edits) {
        if (e.kind == "file")
            t[e.path] = Node{"file", e.payload};
        else if (e.kind == "symlink")
            t[e.path] = Node{"symlink", e.payload};
        else
            t[e.payload] = Node{"file", "user note\n"};
    }
}

void apply_edits_to_disk(const fs::path& root, const std::vector<UserEdit>& edits)
{
    for (const auto& e : edits) {
        const fs::path p = root / e.path;
        if (e.kind == "file") {
            write_text(p, e.payload);
        } else if (e.kind == "symlink") {
            std::error_code ec;
            fs::remove(p, ec);
            fs::create_symlink(e.payload, p);
        } else {
            write_text(root / e.payload, "user note\n");
        }
    }
}

// ============================================================================
// 失败台账（种子 → 复现信息）
// ============================================================================

struct Report {
    explicit Report(std::string t) : title(std::move(t))
    {
    }

    std::string title;
    int ran = 0;
    int not_exercised = 0;  // 属性 B：批次在注入前就被拒 → 注入未生效（**不计通过**）
    std::vector<std::string> not_exercised_seeds;
    std::string not_exercised_note;

    // 归因计数（属性 B）：含 dir→非目录 迁移的种子里，**真的**走到拷贝/接管阶段的有几个、
    // 在拷贝前就被拒的有几个。这是"属性 B 非空转"的直接读数 —— 接管路径要是全被拒掉了，
    // 那一组就只是"在测拒绝"，根本没考到接管与回滚保真。
    int replaced_seeds = 0;
    int replaced_took_over = 0;
    int replaced_refused = 0;

    // ── `/etc` 维度的覆盖读数（本轮新增）────────────────────────────────────────
    // 全部来自**模型**（`model_etc_after` 的分类），而模型又由"终态 == 期望"这条断言与盘面
    // 绑死 ⇒ 这些数字不是"模型的愿望"，是盘面的事实的分类。
    int etc_seeds = 0;                ///< 含 `/etc` 条目的种子数（覆盖的正面读数）
    int etc_dir_takeover = 0;         ///< `/etc` 的「盘上真目录 → 新版本文件/链接」走接管（格数）
    int etc_install_new = 0;          ///< 三哈希 ①：盘上 == 上次装进去的 → 静默换新版（格数）
    int etc_keep_local = 0;           ///< 三哈希 ②：包没改这个配置 → 盘上那份留原位、无 .lpkgnew
    int etc_save_lpkgnew = 0;         ///< 三哈希 ③ / 符号链接条目 → 新版落 `.lpkgnew`
    int etc_dirty_at_breakpoint = 0;  ///< 属性 B：断点时刻 `/etc` 子树已被改动 ⇒ 回滚有活要干
    int etc_rolled_back = 0;          ///< 属性 B：`/etc` 逐项回到批次前（回滚保真的正面读数）
    int etc_dir_took = 0;             ///< 属性 B：含 /etc 的 dir→非目录 且**真的走到拷贝阶段**
    int etc_dir_refused = 0;          ///< 属性 B：含 /etc 的 dir→非目录 却在拷贝前被拒
    std::vector<std::uint64_t> etc_lost_seeds;  ///< 不变量 a 破了（用户那份找不到）的种子
    int etc_record_mismatch = 0;                ///< 不变量 b 破了（记录不是 hash_pkg）的种子数

    // ── xattr 维度的覆盖读数（本轮新增）────────────────────────────────────────
    int xattr_seeds = 0;                ///< 钉到"四迁移"那一格的种子数（覆盖的正面读数）
    int xattr_dir_seeds = 0;            ///< 有目录 xattr 声明的种子数
    int xattr_revoked_seeds = 0;        ///< v2 声明集变小（撤销趟有事可做）的种子数
    int xattr_dirty_at_breakpoint = 0;  ///< 属性 B/C：断点时刻 xattr 已被改过（回滚有活要干）
    int shared_xattr_kept = 0;          ///< 属性 C：合租共用键在卸载后仍被保留（安全生产）
    /// ⚠️ 已知缺口 `DIR_RM` 不记 xattr ⇒ 回滚重建的目录丢 xattr（本轮新维度发现的，未修）：
    /// 命中该缺口的种子数（**量化**，不是通过）
    int xattr_dir_rm_lost = 0;

    // ── 属性 C（卸载）的读数（本轮新增）────────────────────────────────────────
    int ran_c = 0;          ///< 属性 C 跑过的种子数
    int c_rolled_back = 0;  ///< C2：注入失败后回滚保真（形态 + xattr）的种子数
    int c_cleaned = 0;      ///< C1：干净卸载终态 == 模型 + 归属表清干净 的种子数

    struct Entry {
        std::string cat;
        std::uint64_t seed = 0;
        std::string detail;
    };
    std::vector<Entry> failures;

    void fail(const std::string& cat, std::uint64_t seed, const std::string& detail)
    {
        failures.push_back(Entry{cat, seed, detail});
        // 全量逐种子信息落 stderr（断言消息里只放每类的代表，见 message()），
        // 这样 /tmp/prop.log 里一定拿得到"每一个红种子的完整复现信息"。
        std::cerr << "[prop-fail] " << title << " seed=" << seed << " cat=" << cat << "\n"
                  << detail << "\n";
    }

    void note_not_exercised(std::uint64_t seed)
    {
        ++not_exercised;
        not_exercised_seeds.push_back(std::to_string(seed));
    }

    /// 汇总成一条断言消息：按类别分组 + 每类一个代表详情 + 全部失败种子号
    std::string message() const
    {
        std::ostringstream os;
        os << "\n[" << title << "] 种子数=" << ran << " 失败=" << failures.size()
           << " 注入未生效(不计通过)=" << not_exercised << "\n";
        if (replaced_seeds > 0) {
            os << "  · 含 dir→非目录 迁移的种子=" << replaced_seeds
               << "：真的走到接管/拷贝阶段=" << replaced_took_over
               << "，在拷贝前被拒（本组不该有）= " << replaced_refused << "\n";
        }
        if (not_exercised > 0) {
            os << "  · 注入未生效的种子（属性 B 的失败注入没走到拷贝阶段 ⇒ 没考到回滚）：";
            for (size_t i = 0; i < not_exercised_seeds.size() && i < 200; ++i)
                os << (i ? " " : "") << not_exercised_seeds[i];
            if (not_exercised_seeds.size() > 200) os << " ...";
            os << "\n";
            if (!not_exercised_note.empty()) os << "    " << not_exercised_note << "\n";
        }
        if (etc_seeds > 0 ||
            etc_dir_takeover + etc_install_new + etc_keep_local + etc_save_lpkgnew > 0) {
            os << "  · /etc 维度：含 /etc 条目的种子=" << etc_seeds
               << "；三哈希 InstallNew=" << etc_install_new << " KeepLocal=" << etc_keep_local
               << " SaveLpkgnew=" << etc_save_lpkgnew
               << "；/etc 的 dir→非目录 接管=" << etc_dir_takeover
               << "；属性 B 中断点时刻 /etc 已被改动=" << etc_dirty_at_breakpoint
               << "、回滚保真=" << etc_rolled_back;
            if (etc_record_mismatch > 0)
                os << "；**记录不是 hash_pkg 的种子=" << etc_record_mismatch << "**";
            if (!etc_lost_seeds.empty()) {
                os << "；**用户那份找不到的种子：";
                for (auto s : etc_lost_seeds) os << s << " ";
                os << "**";
            }
            os << "\n";
        }
        if (ran_c > 0 || xattr_seeds > 0 || shared_xattr_kept > 0) {
            os << "  · xattr 维度：四迁移钉到的种子=" << xattr_seeds
               << "、有目录 xattr 的种子=" << xattr_dir_seeds
               << "、v2 声明集变小（撤销有事可做）的种子=" << xattr_revoked_seeds
               << "；断点时刻 xattr 已被改动=" << xattr_dirty_at_breakpoint << "\n";
        }
        if (ran_c > 0) {
            os << "  · 属性 C（卸载）：种子数=" << ran_c << " 失败=" << failures.size()
               << " 注入未生效(不计通过)=" << not_exercised << "、回滚保真=" << c_rolled_back
               << "、干净卸载终态==模型+归属表清干净=" << c_cleaned
               << "、合租共用键保住=" << shared_xattr_kept << "\n";
        }
        if (failures.empty()) return os.str();

        std::vector<std::string> cats;
        for (const auto& f : failures)
            if (std::find(cats.begin(), cats.end(), f.cat) == cats.end()) cats.push_back(f.cat);
        for (const auto& cat : cats) {
            std::vector<std::uint64_t> seeds;
            const Entry* rep = nullptr;
            int n = 0;
            for (const auto& f : failures) {
                if (f.cat != cat) continue;
                ++n;
                seeds.push_back(f.seed);
                if (!rep) rep = &f;
            }
            os << "\n── 类别[" << cat << "] " << n << " 个种子\n   种子号：";
            for (size_t i = 0; i < seeds.size() && i < 250; ++i) os << (i ? " " : "") << seeds[i];
            if (seeds.size() > 250) os << " ...";
            os << "\n   代表复现（seed=" << rep->seed << "）：\n" << rep->detail << "\n";
        }
        os << "\n（每个失败种子的完整复现信息已逐条打到 stderr，前缀 [prop-fail]；"
              "单个种子可用 LPKG_PROP_SEED=<n> 重跑）\n";
        return os.str();
    }
};

}  // namespace

// ============================================================================

class UpgradePropertyTest : public IntegrationTestBase
{
protected:
    /**
     * 默认种子数。**这个数字不测"更多样本"，只测"形态覆盖"**：生成器是结构化的
     * （每个种子随机组合 路径形态 × v1→v2 迁移 × 用户改动 × 注入断点），形态空间是有限的，
     * 再多的种子只是在同一批形态上重采样 —— 它不是 fuzz（没有字节级变异、没有
     * `random_device`，种子确定 ⇒ 结果确定）。实测 200 个种子 320 s，而 **全部** 历史红种子
     * 都由同一个缺陷解释（dir→非目录 那一格），加种子并没有换来新发现。
     *
     * 所以默认取 32（约 50 s）：**要深度时手动加**——`LPKG_PROP_SEEDS=200 make test`，
     * 或在发布/夜间跑一次大的；单种子复现用 `LPKG_PROP_SEED=<n>`。
     * 覆盖性由 `run_group` 的兜底保证（见那里：没有 dir→非目录 种子就继续往后取），
     * 因此调小这个数字**不会**让反假绿断言变成靠运气。
     */
    static constexpr int kDefaultSeeds = 32;

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        IntegrationTestBase::TearDown();
    }

    // ── 环境开关（调试/复现用；被用到时打醒目提示）──────────────────────────

    static bool seed_selected(std::uint64_t seed)
    {
        const char* v = std::getenv("LPKG_PROP_SEED");
        if (!v || !*v) return true;
        return std::strtoull(v, nullptr, 10) == seed;
    }

    static int seed_count()
    {
        const char* v = std::getenv("LPKG_PROP_SEEDS");
        if (!v || !*v) return kDefaultSeeds;
        const int n = std::atoi(v);
        return n > 0 ? n : kDefaultSeeds;
    }

    /// 单种子复现模式（`LPKG_PROP_SEED=<n>`）：只跑指定的那一个种子，且**不做覆盖性兜底**。
    static bool single_seed_mode()
    {
        const char* v = std::getenv("LPKG_PROP_SEED");
        return v && *v;
    }

    static void note_env_overrides()
    {
        const char* one = std::getenv("LPKG_PROP_SEED");
        const char* many = std::getenv("LPKG_PROP_SEEDS");
        if ((one && *one) || (many && *many)) {
            std::cerr << "[prop] ⚠ 环境变量生效：LPKG_PROP_SEED=" << (one ? one : "(未设)")
                      << " LPKG_PROP_SEEDS=" << (many ? many : "(未设)")
                      << " —— 本次**不是**全量种子\n";
        }
    }

    // ── 打包 / 安装 ────────────────────────────────────────────────────────

    /// 把一棵树落成内容目录并打包（路径相对 content/）
    ///
    /// `xattrs`：**目录**上的 xattr 声明（路径 → 键 → 值，与 `tree` 同口径）。写在 content/ 上
    /// 之后由 `pack_package`（libarchive 的 disk reader）带进包 —— 这一条已由
    /// `test_dir_xattrs.cpp` 端到端实测（目录的 `user.*` 与 `system.posix_acl_default`
    /// 都能穿过打包/解包落到安装目标上），所以这里的 xattr 是真的进了包的归档。
    std::string pack_tree(const std::string& name, const std::string& ver, const Tree& tree,
                          const std::map<std::string, std::string>& extra_files = {},
                          const XMap& xattrs = {}) const
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        if (fs::exists(work)) fs::remove_all(work);
        const fs::path content = work / "content";
        fs::create_directories(content);

        for (const auto& [path, node] : tree) {  // map 有序 ⇒ 父目录恒先于子条目
            const fs::path p = content / path;
            if (node.kind == "dir") {
                fs::create_directories(p);
            } else if (node.kind == "file") {
                write_text(p, node.payload);
            } else {
                fs::create_directories(p.parent_path());
                fs::create_symlink(node.payload, p);
            }
        }
        // xattr 必须在**目录已建好**之后设（顺序不能反：先设再 create_directories 会被覆盖）
        for (const auto& [path, kv] : xattrs) {
            const fs::path p = content / path;
            if (!fs::is_directory(p)) {
                ADD_FAILURE() << "测试脚手架：xattr 只该挂在目录上，但 " << path << " 不是目录";
                continue;
            }
            for (const auto& [k, v] : kv) set_xattr_or_fail(p, k, v);
        }
        for (const auto& [path, data] : extra_files) write_text(content / path, data);

        const std::string out = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(out, work.string(), name, ver, {}, {}, "man " + name, {});
        return out;
    }

    /// 走真实入口；返回**实际错误消息**（成功为空串）。刻意不用 EXPECT_NO_THROW：
    /// 失败时的异常原文（`error.file_conflict_*` / `error.copy_failed_rollback` …）
    /// 本身就是本测试的重要产出，必须能被断言带进输出。
    std::string install_err(const std::string& pkg_path)
    {
        try {
            install_packages({pkg_path});
        } catch (const LpkgException& e) {
            return e.what();
        } catch (const std::exception& e) {
            return std::string("（非 LpkgException）") + e.what();
        }
        return {};
    }

    std::string installed_version(const std::string& pkg)
    {
        Cache::instance().load();
        return Cache::instance().get_installed_version(pkg);
    }

    /// seed 根之外的散落文件（链接目标；不归任何包 —— 用来证明"目标没被本包动过"）
    void make_scatter()
    {
        for (int i = 0; i < 3; ++i)
            write_text(test_root / ("usr/share/scatter/t" + std::to_string(i) + ".txt"),
                       "scatter t" + std::to_string(i) + "\n");
        for (int i = 0; i < 2; ++i)
            write_text(test_root / ("usr/share/scatter/user-t" + std::to_string(i) + ".txt"),
                       "scatter user t" + std::to_string(i) + "\n");
    }

    int bak_residue() const
    {
        int n = 0;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(test_root, ec)) {
            if (ec) break;
            if (e.path().filename().string().find(".lpkg_bak_") != std::string::npos) ++n;
        }
        return n;
    }

    /// 打包时用的内容目录（`pack_tree` 的落点，测试要拿它算"包内那份"的哈希）
    fs::path content_dir_of(const std::string& name, const std::string& ver) const
    {
        return suite_work_dir / ("_pkg_" + name + "_" + ver) / "content";
    }

    /// `/etc` 侧的两把尺：模型摘要（相对 etc 根）与盘面摘要（同口径）
    std::vector<std::string> etc_summary(const SeedModel& m, const Tree& t) const
    {
        return summary_of_tree(t, m.etc_base);
    }
    std::vector<std::string> etc_disk(const SeedModel& m) const
    {
        return summary_of_disk(m.etc_root(test_root));
    }

    // ── xattr 维度：盘面读数 + 与模型的比对（本轮新增）────────────────────────

    /// usr 侧盘面上的目录 xattr（键与模型同口径：相对 test_root 的路径）
    XMap disk_xattrs_usr(const SeedModel& m) const
    {
        XMap out;
        collect_disk_xattrs(m.root(test_root), m.base, out);
        return out;
    }

    /// `/etc` 侧同上
    XMap disk_xattrs_etc(const SeedModel& m) const
    {
        XMap out;
        collect_disk_xattrs(m.etc_root(test_root), m.etc_base, out);
        return out;
    }

    /**
     * xattr 维度的断言面（usr + `/etc`）。
     *
     * @param report_extra 盘上"模型里没有这个目录"的键是否算问题：属性 A 的终态是**精确**的
     *   ⇒ true；属性 B 重试后盘上可能合法多出"被保留的废弃目录"（里面有用户/别的包的内容）
     *   ⇒ false（那些目录的键另有针对性断言，见共享键那条）。
     *
     * `/etc` 侧**排除 `.lpkgsave` 目录**：整树改名不动 inode ⇒ xattr 跟着走到新名字下，而撤销
     * 打的是**原路径**（那里现在已是别的形态）⇒ `.lpkgsave` 那份保留旧键。它**不在生效位置**
     * （陈旧键的危害是"继续决定继承权限/继续打标签"，而那份已改名离开），语义属"改名保留"，
     * 由 `test_dir_xattrs.cpp` 的固定用例另钉 —— 这里不重复，也不把它算成失败。
     */
    std::string xattr_problems(const SeedModel& m, const Tree& usr_terminal,
                               const Tree& etc_terminal, bool report_extra) const
    {
        std::ostringstream os;
        os << xmap_problems(model_x_upgrade(usr_terminal, m.x_v2, m.x_co), disk_xattrs_usr(m),
                            report_extra);
        XMap want_etc = model_x_upgrade(etc_terminal, m.x_etc2, {});
        for (auto it = want_etc.begin(); it != want_etc.end();) {
            if (it->first.find(K_LPKGSAVE) != std::string::npos)
                it = want_etc.erase(it);
            else
                ++it;
        }
        os << xmap_problems(want_etc, disk_xattrs_etc(m), /*report_extra=*/false);
        // 非目录路径上不许有 xattr：写入侧只对**真目录**写（`is_real_directory(probe)`），
        // 落到文件/链接上说明那条边界破了 —— 这是"xattr 只作用于目录"的直接读数。
        for (const auto& [path, node] : usr_terminal) {
            if (node.kind == "dir") continue;
            const auto x = read_xattrs(m.root(test_root) / path);
            if (!x.empty())
                os << "      · " << path << "：非目录却带着 xattr（写入侧的目录边界破了）\n";
        }
        return os.str();
    }

    /// xattr 状态的**基线快照**（属性 B 回滚保真的比对基准）：路径 → 键 → 值
    void snapshot_xattrs(const SeedModel& m, XMap& usr, XMap& etc) const
    {
        usr = disk_xattrs_usr(m);
        etc = disk_xattrs_etc(m);
    }

    /**
     * 回滚后的 xattr 检查：**逐键严格比**（少键 / 多键 / 值不同一律报）。
     *
     * 订正 2026-09-26：本函数原先带**一条窄口径的宽容**（`may_lose` = 允许"该目录的键整批缺失"
     * 的路径集合），用来把 `DIR_RM` 不记 xattr 那个缺口**量化**而不是当通过 —— 那条缺口已在
     * 本日修好（`OpSink::remove_empty_dir` 现在在写 `DIR_RM` **之前**把该目录的 xattr 逐键记成
     * `XATTR_SET` 行，回滚逆序先重建目录再写回键），**宽容因此整段删除**，恢复严格断言。
     *
     * 顺手留下一条前人踩过的坑（它解释了为什么"宽容"当初只能做到"整批丢键"这一步、不能更省：
     * 也不能"把这些路径直接从期望里删掉" —— 被**搬进 stash** 的目录在断点时刻同样"不在盘上"，
     * 而它的回滚是 **rename 回来**（inode 不变 ⇒ xattr 完好）。把路径从期望里删掉，那条本来正确
     * 的键反而会被"多出一个不该有的键"判红 —— 一个把好种子判红、还把缺口范围算错的假判据。）
     */
    std::string xattr_rollback_problems(const XMap& want, const XMap& got) const
    {
        std::ostringstream os;
        for (const auto& [path, kv] : want) {
            const auto it = got.find(path);
            if (it == got.end()) {
                if (!kv.empty()) {
                    os << "      · " << path << "：期望有键，盘上一条都没有（期望：";
                    for (const auto& [k, v] : kv) os << " " << k << "=" << escape_payload(v);
                    os << "）\n";
                }
                continue;
            }
            for (const auto& [k, v] : kv) {
                const auto kit = it->second.find(k);
                if (kit == it->second.end()) {
                    os << "      · " << path << "：缺键 " << k << "\n";
                } else if (kit->second != v) {
                    os << "      · " << path << "：键 " << k
                       << " 的值不同：期望=" << escape_payload(v)
                       << " 实际=" << escape_payload(kit->second) << "\n";
                }
            }
            for (const auto& [k, v] : it->second) {
                if (!kv.contains(k))
                    os << "      · " << path << "：多出一个不该有的键 " << k << "="
                       << escape_payload(v) << "\n";
            }
        }
        for (const auto& [path, kv] : got) {
            if (want.contains(path) || kv.empty()) continue;
            os << "      · " << path << "：盘上有键但基线里没有这个目录（回滚多还原了东西）\n";
        }
        return os.str();
    }

    /// 两份 xattr 快照的差异（回滚保真用；**逐字节**比，含"键原本不存在" ⇒ 回滚后必须仍不存在）
    std::string xmap_diff(const XMap& want, const XMap& got, const std::string& what) const
    {
        const std::string p = xmap_problems(want, got, /*report_extra=*/true);
        if (p.empty()) return {};
        return "      " + what + " 的 xattr 与基线不符：\n" + p;
    }

    /**
     * 安全不变量 b（`/etc` 存在的意义）：**记录里记的永远是 `hash_pkg`（包内那份的内容），
     * 绝不追认盘上那份**。正反两侧都查：
     *   · v2 里每个 `/etc` **普通文件**条目 → confhashes 必须逐字等于**包内那份**的 sha256
     *     （不管这次三哈希判成了 InstallNew / KeepLocal / SaveLpkgnew —— 盘上可能留着用户那份）；
     *   · **旧键废弃**的路径 → 记录必须已被撤掉（`remove_obsolete_files` 的 `DropOwnership`：
     *     `remove_conf_hash`）。"废弃"的判据是实现的**键形态**（见下），不是"v2 没发文件"。
     * 返回问题清单（空 = 通过）；调用方把原文打进断言消息。**不抛**：断言要在失败时把
     * 别的读数一起带出来。
     */
    std::string etc_conf_hash_problems(const SeedModel& m, const std::string& ver,
                                       const Tree& v1_tree) const
    {
        std::ostringstream os;
        Cache::instance().load();
        auto& cache = Cache::instance();
        const fs::path content = content_dir_of(m.pkg, ver);
        for (const auto& [path, node] : m.etc2) {
            if (node.kind != "file") continue;
            const std::string want = calculate_sha256(content / path);
            const std::string got = cache.get_conf_hash("/" + path, m.pkg);
            if (got != want)
                os << "      · " << path << "：记录=" << (got.empty() ? "(空)" : got)
                   << "，期望（包内那份 hash_pkg）=" << want << "\n";
        }
        for (const auto& [path, node] : v1_tree) {
            if (node.kind != "file") continue;
            const auto it2 = m.etc2.find(path);
            // ⚠️ "要不要撤记录"的判据是**实现里的键形态**，不是"v2 还发不发一个普通文件"：
            //    归档里的**符号链接**按文件键登记（`scan_content_files`：只有真目录带尾斜杠），
            //    所以 v1 的 `/etc/x`（文件）在 v2 变成**符号链接**时，键仍在 `new_set` 里 ⇒
            //    **不是废弃** ⇒ 登记趟给 `Noop`：那条记录既不更新也不撤（留着当以后的
            //    `hash_orig`，见 `record` 处的注释）。只有 v2 **不再提供**该路径、或把它变成了
            //    **目录**（键变成 `/etc/x/`）时，旧的文件键才是废弃的 —— 那时必须撤。
            if (it2 != m.etc2.end() && it2->second.kind != "dir") continue;  // 键仍在 ⇒ 不撤
            const std::string got = cache.get_conf_hash("/" + path, m.pkg);
            if (!got.empty())
                os << "      · " << path
                   << "：v2 不再以非目录条目提供（旧键已废弃），记录该被撤掉，实际=" << got << "\n";
        }
        return os.str();
    }

    /**
     * 安全不变量 a（`ARCH.md` §5.4 不变量 4）：**用户改过的那份配置永不静默丢失**。
     * 对每条 `/etc` 用户改动，断言那份内容（形态 + 字节）能在原位 / `.lpkgsave` / `.lpkgnew`
     * 之一找回。返回问题清单（空 = 通过）。
     *
     * 注意判据取的是**用户改动**（而不是"盘上所有文件"）：`/etc` 里被废弃、被换新版而消失的
     * 那份是**包自己的内容**，不在"永不丢失"的承诺范围内（用户改过的才算）。
     */
    std::string etc_lost_user_content_problems(const Tree& post_disk,
                                               const std::vector<UserEdit>& edits) const
    {
        std::ostringstream os;
        for (const auto& e : edits) {
            // 目录改动（往目录里加文件）表达成"新文件的路径 + 固定内容"
            const std::string path = (e.kind == "dir") ? e.payload : e.path;
            const Node want =
                (e.kind == "dir") ? Node{"file", "user note\n"} : Node{e.kind, e.payload};
            if (!etc_node_findable(post_disk, path, want))
                os << "      · " << path << "（用户改动：" << want.kind << " 内容/目标=\""
                   << escape_payload(want.payload) << "\"）在盘上任何保留位都找不到\n";
        }
        return os.str();
    }

    /// `/etc` 的**期望侧**小结 + 差异/异常原文。刻意不含"包内容 / 盘面"（那两份由 `repro`
    /// 统一打一次）—— 否则同一条失败消息里会把同一段打两遍，读起来更费劲。
    std::string repro_etc(const SeedModel& m, const std::vector<std::string>& want_etc_v1,
                          const std::vector<std::string>& want_etc_v2,
                          const std::string& extra = "") const
    {
        std::ostringstream os;
        os << "    /etc 根=" << m.etc_base << "（下面的 /etc 摘要都相对它）\n";
        if (!want_etc_v1.empty())
            os << "    /etc 期望 v1（" << want_etc_v1.size() << " 条）：\n"
               << render_lines(want_etc_v1);
        if (!want_etc_v2.empty())
            os << "    /etc 期望终态（" << want_etc_v2.size() << " 条）：\n"
               << render_lines(want_etc_v2);
        if (!extra.empty()) os << extra << "\n";
        return os.str();
    }

    /// 独立成一条失败消息时用的**完整** `/etc` 小结（包内容 + 期望 + 盘面 + 归因旗标）
    std::string repro_etc_full(const SeedModel& m, const std::vector<std::string>& want_etc_v1,
                               const std::vector<std::string>& want_etc_v2,
                               const std::string& extra = "") const
    {
        std::ostringstream os;
        os << "    /etc 包内容 v1（" << m.etc1.size() << " 条）：\n"
           << render_lines(etc_summary(m, m.etc1));
        os << "    /etc 包内容 v2（" << m.etc2.size() << " 条）：\n"
           << render_lines(etc_summary(m, m.etc2));
        os << repro_etc(m, want_etc_v1, want_etc_v2, extra);
        os << "    /etc 盘面摘要：\n" << render_lines(etc_disk(m));
        if (m.etc_dir_replaced) {
            os << "    ★ 本种子含 /etc 的 dir→非目录（盘上真目录 → 新版本文件/链接）：";
            for (const auto& p : m.etc_dir_replaced_paths) os << " " << p;
            os << "\n";
        }
        return os.str();
    }

    // ── 复现信息（每个失败自带"哪一格、变成什么"）────────────────────────────

    std::string repro(const SeedModel& m, const std::vector<std::string>& want_v1,
                      const std::vector<std::string>& want_v2, const std::string& extra = "") const
    {
        std::ostringstream os;
        os << "    包名=" << m.pkg << " 根=" << m.base << "（摘要里的路径都相对这个根）\n";
        os << "    v1 模型摘要（" << want_v1.size() << " 条）：\n" << render_lines(want_v1);
        os << "    v2 模型摘要（" << want_v2.size() << " 条）：\n" << render_lines(want_v2);
        os << "    盘面摘要：\n" << render_lines(summary_of_disk(m.root(test_root)));
        // `/etc` 侧：包内容 v1/v2 + 盘面（**只打这一次**；期望侧由调用方按需经 extra 追加）
        os << repro_etc_full(m, {}, {});
        if (m.dir_replaced) {
            os << "    ★ 本种子含 dir→非目录 迁移（整树归本包/本批次就该接管）：";
            for (const auto& p : m.dir_replaced_paths) os << " " << p;
            os << "\n";
        } else {
            os << "    （本种子**不含** dir→非目录 迁移 ⇒ 失败与该迁移无关）\n";
        }
        if (!m.share_dir.empty()) os << "    合租目录：" << m.share_dir << "\n";
        if (!extra.empty()) os << extra << "\n";
        return os.str();
    }

    // ── 属性 A：干净升级 ───────────────────────────────────────────────────

    void seed_property_a(const SeedModel& m, Report& rep)
    {
        ++rep.ran;
        const Tree want_v1_tree = merge_trees(m.v1, m.co);
        const Tree want_v2_tree = merge_trees(m.v2, m.co);
        const auto want_v1 = summary_of_tree(want_v1_tree, m.base);
        const auto want_v2 = summary_of_tree(want_v2_tree, m.base);
        // `/etc` 侧：v1 是全新安装（盘上没有别的东西）⇒ 盘面 == 包内容；v2 的终态由模型算
        // （配置保护的例外都写进 model_etc_after，见那里的判据 1-5）
        const auto want_etc_v1 = etc_summary(m, m.etc1);
        const EtcOutcome etc2 = model_etc_after(m.etc1, m.etc1, m.etc2);
        const auto want_etc_v2 = etc_summary(m, etc2.tree);
        ++rep.etc_seeds;
        rep.etc_install_new += etc2.install_new;
        rep.etc_keep_local += etc2.keep_local;
        rep.etc_save_lpkgnew += etc2.save_lpkgnew;
        rep.etc_dir_takeover += etc2.dir_on_disk_replaced;

        if (!m.co.empty()) {  // 合租方先装（它持有那个共享目录）
            const std::string co_path = pack_tree(m.co_pkg, "1.0", m.co, {}, m.x_co);
            const std::string co_err = install_err(co_path);
            if (!co_err.empty()) {
                rep.fail("A:合租包装不上（脚手架问题）", m.seed, "异常：" + co_err);
                return;
            }
        }

        // ① 装 v1
        const std::string v1_path =
            pack_tree(m.pkg, "1.0", merge_trees(m.v1, m.etc1), {}, merge_xmaps(m.x_v1, m.x_etc1));
        const std::string e1 = install_err(v1_path);
        if (!e1.empty()) {
            rep.fail("A:v1 装不上（脚手架/模型问题）", m.seed,
                     repro(m, want_v1, want_v2, "异常：" + e1));
            return;
        }
        if (installed_version(m.pkg) != "1.0") {
            rep.fail("A:v1 装完 DB 版本不是 1.0（脚手架问题）", m.seed, repro(m, want_v1, want_v2));
            return;
        }
        const auto got_v1 = summary_of_disk(m.root(test_root));
        if (got_v1 != want_v1) {
            rep.fail("A:v1 盘面 != v1 模型（脚手架/模型问题）", m.seed,
                     repro(m, want_v1, want_v2, render_diff(diff_summaries(want_v1, got_v1))));
            return;
        }
        const auto got_etc_v1 = etc_disk(m);
        if (got_etc_v1 != want_etc_v1) {
            rep.fail(
                "A:/etc 盘面 != v1 包内容（脚手架/模型问题）", m.seed,
                repro(m, want_v1, want_v2, render_diff(diff_summaries(want_etc_v1, got_etc_v1))));
            return;
        }

        // ② 装 v2（干净升级）
        const std::string v2_path =
            pack_tree(m.pkg, "2.0", merge_trees(m.v2, m.etc2), {}, merge_xmaps(m.x_v2, m.x_etc2));
        const std::string e2 = install_err(v2_path);
        if (!e2.empty()) {
            // dir → 非目录**不再是缺口**（整棵树都归本包/本批次时接管，见 make_model 与
            // 文件头）：所以**任何**拒绝都是问题。仍按"含不含该迁移"分两类，方便把失败
            // 归因到类型转换本身还是别处。usr 与 `/etc` 两片树都要看（`/etc` 的那一格
            // 是最近才修好的，且今天仍是最容易退回缺陷的一格）。
            std::string cat;
            if (m.dir_replaced || m.etc_dir_replaced)
                cat = "A:v2 被拒（含 dir→非目录：整树归本包就该接管，新问题）";
            else
                cat = "A:v2 被拒（**不含** dir→非目录 ⇒ 新问题）";
            if (m.etc_dir_replaced) cat += "〔含 /etc 侧：缺陷形状〕";
            rep.fail(cat, m.seed, repro(m, want_v1, want_v2, "异常原文：" + e2));
            return;
        }

        // ③ 盘面 == v2 形态
        const auto got_v2 = summary_of_disk(m.root(test_root));
        if (got_v2 != want_v2) {
            const SummaryDiff d = diff_summaries(want_v2, got_v2);
            const auto bad = paths_of_lines(d.only_expected);
            bool touches_gap = false;
            for (const auto& p : m.dir_replaced_paths)
                if (bad.contains(p)) touches_gap = true;
            const std::string cat = touches_gap
                                        ? "A:v2 盘面不符（差异含 dir→非目录 ⇒ 接管/回滚有漏）"
                                        : "A:v2 盘面不符（差异**不含** dir→非目录 ⇒ 新问题）";
            rep.fail(cat, m.seed, repro(m, want_v1, want_v2, render_diff(d)));
            return;
        }

        // ③′ `/etc` 终态 == 模型（例外已写进模型，不在断言处打补丁）
        const auto got_etc_v2 = etc_disk(m);
        if (got_etc_v2 != want_etc_v2) {
            const SummaryDiff d = diff_summaries(want_etc_v2, got_etc_v2);
            const std::string cat = m.etc_dir_replaced
                                        ? "A:/etc 终态 != 模型（本种子含 /etc 的 dir→非目录）"
                                        : "A:/etc 终态 != 模型（不含 /etc 的 dir→非目录 ⇒ 新问题）";
            rep.fail(
                cat, m.seed,
                repro(m, want_v1, want_v2, repro_etc(m, want_etc_v1, want_etc_v2, render_diff(d))));
            return;
        }
        // ③″ 安全不变量 b：记录里记的永远是 hash_pkg，绝不追认盘上那份
        const std::string hash_problems = etc_conf_hash_problems(m, "2.0", m.etc1);
        if (!hash_problems.empty()) {
            ++rep.etc_record_mismatch;
            rep.fail("A:/etc 的 confhashes 记录不是 hash_pkg（**底线**：绝不追认盘上那份）", m.seed,
                     repro(m, want_v1, want_v2, hash_problems));
            return;
        }
        // ③‴ xattr 维度：终态 == 模型（v2 声明 ∪ 别的包持有；本包不再声明的**已撤**）
        {
            const std::string xp =
                xattr_problems(m, want_v2_tree, etc2.tree, /*report_extra=*/true);
            if (!xp.empty()) {
                rep.fail("A:xattr 终态 != 模型（写入 / 撤销 / 共享键）", m.seed,
                         repro(m, want_v1, want_v2, xp));
                return;
            }
            // 合租共用键：**升级**时本包不再声明该目录，而合租方仍持有 ⇒ 键必须还在盘上
            if (!m.share_dir.empty() && !m.x_co.empty()) {
                const auto x = disk_xattrs_usr(m);
                const auto it = x.find(m.share_dir);
                if (m.x_co.contains(m.share_dir) &&
                    (it == x.end() || !it->second.contains(XK_SHARED))) {
                    rep.fail("A:合租共用键在升级后被撤掉了（别的包仍持有 ⇒ 撤销必须不动盘）",
                             m.seed, "      " + m.share_dir + " 期望仍有 " + XK_SHARED + "\n");
                    return;
                }
            }
        }

        if (installed_version(m.pkg) != "2.0") {
            rep.fail("A:v2 装完 DB 版本不是 2.0", m.seed, repro(m, want_v1, want_v2));
            return;
        }
        if (bak_residue() != 0) {
            rep.fail("A:提交后仍有 .lpkg_bak_ 残留", m.seed, repro(m, want_v1, want_v2));
        }
    }

    // ── 属性 B：注入失败后回滚 ─────────────────────────────────────────────

    void seed_property_b(const SeedModel& m, Report& rep)
    {
        ++rep.ran;
        const Tree want_v1_tree = merge_trees(m.v1, m.co);
        const Tree want_v2_tree = merge_trees(m.v2, m.co);
        const auto want_v1 = summary_of_tree(want_v1_tree, m.base);
        const auto want_v2 = summary_of_tree(want_v2_tree, m.base);
        ++rep.etc_seeds;

        if (!m.co.empty()) {
            const std::string co_path = pack_tree(m.co_pkg, "1.0", m.co, {}, m.x_co);
            if (!install_err(co_path).empty()) {
                rep.fail("B:合租包装不上（脚手架问题）", m.seed, "（见 stderr）");
                return;
            }
        }

        // ① 装 v1
        const std::string v1_path =
            pack_tree(m.pkg, "1.0", merge_trees(m.v1, m.etc1), {}, merge_xmaps(m.x_v1, m.x_etc1));
        const std::string e1 = install_err(v1_path);
        if (!e1.empty()) {
            rep.fail("B:v1 装不上（脚手架/模型问题）", m.seed,
                     repro(m, want_v1, want_v2, "异常：" + e1));
            return;
        }
        if (summary_of_disk(m.root(test_root)) != want_v1) {
            rep.fail("B:v1 盘面 != v1 模型（脚手架/模型问题）", m.seed, repro(m, want_v1, want_v2));
            return;
        }
        if (etc_disk(m) != etc_summary(m, m.etc1)) {
            rep.fail("B:/etc 盘面 != v1 包内容（脚手架/模型问题）", m.seed,
                     repro_etc_full(m, {}, {}));
            return;
        }

        // ② 用户改动（模型与盘面各做一遍，然后两边必须一致）
        SlotGen g;
        g.rng.seed(m.seed * 7919ULL + 13ULL);
        const auto edits = plan_user_edits(m, g);
        Tree edited_tree = want_v1_tree;
        apply_edits_to_tree(edited_tree, edits);
        apply_edits_to_disk(test_root, edits);
        const auto want_edited = summary_of_tree(edited_tree, m.base);
        const auto got_edited = summary_of_disk(m.root(test_root));
        if (want_edited != got_edited) {
            rep.fail(
                "B:用户改动后盘面 != 改动后模型（脚手架问题）", m.seed,
                repro(m, want_v1, want_v2, render_diff(diff_summaries(want_edited, got_edited))));
            return;
        }
        // 回滚的基准 = **改动后**的实际盘面（逐项快照；不是 v1 原始形态）
        const auto baseline = got_edited;

        // ②′ `/etc` 的用户改动：同一套手法（模型 + 盘面各做一遍，再绑死）。
        //     刻意用**独立 RNG 流**：usr 侧的改动逐字保持原样，加 /etc 维度不改变既有种子。
        SlotGen g_etc;
        g_etc.rng.seed(m.seed * 104729ULL + 23ULL);
        const auto etc_edits = plan_etc_user_edits(m, g_etc);
        Tree edited_etc = m.etc1;
        apply_edits_to_tree(edited_etc, etc_edits);
        apply_edits_to_disk(test_root, etc_edits);
        const auto want_etc_edited = etc_summary(m, edited_etc);
        const auto got_etc_edited = etc_disk(m);
        if (want_etc_edited != got_etc_edited) {
            rep.fail("B:/etc 用户改动后盘面 != 改动后模型（脚手架问题）", m.seed,
                     repro_etc_full(m, want_etc_edited, {},
                                    render_diff(diff_summaries(want_etc_edited, got_etc_edited))));
            return;
        }
        const auto etc_baseline = got_etc_edited;
        // xattr 的**回滚基准**（同样取"改动后"的实际盘面）：与形态基线同一时刻、同一口径
        XMap x_baseline_usr, x_baseline_etc;
        snapshot_xattrs(m, x_baseline_usr, x_baseline_etc);

        // ③ 注入中途失败（COPY 已写 WAL、rename 未做）
        const std::string v2_path =
            pack_tree(m.pkg, "2.0", merge_trees(m.v2, m.etc2), {}, merge_xmaps(m.x_v2, m.x_etc2));
        bool bp_hit = false;
        // 断点时刻的盘面快照：用来证明"这次回滚**确实有东西要撤**"（见下面的断言）。
        // 它是属性 B 最要害的一条反腐：若断点是在"什么都没改"的时刻命中的，那么"回滚后
        // 盘面 == 基线"就是恒真的废话（什么都没动，自然还一样）—— 那种绿比没有测试更糟。
        // `/etc` 侧同理（`etc_dirty_at_breakpoint`）：让开趟已经把那几份配置搬进了 stash，
        // 所以断点时刻的 `/etc` 必然与基线不同 —— 这正是 `ARCH.md` §5.5 说的"配置短暂不在盘上"。
        std::vector<std::string> at_breakpoint;
        std::vector<std::string> at_breakpoint_etc;
        XMap at_breakpoint_x;  // 断点时刻的 xattr 快照（证明"xattr 回滚确实有物可撤"）
        // 断点时刻的盘面**树**快照：用来判定"哪些目录已被前向趟删掉"（`DIR_RM` 命中的对象），
        // 供下面那条**窄口径**的已知缺口宽容使用（见 ④′ 的 ⚠️）。
        Tree at_bp_usr;
        Tree at_bp_etc;
        BreakpointManager::instance().set("copy_after_wal_" + m.pkg, [&] {
            bp_hit = true;
            at_breakpoint = summary_of_disk(m.root(test_root));
            at_breakpoint_etc = etc_disk(m);
            at_breakpoint_x = disk_xattrs_usr(m);
            collect_disk_tree(m.root(test_root), m.base, at_bp_usr);
            collect_disk_tree(m.etc_root(test_root), m.etc_base, at_bp_etc);
            throw LpkgException("injected copy failure");
        });
        const std::string e2 = install_err(v2_path);
        BreakpointManager::instance().clear_all();

        if (e2.empty()) {
            rep.fail("B:注入后 v2 竟然装成功（断点没接上）", m.seed,
                     repro(m, want_v1, want_v2,
                           "（断点命中=" + std::string(bp_hit ? "是" : "否") + "，异常为空）"));
            return;
        }

        // 归因计数：含 dir→非目录 的种子里，接管路径到底有没有被走到（见 Report 的说明）
        if (m.dir_replaced) {
            ++rep.replaced_seeds;
            if (bp_hit)
                ++rep.replaced_took_over;
            else
                ++rep.replaced_refused;
        }
        if (m.etc_dir_replaced) {
            if (bp_hit)
                ++rep.etc_dir_took;
            else
                ++rep.etc_dir_refused;
        }

        if (!bp_hit) {
            // 批次在拷贝阶段之前就被拒（整批预检）→ 本次**没有真的注入过失败**。
            // 这种情况一律不计通过。生成器已不往"会被非目录替换的目录里"塞无主文件
            // （plan_user_edits）⇒ 这个组里**不该**再出现"正确拒绝"这一来源，此数应当为 0
            // （见 CleanUpgrade_AllTransitions 的 EXPECT_EQ(b.not_exercised, 0)）。
            rep.note_not_exercised(m.seed);
            rep.not_exercised_note =
                "（生成器已不往「会被非目录替换的目录里」塞无主文件 ⇒ 本组不该再有「正确拒绝」"
                "这一来源；此数应当为 0，出现即说明批次在拷贝阶段之前被**别的原因**拒了）";
            const auto got = summary_of_disk(m.root(test_root));
            const auto got_etc = etc_disk(m);
            if (got != baseline || got_etc != etc_baseline) {
                rep.fail("B:未进拷贝阶段却动了盘（**新发现**）", m.seed,
                         repro(m, want_v1, want_v2,
                               render_diff(diff_summaries(baseline, got)) +
                                   repro_etc(m, etc_baseline, {},
                                             render_diff(diff_summaries(etc_baseline, got_etc))) +
                                   "异常原文：" + e2));
            } else if (!m.dir_replaced) {
                // `/etc` 侧含这一格时也要报（`/etc` 的 dir→非目录 今天应当接管成功，被拒=新问题）
                const std::string side =
                    m.etc_dir_replaced
                        ? "B:注入未生效但本种子含 /etc 的 dir→非目录（该格今天应当"
                          "接管成功，被拒即新问题）"
                        : "B:注入未生效且本种子**不含** dir→非目录（⇒ 拷贝阶段为何没到？"
                          "新发现）";
                rep.fail(side, m.seed, repro(m, want_v1, want_v2, "异常原文：" + e2));
            }
            return;
        }

        // ④ 回滚保真：盘面逐项回到"改动后的 v1"
        //    —— 前提是这一步之前**盘面确实被改过**（否则这条断言是恒真的废话，见 at_breakpoint）
        const auto got_rb = summary_of_disk(m.root(test_root));
        const auto got_rb_etc = etc_disk(m);
        if (at_breakpoint_etc != etc_baseline) ++rep.etc_dirty_at_breakpoint;
        if (at_breakpoint == baseline) {
            rep.fail("B:断点命中时盘面与基线逐项相同（回滚无物可撤 ⇒ 本种子是空转，不算通过）",
                     m.seed, repro(m, want_v1, want_v2, "异常原文：" + e2));
            return;
        }
        const SummaryDiff d_usr = diff_summaries(baseline, got_rb);
        const SummaryDiff d_etc = diff_summaries(etc_baseline, got_rb_etc);
        if (!d_usr.empty() || !d_etc.empty()) {
            rep.fail("B:回滚后盘面 != 改动后的 v1（回滚保真失败）", m.seed,
                     repro(m, want_v1, want_v2,
                           render_diff(d_usr) + repro_etc(m, etc_baseline, {}, render_diff(d_etc)) +
                               "异常原文：" + e2));
            return;
        }
        // ④′ xattr 也要**逐字节**回到改动后的 v1（含"键原本不存在 ⇒ 回滚后必须仍不存在"）
        //
        // 订正 2026-09-26：这一格原先带一条**窄口径宽容**，用来量化一个当时未修的真缺口 ——
        // `remove_empty_dir` 写的 `DIR_RM <path> <mode> <uid> <gid>` **不含 xattr**，而回滚侧的
        // `Undo::RecreateDir` 只 `create_directories` + `lchown`/`chmod` ⇒ **被 rmdir 又重建的
        // 目录丢掉那份 ACL / SELinux 标签**（本轮新加的 xattr 维度当场抓到，实测 2/32 种子）。
        // **缺口已修**：`remove_empty_dir` 现在在写 `DIR_RM` **之前**把该目录的 xattr 逐键记成
        // `XATTR_SET` 行；回滚是逆序的 ⇒ 先 `RecreateDir` 再把键写回去。宽容整段删除，恢复严格比。
        const XMap got_rb_x_usr = disk_xattrs_usr(m);
        const XMap got_rb_x_etc = disk_xattrs_etc(m);
        // 绊线：**任何**基线里有 xattr 的目录在回滚后丢键都记一次 —— 修好之后应**恒为 0**。
        // 它不是断言（严格比在下面几行），而是"缺口回潮"时能一眼看见的那个数：断言会红，
        // 而这个读数会告诉我们"红的正是那一类"。
        bool lost_any = false;
        for (const auto& [path, kv] : x_baseline_usr) {
            if (kv.empty()) continue;
            const auto g = got_rb_x_usr.find(path);
            if (g == got_rb_x_usr.end() || g->second.size() < kv.size()) lost_any = true;
        }
        for (const auto& [path, kv] : x_baseline_etc) {
            if (kv.empty()) continue;
            const auto g = got_rb_x_etc.find(path);
            if (g == got_rb_x_etc.end() || g->second.size() < kv.size()) lost_any = true;
        }
        if (lost_any) ++rep.xattr_dir_rm_lost;
        const std::string dx_rb = xattr_rollback_problems(x_baseline_usr, got_rb_x_usr) +
                                  xattr_rollback_problems(x_baseline_etc, got_rb_x_etc);
        if (!dx_rb.empty()) {
            rep.fail("B:回滚后 xattr != 改动后的 v1（xattr 回滚保真失败）", m.seed,
                     repro(m, want_v1, want_v2, dx_rb + "异常原文：" + e2));
            return;
        }
        if (at_breakpoint_x != x_baseline_usr) ++rep.xattr_dirty_at_breakpoint;
        ++rep.etc_rolled_back;
        const std::string ver_after = installed_version(m.pkg);
        if (ver_after != "1.0") {
            rep.fail("B:回滚后 DB 版本不是 1.0（实际 \"" + ver_after + "\"）", m.seed,
                     repro(m, want_v1, want_v2));
            return;
        }
        if (bak_residue() != 0) {
            rep.fail("B:回滚后仍有 .lpkg_bak_ 残留", m.seed, repro(m, want_v1, want_v2));
            return;
        }

        // ⑤ 回滚后再装 v2 必须成功，且 v2 的条目全部就位。
        //    这里只要求"v2 ⊆ 盘面"（子集）而不是相等：用户改动在**保留的目录**里可能合法留下
        //    （无人持有的文件、废弃目录因非空而保留），那不属于回滚/重试的错。
        //    **`/etc` 侧则要求"终态 == 模型"**（不是子集）：`/etc` 的例外全都是**确定的**
        //    （三哈希的判定输入在重试时与首次升级逐字相同：盘面 = 回滚后的基线、confhashes =
        //      v1 的记录 —— `install_packages` 入口会 `Cache::load()` 把失败尝试的内存改动丢掉），
        //    所以终态必须是模型算出来的那一份，不能用子集糊过去。
        const std::string e3 = install_err(v2_path);
        if (!e3.empty()) {
            rep.fail("B:回滚后重试装 v2 失败", m.seed,
                     repro(m, want_v1, want_v2, "重试异常原文：" + e3));
            return;
        }
        const auto got_retry = summary_of_disk(m.root(test_root));
        const SummaryDiff missing = diff_summaries(want_v2, got_retry);
        if (!missing.only_expected.empty()) {
            rep.fail("B:重试装 v2 成功但盘上缺 v2 的条目", m.seed,
                     repro(m, want_v1, want_v2, render_diff(missing)));
            return;
        }
        const EtcOutcome etc_retry = model_etc_after(m.etc1, edited_etc, m.etc2);
        const auto want_etc_retry = etc_summary(m, etc_retry.tree);
        const auto got_etc_retry = etc_disk(m);
        if (got_etc_retry != want_etc_retry) {
            rep.fail("B:重试装 v2 后 /etc 终态 != 模型", m.seed,
                     repro(m, want_v1, want_v2,
                           repro_etc(m, etc_baseline, want_etc_retry,
                                     render_diff(diff_summaries(want_etc_retry, got_etc_retry)))));
            return;
        }
        // 覆盖读数（来自模型；上面那条断言已把模型与盘面绑死）
        rep.etc_install_new += etc_retry.install_new;
        rep.etc_keep_local += etc_retry.keep_local;
        rep.etc_save_lpkgnew += etc_retry.save_lpkgnew;
        rep.etc_dir_takeover += etc_retry.dir_on_disk_replaced;

        // ⑤′ 安全不变量 a：用户改过的那份配置**永不静默丢失**（独立于模型的第二道网）
        const Tree post_disk = [&] {
            Tree t;
            collect_disk_tree(m.etc_root(test_root), m.etc_base, t);
            return t;
        }();
        const std::string lost = etc_lost_user_content_problems(post_disk, etc_edits);
        if (!lost.empty()) {
            rep.etc_lost_seeds.push_back(m.seed);
            rep.fail("B:/etc 用户改过的那份配置在盘上找不到了（**底线**：永不静默丢失）", m.seed,
                     repro(m, want_v1, want_v2, repro_etc(m, etc_baseline, want_etc_retry, lost)));
        }
        // ⑤‴ xattr 终态 == 模型（重试路径；`report_extra=false` —— 盘上可能合法多出
        //      "被保留的废弃目录"，那种目录的键另有针对性断言）
        {
            const std::string xp = xattr_problems(m, want_v2_tree, etc_retry.tree, false);
            if (!xp.empty()) {
                rep.fail("B:重试装 v2 后 xattr != 模型", m.seed, repro(m, want_v1, want_v2, xp));
                return;
            }
        }
        // ⑤″ 安全不变量 b：记录里记的永远是 hash_pkg
        const std::string hash_problems = etc_conf_hash_problems(m, "2.0", m.etc1);
        if (!hash_problems.empty()) {
            ++rep.etc_record_mismatch;
            rep.fail("B:/etc 的 confhashes 记录不是 hash_pkg（**底线**：绝不追认盘上那份）", m.seed,
                     repro(m, want_v1, want_v2, hash_problems));
        }
    }

    // ── 属性 C：卸载（本轮新增 —— 这是此前唯一缺的整条操作腿）────────────────────

    /**
     * 卸载之后的**期望终态**（模型 = `do_remove_package` 阶段 A/B + `remove_empty_owned_dirs`
     * 的合成）。政策逐条读自实现：
     *
     *   · `/etc` 的**非目录** owned 条目 → 改名 `<路径>.lpkgsave`（`SAVE_CONF`；只有
     *     `--purge-config` 才真删 —— 本属性用默认 false）；
     *   · 其余非目录 owned 条目 → 搬进 stash（**提交后**随 stash 一起真删）⇒ 盘上消失；
     *   · **目录** owned 条目 → 最深优先、**仅最后持有者**、必须是真目录、**此刻为空** 才 rmdir；
     *     非空就整树保留 —— 非空的三类来源：用户塞的无主文件、别名包/共享目录、以及**上一步
     *     改名出来的 `.lpkgsave`**（所以文件处置必须先于目录判空，顺序不可反）；
     *   · 合租槽位（另一个包也持有）→ 归属还在 ⇒ 整树保留（引用计数）；
     *   · xattr：见 `model_x_remove`（本包声明过的键逐个撤，别的包仍持有的留下）。
     *
     * ⚠️ 本模型只覆盖**种子根之内**的条目。归档里还带着 `usr/`、`usr/share/` 这些**祖先目录**
     * 条目（包同样持有它们），但它们恒有别的包/散落文件的内容 ⇒ 永远非空、永远保留，
     * 与"种子根之内"的比对无关（`collect_disk_tree` 也只收种子根之内）。
     */
    Tree model_remove_after(const SeedModel& m, const Tree& disk_before) const
    {
        Tree t = disk_before;
        const Tree owned = merge_trees(m.v1, m.etc1);
        const std::string etc_prefix = m.etc_base + "/";

        // ① 非目录 owned：`/etc` 改名保留，其余真删（模型里就是抹掉）
        for (const auto& [path, node] : owned) {
            if (node.kind == "dir") continue;
            if (!t.contains(path)) continue;  // 盘上本来就没有（用户删了 / 从未落位）
            if (path.starts_with(etc_prefix))
                model_save_config(t, path);
            else
                t.erase(path);
        }
        // ② owned 目录：最深优先（与实现同一判据：字符串长度降序）
        std::vector<std::string> dirs;
        for (const auto& [path, node] : owned)
            if (node.kind == "dir") dirs.push_back(path);
        std::ranges::sort(
            dirs, [](const std::string& x, const std::string& y) { return x.size() > y.size(); });
        for (const auto& d : dirs) {
            // 合租槽位**自己**由合租方持有 ⇒ 保留（引用计数）。但它的**子条目**仍归本包，
            // 上一步已按各自归属处置过（非目录的被删、目录的在这里按"空 & 最后持有者"判）——
            // 所以这里只跳 `share_dir` **本身**，不能跳整棵子树（踩过：跳子树会把
            // `C0/c1` 也留在模型里，与盘面不符 —— 那是模型的错，不是产品的错）。
            if (!m.share_dir.empty() && d == m.share_dir) continue;
            const auto it = t.find(d);
            if (it == t.end() || it->second.kind != "dir") continue;
            const std::string prefix = d + "/";
            bool occupied = false;
            for (const auto& [p, n] : t) {
                if (p.starts_with(prefix)) {
                    occupied = true;
                    break;
                }
            }
            if (!occupied) t.erase(d);
        }
        return t;
    }

    /**
     * 属性 C：**卸载**。与 A/B 同构的三件事：
     *   C1（干净卸载）—— 装 v1 → 随机用户改动 → 卸载 → 断言终态 == 模型 + 归属表清干净；
     *   C2（注入失败 → 回滚保真）—— 在 `rm_before_file_removal_` 注入 → 断言盘面（**含 xattr**）
     *      逐字节回到卸载前 + DB 版本仍是 v1 + 归属表回到卸载前。
     *
     * 注入点选 `rm_before_file_removal_`（阶段 A 的 BACKUP/改名已做完、文件删除前）：那一刻
     * 盘面**确实被改过**（文件已搬进 stash / `/etc` 已改名）⇒ "回滚后 == 基线"不是恒真废话
     * （与属性 B 的 `at_breakpoint != baseline` 同一条反腐）。
     */
    void seed_property_c(const SeedModel& m, Report& rep)
    {
        ++rep.ran_c;
        if (!m.co.empty()) {
            const std::string co_path = pack_tree(m.co_pkg, "1.0", m.co, {}, m.x_co);
            if (!install_err(co_path).empty()) {
                rep.fail("C:合租包装不上（脚手架问题）", m.seed, "（见 stderr）");
                return;
            }
        }
        const std::string v1_path =
            pack_tree(m.pkg, "1.0", merge_trees(m.v1, m.etc1), {}, merge_xmaps(m.x_v1, m.x_etc1));
        const std::string e1 = install_err(v1_path);
        if (!e1.empty()) {
            rep.fail("C:v1 装不上（脚手架/模型问题）", m.seed, "异常：" + e1);
            return;
        }
        if (installed_version(m.pkg) != "1.0") {
            rep.fail("C:v1 装完 DB 版本不是 1.0（脚手架问题）", m.seed, "（见 stderr）");
            return;
        }

        // 用户改动（与属性 B 同一套：模型与盘面各做一遍再绑死）
        SlotGen g;
        g.rng.seed(m.seed * 7919ULL + 13ULL);
        const auto edits = plan_user_edits(m, g);
        apply_edits_to_disk(test_root, edits);
        SlotGen g_etc;
        g_etc.rng.seed(m.seed * 104729ULL + 23ULL);
        const auto etc_edits = plan_etc_user_edits(m, g_etc);
        apply_edits_to_disk(test_root, etc_edits);

        // ── 基线：卸载**之前**的盘面（形态 + xattr）────────────────────────────
        Tree base_usr, base_etc;
        collect_disk_tree(m.root(test_root), m.base, base_usr);
        collect_disk_tree(m.etc_root(test_root), m.etc_base, base_etc);
        XMap base_x_usr, base_x_etc;
        snapshot_xattrs(m, base_x_usr, base_x_etc);

        // ── C2：注入失败 → 回滚 ────────────────────────────────────────────────
        bool bp_hit = false;
        Tree at_bp_usr;
        XMap at_bp_x;
        BreakpointManager::instance().set("rm_before_file_removal_" + m.pkg, [&] {
            bp_hit = true;
            collect_disk_tree(m.root(test_root), m.base, at_bp_usr);
            at_bp_x = disk_xattrs_usr(m);
            throw LpkgException("injected removal failure");
        });
        std::string rm_err;
        try {
            remove_packages({m.pkg}, /*force=*/false, /*purge_config=*/false);
        } catch (const LpkgException& e) {
            rm_err = e.what();
        } catch (const std::exception& e) {
            rm_err = std::string("（非 LpkgException）") + e.what();
        }
        BreakpointManager::instance().clear_all();
        if (rm_err.empty()) {
            rep.fail("C:注入后卸载竟然成功（断点没接上）", m.seed,
                     "（断点命中=" + std::string(bp_hit ? "是" : "否") + "，异常为空）");
            return;
        }
        if (m.share_dir.empty() && !m.xattr_revoked_paths.empty()) ++rep.xattr_revoked_seeds;
        if (!bp_hit) {
            // 预检拒绝（`check_removal_preconditions`）等 ⇒ 没真的进到文件操作。
            // 本组不该出现：生成器不造"共享**文件**"（合租只共用**目录**，而共享目录不拦卸载 ——
            // `check_removal_preconditions` 对目录键直接 `continue`），也没有别的包依赖。
            rep.note_not_exercised(m.seed);
            rep.not_exercised_note =
                "（卸载组里「注入未生效」应当恒为 0：生成器不造共享文件、不造反向依赖，"
                "`check_removal_preconditions` "
                "不该拒绝任何种子；出现即说明预检被**别的原因**拒了）";
            return;
        }
        // 反腐：断点时刻盘面必须**确实被改过**（否则"回滚后 == 基线"是恒真的废话）
        Tree got_rb_usr, got_rb_etc;
        collect_disk_tree(m.root(test_root), m.base, got_rb_usr);
        collect_disk_tree(m.etc_root(test_root), m.etc_base, got_rb_etc);
        if (summary_of_tree(at_bp_usr, m.base) == summary_of_tree(base_usr, m.base)) {
            rep.fail("C:断点命中时盘面与基线逐项相同（回滚无物可撤 ⇒ 本种子是空转，不算通过）",
                     m.seed, "异常原文：" + rm_err);
            return;
        }
        XMap got_x_usr, got_x_etc;
        snapshot_xattrs(m, got_x_usr, got_x_etc);
        // 回滚保真：形态 + xattr 都要回到基线
        const auto d_usr =
            diff_summaries(summary_of_tree(base_usr, m.base), summary_of_tree(got_rb_usr, m.base));
        const auto d_etc = diff_summaries(summary_of_tree(base_etc, m.etc_base),
                                          summary_of_tree(got_rb_etc, m.etc_base));
        const std::string dx = xmap_diff(base_x_usr, got_x_usr, "usr 侧") +
                               xmap_diff(base_x_etc, got_x_etc, "/etc 侧");
        if (!d_usr.empty() || !d_etc.empty() || !dx.empty()) {
            rep.fail("C:回滚后盘面（含 xattr）!= 卸载前（回滚保真失败）", m.seed,
                     render_diff(d_usr) + render_diff(d_etc) + dx + "异常原文：" + rm_err);
            return;
        }
        if (at_bp_x != base_x_usr) ++rep.xattr_dirty_at_breakpoint;
        if (installed_version(m.pkg) != "1.0") {
            rep.fail("C:回滚后 DB 版本不是 1.0（实际 \"" + installed_version(m.pkg) + "\"）",
                     m.seed, "（见 stderr）");
            return;
        }
        if (bak_residue() != 0) {
            rep.fail("C:回滚后仍有 .lpkg_bak_ 残留", m.seed, "（见 stderr）");
            return;
        }
        ++rep.c_rolled_back;

        // ── C1：干净卸载 ───────────────────────────────────────────────────────
        try {
            remove_packages({m.pkg}, /*force=*/false, /*purge_config=*/false);
        } catch (const std::exception& e) {
            rep.fail("C:卸载失败", m.seed, "异常原文：" + std::string(e.what()));
            return;
        }
        Tree got_usr, got_etc;
        collect_disk_tree(m.root(test_root), m.base, got_usr);
        collect_disk_tree(m.etc_root(test_root), m.etc_base, got_etc);
        const Tree want_usr = model_remove_after(m, base_usr);
        const Tree want_etc = model_remove_after(m, base_etc);
        const auto c_usr =
            diff_summaries(summary_of_tree(want_usr, m.base), summary_of_tree(got_usr, m.base));
        const auto c_etc = diff_summaries(summary_of_tree(want_etc, m.etc_base),
                                          summary_of_tree(got_etc, m.etc_base));
        if (!c_usr.empty() || !c_etc.empty()) {
            rep.fail(
                "C:卸载终态 != 模型（文件/目录/改名保留）", m.seed,
                render_diff(c_usr) + render_diff(c_etc) +
                    "    （模型依据：`do_remove_package` 阶段 A/B + `remove_empty_owned_dirs`）\n");
            return;
        }
        // xattr 终态：本包声明过的键全撤（别的包仍持有的留下）
        XMap got_x2_usr, got_x2_etc;
        snapshot_xattrs(m, got_x2_usr, got_x2_etc);
        const std::string xp = xmap_problems(model_x_remove(want_usr, m.x_v1, m.x_co), got_x2_usr,
                                             /*report_extra=*/true) +
                               xmap_problems(model_x_remove(want_etc, m.x_etc1, {}), got_x2_etc,
                                             /*report_extra=*/false);
        if (!xp.empty()) {
            rep.fail("C:卸载后 xattr != 模型（撤销漏了 / 撤了别人的键）", m.seed, xp);
            return;
        }
        // 合租共用键的**针对性**断言：另一个包仍持有 ⇒ 键必须还在盘上、且归属回到只有它
        if (!m.share_dir.empty()) {
            const auto it = got_x2_usr.find(m.share_dir);
            const bool key_on_disk = it != got_x2_usr.end() && it->second.contains(XK_SHARED);
            if (!key_on_disk) {
                rep.fail("C:合租共用键被撤掉了（**别的包仍持有** ⇒ 撤销必须不动盘）", m.seed,
                         "      " + m.share_dir + " 上期望仍有 " + XK_SHARED + "\n");
                return;
            }
            Cache::instance().load();
            const auto owners =
                Cache::instance().get_xattr_key_owners("/" + m.share_dir + "/", XK_SHARED);
            if (owners.contains(m.pkg) || !owners.contains(m.co_pkg)) {
                rep.fail("C:合租共用键的归属不对（本包该摘掉、合租方该留着）", m.seed,
                         "      实际属主集合大小=" + std::to_string(owners.size()) + "\n");
                return;
            }
            ++rep.shared_xattr_kept;
        }
        // 归属表清干净
        Cache::instance().load();
        auto& cache = Cache::instance();
        if (!cache.get_installed_version(m.pkg).empty()) {
            rep.fail("C:卸载后 DB 里仍有本包", m.seed, "（见 stderr）");
            return;
        }
        if (!cache.get_package_files(m.pkg).empty()) {
            rep.fail("C:卸载后 files.db 里仍留着本包的路径", m.seed, "（见 stderr）");
            return;
        }
        if (!cache.get_package_xattr_keys(m.pkg).empty()) {
            rep.fail("C:卸载后 xattrkeys.db 里仍留着本包的键归属", m.seed, "（见 stderr）");
            return;
        }
        for (const auto& [path, node] : m.etc1) {
            if (node.kind != "file") continue;
            if (!cache.get_conf_hash("/" + path, m.pkg).empty()) {
                rep.fail("C:卸载后 confhashes 里仍留着本包的记录（记录必须随包走）", m.seed,
                         "      " + path + "\n");
                return;
            }
        }
        if (bak_residue() != 0) {
            rep.fail("C:卸载提交后仍有 .lpkg_bak_ 残留", m.seed, "（见 stderr）");
            return;
        }
        ++rep.c_cleaned;
    }

    /// 跑一组种子（两个属性各一遍；两条属性各占一套包名/路径前缀，见 make_model 的 ns）
    void run_group(bool allow_dir_replaced, Report& a, Report& b, Report& c)
    {
        note_env_overrides();
        make_scatter();
        const int n = seed_count();
        int share_seeds = 0;         // 带"另一个包共享同一目录"的种子数（引用计数场景）
        int replaced_seeds = 0;      // 带 dir→非目录 迁移的种子数（接管路径场景）
        int etc_replaced_seeds = 0;  // 带 **/etc** 的 dir→非目录 迁移的种子数（缺陷形状）
        auto run_one = [&](std::uint64_t seed) {
            const SeedModel ma = make_model(seed, allow_dir_replaced, "a");
            if (!ma.share_dir.empty()) ++share_seeds;
            if (ma.dir_replaced) ++replaced_seeds;
            if (ma.etc_dir_replaced) ++etc_replaced_seeds;
            // xattr 维度的覆盖读数（模型事实；三条属性同构 ⇒ 记一次即可）
            if (ma.xattr_transitions_pinned) ++c.xattr_seeds;
            if (!ma.x_v1.empty()) ++c.xattr_dir_seeds;
            if (!ma.xattr_revoked_paths.empty()) ++c.xattr_revoked_seeds;
            seed_property_a(ma, a);
            seed_property_b(make_model(seed, allow_dir_replaced, "b"), b);
            seed_property_c(make_model(seed, allow_dir_replaced, "c"), c);
        };
        for (int i = 0; i < n; ++i) {
            const std::uint64_t seed = static_cast<std::uint64_t>(i);
            if (!seed_selected(seed)) continue;
            run_one(seed);
        }

        // ── 覆盖性兜底：保证"至少一个种子含 dir→非目录"（usr 与 `/etc` 各一条）────────
        // `EXPECT_GT(b.replaced_took_over, 0)` / `EXPECT_GT(b.etc_dir_took, 0)` 是反假绿读数
        // （防"绿是空转出来的"）。种子数调小之后，枚举范围内可能一个 dir→非目录 都没有 ——
        // 那时那两条断言就会**因为覆盖变空而红**，而不是因为代码坏了。所以这里显式补：
        // 继续往后取种子直到两条都出现（上限 kCoverageProbe 个）。种子序列是确定性的
        // （`make_model(seed)` 只由 seed 派生），因此补哪几个种子也是确定的 —— 不是"碰运气"，
        // 是"结构性保证覆盖"。**两片树都要探**：/etc 的那一格是最近那处缺陷的形状，
        // 只兜 usr 侧等于把新维度交给运气（加种子不解决覆盖 —— 见文件头那条结论）。
        // 单种子复现模式（LPKG_PROP_SEED）下**不补**：那时用户要的就是"只跑这一个"。
        // **只在"允许 dir→非目录"的那一组兜底**：受限组（allow_dir_replaced=false）按构造
        // 永远不会有这一格（usr 与 /etc 都禁），探下去是白跑（实测：不加这个守卫，受限组会
        // 多跑 64 个种子 ≈ 100 s，而且探到上限也不会有结果）。
        constexpr int kCoverageProbe = 64;
        if (allow_dir_replaced && (replaced_seeds == 0 || etc_replaced_seeds == 0) &&
            !single_seed_mode()) {
            // 缺的是哪一侧要在**补跑之前**记下来（跑完两侧都 > 0，再算就算不出来了）
            std::string missing;
            if (replaced_seeds == 0) missing += "【usr 侧】";
            if (etc_replaced_seeds == 0) missing += "【/etc 侧】";
            for (int probe = n; probe < n + kCoverageProbe; ++probe) {
                run_one(static_cast<std::uint64_t>(probe));
                if (replaced_seeds > 0 && etc_replaced_seeds > 0) {
                    std::cerr << "[prop] 覆盖性兜底：枚举的 " << n << " 个种子里" << missing
                              << "没有 dir→非目录，补跑到 seed=" << probe
                              << " 才覆盖到（这是**确定性**的，不是碰运气）\n";
                    break;
                }
            }
        }
        // ── xattr 维度的覆盖性兜底 ──────────────────────────────────────────────
        // 与上面同一条纪律：`pin_xattr_transitions` 只在"该种子有**两版都是目录**的路径"时才
        // 钉得成，而"四迁移覆盖非空"是安全性质（陈旧 `posix_acl_default` 会继续放权限）
        // ⇒ 枚举范围内一个都没钉到就继续往后取种子（确定性，不是碰运气）。
        // 单种子复现模式下不补（用户要的就是"只跑这一个"）。
        if ((c.xattr_seeds == 0 || c.xattr_dir_seeds == 0 || c.xattr_revoked_seeds == 0) &&
            !single_seed_mode()) {
            std::string missing;
            if (c.xattr_seeds == 0) missing += "【四迁移未钉到】";
            if (c.xattr_dir_seeds == 0) missing += "【没有任何目录 xattr】";
            if (c.xattr_revoked_seeds == 0) missing += "【撤销趟无事可做】";
            for (int probe = n; probe < n + kCoverageProbe; ++probe) {
                run_one(static_cast<std::uint64_t>(probe));
                if (c.xattr_seeds > 0 && c.xattr_dir_seeds > 0 && c.xattr_revoked_seeds > 0) {
                    std::cerr << "[prop] xattr 覆盖性兜底：枚举的 " << n << " 个种子里" << missing
                              << "，补跑到 seed=" << probe << " 才覆盖到（确定性，不是碰运气）\n";
                    break;
                }
            }
        }
        std::cerr << "[prop] " << a.title << "：种子数=" << a.ran << " 失败=" << a.failures.size()
                  << " / " << b.title << "：失败=" << b.failures.size()
                  << " 注入未生效=" << b.not_exercised << "（其中带合租目录的种子=" << share_seeds
                  << "，带 dir→非目录 的种子=" << replaced_seeds
                  << "，带 /etc 的 dir→非目录 的种子=" << etc_replaced_seeds << "）\n";
        std::cerr << "[prop-etc] /etc 维度读数：三哈希 InstallNew="
                  << a.etc_install_new + b.etc_install_new
                  << " KeepLocal=" << a.etc_keep_local + b.etc_keep_local
                  << " SaveLpkgnew=" << a.etc_save_lpkgnew + b.etc_save_lpkgnew
                  << "；dir→非目录 接管(格)=" << a.etc_dir_takeover + b.etc_dir_takeover
                  << "；属性 B：/etc 的 dir→非目录 真的走到拷贝阶段=" << b.etc_dir_took
                  << " 拷贝前被拒=" << b.etc_dir_refused
                  << "，断点时刻 /etc 已被改动=" << b.etc_dirty_at_breakpoint
                  << "，回滚保真=" << b.etc_rolled_back
                  << "，不变量 a 破=" << b.etc_lost_seeds.size()
                  << "，不变量 b 破=" << a.etc_record_mismatch + b.etc_record_mismatch << "\n";
        std::cerr << "[prop-xattr] xattr 维度读数：四迁移钉到的种子=" << c.xattr_seeds
                  << "，有目录 xattr 的种子=" << c.xattr_dir_seeds
                  << "，v2 声明集变小（撤销有事可做）的种子=" << c.xattr_revoked_seeds
                  << "；属性 B/C 断点时刻 xattr 已被改动="
                  << b.xattr_dirty_at_breakpoint + c.xattr_dirty_at_breakpoint
                  << "；属性 C：合租共用键保住的种子=" << c.shared_xattr_kept
                  << "；绊线「回滚后目录丢 xattr」命中种子（应恒 0）="
                  << b.xattr_dir_rm_lost + c.xattr_dir_rm_lost << "\n";
        std::cerr << "[prop-uninstall] 属性 C（卸载）读数：种子数=" << c.ran_c
                  << " 失败=" << c.failures.size() << "，注入未生效=" << c.not_exercised
                  << "（应当恒 0），回滚保真=" << c.c_rolled_back
                  << "，干净卸载终态==模型+归属表清干净=" << c.c_cleaned << "\n";
    }
};

// ============================================================================
// 完整随机（含 dir→文件 / dir→符号链接）：dir→非目录 现在**走接管**（整树归本包/本批次）
// ============================================================================

/**
 * 属性 A 全量：**含** dir→非目录。整批预检的条件是 `dir_tree_entirely_ours`（pacman 的
 * `dir_belongsto_pkgs`）—— 种子里那些目录整棵都属于本包（用户改动只改我们自己的条目，
 * 见 plan_user_edits）⇒ **接管成功**，属性 A 应当全绿。
 * 拒绝只可能来自"树里有别的包/无主的条目"，那一类由固定用例
 * DirReplacedByNonDirWithUnownedContentIsRefused 单独钉（本文件不应再有"正确拒绝"的种子）。
 */
TEST_F(UpgradePropertyTest, CleanUpgrade_AllTransitions)
{
    Report a{"属性A 干净升级（全量形态）"}, b{"属性B 回滚保真（全量形态）"},
        c{"属性C 卸载（全量形态）"};
    run_group(/*allow_dir_replaced=*/true, a, b, c);
    EXPECT_TRUE(a.failures.empty()) << a.message();
    EXPECT_TRUE(b.failures.empty()) << b.message();
    // 属性 B 不许空转：每个种子都必须真的走到"接管/拷贝阶段"让断点命中。
    // 全量组里 dir→非目录 已不再是"预检即拒"的来源（整树归本包就接管，生成器也不再往
    // 会被非目录替换的目录里塞无主文件）⇒ 这个数应当为 0；出现即批次在拷贝前被别的原因
    // 拒了，那是**新问题**，不能靠"计入 not_exercised 就不了了之"。
    EXPECT_EQ(b.not_exercised, 0) << "有种子没走到拷贝阶段（注入未生效）—— 属性 B 对它们是空转的"
                                     "（正确拒绝的那一类已被生成器剔除，见 plan_user_edits）"
                                  << b.message();
    // 正面读数：含 dir→非目录 的种子必须大量真的走接管路径（否则"绿"是空转出来的）
    EXPECT_GT(b.replaced_took_over, 0)
        << "没有任何含 dir→非目录 的种子真的走到接管路径 —— 本组覆盖已变空";
    // ── `/etc` 维度的覆盖非空（本轮新增；这就是"加维度"换来的探测力）──────────────
    EXPECT_GT(a.etc_seeds, 0) << "`/etc` 维度一个种子都没覆盖到";
    EXPECT_GT(b.etc_dir_took, 0)
        << "没有任何 `/etc` 的「盘上真目录 → 新版本文件/链接」真的走到拷贝阶段 ——"
           "本仓库最近最大的缺陷（两条腿都装不上）就在这一格，覆盖不能为空"
        << b.message();
    EXPECT_GT(b.etc_keep_local, 0)
        << "三哈希 ②（KeepLocal，用户改过 + 包没改这个配置）一次都没走到 —— 覆盖已变空";
    EXPECT_GT(b.etc_save_lpkgnew, 0) << "三哈希 ③（落 `.lpkgnew`）一次都没走到 —— 覆盖已变空";
    EXPECT_GT(b.etc_dirty_at_breakpoint, 0)
        << "断点时刻 `/etc` 与基线一字不差 ⇒ `/etc` 的回滚保真是空转的（回滚没有 /etc 的活要干）";
    EXPECT_EQ(b.etc_rolled_back, b.ran)
        << "有种子没走到「/etc 回滚保真」这一条（它前面某条断言先失败了，见上面的失败台账）";
    EXPECT_TRUE(b.etc_lost_seeds.empty())
        << "有种子破了「用户改过的配置永不静默丢失」这条底线：" << b.message();
    EXPECT_EQ(a.etc_record_mismatch + b.etc_record_mismatch, 0)
        << "有种子破了「记录永远写 hash_pkg、绝不追认盘上那份」这条底线：" << a.message()
        << b.message();

    // ── 属性 C（卸载）：本仓库此前**唯一缺的整条操作腿**的覆盖非空 ──────────────
    EXPECT_TRUE(c.failures.empty()) << c.message();
    EXPECT_EQ(c.not_exercised, 0)
        << "卸载组有种子没走到文件操作（注入未生效）—— 生成器不造共享**文件**、不造反向依赖，"
           "`check_removal_preconditions` 不该拒绝任何种子"
        << c.message();
    EXPECT_GT(c.c_rolled_back, 0) << "「卸载注入失败 → 回滚保真」一次都没走到 —— 覆盖已变空";
    EXPECT_GT(c.c_cleaned, 0) << "「干净卸载终态 == 模型 + 归属表清干净」一次都没走到";
    EXPECT_GT(c.shared_xattr_kept, 0)
        << "合租共用键的安全生产（别的包仍持有 ⇒ 撤销**必须不动盘**）一次都没走到 ——"
           "按目录判、或按「本包不再声明」判，都会撤掉别人的键，这一格必须被考到";
    // ── xattr 维度的覆盖非空（这就是"加维度"换来的探测力）────────────────────
    EXPECT_GT(c.xattr_seeds, 0) << "xattr 的四种迁移一个种子都没钉到 —— 覆盖已变空";
    EXPECT_GT(c.xattr_dir_seeds, 0) << "没有任何种子带目录 xattr —— 覆盖已变空";
    EXPECT_GT(c.xattr_revoked_seeds, 0)
        << "没有任何种子让 v2 的声明集变小 ⇒ 撤销趟无事可做 ——「新版本不再声明的键必须撤」"
           "（陈旧 `posix_acl_default` 会继续放权限）这条安全性质没被考到";
    EXPECT_GT(b.xattr_dirty_at_breakpoint + c.xattr_dirty_at_breakpoint, 0)
        << "断点时刻 xattr 与基线一字不差 ⇒ xattr 的回滚保真是空转的（回滚没有 xattr 的活要干）";
}

/**
 * 属性 A 受限：v1 是目录的路径在 v2 里必须还是目录 ⇒ 绕开**接管路径**那一格。
 * 这一组与全量组今天都应当是绿的；它红了 = 通用路径（类型变更/废弃/新增/共享目录/
 * 回滚保真）上有新漏格。
 */
TEST_F(UpgradePropertyTest, CleanUpgrade_NoDirReplaced)
{
    Report a{"属性A 干净升级（禁 dir→非目录）"}, b{"属性B 回滚保真（禁 dir→非目录）"},
        c{"属性C 卸载（禁 dir→非目录）"};
    run_group(/*allow_dir_replaced=*/false, a, b, c);
    EXPECT_TRUE(a.failures.empty()) << a.message();
    EXPECT_TRUE(b.failures.empty()) << b.message();
    // 这一组禁掉了 dir→非目录 ⇒ 注入必须**每一个种子都生效**。
    // 这条断言是本文件对"假绿"的正面防线：注入没生效就不算考过。
    EXPECT_EQ(b.not_exercised, 0)
        << "本组禁用了 dir→非目录，却仍有种子没走到拷贝阶段（注入未生效）——"
           "「注入生效」这条前提被破坏了，属性 B 对这些种子是空转的";
    // `/etc` 这一组里仍必须有：三哈希的三条分支（它们与 dir→非目录 无关，见 gen_etc_model）
    EXPECT_GT(a.etc_seeds, 0);
    EXPECT_GT(b.etc_keep_local, 0) << "三哈希 KeepLocal 没被走到（本组`/etc` 覆盖已变空）";
    EXPECT_GT(b.etc_save_lpkgnew, 0) << "三哈希 `.lpkgnew` 没被走到（本组`/etc` 覆盖已变空）";
    EXPECT_GT(b.etc_dirty_at_breakpoint, 0) << "断点时刻 `/etc` 没被动过 ⇒ /etc 回滚保真空转";
    EXPECT_EQ(b.etc_rolled_back, b.ran)
        << "有种子没走到「/etc 回滚保真」这一条（见上面的失败台账）";
    EXPECT_TRUE(b.etc_lost_seeds.empty())
        << "有种子破了「用户改过的配置永不静默丢失」这条底线：" << b.message();
    // 本组禁 dir→非目录 ⇒ `/etc` 的那一格也按构造不存在（gen_etc_model 与 usr 侧同一判据）
    EXPECT_EQ(b.etc_dir_took, 0) << "本组禁用了 dir→非目录，`/etc` 侧却出现了这一格";
    // ── 属性 C 与 xattr 维度在本组同样必须非空（它们与 dir→非目录 无关）──────────
    EXPECT_TRUE(c.failures.empty()) << c.message();
    EXPECT_EQ(c.not_exercised, 0) << "卸载组有种子没走到文件操作（注入未生效）" << c.message();
    EXPECT_GT(c.c_rolled_back, 0) << "卸载回滚保真一次都没走到";
    EXPECT_GT(c.c_cleaned, 0) << "干净卸载终态一次都没走到";
    EXPECT_GT(c.xattr_seeds, 0) << "xattr 四迁移没被钉到（本组覆盖已变空）";
    EXPECT_GT(c.xattr_revoked_seeds, 0) << "撤销趟无事可做（本组 xattr 覆盖已变空）";
}

// ============================================================================
// 固定用例：dir → 非目录 的**正确拒绝**（随机组已不覆盖，避免与"注入未生效"混淆）
// ============================================================================

/**
 * 「目录含**无主内容** + 该目录要被**非目录**替换 ⇒ 整批拒绝、盘面一字未动」。
 *
 * 这是 `dir_tree_entirely_ours`（pacman 的 `conflict.c: dir_belongsto_pkgs`）的**反面**：
 * 放行条件是"整棵子树都归本包 / 本批次升级的包"，只要树里有**一个**无人持有（或属于别的包）
 * 的条目就判否 —— 整树搬走会毁掉那个条目，pacman 的语义就是宁可拒绝升级、让用户自己处理。
 * 这里用**无人持有**的（用户往包里那个目录塞的文件）；"别的包持有"那一腿由
 * test_type_transition_matrix.cpp 的 DirToFileWithForeignPackageContentIsRefused 钉。
 *
 * 断言链（缺一不可，否则"拒绝"与"接管被写坏了"分不开）：
 *   ① 被拒 + 报错点名该路径与 `error.unknown_manual_file`（**真实**冲突源）；
 *   ② 盘面逐项零改动（含那个无主文件、含 v2 的伴生文件没落、含无 stash 残留）、DB 版本不变；
 *   ③ **把那个无主文件移走之后，同一次升级必须成功** —— 这一条把"拒绝的原因"钉死在那个
 *      文件上：否则"被拒"也可能只是接管路径整个坏了，而 ① ② 都照样满足。
 */
TEST_F(UpgradePropertyTest, DirReplacedByNonDirWithUnownedContentIsRefused)
{
    note_env_overrides();

    const std::string pkg = "pfx_unowned";
    const std::string base = "usr/share/prop/fixed/unowned";
    const std::string dir_path = base + "/S0";
    const std::string companion = base + "/companion.txt";
    const std::string unowned = dir_path + "/user-note.txt";

    // v1：目录 S0（含我们自己一个文件）+ 恒在的伴生普通文件（保证断点可达）
    Tree v1;
    v1[dir_path + "/a.txt"] = Node{"file", "v1 a\n"};
    v1[companion] = Node{"file", "companion v1\n"};
    // v2：S0 变成**普通文件**（dir → 非目录），伴生文件换内容
    Tree v2;
    v2[dir_path] = Node{"file", "v2 now a file\n"};
    v2[companion] = Node{"file", "companion v2\n"};

    const std::string v1_path = pack_tree(pkg, "1.0", v1);
    const std::string e1 = install_err(v1_path);
    ASSERT_TRUE(e1.empty()) << "前置：v1 装不上：" << e1;
    ASSERT_TRUE(fs::is_directory(test_root / dir_path));

    // ② 用户改动留痕：往包里那个目录塞一个**无人持有**的文件
    write_text(test_root / unowned, "user note\n");
    const auto baseline = summary_of_disk(test_root / base);

    // ③ 升级必须被拒 —— 且拒绝的理由要点名**那个真实冲突源**
    const std::string v2_path = pack_tree(pkg, "2.0", v2);
    const std::string msg = install_err(v2_path);
    ASSERT_FALSE(msg.empty())
        << "目录树里有无人持有的条目 ⇒ dir→非目录 必须被拒绝（整树搬走会毁掉那个文件）";
    std::cerr << "[prop-fixed] " << pkg << " 拒绝原文：" << msg << "\n";
    EXPECT_NE(msg.find(dir_path), std::string::npos) << "拒绝信息没点名冲突路径：" << msg;
    EXPECT_NE(msg.find(get_string("error.unknown_manual_file")), std::string::npos)
        << "无主条目 → 判据必须是 " << get_string("error.unknown_manual_file") << "：" << msg;

    // ④ 盘面逐项零改动（整批预检在**进事务之前**拒绝）+ DB 版本不变 + 无 stash 残留
    const SummaryDiff d = diff_summaries(baseline, summary_of_disk(test_root / base));
    EXPECT_TRUE(d.empty()) << "被拒时盘面必须一字未动：\n" << render_diff(d);
    EXPECT_EQ(read_text(test_root / unowned), "user note\n") << "无主文件必须原样还在";
    EXPECT_EQ(installed_version(pkg), "1.0") << "被拒后 DB 版本不该变";
    EXPECT_EQ(bak_residue(), 0) << "拒绝发生在进事务之前 → 不该有任何 stash 残留";

    // ⑤ 把那个无主文件移走 → 同一次升级必须成功（否则"被拒"的归因就不成立）
    std::error_code ec;
    fs::remove(test_root / unowned, ec);
    ASSERT_FALSE(fs::exists(test_root / unowned));
    const std::string e2 = install_err(v2_path);
    if (!e2.empty()) std::cerr << "[prop-fixed] " << pkg << " 移走无主文件后仍失败：" << e2 << "\n";
    EXPECT_TRUE(e2.empty()) << "无主文件已移走（整棵树又全归本包）⇒ dir→非目录 应当接管成功："
                            << e2;
    EXPECT_TRUE(fs::is_regular_file(test_root / dir_path)) << "S0 应被接管成普通文件";
    EXPECT_FALSE(fs::is_directory(test_root / dir_path));
    EXPECT_EQ(read_text(test_root / companion), "companion v2\n");
    EXPECT_EQ(installed_version(pkg), "2.0");
    EXPECT_EQ(bak_residue(), 0) << "提交后不该有 .lpkg_bak_ 残留";
}

// ============================================================================
// /etc 前缀：固定组合，**记录**行为差异（不要求满足属性 A/B 的一般形态）
// ============================================================================

/** `/etc` 的 9 种组合（v1 形态 → v2 形态）+ 一条"必然走 COPY 分支"的非 /etc 伴生文件 */
TEST_F(UpgradePropertyTest, EtcPrefixCombinations_Recorded)
{
    note_env_overrides();
    make_scatter();

    const char* const kinds[3] = {"file", "dir", "symlink"};
    int idx = 0;
    for (const char* k1 : kinds) {
        for (const char* k2 : kinds) {
            const std::string pkg = "pe" + std::to_string(idx);
            const std::string thing = "etc/" + pkg + "/thing";
            // 伴生路径**逐组合唯一**：它由本组合的包独占，别的组合也发同一个路径就会判文件冲突
            const std::string companion = "usr/share/pe" + std::to_string(idx) + "/companion.txt";
            const fs::path etc_root = test_root / "etc" / pkg;

            // 伴生普通文件：断点只挂在普通文件的 COPY 分支上，而 /etc 条目可能整段走
            // "保留用户文件 / 落 .lpkgnew"（那条路**没有**断点）——有它才保证注入生效。
            const Tree v1_all = [&] {
                Tree t;
                if (std::string(k1) == "file")
                    t[thing] = Node{"file", "v1 conf\n"};
                else if (std::string(k1) == "dir")
                    t[thing + "/a.conf"] = Node{"file", "v1 a\n"};
                else
                    t[thing] = Node{"symlink", "thing-target.conf"};
                t[companion] = Node{"file", "companion v1\n"};
                return t;
            }();
            const Tree v2_all = [&] {
                Tree t;
                if (std::string(k2) == "file")
                    t[thing] = Node{"file", "v2 conf\n"};
                else if (std::string(k2) == "dir")
                    t[thing + "/b.conf"] = Node{"file", "v2 b\n"};
                else
                    t[thing] = Node{"symlink", "thing-target.conf"};
                t[companion] = Node{"file", "companion v2\n"};
                return t;
            }();

            const std::string v1_path = pack_tree(pkg, "1.0", v1_all);
            const std::string e1 = install_err(v1_path);
            ASSERT_TRUE(e1.empty()) << pkg << "：v1 装不上：" << e1;
            const auto after_v1 = summary_of_disk(etc_root);

            // 用户改动（与属性 B 同款：文件改内容 / 目录加文件 / 链接换目标）
            if (std::string(k1) == "file")
                write_text(test_root / thing, "user edited\n");
            else if (std::string(k1) == "dir")
                write_text(test_root / (thing + "/user-note.conf"), "user note\n");
            else {
                std::error_code ec;
                fs::remove(test_root / thing, ec);
                fs::create_symlink("thing-user.conf", test_root / thing);
            }
            const auto baseline = summary_of_disk(etc_root);

            // 注入失败 → 回滚保真（这条与 /etc 政策无关，是事务不变量，故照钉不误）
            const std::string v2_path = pack_tree(pkg, "2.0", v2_all);
            bool bp_hit = false;
            BreakpointManager::instance().set("copy_after_wal_" + pkg, [&bp_hit] {
                bp_hit = true;
                throw LpkgException("injected copy failure");
            });
            const std::string e2 = install_err(v2_path);
            BreakpointManager::instance().clear_all();
            const auto after_rollback = summary_of_disk(etc_root);

            std::cerr << "[etc-record] " << pkg << " " << k1 << "→" << k2
                      << " | 注入命中=" << (bp_hit ? "是" : "否")
                      << " | 注入后异常=" << (e2.empty() ? "(无：竟然成功)" : e2) << "\n";
            std::cerr << "[etc-record]   改动后(基准)：" << (baseline.empty() ? "(空)" : "") << "\n"
                      << render_lines(baseline);
            std::cerr << "[etc-record]   回滚后：\n" << render_lines(after_rollback);

            EXPECT_FALSE(e2.empty())
                << pkg << "：注入后 v2 装成功（" << k1 << "→" << k2 << "，注入没生效）";
            if (bp_hit) {
                const SummaryDiff d = diff_summaries(baseline, after_rollback);
                EXPECT_TRUE(d.empty()) << pkg << " " << k1 << "→" << k2
                                       << "：/etc 的注入失败回滚没有逐项回到改动后的 v1\n"
                                       << render_diff(d);
            } else {
                // 预检即拒（一个文件都没动）⇒ 必须与改动后**逐项一致**（这也是不变量）
                const SummaryDiff d = diff_summaries(baseline, after_rollback);
                EXPECT_TRUE(d.empty())
                    << pkg << " " << k1 << "→" << k2 << "：预检拒绝却动了 /etc 盘面\n"
                    << render_diff(d);
            }
            Cache::instance().load();
            EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0")
                << pkg << "：回滚后 DB 版本不是 v1";

            // 干净装 v2：**只记录**（/etc 政策差异：.lpkgsave / .lpkgnew / 废弃配置留在盘上）
            const std::string e3 = install_err(v2_path);
            Cache::instance().load();
            std::cerr << "[etc-record]   " << k1 << "→" << k2 << " 干净装 v2："
                      << (e3.empty() ? "成功" : ("失败：" + e3))
                      << " | DB 版本=" << Cache::instance().get_installed_version(pkg) << "\n";
            // 这一份摘要里既有 `<thing>` 本身，也有 /etc 政策产生的兄弟文件
            // （`<thing>.lpkgnew` / `<thing>.lpkgsave`）——差异因此可以直接对照出来
            std::cerr << "[etc-record]   v2 装后 /etc 子树：\n"
                      << render_lines(summary_of_disk(etc_root));
            if (e3.empty()) {
                Cache::instance().load();
                EXPECT_EQ(Cache::instance().get_installed_version(pkg), "2.0") << pkg;
            }
            std::cerr << "[etc-record]   v1 装后（对照）：\n" << render_lines(after_v1);
            ++idx;
        }
    }
}

// ============================================================================
// 单种子复现入口
// ============================================================================

/**
 * `LPKG_PROP_SEED=<n>` 时**只**跑那一个种子，四个组合（两个变体 × 属性 A/B）各一遍，并把
 * "模型 v1 / 模型 v2"逐条打出来。没设变量时整条用例 SKIP。
 * 用途：全量用例报 `seed=137` 红了之后，一条命令把那次失败原样重放：
 *   LPKG_PROP_SEED=137 ./build/run_tests --gtest_filter='UpgradePropertyTest.SingleSeedReplay'
 * （本用例对该种子**照常断言**：种子红它就红 —— 它就是"复现"本身，不是"跑一遍看看"。）
 */
TEST_F(UpgradePropertyTest, SingleSeedReplay)
{
    const char* v = std::getenv("LPKG_PROP_SEED");
    if (!v || !*v) GTEST_SKIP() << "未设置 LPKG_PROP_SEED —— 本用例只在复现时跑";
    const std::uint64_t seed = std::strtoull(v, nullptr, 10);
    make_scatter();

    for (const bool allow_dir_replaced : {true, false}) {
        const std::string tag = allow_dir_replaced ? "全量形态" : "禁 dir→非目录";
        std::cerr << "\n[prop-replay] seed=" << seed << " 变体=" << tag << "\n";
        note_env_overrides();
        // 与 run_group 一致：属性 A / B 各占一套包名与路径前缀（ns=a/b）。**不能**两条属性
        // 共用一个模型 —— 同一个 test_root 里 A 会把包升到 2.0，B 再用同一套路径装 v1 就成了
        // "2.0 降级回 1.0"，撞的是另一套语义（实测整批预检直接判 file conflict）。
        for (const char* ns : {"a", "b"}) {
            const bool is_a = (std::string(ns) == "a");
            const SeedModel m = make_model(seed, allow_dir_replaced, ns);
            std::cerr << "[prop-replay] 属性" << ns << "：包名=" << m.pkg << " 根=" << m.base
                      << "\n";
            std::cerr << "[prop-replay]   v1 模型：\n"
                      << render_lines(summary_of_tree(m.v1, m.base));
            std::cerr << "[prop-replay]   v2 模型：\n"
                      << render_lines(summary_of_tree(m.v2, m.base));
            if (!m.share_dir.empty())
                std::cerr << "[prop-replay]   合租目录=" << m.share_dir << "\n";
            // `/etc` 侧（本轮新增维度）：包内容 + 期望终态（模型）+ 当前盘面，三份一起打
            std::cerr << "[prop-replay]   /etc 包内容 v1（根=" << m.etc_base << "）：\n"
                      << render_lines(etc_summary(m, m.etc1));
            std::cerr << "[prop-replay]   /etc 包内容 v2：\n"
                      << render_lines(etc_summary(m, m.etc2));
            std::cerr << "[prop-replay]   /etc 期望终态（干净升级的模型）：\n"
                      << render_lines(etc_summary(m, model_etc_after(m.etc1, m.etc1, m.etc2).tree));
            std::cerr << "[prop-replay]   /etc 盘面（此刻）：\n" << render_lines(etc_disk(m));
            if (m.etc_dir_replaced) {
                std::cerr << "[prop-replay]   ★ 本种子含 /etc 的 dir→非目录：";
                for (const auto& p : m.etc_dir_replaced_paths) std::cerr << " " << p;
                std::cerr << "\n";
            }

            Report r{"replay 属性" + std::string(is_a ? "A" : "B")};
            if (is_a)
                seed_property_a(m, r);
            else
                seed_property_b(m, r);
            EXPECT_TRUE(r.failures.empty()) << r.message();
        }
    }
}
