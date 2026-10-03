/**
 * test_overwrite_globs.cpp — `--overwrite <glob>[,<glob>…]`：**按路径**豁免文件冲突
 *
 * 对照物是 pacman 的 `--overwrite`：可重复、逗号分隔、`!` 取反、后写的覆盖先写的
 * （`_alpm_fnmatch_patterns` 倒序遍历），并且**只在命中路径上**豁免 —— 不是"开了就全局
 * 放行"。本文件钉住的正是"按路径"这一条：
 *   ① 命中模式的冲突路径被豁免：包装得上、文件被接管、**所有权真的转走**；
 *   ② 同一模式下**未命中**的冲突照旧中止（点名真实持有者），且批次被整批预检拦在
 *      "一个文件都没动"之前；
 *   ③ `!` 取反生效（`*` 放行全部 + `!<glob>` 明确否决某路径 → 该路径仍冲突）；
 *   ④ 后写的模式赢（反序给同一对模式 → 结果反转）；
 *   ⑤ `--force-overwrite` 仍等价于 `'*'`（向后兼容入口）；
 *   ⑥ **方向性规则不变**：归档**目录**条目撞盘上文件/符号链接可被豁免；归档**文件**
 *      撞真目录**永不**豁免（命中模式也不行）—— pacman 的 case 5 同样不放行。
 *
 * 夹具说明：冲突的"对方"都是**真实持有者**（先装好的另一个包）或盘上无人持有的手工
 * 文件/目录 —— 判定里的 force/overwrite 只该改变"是否中止"，不该改变"点名谁"。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class OverwriteGlobsTest : public IntegrationTestBase
{
protected:
    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        Config::instance().set_no_hooks_mode(true);
        // 覆盖豁免模式是**进程级**状态：每个用例都从"什么都不豁免"开始
        Config::instance().set_overwrite_patterns({});
    }

    void TearDown() override
    {
        Config::instance().set_overwrite_patterns({});
        Config::instance().set_force_overwrite_mode(false);
        IntegrationTestBase::TearDown();
    }

    static void write_file(const fs::path& p, const std::string& content = "x\n")
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }

    static std::string read_text(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    /** 打一个包：content 由 fill 回调填（相对 content/ 的路径） */
    template <typename F>
    std::string pack(const std::string& name, const std::string& ver,
                     const std::vector<std::string>& deps, F fill)
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::remove_all(work);
        fs::create_directories(work / "content");
        fill(work / "content");
        const std::string path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(path, work.string(), name, ver, deps, {}, "man " + name, {});
        return path;
    }

    static bool owned_by(const std::string& path, const std::string& pkg)
    {
        return Cache::instance().get_file_owners(path).contains(pkg);
    }
};

// ============================================================================
// ① 命中模式的冲突路径被豁免：装得上、文件被接管、所有权真的转走
// ============================================================================

TEST_F(OverwriteGlobsTest, MatchingGlobExemptsThatPathOnly)
{
    // 真实持有者（**不在**本批次里）
    const std::string owner = pack("og_owner_a", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/a/keep.txt", "owned by og_owner_a\n");
    });
    ASSERT_NO_THROW(install_packages({owner}));

    // 本批次的包发同一个文件 → 与持有者冲突
    const std::string w = pack("og_w_a", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/a/keep.txt", "from og_w_a\n");
    });

    // fixture 自检：空模式列表（默认）下这个冲突必须中止 —— 证明下面的成功来自模式命中
    EXPECT_THROW(install_packages({w}), LpkgException) << "空列表 = 什么都不豁免：冲突必须照旧中止";

    Config::instance().set_overwrite_patterns({"/usr/share/a/*"});
    ASSERT_NO_THROW(install_packages({w})) << "命中模式的冲突路径必须被豁免";
    EXPECT_EQ(read_text(test_root / "usr/share/a/keep.txt"), "from og_w_a\n");
    EXPECT_TRUE(owned_by("/usr/share/a/keep.txt", "og_w_a"));
    EXPECT_FALSE(owned_by("/usr/share/a/keep.txt", "og_owner_a"))
        << "被豁免的路径上所有权必须真的转走（不是两边都持有）";
}

// ============================================================================
// ② 同一模式下未命中的冲突照旧中止：只豁免命中路径，不是全局放行
// ============================================================================

TEST_F(OverwriteGlobsTest, NonMatchingConflictInTheSameBatchStillAborts)
{
    const std::string oa = pack("og_owner_a", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/a/keep.txt", "owned by og_owner_a\n");
    });
    const std::string ob = pack("og_owner_b", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/b/other.txt", "owned by og_owner_b\n");
    });
    ASSERT_NO_THROW(install_packages({oa, ob}));

    // 批次 [og_w_a, og_w_b]：og_w_b 依赖 og_w_a → og_w_a 先处理（依赖优先），冲突落在
    // **第二个**成员上；两个成员各撞一个路径，只有 a/* 命中模式。
    const std::string wa = pack("og_w_a", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/a/keep.txt", "from og_w_a\n");
    });
    const std::string wb = pack("og_w_b", "1.0", {"og_w_a"}, [](const fs::path& c) {
        write_file(c / "usr/share/b/other.txt", "from og_w_b\n");
    });

    Config::instance().set_overwrite_patterns({"/usr/share/a/*"});

    std::string msg;
    try {
        install_packages({wa, wb});
        FAIL() << "b/other.txt 不命中任何模式，同批次必须中止（不是全局放行）";
    } catch (const LpkgException& e) {
        msg = e.what();
    }

    // 点名**未命中**的那个真实持有者，且不把被豁免的路径算进冲突清单
    EXPECT_NE(msg.find("other.txt"), std::string::npos) << "冲突信息没点名真正的冲突路径：" << msg;
    EXPECT_NE(msg.find("og_owner_b"), std::string::npos)
        << "冲突信息必须点名真实持有者 og_owner_b：" << msg;
    EXPECT_EQ(msg.find("keep.txt"), std::string::npos)
        << "命中模式的路径不该出现在冲突清单里：" << msg;

    // 整批预检在事务之前拒绝 → 一个文件都没动
    EXPECT_EQ(read_text(test_root / "usr/share/a/keep.txt"), "owned by og_owner_a\n")
        << "批次被拒时被豁免的路径也一个字都不该改（预检拦在动盘之前）";
    EXPECT_TRUE(owned_by("/usr/share/a/keep.txt", "og_owner_a"));
    EXPECT_FALSE(fs::exists(test_root / "usr/share/b/other.txt.lpkgtmp"));
    EXPECT_EQ(read_text(test_root / "usr/share/b/other.txt"), "owned by og_owner_b\n");
}

// ============================================================================
// ②' 把第二个冲突也加进模式列表 → 同一批次必须装得上（证明上面失败的原因是"没命中"，
//     而不是别的什么让这个批次永远装不了）
// ============================================================================

TEST_F(OverwriteGlobsTest, BothPathsInstalledWhenBothMatchTheList)
{
    const std::string oa = pack("og_owner_a", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/a/keep.txt", "owned by og_owner_a\n");
    });
    const std::string ob = pack("og_owner_b", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/b/other.txt", "owned by og_owner_b\n");
    });
    ASSERT_NO_THROW(install_packages({oa, ob}));

    const std::string wa = pack("og_w_a", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/a/keep.txt", "from og_w_a\n");
    });
    const std::string wb = pack("og_w_b", "1.0", {"og_w_a"}, [](const fs::path& c) {
        write_file(c / "usr/share/b/other.txt", "from og_w_b\n");
    });

    Config::instance().set_overwrite_patterns({"/usr/share/a/*", "/usr/share/b/*"});
    ASSERT_NO_THROW(install_packages({wa, wb})) << "两个冲突路径都命中模式，批次必须成功";

    EXPECT_EQ(read_text(test_root / "usr/share/a/keep.txt"), "from og_w_a\n");
    EXPECT_EQ(read_text(test_root / "usr/share/b/other.txt"), "from og_w_b\n");
    EXPECT_TRUE(owned_by("/usr/share/a/keep.txt", "og_w_a"));
    EXPECT_TRUE(owned_by("/usr/share/b/other.txt", "og_w_b"));
    EXPECT_FALSE(owned_by("/usr/share/b/other.txt", "og_owner_b"));
}

// ============================================================================
// ③ `!` 取反：命中取反模式的路径**明确不豁免**（即使更宽松的模式先写）
// ④ 后写的赢：反序给出同一对模式 → 结果反转（倒序遍历）
// ============================================================================

TEST_F(OverwriteGlobsTest, NegatedPatternVetoesAndLaterPatternWins)
{
    const std::string oa = pack("og_owner_a", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/a/keep.txt", "owned by og_owner_a\n");
    });
    const std::string ob = pack("og_owner_b", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/b/other.txt", "owned by og_owner_b\n");
    });
    ASSERT_NO_THROW(install_packages({oa, ob}));

    const std::string wa = pack("og_w_a", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/a/keep.txt", "from og_w_a\n");
    });
    const std::string wb = pack("og_w_b", "1.0", {"og_w_a"}, [](const fs::path& c) {
        write_file(c / "usr/share/b/other.txt", "from og_w_b\n");
    });

    // ③ `*` 放行全部，但 `!...` 明确否决 b/* → b/other.txt 仍冲突、批次中止
    Config::instance().set_overwrite_patterns({"*", "!/usr/share/b/*"});
    std::string msg;
    try {
        install_packages({wa, wb});
        FAIL() << "'!' 取反必须让 b/other.txt 回到冲突判据里";
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    EXPECT_NE(msg.find("other.txt"), std::string::npos) << msg;
    EXPECT_EQ(msg.find("keep.txt"), std::string::npos) << "被 `*` 豁免的路径不该报冲突：" << msg;
    EXPECT_EQ(read_text(test_root / "usr/share/b/other.txt"), "owned by og_owner_b\n");

    // ④ 同一对模式反序（后写的普通模式赢）→ 两个路径都豁免，批次成功
    Config::instance().set_overwrite_patterns({"*", "!/usr/share/b/*", "/usr/share/b/*"});
    ASSERT_NO_THROW(install_packages({wa, wb})) << "后写的普通模式必须覆盖先前的取反";
    EXPECT_EQ(read_text(test_root / "usr/share/b/other.txt"), "from og_w_b\n");
    EXPECT_TRUE(owned_by("/usr/share/b/other.txt", "og_w_b"));
}

// ============================================================================
// ⑤ `--force-overwrite` 仍等价于 `'*'`（向后兼容：farm/脚本与既有测试都在用）
// ============================================================================

TEST_F(OverwriteGlobsTest, ForceOverwriteStillEqualsStarGlob)
{
    const std::string oa = pack("og_owner_a", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/a/keep.txt", "owned by og_owner_a\n");
    });
    const std::string ob = pack("og_owner_b", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/b/other.txt", "owned by og_owner_b\n");
    });
    ASSERT_NO_THROW(install_packages({oa, ob}));

    const std::string wa = pack("og_w_a", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/a/keep.txt", "from og_w_a\n");
    });
    const std::string wb = pack("og_w_b", "1.0", {"og_w_a"}, [](const fs::path& c) {
        write_file(c / "usr/share/b/other.txt", "from og_w_b\n");
    });

    // 自检：开关关着时同一批次形态必须中止（证明下面的成功确实来自 '*' 豁免）
    EXPECT_THROW(install_packages({wa, wb}), LpkgException) << "未豁免时两个冲突路径都必须中止";

    Config::instance().set_force_overwrite_mode(true);
    ASSERT_NO_THROW(install_packages({wa, wb}))
        << "--force-overwrite ≡ --overwrite '*'：任何路径都豁免";
    EXPECT_EQ(read_text(test_root / "usr/share/a/keep.txt"), "from og_w_a\n");
    EXPECT_EQ(read_text(test_root / "usr/share/b/other.txt"), "from og_w_b\n");
    EXPECT_TRUE(owned_by("/usr/share/a/keep.txt", "og_w_a"));
    EXPECT_TRUE(owned_by("/usr/share/b/other.txt", "og_w_b"));
    Config::instance().set_force_overwrite_mode(false);
}

// ============================================================================
// ⑥ 方向性规则不变：归档**目录**条目撞盘上文件可豁免；归档**文件**撞真目录永不豁免
// ============================================================================

TEST_F(OverwriteGlobsTest, FileEntryOverRealDirectoryIsNeverExempted)
{
    // 盘上无人持有的真目录
    fs::create_directories(test_root / "usr/share/ogdir");
    // 归档里是**文件** usr/share/ogdir → 撞真目录
    const std::string p = pack("og_filedir", "1.0", {}, [](const fs::path& c) {
        write_file(c / "usr/share/ogdir", "a file\n");
    });

    Config::instance().set_overwrite_patterns({"*"});
    std::string msg;
    try {
        install_packages({p});
        FAIL() << "归档文件撞真目录永不豁免（pacman 的 not overwriting dir with file）";
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    EXPECT_NE(msg.find("ogdir"), std::string::npos) << msg;
    EXPECT_TRUE(fs::is_directory(test_root / "usr/share/ogdir")) << "真目录必须原样保留";
    EXPECT_TRUE(Cache::instance().get_installed_version("og_filedir").empty())
        << "被拒绝时不该登记安装";
}

TEST_F(OverwriteGlobsTest, DirEntryOverDiskFileIsExemptedWhenMatched)
{
    // 盘上无人持有的**手工文件**；归档里是**目录**条目 usr/share/ogfile/
    write_file(test_root / "usr/share/ogfile", "manual on disk\n");
    const std::string p = pack("og_dirfile", "1.0", {}, [](const fs::path& c) {
        fs::create_directories(c / "usr/share/ogfile");
        write_file(c / "usr/share/ogfile/inside.txt", "from og_dirfile\n");
    });

    // 先自检：不豁免时这个方向也判冲突（"目录条目接管盘上文件"属类型变更）。
    // 判据落到"点名了谁"上：路径 + 无人持有的判据文本 —— 只断言"抛了异常"的话，
    // "包没找到 / 别的什么原因"同样能让它绿。
    std::string msg;
    try {
        install_packages({p});
        FAIL() << "归档目录条目撞盘上文件，未被豁免时必须判冲突中止";
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    EXPECT_NE(msg.find("usr/share/ogfile"), std::string::npos) << msg;
    EXPECT_NE(msg.find(string_format("error.file_conflict_unowned", "/usr/share/ogfile/")),
              std::string::npos)
        << msg;
    EXPECT_EQ(msg.find(get_string("error.unknown_manual_file")), std::string::npos)
        << "占位文本不该被当成持有者名渲染进报告：" << msg;
    EXPECT_TRUE(fs::is_regular_file(test_root / "usr/share/ogfile"))
        << "被拒绝时盘上文件必须原样保留";

    Config::instance().set_overwrite_patterns({"/usr/share/ogfile"});
    ASSERT_NO_THROW(install_packages({p})) << "目录条目撞盘上文件命中模式 → 豁免";
    EXPECT_TRUE(fs::is_directory(test_root / "usr/share/ogfile"));
    EXPECT_EQ(read_text(test_root / "usr/share/ogfile/inside.txt"), "from og_dirfile\n");
}

// ============================================================================
// 模式列表的纯语义（不装包，直接问 Config）：归一化、两形态匹配、空列表、组装规则
// ============================================================================

TEST_F(OverwriteGlobsTest, OverwriteAllowsSemantics)
{
    using V = std::vector<std::string>;
    auto& cfg = Config::instance();

    // 空列表 = 什么都不豁免
    cfg.set_overwrite_patterns({});
    EXPECT_FALSE(cfg.overwrite_allows("/usr/share/a/x"));

    cfg.set_overwrite_patterns({"/usr/share/a/*"});
    EXPECT_TRUE(cfg.overwrite_allows("/usr/share/a/x"));
    EXPECT_FALSE(cfg.overwrite_allows("/usr/share/b/x"));
    // `*` 跨 `/` 匹配（fnmatch flags=0，与 pacman 同）→ 子目录也命中
    EXPECT_TRUE(cfg.overwrite_allows("/usr/share/a/deep/x"));
    // 相对形态（pacman `_alpm_can_overwrite_file` 同时试两种形态）
    EXPECT_TRUE(cfg.overwrite_allows("usr/share/a/x"));
    // 尾斜杠归一：目录条目的形态差异不该影响判定
    cfg.set_overwrite_patterns({"/usr/share/a"});
    EXPECT_TRUE(cfg.overwrite_allows("/usr/share/a/"));
    EXPECT_FALSE(cfg.overwrite_allows("/usr/share/a/b"));

    // `!` 取反 + 后写的赢
    cfg.set_overwrite_patterns({"*", "!/usr/share/b/*"});
    EXPECT_TRUE(cfg.overwrite_allows("/usr/share/a/x"));
    EXPECT_FALSE(cfg.overwrite_allows("/usr/share/b/x"));
    cfg.set_overwrite_patterns({"*", "!/usr/share/b/*", "/usr/share/b/c"});
    EXPECT_TRUE(cfg.overwrite_allows("/usr/share/b/c"));
    EXPECT_FALSE(cfg.overwrite_allows("/usr/share/b/d"));

    // 组装规则：`--force-overwrite` 的 '*' 排在最前（优先级最低）
    EXPECT_EQ(Config::compose_overwrite_patterns(true, {}), (V{"*"}));
    EXPECT_EQ(Config::compose_overwrite_patterns(false, {"/a/*"}), (V{"/a/*"}));
    EXPECT_EQ(Config::compose_overwrite_patterns(true, {"!/a/*", "/a/b"}),
              (V{"*", "!/a/*", "/a/b"}));
    cfg.set_overwrite_patterns(Config::compose_overwrite_patterns(true, {"!/usr/share/b/*"}));
    EXPECT_TRUE(cfg.overwrite_allows("/usr/share/a/x"));
    EXPECT_FALSE(cfg.overwrite_allows("/usr/share/b/x"));

    cfg.set_overwrite_patterns({});
}

// ============================================================================
// 尾斜杠语义（2026-10-03）：模式侧**剥尾斜杠**（`usr/lib/foo/` ≡ `usr/lib/foo`），
// 但**剥完只剩通配符的不剥**（`*/`、`??/` 保持原样 ⇒ 依旧不匹配 = fail-closed）
// ============================================================================

TEST_F(OverwriteGlobsTest, TrailingSlashInPatternIsStrippedUnlessOnlyWildcards)
{
    auto& cfg = Config::instance();

    // ① 模式写尾斜杠 = 同一路径：剥掉后 fnmatch 才拿到 `usr/lib/foo` 去比 `usr/lib/foo`。
    //    修复前这里**静默不匹配**（fnmatch 拿 `usr/lib/foo/` 比 `usr/lib/foo` 永远失败）。
    //    缺陷下会红：若"剥尾斜杠"这步被拿掉（又只剩剥前导斜杠），下面三条 EXPECT_TRUE
    //    全部从 true 变 false；反过来若实现被改成"截断目录前缀"，最后那条 EXPECT_FALSE 会红。
    cfg.set_overwrite_patterns({"/usr/lib/foo/"});
    EXPECT_TRUE(cfg.overwrite_allows("/usr/lib/foo")) << "模式尾斜杠必须剥掉，与裸形态等价";
    EXPECT_TRUE(cfg.overwrite_allows("usr/lib/foo")) << "前导斜杠差异也不该影响";
    EXPECT_TRUE(cfg.overwrite_allows("/usr/lib/foo/")) << "路径侧本就归一（两个方向都剥）";
    EXPECT_FALSE(cfg.overwrite_allows("/usr/lib/foobar")) << "剥尾斜杠不能被实现成'截断到目录前缀'";

    // ② 剥完**只剩通配符**的不剥：`*/` 保持原样 ⇒ fnmatch("*/", "usr/lib/foo") 不匹配。
    //    这是**有意的 fail-closed** —— `*` 在 fnmatch（flags=0）里跨 `/` 匹配，若把 `*/`
    //    剥成 `*`（或直接跟 `*` 等价），用户"以为只针对目录"的写法会被静默放大成
    //    "豁免**一切**冲突"（豁免会跳过文件冲突检查，是危险方向）。保持不匹配 ⇒ 照常报冲突，
    //    用户看得见冲突就有机会把模式写清楚。
    //    缺陷下会红：若剥尾斜杠时**漏了** only_wildcards 守卫（无条件剥），`*/` → `*`
    //    就会命中一切 —— 这条 EXPECT_FALSE 从 false 变 true。`??/` 同理。
    cfg.set_overwrite_patterns({"*/"});
    EXPECT_FALSE(cfg.overwrite_allows("/usr/lib/foo"))
        << "`*/` 剥成 `*` 会把'只针对目录'的写法放大成豁免一切冲突（危险方向）";
    cfg.set_overwrite_patterns(
        {"??"
         "/"});  // 拆开写：`??/` 是 trigraph，会被 -Werror 拦
    EXPECT_FALSE(cfg.overwrite_allows("/usr/lib/foo"))
        << "`??` `/` 剥成 `??` 同样会静默放大豁免范围";

    // ③ 取反方向正确：`!` 剥完尾斜杠后仍钉住**那个路径**（否则取反的尾斜杠写法静默失效，
    //    用户以为否决了、其实没否决）。缺陷下会红：若取反模式没走同一套剥斜杠逻辑，
    //    `!/usr/lib/foo/` 匹配不到 `usr/lib/foo` ⇒ 该路径会被前面的 `*` 放行 ⇒
    //    第一条 EXPECT_FALSE 从 false 变 true。
    cfg.set_overwrite_patterns({"*", "!/usr/lib/foo/"});
    EXPECT_FALSE(cfg.overwrite_allows("/usr/lib/foo"))
        << "`!/usr/lib/foo/` 必须否决 usr/lib/foo（否则取反的尾斜杠写法静默失效）";
    EXPECT_TRUE(cfg.overwrite_allows("/usr/lib/bar")) << "被 `*` 放行的别的路径不受这条取反影响";

    // ④ 对照：裸 `*`（不含尾斜杠、根本不进"剥尾斜杠"分支）仍命中一切 —— 证明 ② 的红
    //    确实来自 only_wildcards 守卫，而不是"所有含 `*` 的模式都被禁掉了"。
    cfg.set_overwrite_patterns({"*"});
    EXPECT_TRUE(cfg.overwrite_allows("/usr/lib/foo"));
    EXPECT_TRUE(cfg.overwrite_allows("anywhere/at/all"));

    cfg.set_overwrite_patterns({});
}
