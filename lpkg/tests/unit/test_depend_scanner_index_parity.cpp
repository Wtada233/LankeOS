/**
 * test_depend_scanner_index_parity.cpp —— `depend remove` / `depend abibreak` 必须与
 * `repository.cpp` 用**同一套**索引解析
 *
 * 缺口（CLAUDE.md §3「depend_scanner 有第二套索引解析器」）：`pkg/depend_scanner.cpp` 的
 * `build_repo_revdep_map()` 曾自带一份索引行解析，要求版本块 **≥5 字段**
 * （`if (vh.size() < 5) continue;`），而 `repo/repository.cpp` 那份**有意**容忍 4 字段
 * —— 4 字段版本块 = `版本:哈希:依赖:提供`（provides 在 `vh[3]`、不写 needed_so），
 * 旧写入器/部分写入器就是这么写的。
 *
 * 后果**不是报错而是错误的答案**：那些行在反向依赖图里被整行丢掉 → 提供者不存在 →
 * `lpkg depend remove <包>` / `depend abibreak <包>` 静默打印"无受影响包"，而仓库里
 * 明明有一整串包需要它。用户据此删包 / 判断重建面，拿到的是一份少算的清单。
 *
 * 修法：字段切分统一到 `base/utils.cpp` 的 `parse_repo_index_line()`（唯一实现），
 * 并把"取哪个版本"从"索引里的最后一块"改成**版本号最大者**（= repository.cpp 排序后
 * 的"最新版"；写入顺序不是版本序）。
 *
 * 注意这里的**边界**：4 字段的块没有 needed_so 字段，所以它能当**提供者**（provides 在
 * vh[3]）却当不了**依赖者**（无从得知它需要谁的 SONAME）—— 见
 * FourFieldDependentCannotBeInferred，这条边界是有意保留的，不是漏修。
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/depend_scanner.hpp"
#include "../../main/src/repo/repository.hpp"

namespace fs = std::filesystem;

class IndexParityTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_depscan_parity_test");
        fs::remove_all(suite_work_dir);
        test_root = suite_work_dir / "root";
        fs::create_directories(test_root);

        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    /// 把原始索引文本写到 build_repo_revdep_map() 优先读取的位置
    void write_raw_index(const std::string& content)
    {
        const fs::path idx = Config::get_tmp_dir() / constants::REPO_INDEX_TMP;
        fs::create_directories(idx.parent_path());
        std::ofstream f(idx);
        f << content;
    }

    static int count_status(const depscan::ScanNode& node, depscan::ScanStatus s)
    {
        int n = (node.status == s) ? 1 : 0;
        for (const auto& c : node.children) n += count_status(c, s);
        return n;
    }

    static bool has_child(const depscan::ScanNode& node, const std::string& name)
    {
        for (const auto& c : node.children)
            if (c.name == name) return true;
        return false;
    }

    /// 收集整棵树（含根）的所有节点名，用于"点名了谁"的断言
    static void collect(const depscan::ScanNode& node, std::set<std::string>& out)
    {
        out.insert(node.name);
        for (const auto& c : node.children) collect(c, out);
    }
};

// ── 5 字段（完整）索引：基线，必修后仍必须工作（回归闸门） ────────────────────────
TEST_F(IndexParityTest, FiveFieldIndexListsAffectedPackages)
{
    // libA 提供 liba.so.1；appB 需要 liba.so.1
    write_raw_index(
        "libA|1.0:hhh::liba.so.1:\n"
        "appB|2.0:hhh::,:liba.so.1\n");

    auto tree = depscan::scan_remove_tree("libA");
    EXPECT_TRUE(has_child(tree, "appB")) << "5 字段索引下 appB 必须出现在受影响列表";
    EXPECT_EQ(count_status(tree, depscan::ScanStatus::REMOVED), 2);

    auto abi = depscan::scan_abibreak_tree("libA");
    EXPECT_TRUE(has_child(abi, "appB"));
    EXPECT_EQ(abi.children[0].status, depscan::ScanStatus::REBUILD);
}

// ── 4 字段版本块（provides 在 vh[3]、无 needed_so）：提供者行**不得被丢掉** ────────
TEST_F(IndexParityTest, FourFieldProviderLineStillRegistersProvider)
{
    // libA 的版本块只有 4 段：版本:哈希:依赖:提供 —— provides 在 vh[3]，没有 needed_so
    write_raw_index(
        "libA|1.0:hhh::liba.so.1\n"
        "appB|2.0:hhh::,:liba.so.1\n");

    auto tree = depscan::scan_remove_tree("libA");
    std::set<std::string> names;
    collect(tree, names);
    EXPECT_TRUE(names.contains("appB"))
        << "4 字段的提供者行被整行丢掉 → depend remove 静默报「无受影响包」（错误答案）";

    auto abi = depscan::scan_abibreak_tree("libA");
    EXPECT_TRUE(has_child(abi, "appB")) << "depend abibreak 同样必须列出直接依赖者";
}

// ── 包级 provides 回退（版本级为空，provides 写在行内第 3 段）────────────────────
// repository.cpp 明确容忍这种旧格式（"旧格式/部分写入器会写在那里"）；depend_scanner
// 此前**完全不看**该字段 → 提供者同样丢失。
TEST_F(IndexParityTest, PackageLevelProvidesFallbackIsHonoured)
{
    write_raw_index(
        "libA|1.0:hhh::|liba.so.1\n"
        "appB|2.0:hhh::,:liba.so.1\n");

    auto tree = depscan::scan_remove_tree("libA");
    std::set<std::string> names;
    collect(tree, names);
    EXPECT_TRUE(names.contains("appB"))
        << "包级 provides（行内第 3 段）是 repository.cpp 有意兼容的形态，depend 侧必须同样认";
}

// ── "最新版"取版本号最大者，不是索引里的最后一块 ────────────────────────────────
// 旧实现取 `blocks.back()`（写入顺序），与 repository.cpp 排序后的"最新版"可以不一致。
TEST_F(IndexParityTest, LatestVersionIsByVersionOrderNotFileOrder)
{
    // 写入顺序把 2.0 写在前面：2.0 提供 liba.so.2，1.0 提供 liba.so.1（已废弃）
    write_raw_index(
        "libA|2.0:hhh::liba.so.2:;1.0:hhh::liba.so.1:\n"
        "appNew|1.0:hhh::,:liba.so.2\n"
        "appOld|1.0:hhh::,:liba.so.1\n");

    auto tree = depscan::scan_remove_tree("libA");
    std::set<std::string> names;
    collect(tree, names);
    EXPECT_TRUE(names.contains("appNew")) << "最新版（版本序 2.0）的 provides 必须参与反图";
    EXPECT_FALSE(names.contains("appOld"))
        << "旧版本（1.0）的 provides 不该参与反图（只按最新版建图）";
}

// ── 对照：**同一份索引**喂给两个消费者，字段必须一模一样 ─────────────────────────
// 这是本条缺口最直接的不变量：repository.cpp（建包表）与 depend_scanner.cpp（建反图）
// 读的是同一份 index.txt，任何"字段数不同就丢行"的判据都会让两边对同一行给出相反结论。
TEST_F(IndexParityTest, RepositoryAndScannerAgreeOnSameIndex)
{
    // 同一份索引写到两个位置：depend_scanner 优先读 tmp 副本，Repository 读镜像目录
    const std::string index =
        "libA|1.0:hhh::liba.so.1\n"     // 4 段版本块（旧/部分写入器形态）
        "appB|2.0:hhh::,:liba.so.1\n";  // 5 段：appB 需要 liba.so.1
    write_raw_index(index);
    const fs::path mirror = suite_work_dir / "mirror";
    fs::create_directories(mirror / "x86_64");
    std::ofstream(mirror / "x86_64" / "index.txt") << index;
    {
        fs::create_directories(Config::instance().mirror_conf().parent_path());
        std::ofstream mc(Config::instance().mirror_conf());
        mc << "file://" << mirror.string() << "/\n";
        mc.flush();
    }
    Config::instance().set_architecture("x86_64");

    // repository 侧：认这个包，且 provides 要带出来
    Repository repo;
    repo.load_index();
    auto info = repo.find_package("libA", "1.0");
    ASSERT_TRUE(info.has_value()) << "repository.cpp 有意容忍 4 字段版本块，这里必须命中";
    EXPECT_NE(std::find(info->provides.begin(), info->provides.end(), "liba.so.1"),
              info->provides.end())
        << "4 字段块的 provides 在 vh[3]，必须解析出来（否则两边结论相反）";

    // depend 侧：同一条索引行必须给出**同一个**提供者 → appB 出现在受影响列表里
    auto tree = depscan::scan_remove_tree("libA");
    EXPECT_TRUE(has_child(tree, "appB"))
        << "两边对同一行给出相反结论 = depend_scanner 又有了第二套解析器";
}

// ── 边界（有意保留）：4 字段的**依赖者**无法推断 ────────────────────────────────
// 4 字段块没有 needed_so 字段 —— 它当得了提供者，当不了依赖者。这不是漏修：
// 信息根本不在索引里（needed_so 是 ELF DT_NEEDED 扫描的产物，猜不出来）。
//
// ⚠️ 写 4 字段行时**不能**在末尾补 ':'：`a:b:c:d:` 会被切成 5 段（第 5 段空串），
//    于是又变成"有 needed_so 字段但为空"的 5 字段行 —— 那是另一回事（旧解析也认）。
TEST_F(IndexParityTest, FourFieldDependentCannotBeInferred)
{
    write_raw_index(
        "libA|1.0:hhh::liba.so.1:\n"
        "appB|2.0:hhh::\n");  // 4 段：版本:哈希:依赖:提供（提供为空，无 needed_so）

    auto tree = depscan::scan_remove_tree("libA");
    EXPECT_FALSE(has_child(tree, "appB"))
        << "4 字段块没有 needed_so 字段，反图无从推断它是依赖者 —— 这属于索引信息缺失，"
           "不是解析器丢行（对照 FourFieldProviderLineStillRegistersProvider）";
}
