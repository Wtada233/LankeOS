#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "../../main/src/base/constants.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/repo/repository.hpp"

namespace fs = std::filesystem;

class AggregatedIndexTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path root;
    fs::path index_dir;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();

        suite_work_dir = fs::absolute("tmp_aggregated_index_test");
        fs::remove_all(suite_work_dir);

        root = suite_work_dir / "root";
        index_dir = suite_work_dir / "mirror" / "x86_64";
        fs::create_directories(index_dir);
        fs::create_directories(root / "etc" / "lpkg");

        Config::instance().set_root_path(root.string());
        Config::instance().set_architecture("x86_64");
        Config::instance().init_filesystem();

        std::ofstream(root / "etc/lpkg/mirror.conf")
            << "file://" << (suite_work_dir / "mirror").string() << "/" << std::endl;
    }

    void write_index(const std::string& content)
    {
        std::ofstream f(index_dir / "index.txt");
        f << content;
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }
};

TEST_F(AggregatedIndexTest, SingleVersionPerLine)
{
    write_index(
        "zlib|1.2.13:abc123::|\n"
        "libfoo|2.0:def456:glibc::|\n");

    Repository repo;
    repo.load_index();

    auto pkg = repo.find_package("zlib");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->name, "zlib");
    EXPECT_EQ(pkg->version, "1.2.13");
    EXPECT_EQ(pkg->sha256, "abc123");

    pkg = repo.find_package("libfoo");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "2.0");
    ASSERT_EQ(pkg->dependencies.size(), 1);
    EXPECT_EQ(pkg->dependencies[0].name, "glibc");
}

TEST_F(AggregatedIndexTest, MultipleVersionsOneLine)
{
    // New aggregated format: ver1:hash1:deps;ver2:hash2:deps
    write_index(
        "zlib|1.2.13:abc123::;1.3:def456::|\n"
        "libfoo|2.0:aaa111:glibc>=2.35:;2.1:bbb222:ncurses,glibc>=2.35:|\n");

    Repository repo;
    repo.load_index();

    // Latest version via find_package(name)
    auto pkg = repo.find_package("zlib");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "1.3");
    EXPECT_EQ(pkg->sha256, "def456");

    // Exact version lookup
    pkg = repo.find_package("zlib", "1.2.13");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "1.2.13");
    EXPECT_EQ(pkg->sha256, "abc123");

    // libfoo with deps
    pkg = repo.find_package("libfoo", "2.0");
    ASSERT_TRUE(pkg.has_value());
    ASSERT_EQ(pkg->dependencies.size(), 1);
    EXPECT_EQ(pkg->dependencies[0].name, "glibc");
    ASSERT_EQ(pkg->dependencies[0].constraints.size(), 1);
    EXPECT_EQ(pkg->dependencies[0].constraints[0].op, ">=");
    EXPECT_EQ(pkg->dependencies[0].constraints[0].version, "2.35");

    pkg = repo.find_package("libfoo");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "2.1");
    ASSERT_EQ(pkg->dependencies.size(), 2);
}

TEST_F(AggregatedIndexTest, BestMatchingVersion)
{
    write_index("zlib|1.0:aaaa::;1.5:bbbb::;2.0:cccc::|\n");

    Repository repo;
    repo.load_index();

    // >= 1.5 should return 2.0 (latest satisfying)
    auto pkg = repo.find_best_matching_version("zlib", ">=", "1.5");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "2.0");

    // < 2.0 should return 1.5
    pkg = repo.find_best_matching_version("zlib", "<", "2.0");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "1.5");

    // = 1.0 exact
    pkg = repo.find_best_matching_version("zlib", "=", "1.0");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "1.0");
}

TEST_F(AggregatedIndexTest, ProvidesParsedCorrectly)
{
    write_index(
        "vim|9.1:aaa:glibc:editor:|\n"
        "busybox|1.36:bbb::sh,shell:|\n"
        "zlib|1.2:ccc::|\n");

    Repository repo;
    repo.load_index();

    // find_provider for "editor"
    auto pkg = repo.find_provider("editor");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->name, "vim");

    // find_provider for "sh"
    pkg = repo.find_provider("sh");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->name, "busybox");
}

TEST_F(AggregatedIndexTest, MixedAggregatedAndSimpleLines)
{
    // Mix: some packages single-version, some multi-version
    write_index(
        "zlib|1.2.13:a1::|\n"
        "glibc|2.35:b1::;2.36:b2::|\n"
        "coreutils|9.0:c1::|\n"
        "libfoo|1.0:d1:ncurses:;2.0:d2:ncurses,glibc:|\n");

    Repository repo;
    repo.load_index();

    EXPECT_TRUE(repo.find_package("zlib").has_value());
    EXPECT_TRUE(repo.find_package("glibc").has_value());
    EXPECT_TRUE(repo.find_package("coreutils").has_value());
    EXPECT_TRUE(repo.find_package("libfoo").has_value());

    // glibc should have both versions loaded
    EXPECT_TRUE(repo.find_package("glibc", "2.35").has_value());
    EXPECT_TRUE(repo.find_package("glibc", "2.36").has_value());

    // libfoo with deps on version 2.0
    auto pkg = repo.find_package("libfoo", "2.0");
    ASSERT_TRUE(pkg.has_value());
    ASSERT_EQ(pkg->dependencies.size(), 2);
}

TEST_F(AggregatedIndexTest, ProvidesWithAggregatedVersions)
{
    write_index(
        "vim|9.0:aaa:ncurses:editor,text-editor:;9.1:bbb:ncurses,glibc:editor,text-editor:|\n");

    Repository repo;
    repo.load_index();

    // Both versions loaded
    EXPECT_TRUE(repo.find_package("vim", "9.0").has_value());
    EXPECT_TRUE(repo.find_package("vim", "9.1").has_value());

    // Provides set on both versions (line-level, shared)
    auto pkg = repo.find_provider("editor");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->name, "vim");

    pkg = repo.find_provider("text-editor");
    ASSERT_TRUE(pkg.has_value());
}

TEST_F(AggregatedIndexTest, EmptyIndex)
{
    write_index("");

    Repository repo;
    repo.load_index();

    EXPECT_FALSE(repo.find_package("anything").has_value());
}

TEST_F(AggregatedIndexTest, CommentLines)
{
    write_index(
        "# this is a comment\n"
        "zlib|1.0:a::|\n"
        "# another comment\n"
        "libfoo|2.0:b::|\n");

    Repository repo;
    repo.load_index();

    EXPECT_TRUE(repo.find_package("zlib").has_value());
    EXPECT_TRUE(repo.find_package("libfoo").has_value());
}

TEST_F(AggregatedIndexTest, VersionSortingAcrossAggregatedLine)
{
    write_index("zlib|1.0:a:;1.5:b:;0.9:c:\n");

    Repository repo;
    repo.load_index();

    // find_package(name) without version should return latest
    auto pkg = repo.find_package("zlib");
    ASSERT_TRUE(pkg.has_value());
    EXPECT_EQ(pkg->version, "1.5");
}

// 回归：find_provider 必须返回真正提供该 capability 的版本。
// 曾直接返回 find_package(name)（最新版）——旧版提供、最新版不提供的
// capability 会被误判为"无提供者"，导致假"未解析 SONAME"失败。
TEST_F(AggregatedIndexTest, FindProviderReturnsVersionThatActuallyProvides)
{
    write_index("libfoo|1.0:aaa::libold.so.1:libc.so.6;2.0:bbb::libnew.so.1:libc.so.6|\n");

    Repository repo;
    repo.load_index();

    // libold.so.1 只有 v1.0 提供 → 必须返回 v1.0，而不是最新版 v2.0
    auto old_prov = repo.find_provider("libold.so.1");
    ASSERT_TRUE(old_prov.has_value());
    EXPECT_EQ(old_prov->version, "1.0");

    // libnew.so.1 由最新版提供 → 返回 v2.0
    auto new_prov = repo.find_provider("libnew.so.1");
    ASSERT_TRUE(new_prov.has_value());
    EXPECT_EQ(new_prov->version, "2.0");

    // 无提供者 → nullopt
    EXPECT_FALSE(repo.find_provider("libmissing.so.1").has_value());
}

// ============================================================================
// 索引 deps 字段里的复合约束（TODO.md D2）
//
// 索引用 ',' 连接各依赖，而依赖语法**本身**也用 ',' 表达复合约束
// （`"cmake >= 3.20, < 4.0"` 是一个依赖）。旧解析直接按 ',' 拆再逐个解析，
// 于是 "< 4.0" 变成**空名依赖**（libsolv ID_EMPTY：不解析不报错，求解静默产出空事务）。
// 修法：以"操作符开头"为判据把续接片段合回上一条 —— 依赖名不会这样开头。
// ============================================================================

TEST_F(AggregatedIndexTest, CompoundConstraintInDepsFieldStaysOneDependency)
{
    write_index("libfoo|1.0:aaa:cmake >= 3.20, < 4.0::|\n");

    Repository repo;
    repo.load_index();
    auto info = repo.find_package("libfoo", "1.0");
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->dependencies.size(), 1u) << "复合约束被拆成了多个依赖";
    EXPECT_EQ(info->dependencies[0].name, "cmake");
    ASSERT_EQ(info->dependencies[0].constraints.size(), 2u);
    EXPECT_EQ(info->dependencies[0].constraints[0].op, ">=");
    EXPECT_EQ(info->dependencies[0].constraints[0].version, "3.20");
    EXPECT_EQ(info->dependencies[0].constraints[1].op, "<");
    EXPECT_EQ(info->dependencies[0].constraints[1].version, "4.0");
}

TEST_F(AggregatedIndexTest, CompoundConstraintMixedWithPlainDeps)
{
    write_index("app|1.0:aaa:glibc, cmake >= 3.20, < 4.0, ncurses::|\n");

    Repository repo;
    repo.load_index();
    auto info = repo.find_package("app", "1.0");
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->dependencies.size(), 3u);  // glibc / cmake(复合) / ncurses
    EXPECT_EQ(info->dependencies[0].name, "glibc");
    EXPECT_EQ(info->dependencies[1].name, "cmake");
    EXPECT_EQ(info->dependencies[1].constraints.size(), 2u);
    EXPECT_EQ(info->dependencies[2].name, "ncurses");
    // 关键：**没有任何空名依赖**
    for (const auto& d : info->dependencies) EXPECT_FALSE(d.name.empty());
}

TEST_F(AggregatedIndexTest, EmptyAndTrailingCommasProduceNoDependencies)
{
    write_index("empty1|1.0:aaa:,::|\nempty2|1.0:aaa:glibc,,::|\n");

    Repository repo;
    repo.load_index();

    auto e1 = repo.find_package("empty1", "1.0");
    ASSERT_TRUE(e1.has_value());
    EXPECT_TRUE(e1->dependencies.empty()) << "空 deps 字段不应产生依赖";

    auto e2 = repo.find_package("empty2", "1.0");
    ASSERT_TRUE(e2.has_value());
    ASSERT_EQ(e2->dependencies.size(), 1u);
    EXPECT_EQ(e2->dependencies[0].name, "glibc");
}

/**
 * 索引路径**是个目录**（"存在但根本读不出内容"的一种）⇒ 仓库为空，但**必须留下可见告警**。
 *
 * ## 实测（2026-09-26）：目录**不会**让 `ifstream::open` 失败
 * 我原先按"造个目录就能命中 `repo_index_unreadable` 分支"写，**红**了 —— 捕获到的 stderr 是
 * `Warning: Repository index parsed to zero packages (empty or truncated?): …/index.txt`。
 * 原因：Linux 上 `std::ifstream` **打开目录是成功的**（失败的是随后的读），于是
 * `!file.is_open()` 那道闸不拦它，流程落到"解析出 0 个包"那条告警上。
 * ⇒ 源码里"文件存在但打不开（权限 / **竟是个目录**）"这句的**目录那半是错的**，已订正。
 *
 * ## 由此带出的更重要结论：`repo_index_unreadable` 分支**在真实条件下几乎不可达**
 * lpkg 永远以 root 跑 ⇒ 权限位挡不住它；目录又能被打开。要构造出 `open` 真失败得靠 FIFO/设备
 * 之类，属人为场景。**真正兜住"索引损坏/是目录/被截断"的是"解析出 0 个包"这条告警** ——
 * 它就是用户可见的那个"不静默"。所以本用例钉的是**那条**（可达、且是真实防线）。
 *
 * ## 为什么"不静默"是这里唯一能断言的行为差异
 * "静默当成空仓库"与"告警后当成空仓库"给出的 `packages()` **都是空** —— 只有告警能区分。
 * 而静默的后果不是"少几个包"，而是上层把"仓库为空"读成"一切正常"：`lpkg upgrade` 会打印
 * "所有包都已是最新版本"并 exit 0（D4 事故形态）。
 * `log_warning` 落 `std::cerr`（`base/utils.cpp:103`），故换 `rdbuf` 捕获
 * （同 `tests/unit/test_cli_dispatch.cpp` 的手法）。
 */
TEST_F(AggregatedIndexTest, DirectoryIndexIsReportedNotEmptyRepo)
{
    // 用一个**目录**占住索引路径：它存在（`exists_follow` 为真）但读不出任何行
    fs::create_directories(index_dir / "index.txt");

    std::ostringstream cap;
    auto* old = std::cerr.rdbuf(cap.rdbuf());
    Repository repo;
    repo.load_index();
    std::cerr.rdbuf(old);

    EXPECT_TRUE(repo.packages().empty());

    // 关键锚点：**告警真的出现了**（键存在、且被渲染出来）。模板带 `{}`，取占位符前的前缀匹配。
    const std::string tmpl = get_string("warning.repo_index_empty");
    const auto cut = tmpl.find("{}");
    const std::string needle = tmpl.substr(0, cut == std::string::npos ? tmpl.size() : cut);
    ASSERT_FALSE(needle.empty()) << "l10n 键缺失（get_string 回了 [MISSING_STRING]？）：" << tmpl;
    EXPECT_NE(cap.str().find(needle), std::string::npos)
        << "索引读不出内容时**必须留下可见告警**，否则就是静默当成空仓库。捕获到的 stderr：\n"
        << cap.str();
}
