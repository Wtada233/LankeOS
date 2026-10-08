/**
 * test_metadata_token_safety.cpp — metadata 的 `deps` / `provides` / `needed_so` 不得带控制字符
 *
 * 归档**成员名**早就有消毒（`archive.cpp` 的 `member_name_rejection_message`），
 * 但这三个字段没有 —— 而它们同样会进**行式 / 制表符分帧**的状态文件：
 *   · `deps/<pkg>`、`needed_so/<pkg>`：一行一条（`\n` 注入 ⇒ 读回时凭空多出一个依赖/SONAME）；
 *   · `provides.db`：`<capability>\t<pkgs>`（`\t` 注入 ⇒ 键被截断、提供者串错位）。
 * 后果：`provides = ["a\ncapX\tE"]` 会读回幽灵提供者，而 `dep_satisfied_on_disk()` 只看
 * "这个 capability 有没有提供者" ⇒ **假满足**依赖（装出坏系统）；`deps`/`needed_so` 里的 `\n`
 * 还会污染反向依赖图、**阻止**正常卸载（DoS）。
 *
 * **为什么 deps/needed_so 两格直接点名 `read_package_metadata()` 而不是走 `install_packages()`**：
 * 走安装路径时，**求解器会先撞上**"`alpha` 没有提供者"（依赖串被 `\n` 截出来的是 `alpha`）——
 * 也就是说整包确实会被拒，但拒它的是另一道闸，用例就分不清是**哪一条**判据生效（那是"测试前提
 * 写错"的典型形态）。`provides` 那格没这个问题（它不参与求解），所以留着走端到端。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/pkg/install_common.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
/// 断言"这条 metadata 被拒，且报错点名了元数据文件与出问题的字段"。
void expect_refused_naming(const fs::path& meta_dir, const char* field)
{
    std::string name;
    std::string version;
    std::vector<std::string> deps;
    std::vector<std::string> provides;
    std::vector<std::string> provides_soname;
    std::vector<std::string> needed_so;
    std::string man;
    try {
        detail::read_package_metadata(meta_dir, name, version, deps, provides, provides_soname,
                                      needed_so, man);
        FAIL() << "含控制字符的 " << field << " 必须被拒：" << meta_dir;
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("metadata.json"), std::string::npos)
            << "报错必须点名元数据文件（否则用户不知道是哪个文件的哪一段）: " << msg;
        EXPECT_NE(msg.find(field), std::string::npos) << "报错必须点名字段: " << msg;
    }
}

/// 写一份只含被测字段的 metadata.json，返回它所在的"已解压包目录"。
fs::path write_metadata(const fs::path& dir, const json& meta)
{
    fs::create_directories(dir);
    std::ofstream(dir / std::string(constants::PKG_METADATA_FILE)) << meta.dump();
    return dir;
}
}  // namespace

class MetadataTokenSafetyTest : public IntegrationTestBase
{
};

TEST_F(MetadataTokenSafetyTest, DependencyWithNewlineIsRefused)
{
    const fs::path d =
        write_metadata(suite_work_dir / "meta_deps",
                       {{"name", "tokendep"}, {"version", "1.0"}, {"deps", {"alpha\nbeta"}}});
    expect_refused_naming(d, "deps");
}

TEST_F(MetadataTokenSafetyTest, ProvideWithTabIsRefused)
{
    // TAB 破坏的是 provides.db 的**键**（`key\tvalues`），不是 WAL（那是空格分帧的）。
    const fs::path d =
        write_metadata(suite_work_dir / "meta_prov",
                       {{"name", "tokenprov"}, {"version", "1.0"}, {"provides", {"capX\tE"}}});
    expect_refused_naming(d, "provides");
}

TEST_F(MetadataTokenSafetyTest, NeededSoWithNewlineIsRefused)
{
    const fs::path d = write_metadata(
        suite_work_dir / "meta_so",
        {{"name", "tokenso"}, {"version", "1.0"}, {"needed_so", {"liba.so.1\nlibb.so.2"}}});
    expect_refused_naming(d, "needed_so");
}

TEST_F(MetadataTokenSafetyTest, CommaInDependencyStringIsStillAccepted)
{
    // 判据**只拒控制字符**：依赖串允许带约束（`"cmake >= 3.20, < 4.0"`，见
    // `vercmp/dep_parser.cpp` 对逗号的处理）—— 拒了会误伤合法包。
    const fs::path d = write_metadata(
        suite_work_dir / "meta_comma",
        {{"name", "tokencomma"}, {"version", "1.0"}, {"deps", {"cmake >= 3.20, < 4.0"}}});
    std::string name, version, man;
    std::vector<std::string> deps, provides, provides_soname, needed_so;
    ASSERT_NO_THROW(detail::read_package_metadata(d, name, version, deps, provides, provides_soname,
                                                  needed_so, man));
    ASSERT_EQ(deps.size(), 1u);
    EXPECT_EQ(deps[0], "cmake >= 3.20, < 4.0");
}

TEST_F(MetadataTokenSafetyTest, CorruptedProvidesIsRefusedOnTheRealInstallPath)
{
    // 端到端对照：`provides` 不参与求解 ⇒ 整包真的会走到解析那一步被拒（点名 metadata.json）。
    // 参考本文件顶部说明：deps/needed_so 会在更早的求解阶段被另一条判据拦下，所以那两格
    // 用上面的直调形态覆盖。
    const std::string pkg = create_pkg("tokenprov_e2e", "1.0", {}, {"capY\tZ"});
    try {
        install_packages({pkg}, "");
        FAIL() << "含 TAB 的 provides 必须整包拒绝";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        // ⚠️ 本地 `.lpkg` 候选这条路，`provides` 在读**归档里的元数据**时就被分帧字符校验
        // 拦下（早于读解压后的 `metadata.json`）⇒ 报错点名的是**用户交上来的那个 `.lpkg`
        // 文件**，不是 `metadata.json`。不变量没变（报错仍然点名"是哪个文件的哪一段"），
        // 变的是被断言的那个字符串。
        EXPECT_NE(msg.find(pkg), std::string::npos)
            << "报错必须点名用户交上来的那个包文件：" << msg;
        EXPECT_NE(msg.find("provides"), std::string::npos) << msg;
    }
}

/**
 * `provides` 不得使用求解器内部的 SONAME 命名空间前缀（`so:`）。
 *
 * 池里只有两个命名空间：`deps`/`provides` 走**裸名**、`needed_so`/`provides_soname` 走
 * `so:`。`constants.hpp` 原先断言"包名不可能含 `:`，所以裸名撞不进 `so:` 空间" —— 那句话
 * **只对包名成立**，`provides` 的项走的是同一条裸名路径、却没有任何地方校验过 `:`。
 * 于是 `provides: ["so:libfoo.so.1"]` 与 `needed_so: ["libfoo.so.1"]` 会灌出**同一个 pool id**：
 * 求解器当 SONAME 接受，而安装期的 `soname_satisfied()`（只看 `provides_soname`）拒绝 ——
 * 正是"求解器说能装、安装期拒装"那个分叉（本仓库 8.0.0 专门根除过的形态）。
 */
TEST_F(MetadataTokenSafetyTest, ProvideUsingTheSolverSonameNamespaceIsRefused)
{
    const fs::path d = write_metadata(
        suite_work_dir / "meta_so_ns",
        {{"name", "sonsprov"}, {"version", "1.0"}, {"provides", {"so:libfoo.so.1"}}});
    std::string name, version, man;
    std::vector<std::string> deps, provides, provides_soname, needed_so;
    try {
        detail::read_package_metadata(d, name, version, deps, provides, provides_soname, needed_so,
                                      man);
        FAIL() << "provides 用了 `so:` 前缀，必须被拒";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("metadata.json"), std::string::npos) << msg;
        EXPECT_NE(msg.find("so:libfoo.so.1"), std::string::npos)
            << "报错必须点名是哪一条能力：" << msg;
    }
}

/**
 * 端到端：**本地 `.lpkg` 那条路**才是这个缺陷真正可达的地方。
 *
 * 仓库来源的包撞不到 —— 索引版本块是"恰好 6 个冒号字段"，`provides` 带 `:` 会让字段数变 7、
 * 整个版本块被丢弃；而本地 `.lpkg` 的计划字段与校验读的是同一份 ⇒ 比对恒等 ⇒ 放行。
 */
TEST_F(MetadataTokenSafetyTest, ReservedProvidesPrefixIsRefusedOnTheLocalPackagePath)
{
    const std::string pkg = create_pkg("sonns_e2e", "1.0", {}, {"so:libfoo.so.1"});
    try {
        install_packages({pkg}, "");
        FAIL() << "本地 .lpkg 的 provides 用了 `so:` 前缀，必须整包拒绝";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find(pkg), std::string::npos)
            << "报错必须点名用户交上来的那个包文件：" << msg;
        EXPECT_NE(msg.find("so:libfoo.so.1"), std::string::npos)
            << "报错必须点名是哪一条能力：" << msg;
    }
}

/// 对照：**普通的虚拟能力**照常接受并安装 —— 别把正常的 `provides` 一起拒了。
TEST_F(MetadataTokenSafetyTest, OrdinaryVirtualCapabilityIsStillAccepted)
{
    const std::string pkg = create_pkg("vcap_ok", "1.0", {}, {"java-runtime"});
    ASSERT_NO_THROW(install_packages({pkg}, "")) << "普通虚拟能力必须照常安装";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/vcap_ok"));
}

TEST_F(MetadataTokenSafetyTest, CleanPackageStillInstalls)
{
    // 对照：同样的 fixture、同样的路径，只是字段干净 → 必须装上。
    // 没有这一条，上面几条在"本地包根本装不上"时会一起假绿。
    // （`deps` 留空：沙箱里没有 `alpha` 的提供者，带上它会被求解器拒 —— 那是另一条判据。）
    const std::string pkg = create_pkg("tokenclean", "1.0", {}, {}, {}, {});
    ASSERT_NO_THROW(install_packages({pkg}, "")) << "干净的包必须照常安装";
    EXPECT_TRUE(fs::exists(test_root / "usr/bin/tokenclean"));
}
