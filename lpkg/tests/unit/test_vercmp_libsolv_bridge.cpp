/**
 * test_vercmp_libsolv_bridge.cpp — **版本桥的等价性闸门**
 *
 * 这条桥要保证一件事：**libsolv 的依赖匹配 == lpkg 的 `version_satisfies()`**。
 * 求解器按前者出方案、安装期按后者验收 —— 两者一旦不一致，就会出现
 *   · 假不满足：libsolv 找不到提供者 ⇒ 事务无解（明明装得上却报缺依赖）；
 *   · 假满足：libsolv 认为满足、装到一半被安装期判据拒掉（`error.dep_version_mismatch`）
 *     整批回滚；或更糟 —— 求解器选了个 lpkg 认为不满足的版本，而校验恰好被绕过。
 *
 * 本文件把 (候选版本 × 约束) 的**整个矩阵**逐一比对，两边结论必须处处相等。
 * 这是 `to_libsolv_evr()` 那套编码的回归闸门：换编码、改归一化、动 libsolv 版本，
 * 只要语义漂了，这里立刻红。
 *
 * 历史：2026-10-03 之前，`+release` 被映射进 libsolv 的 **release 槽位**（`+`→`-`），
 * 而 libsolv 的依赖匹配走 `EVRCMP_MATCH_RELEASE`（`src/pooldep.c` 的
 * `pool_intersect_evrs`）—— 它把"有一侧没有 release"当**通配**（±2 两个特例分支）。
 * 同一矩阵上实测 **78 处**不一致，**两个方向都有**（`= 1.0` 匹配 `1.0+1`；`> 1.0+1`
 * 反过来匹配 `1.0`；`>= 1.0` 又不匹配 `1.0+1`）。现在用 `^`（caret）作 release 分隔符，
 * 全串不含 `-` ⇒ 那些特例分支不可能触发，矩阵上不一致数 = 0。
 */

#include <gtest/gtest.h>

// libsolv 的头文件把 `requires` 当结构体成员名，而它在 C++20 里是关键字 ——
// `solver.cpp` 就是这么绕的（同一个变通，别在这里发明第二种）。
#define requires solv_requires
#include <solv/evr.h>
#include <solv/pool.h>
#include <solv/repo.h>

#include <cstddef>
#include <string>
#include <vector>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/vercmp/version.hpp"

namespace
{

/**
 * 语料：真实索引里抽的版本 + 边界样本。
 * 真实样本覆盖了 `+release`（807 个版本里有 `+`）、多位版本号、以及预发布。
 */
const std::vector<std::string>& corpus()
{
    static const std::vector<std::string> v = {
        "0.0.2+1",  "1.0",      "1.0+0",     "1.0+1",    "1.0+1+2",    "1.0+2",
        "1.0+rc1",  "1.0-rc1",  "1.0-rc1+2", "1.0.0+5",  "1.0.1",      "1.0-a",
        "1.0_beta", "1.86.0+4", "10.0",      "13.0.2+2", "13.2.1+1",   "2.0+1",
        "261.2+3",  "261+3",    "6.12",      "9.9",      "20240101+1", "1.2.3",
    };
    return v;
}

struct RelOp {
    const char* text;
    int flags;  ///< 与 `solver.cpp` 的 `rel_op()` 逐条对应
};

const std::vector<RelOp>& ops()
{
    static const std::vector<RelOp> v = {
        {"=", REL_EQ},           {"==", REL_EQ}, {"!=", REL_GT | REL_LT}, {">", REL_GT},
        {">=", REL_EQ | REL_GT}, {"<", REL_LT},  {"<=", REL_EQ | REL_LT},
    };
    return v;
}

}  // namespace

TEST(VersionBridge, LibsolvDependencyMatchingEqualsVersionSatisfies)
{
    // 建池：语料里每个版本一个 solvable，**自提供 `name = evr`** ——
    // 与 `solver.cpp` 的 `build_pool()` 逐字同款（自提供不带版本会让"lib >= 2.0"被 lib 1.0 满足）。
    Pool* pool = pool_create();
    // libsolv 的默认 disttype 就是 RPM（`pool.c` 的 `#else` 分支），与 lpkg 实际跑的池一致。
    pool_setdisttype(pool, DISTTYPE_RPM);
    Repo* repo = repo_create(pool, "corpus");
    const Id name = pool_str2id(pool, "bridge-pkg", 1);
    const auto& vers = corpus();
    std::vector<Id> evr_ids;
    evr_ids.reserve(vers.size());
    for (const auto& v : vers) {
        Solvable* s = pool_id2solvable(pool, repo_add_solvable(repo));
        s->name = name;
        s->evr = pool_str2id(pool, to_libsolv_evr(v).c_str(), 1);
        solvable_add_deparray(s, SOLVABLE_PROVIDES, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1),
                              0);
        evr_ids.push_back(s->evr);
    }
    pool_createwhatprovides(pool);

    int checked = 0;
    for (const auto& op : ops()) {
        for (const auto& dep : vers) {
            const Id dep_id = pool_rel2id(
                pool, name, pool_str2id(pool, to_libsolv_evr(dep).c_str(), 1), op.flags, 1);
            // libsolv 认为"这个依赖被谁满足"
            std::vector<bool> matched(vers.size(), false);
            for (Id* p = pool_whatprovides_ptr(pool, dep_id); *p; p++) {
                const Solvable* s = pool_id2solvable(pool, *p);
                if (s->name != name) continue;
                for (std::size_t i = 0; i < evr_ids.size(); ++i)
                    if (s->evr == evr_ids[i]) matched[i] = true;
            }
            for (std::size_t i = 0; i < vers.size(); ++i) {
                const bool libsolv_says = matched[i];
                const bool lpkg_says = version_satisfies(vers[i], op.text, dep);
                EXPECT_EQ(libsolv_says, lpkg_says)
                    << "桥接语义分叉：libsolv 认为 `bridge-pkg " << op.text << " " << dep << "` "
                    << (libsolv_says ? "被" : "不被") << " 版本 " << vers[i]
                    << " 满足，而 lpkg 的 version_satisfies 说相反"
                    << "（这正是「求出来的方案被自己拒掉」/「装出坏系统」的来源）";
                ++checked;
            }
        }
    }
    // 结构性锚点：矩阵必须真的跑了（这条防的是"循环没进、用例空转"）
    EXPECT_EQ(checked, static_cast<int>(ops().size() * vers.size() * vers.size()));
    pool_free(pool);
}

TEST(VersionBridge, EncodingRoundTripsLosslessly)
{
    // 编码必须是**一一对应**：否则 `from_libsolv_evr` 还给调用方的版本号是错的
    // （求解结果落到 `InstallPlan.version`、`pkgs` 库、以及错误消息里）。
    for (const auto& v : corpus()) {
        EXPECT_EQ(from_libsolv_evr(to_libsolv_evr(v)), v) << "往返有损: " << v;
    }
}

TEST(VersionBridge, DegenerateEmptyReleaseIsTreatedAsNoRelease)
{
    // `1.0+`（`+` 后为空）在 lpkg 的判据里**等于** `1.0`（`split_release` 给出空 release ⇒
    // `evr_cmp("1.0+","1.0") == 0`）。编码必须同判，否则会编出 `1.0^^`，而它在 libsolv 里
    // **大于** `1.0` ⇒ 桥在这个边界上不再保序（实测 6 对退化串分叉）。
    // 真实版本不以 `+` 结尾（索引 678 个里 0 个），所以这条是**钉边界**而不是修现实缺陷。
    EXPECT_EQ(to_libsolv_evr("1.0+"), to_libsolv_evr("1.0"));
    EXPECT_EQ(to_libsolv_evr("1.0+"), "1.0");
}

TEST(VersionBridge, ReservedCharactersAreRefusedNotSilentlyMisencoded)
{
    // `^` 是 release 分隔符、`~` 是 libsolv 的预发布标记、`:` 是 epoch —— 它们一旦出现在
    // lpkg 版本里，编码就不再是一一对应（`1.0^2` 会被当成"1.0 带 release 2"）。
    // 实测真实索引 678 个版本里这三个字符一个都没有 ⇒ 拒它们不误伤任何现存包。
    for (const char* bad : {"1.0^2", "1.0~beta", "1:1.0"})
        EXPECT_THROW(to_libsolv_evr(bad), LpkgException) << bad;
    // 反面对照：正常版本（含 release、含预发布 `-`、含 `_`）必须照常编码。
    for (const char* ok : {"1.0", "1.0+1", "1.0-rc1", "1.0-rc1+2", "1.0_beta"})
        EXPECT_NO_THROW(to_libsolv_evr(ok)) << ok;
}
