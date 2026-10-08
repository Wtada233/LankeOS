/**
 * test_depend_scanner_index_parity.cpp —— `depend remove` / `depend abibreak` 必须与
 * `repository.cpp` 用**同一套**索引解析
 *
 * 缺口（「depend_scanner 有第二套索引解析器」）：`pkg/depend_scanner.cpp` 的
 * `build_repo_revdep_map()` 曾自带一份索引行解析，要求版本块 **≥5 字段**，
 * 而 `repo/repository.cpp` 那份当时**有意**容忍更少字段 —— 两份判据不一致 ⇒ 同一行在
 * 反向依赖图里被整行丢掉。
 *
 * 后果**不是报错而是错误的答案**：提供者不存在 → `lpkg depend remove <包>` /
 * `depend abibreak <包>` 静默打印"无受影响包"，而仓库里明明有一整串包需要它。用户据此
 * 删包 / 判断重建面，拿到的是一份少算的清单。
 *
 * 修法：字段切分统一到 `base/utils.cpp` 的 `parse_repo_index_line()`（唯一实现），
 * 并把"取哪个版本"从"索引里的最后一块"改成**版本号最大者**（= repository.cpp 排序后
 * 的"最新版"；写入顺序不是版本序）。
 *
 * ⚠️ 版本块现在是**恰好 6 字段**（`版本:哈希:依赖:provides:provides_soname:needed_so`）、
 * **没有**行级 provides。现在非 6 字段的版本块被**两侧同样跳过**（parity 仍成立，只是
 * "丢掉"的行本身就是畸形行）。
 */

#include <gtest/gtest.h>
#include <unistd.h>

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

        suite_work_dir = fs::absolute("tmp_depscan_parity_test_" + std::to_string(::getpid()));
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

// ── 6 字段（完整）索引：基线（回归闸门） ────────────────────────────────────────
TEST_F(IndexParityTest, SixFieldIndexListsAffectedPackages)
{
    // libA 提供 liba.so.1（provides_soname）；appB 需要 liba.so.1
    write_raw_index(
        "libA|1.0:hhh:::liba.so.1:|\n"
        "appB|2.0:hhh::,::liba.so.1|\n");

    auto tree = depscan::scan_remove_tree("libA");
    EXPECT_TRUE(has_child(tree, "appB")) << "6 字段索引下 appB 必须出现在受影响列表";
    EXPECT_EQ(count_status(tree, depscan::ScanStatus::REMOVED), 2);

    auto abi = depscan::scan_abibreak_tree("libA");
    EXPECT_TRUE(has_child(abi, "appB"));
    EXPECT_EQ(abi.children[0].status, depscan::ScanStatus::REBUILD);
}

// ── "最新版"取版本号最大者，不是索引里的最后一块 ────────────────────────────────
// 旧实现取 `blocks.back()`（写入顺序），与 repository.cpp 排序后的"最新版"可以不一致。
TEST_F(IndexParityTest, LatestVersionIsByVersionOrderNotFileOrder)
{
    // 写入顺序把 2.0 写在前面：2.0 提供 liba.so.2，1.0 提供 liba.so.1（已废弃）
    write_raw_index(
        "libA|2.0:hhh:::liba.so.2:;1.0:hhh:::liba.so.1:|\n"
        "appNew|1.0:hhh::,::liba.so.2|\n"
        "appOld|1.0:hhh::,::liba.so.1|\n");

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
        "libA|1.0:hhh:::liba.so.1:|\n"    // 4 段版本块（旧/部分写入器形态）
        "appB|2.0:hhh::,::liba.so.1|\n";  // 5 段：appB 需要 liba.so.1
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

    // repository 侧：认这个包，且 provides_soname 要带出来
    Repository repo;
    repo.load_index();
    auto info = repo.find_package("libA", "1.0");
    ASSERT_TRUE(info.has_value()) << "6 字段索引必须被 repository 解析出来，这里必须命中";
    EXPECT_NE(std::find(info->provides_soname.begin(), info->provides_soname.end(), "liba.so.1"),
              info->provides_soname.end())
        << "libA 导出的 SONAME 在 provides_soname（第 5 字段），必须解析出来（否则两边结论相反）";

    // depend 侧：同一条索引行必须给出**同一个**提供者 → appB 出现在受影响列表里
    auto tree = depscan::scan_remove_tree("libA");
    EXPECT_TRUE(has_child(tree, "appB"))
        << "两边对同一行给出相反结论 = depend_scanner 又有了第二套解析器";
}

// ── 边界（有意保留）：needed_so 为空的**依赖者**无法推断 ────────────────────────
// 一条 6 字段的块若 needed_so 为空、deps 也为空，则它既不链任何 SONAME、也没声明包依赖
// —— 反图无从推断它是依赖者。这不是漏修：信息根本不在索引里（needed_so 是 ELF
// DT_NEEDED 扫描的产物，猜不出来）。
TEST_F(IndexParityTest, EmptyNeededSoCannotBeInferred)
{
    write_raw_index(
        "libA|1.0:hhh:::liba.so.1:|\n"
        "appB|2.0:hhh::::|\n");  // deps 与 needed_so 都为空

    auto tree = depscan::scan_remove_tree("libA");
    EXPECT_FALSE(has_child(tree, "appB"))
        << "appB 既没链 SONAME、也没声明 deps，反图无从推断它是依赖者 —— 属于索引信息缺失，"
           "不是解析器丢行";
}

// ── 显式 `deps` 边：**包名依赖**也必须进反图，不能只看 needed_so ────────────────
//
// 反图此前只认 SONAME 边（needed_so → provides），**完全不看**行内的 deps 段。本仓库的
// `deps` 绝大多数是 farm 从 needed_so 推导的（所以两种边通常重合、缺陷看不出来），但
// 例外是手写的：纯 Python 包（`python-*`）、`xwayland`、以及
// **dlopen** 加载的依赖（`kf-networkmanager-qt` → `networkmanager`、`kf-kapidox` →
// `python-jinja`）—— 它们编译期不链接、ELF 里没有 DT_NEEDED，deps 是**唯一**的依赖声明。
//
// 对这类包，只看 SONAME 边 = 漏掉整条边 ⇒ `depend remove <被依赖者>` 与
// `depend abibreak` 给出**少算的清单**（静默的错误答案，不是报错）。
//
// 索引行格式：name|ver:hash:deps:provides:provides_soname:needed_so（**恰好 6 段**）。
TEST_F(IndexParityTest, ExplicitDependencyEdgeIsHonoured)
{
    // appB 显式 deps=libA（第 3 段），而它的 needed_so 段为空 ——
    // 即"声明了包依赖、却不链接任何 SONAME"。手写 deps 的包就是这个形态。
    write_raw_index(
        "libA|1.0:hhh::::|\n"
        "appB|2.0:hhh:libA:::|\n");

    auto tree = depscan::scan_remove_tree("libA");
    EXPECT_TRUE(has_child(tree, "appB"))
        << "显式 deps 边被漏掉 → depend remove 对手写 deps 的包（Python/dlopen 场景）"
           "给出少算的清单，而且不报错";
    EXPECT_EQ(count_status(tree, depscan::ScanStatus::REMOVED), 2)
        << "受影响集应含 libA 自身与依赖它的 appB";
}

// ── `depend abibreak` 只看 SONAME 边（与 remove 有意不同）───────────────────────
// abibreak 问的是"ABI 断裂后谁**需要重构建**"：只声明包依赖、并不链接该 .so 的包
// **不该**被报成要重构建（它根本没链那个库）。所以上面新加的 deps 边**只进 remove 的图**。
TEST_F(IndexParityTest, AbibreakIgnoresExplicitDependencyEdge)
{
    // 同一份索引：appB 显式 deps=libA，但 needed_so 为空（没链 libA 提供的任何 .so）
    write_raw_index(
        "libA|1.0:hhh:::liba.so.1:|\n"
        "appB|2.0:hhh:libA:::|\n");

    auto abi = depscan::scan_abibreak_tree("libA");
    EXPECT_FALSE(has_child(abi, "appB"))
        << "appB 没有链接 libA 的 .so，ABI 断裂不需要它重构建 —— 把包名依赖并进 abibreak "
           "会让 REBUILD 清单虚增";
    EXPECT_EQ(abi.name, "libA") << "目标包自身是根节点";
}

// ── show_all 分支复用同一份仓库包名清单 ────────────────────────────────
TEST_F(IndexParityTest, RemoveShowAllStillListsUnaffectedRepoPackages)
{
    // `scan_remove_tree` 曾把仓库包名清单算两遍（存在性判定一次、show_all 再一次），
    // 一条 `depend remove --all` 会加载/排序索引多遍。改成只算一次并复用后，show_all
    // 仍必须把**不受影响**的仓库包作为 KEEP 列出（不能因为复用就漏掉它们）。
    const std::string index =
        "libA|1.0:hhh:::liba.so.1:|\n"   // 提供 liba.so.1
        "appB|2.0:hhh::,::liba.so.1|\n"  // 依赖 liba.so.1 → 受影响
        "pkgZ|3.0:hhh::::|\n";           // 6 字段，与 libA 无关 → 不受影响
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

    auto tree = depscan::scan_remove_tree("libA", /*show_all=*/true);
    EXPECT_TRUE(has_child(tree, "appB")) << "受影响包 appB 必须出现";
    EXPECT_TRUE(has_child(tree, "pkgZ")) << "不受影响的仓库包必须作为 KEEP 列出（show_all）";
    for (const auto& c : tree.children) {
        if (c.name == "pkgZ") {
            EXPECT_EQ(c.status, depscan::ScanStatus::KEEP);
        }
    }
}
