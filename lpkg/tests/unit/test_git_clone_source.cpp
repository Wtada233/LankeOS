#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "base/exception.hpp"
#include "builder_executor.hpp"
#include "i18n/localization.hpp"

namespace fs = std::filesystem;

// ============================================================================
// `git+<url>@<ref>` 克隆路径（libgit2）—— 真实构建里天天走，此前**零直接覆盖**。
//
// 全部用**本地 fixture 仓库 + `file://` URL**，不碰网络：SetUp 里用 `git` 二进制
// 造一个含 2 个 commit / 1 个轻量 tag / 1 个 annotated tag / 1 个分支的仓库，
// 然后让 libgit2 从它克隆。
//
// 这些用例是 `clone_git_source` 拆分（2026-09-26）的**行为基线**：拆的是形状不是语义，
// 所以它们必须在拆分前后都绿。
// ============================================================================

namespace
{

/// `git` 二进制在这个测试环境里可用吗？（不可用则跳过整个套件，而不是假绿）
bool git_binary_available()
{
    return std::system("git --version >/dev/null 2>&1") == 0;
}

/// 跑一条 shell 命令，返回退出码（fixture 路径无空格，双引号包裹即足够）
int run_cmd(const std::string& cmd)
{
    return std::system(cmd.c_str());
}

std::string read_file(const fs::path& p)
{
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << content;
}

}  // namespace

class GitCloneSourceTest : public ::testing::Test
{
protected:
    fs::path root;       // 本用例独占的临时根
    fs::path upstream;   // 本地"上游"仓库（非 bare，libgit2 直接 file:// 克隆它）
    fs::path work_root;  // 克隆目标父目录
    std::string url_base;

    static constexpr const char* kRepoName = "upstream-repo";

    void SetUp() override
    {
        init_localization();
        // 先定 root：GTEST_SKIP 之后 TearDown 仍会跑，那时 root 必须是合法路径
        root = fs::current_path() / "tmp_git_clone_source_test";
        fs::remove_all(root);

        if (!git_binary_available()) {
            GTEST_SKIP() << "测试环境里没有 git 二进制，无法造本地 fixture 仓库";
        }

        work_root = root / "work";
        fs::create_directories(work_root);
        upstream = root / kRepoName;

        create_upstream_repo();
        url_base = "file://" + upstream.string();
    }

    void TearDown() override
    {
        fs::remove_all(root);
    }

    /// 克隆结果目录（`work_root/<仓库名>`，`.git` 后缀被剥掉）
    fs::path dest() const
    {
        return work_root / kRepoName;
    }

    std::string git(const std::string& args) const
    {
        return "git -C \"" + upstream.string() + "\" " + args;
    }

    void must_run(const std::string& cmd) const
    {
        ASSERT_EQ(run_cmd(cmd), 0) << "fixture 命令失败: " << cmd;
    }

    /**
     * 上游仓库形态（file.txt 的内容就是"哪个 commit"的指纹）：
     *   c1 "main\n"  ← 轻量 tag v1.0
     *   c2 "second\n"← annotated tag v2.0（覆盖 peel 到 commit 的那条路）
     *   branch `feature` 从 c1 拉出，额外有 feature.txt
     * 默认分支 = main（指向 c2）
     */
    void create_upstream_repo() const
    {
        must_run("git init -q -b main \"" + upstream.string() + "\"");
        must_run(git("config user.email test@example.com"));
        must_run(git("config user.name \"Lpkg Test\""));
        must_run(git("config commit.gpgsign false"));
        must_run(git("config tag.gpgsign false"));

        write_file(upstream / "file.txt", "main\n");
        must_run(git("add -A"));
        must_run(git("commit -q -m c1"));
        must_run(git("tag v1.0"));

        write_file(upstream / "file.txt", "second\n");
        must_run(git("add -A"));
        must_run(git("commit -q -m c2"));
        must_run(git("tag -a v2.0 -m \"release 2.0\""));

        // 分支从 c1 拉出：内容与 v1.0 相同，另有 feature.txt —— 用它把"分支 ref"与
        // "tag ref"、"默认 HEAD"三者区分开
        must_run(git("checkout -q -b feature v1.0"));
        write_file(upstream / "feature.txt", "only-on-feature\n");
        must_run(git("add -A"));
        must_run(git("commit -q -m c3"));
        must_run(git("checkout -q main"));

        // 名字里带 '/' 的分支：用来钉住 parse_git_url 的已知限制（见下方用例）
        must_run(git("branch topic/slash v1.0"));
    }
};

// ── ① 正常克隆（tag / 分支 / 默认 HEAD 三种 ref 形态）────────────────────────

TEST_F(GitCloneSourceTest, CloneTagChecksOutThatTag)
{
    clone_git_source("git+" + url_base + "@v1.0", work_root);

    ASSERT_TRUE(fs::is_directory(dest())) << "克隆目标目录不存在: " << dest();
    EXPECT_TRUE(fs::exists(dest() / ".git")) << "目标不是 git 仓库";
    EXPECT_EQ(read_file(dest() / "file.txt"), "main\n") << "checkout 的不是 v1.0 那个 commit";
    EXPECT_FALSE(fs::exists(dest() / "feature.txt")) << "v1.0 上不该有分支的文件";
}

TEST_F(GitCloneSourceTest, CloneAnnotatedTagPeelsToCommit)
{
    // annotated tag 是 tag 对象、不是 commit：必须 peel 到 commit 才能 checkout
    clone_git_source("git+" + url_base + "@v2.0", work_root);

    ASSERT_TRUE(fs::is_directory(dest()));
    EXPECT_EQ(read_file(dest() / "file.txt"), "second\n")
        << "annotated tag 没有被 peel 到它指向的 commit";
}

TEST_F(GitCloneSourceTest, CloneBranchChecksOutBranchTip)
{
    clone_git_source("git+" + url_base + "@feature", work_root);

    ASSERT_TRUE(fs::is_directory(dest()));
    // 分支 ref 不在 refs/tags 也不在 refs/heads（只拉成 refs/remotes/origin/<ref>）
    // → 覆盖 clone_git_source 里 "revparse 失败则回退 refs/remotes/origin/" 那条路
    EXPECT_EQ(read_file(dest() / "feature.txt"), "only-on-feature\n")
        << "分支 ref 没有 checkout 到分支 tip";
    EXPECT_EQ(read_file(dest() / "file.txt"), "main\n");
}

TEST_F(GitCloneSourceTest, CloneWithoutRefUsesDefaultHead)
{
    // 无 @ref → ref 缺省 HEAD → 走 git_clone + git_checkout_head（与指定 ref 是两条路）
    clone_git_source("git+" + url_base, work_root);

    ASSERT_TRUE(fs::is_directory(dest()));
    EXPECT_EQ(read_file(dest() / "file.txt"), "second\n") << "默认分支（main = c2）没有被 checkout";
    EXPECT_FALSE(fs::exists(dest() / "feature.txt"));
}

TEST_F(GitCloneSourceTest, CloneStripsGitSuffixFromDestinationName)
{
    // `…/upstream-repo.git` → 目标目录名是 upstream-repo（剥掉 .git）
    const fs::path aliased = root / (std::string(kRepoName) + ".git");
    std::error_code ec;
    fs::create_directory_symlink(upstream, aliased, ec);
    ASSERT_FALSE(ec) << "造不出 …/upstream-repo.git 别名: " << ec.message();

    clone_git_source("git+file://" + aliased.string() + "@v1.0", work_root);

    EXPECT_TRUE(fs::is_directory(dest())) << "目标目录名没有剥掉 .git 后缀";
    EXPECT_EQ(read_file(dest() / "file.txt"), "main\n");
}

// ── ② 目标已存在 ─────────────────────────────────────────────────────────────
//
// **实测语义（2026-09-26，与代码一致）**：`clone_git_source` 在克隆前对 `dest`
// 无条件 `fs::remove_all(dest)` —— 既不报错也不增量复用，而是**先清空再全新克隆**。
// 下面两条按实测语义钉住：残留内容必须消失，而不是被保留/合并或让克隆失败。

TEST_F(GitCloneSourceTest, ExistingDestinationDirectoryIsWipedAndRecloned)
{
    fs::create_directories(dest());
    write_file(dest() / "stray.txt", "stale\n");
    write_file(dest() / "nested/deep.txt", "stale\n");

    clone_git_source("git+" + url_base + "@v1.0", work_root);

    EXPECT_FALSE(fs::exists(dest() / "stray.txt")) << "旧目录内容没有被清掉";
    EXPECT_FALSE(fs::exists(dest() / "nested")) << "旧目录的嵌套子目录没有被清掉";
    EXPECT_EQ(read_file(dest() / "file.txt"), "main\n") << "重置后没有真正克隆";
}

TEST_F(GitCloneSourceTest, ExistingDestinationAsRegularFileIsAlsoReplaced)
{
    // dest 是**普通文件**（不是目录）时同样被 remove_all 清掉 —— 顺带证明
    // `safe_name_from_url` + dest 推导没有被 `..`/空名之类的输入带偏
    write_file(dest(), "i am a file, not a directory\n");
    ASSERT_TRUE(fs::is_regular_file(dest()));

    clone_git_source("git+" + url_base + "@v1.0", work_root);

    ASSERT_TRUE(fs::is_directory(dest())) << "挡路的普通文件没有被清掉";
    EXPECT_EQ(read_file(dest() / "file.txt"), "main\n");
}

// ── ③ 失败路径：消息必须点名 URL / ref ───────────────────────────────────────

TEST_F(GitCloneSourceTest, NonexistentRepoFailsAndNamesTheUrl)
{
    const std::string bad_url = "file://" + (root / "no-such-repo").string();

    try {
        clone_git_source("git+" + bad_url + "@v1.0", work_root);
        FAIL() << "克隆一个不存在的仓库竟然成功了";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find(bad_url), std::string::npos)
            << "报错没有点名 URL（只有一句含糊的\"克隆失败\"）: " << msg;
        // 顺带钉住 l10n 键存在：键缺失时 get_string 吐的是 "[MISSING_STRING: …]"，
        // 那种报错对用户同样毫无信息量
        EXPECT_EQ(msg.find("[MISSING_STRING"), std::string::npos)
            << "l10n 缺少该报错键，报错成了占位符: " << msg;
    }
}

TEST_F(GitCloneSourceTest, NonexistentRefFailsAndNamesTheRefAndUrl)
{
    try {
        clone_git_source("git+" + url_base + "@no-such-ref", work_root);
        FAIL() << "克隆一个不存在的 ref 竟然成功了";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("no-such-ref"), std::string::npos) << "报错没有点名 ref: " << msg;
        EXPECT_NE(msg.find(url_base), std::string::npos) << "报错没有点名 URL: " << msg;
    }
}

TEST_F(GitCloneSourceTest, RelativeParentRefIsRejectedBeforeAnyClone)
{
    // `git+https://host/a/b/..` → 目标名推导出 ".."，必须在碰盘**之前**被拒
    // （否则 `work_root/".."` 的 remove_all 会删掉整个构建目录）
    EXPECT_THROW(clone_git_source("git+https://host/a/b/..", work_root), LpkgException);
    EXPECT_TRUE(fs::exists(work_root)) << "work_root 被删掉了";
}

TEST_F(GitCloneSourceTest, BranchNameWithSlashIsNotSupported)
{
    // 名字带 '/' 的分支（`topic/slash`）今天**取不到**：parse_git_url 把 `@topic/slash`
    // 整段留在 URL 上 → 克隆的是一个不存在的 URL → 以 error.git_clone_failed 失败。
    // 报错里点名的是那个被拼坏的 URL（含 "topic/slash"），用户能据此定位。
    // ⚠️ 现状即为如此，本轮（纯搬移）不改；修法见 parse_git_url 处的 ⚠️ 说明。
    try {
        clone_git_source("git+" + url_base + "@topic/slash", work_root);
        FAIL() << "`@topic/slash` 被当成 ref 解析了（限制已消失？请更新本用例与注释）";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("topic/slash"), std::string::npos)
            << "报错没有带上被拼坏的 URL，用户无法定位: " << msg;
    }
}

TEST_F(GitCloneSourceTest, FailedCloneLeavesPartialRepoBehind)
{
    // ⚠️ **实测（2026-09-26）**：`clone_git_source` 失败时**不清理半成品目录** ——
    // `prepare_repo` 每轮先 `remove_all(dest)` 再 `git_repository_init(dest)`，fetch 失败后
    // 直接返回错误码退出循环，那个刚 init 出来的空仓库（只有 `.git/`）**留在盘上**。
    // 本用例只是把现状钉住（本轮纯搬移，语义不动）；**要不要清理另议** ——
    // 半成品是空仓库、下次同 URL 克隆会先被 remove_all 掉，所以不是"污染"级别的问题。
    try {
        clone_git_source("git+file://" + (root / "no-such-repo").string() + "@v1.0", work_root);
        FAIL() << "克隆一个不存在的仓库竟然成功了";
    } catch (const LpkgException&) {
        // 预期路径
    }

    const fs::path half = work_root / "no-such-repo";
    EXPECT_TRUE(fs::is_directory(half))
        << "半成品目录被清掉了（清理逻辑变了？请同步更新本用例的说明）";
    EXPECT_TRUE(fs::exists(half / ".git")) << "留在盘上的不是 init 出来的空仓库";
    EXPECT_FALSE(fs::exists(half / "file.txt")) << "失败的克隆竟然 checkout 出了文件";
}

// ── URL 解析（拆分时被抽出的纯函数，逐条钉住既有语义）────────────────────────

TEST(GitUrlParsingTest, DetectsGitScheme)
{
    EXPECT_TRUE(is_git_url("git+https://host/x.git"));
    EXPECT_TRUE(is_git_url("git+file:///srv/x"));
    EXPECT_FALSE(is_git_url("https://host/x.tar.gz"));
    EXPECT_FALSE(is_git_url(""));
}

TEST(GitUrlParsingTest, SplitsRefAfterLastSlash)
{
    std::string url, ref;

    parse_git_url("git+https://host/g/repo.git@v1.2.3", url, ref);
    EXPECT_EQ(url, "https://host/g/repo.git");
    EXPECT_EQ(ref, "v1.2.3");

    // 无 ref → HEAD
    parse_git_url("git+https://host/g/repo.git", url, ref);
    EXPECT_EQ(url, "https://host/g/repo.git");
    EXPECT_EQ(ref, "HEAD");

    // URL 自带的认证信息里的 '@' 在最后一个 '/' **之前**，不能当 ref 分隔符
    parse_git_url("git+ssh://git@host/repo.git", url, ref);
    EXPECT_EQ(url, "ssh://git@host/repo.git");
    EXPECT_EQ(ref, "HEAD");

    parse_git_url("git+ssh://git@host/repo.git@main", url, ref);
    EXPECT_EQ(url, "ssh://git@host/repo.git");
    EXPECT_EQ(ref, "main");

    // ⚠️ **已知限制（实测 2026-09-26，非本轮引入）**：ref 里**不能有 '/'**。分隔规则是
    // "取最后一个 '/' 之后的 '@'"，所以 `@feature/x` 里那个 '@' 在最后一个 '/' 之前
    // → 整串被当成"无 ref"，`/x` 留在 URL 上、ref 回落 HEAD。后果是**静默克隆默认分支**
    // （不是报错）—— `git+<url>@feature/x` 这种写法今天拿不到分支 x。
    // 本轮只搬移不改语义，故按现状钉住；要支持得改成"先在 refs/heads / refs/tags 里探测"。
    parse_git_url("git+https://host/repo.git@feature/x", url, ref);
    EXPECT_EQ(url, "https://host/repo.git@feature/x");
    EXPECT_EQ(ref, "HEAD");
}
