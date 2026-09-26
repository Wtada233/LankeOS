#include <git2.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "base/exception.hpp"
#include "builder_executor.hpp"
#include "i18n/localization.hpp"

namespace fs = std::filesystem;

// ============================================================================
// `git+` 源的 **submodule 递归拉取** —— `update_submodules` / `update_one_submodule`
// （builder_executor.cpp），`--recurse-submodules` 的等价物。
//
// 这两条函数在真实配方里在用（根 CLAUDE.md 的源码约定：git+ 源不额外加 git build_dep，
// 因为 "子模块 --recurse-submodules 自动拉"），而在此之前 tests/ 里**一次都没引用过**
// —— 只能靠 run_build 间接撞。
//
// 手法与 test_git_clone_source.cpp 完全一致：本地 fixture 仓库 + `file://`，**全程不碰网**；
// `git` 二进制只用来**造** fixture，被验证的是 lpkg 里 libgit2 的那条路。入口是
// `clone_git_source`（`update_submodules` 在生产里唯一的调用者 —— 它只在刚克隆完的新仓库上跑）。
//
// ⚠️ **造 fixture 的关键坑（git >= 2.38.1）**：`git submodule add file://…` 默认被
// `protocol.file.allow=user` 拦住：
//     fatal: transport 'file' not allowed
//     fatal: clone of 'file:///…' into submodule path '…' failed
// 必须 `git -c protocol.file.allow=always submodule add …`。
// **这纯粹是 git 二进制（submodule--helper）的限制**：libgit2 不发子进程、也不读这个配置，
// 它只按 `.gitmodules` 里记的 URL 自己 fetch —— 所以生产路径（lpkg 用 libgit2）不受影响。
// 本文件的用例本身就是证据：fixture 用 `-c protocol.file.allow=always` 造出来之后，
// libgit2 侧把同样的 `file://` 子模块正常拉了下来。
// ============================================================================

namespace
{

/// `git` 二进制在这个测试环境里可用吗？（不可用则跳过，而不是假绿）
bool git_binary_available()
{
    return std::system("git --version >/dev/null 2>&1") == 0;
}

/// 跑一条 shell 命令，返回退出码（fixture 路径无空格，双引号包裹即足够）
int run_cmd(const std::string& cmd)
{
    return std::system(cmd.c_str());
}

/// 跑一条命令并捕获 stdout（去掉行尾换行）；非 0 退出返回 false 且 out 清空。
bool run_capture(const std::string& cmd, std::string& out)
{
    out.clear();
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        return false;
    }
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        out += buf;
    }
    const int rc = ::pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    if (rc != 0) {
        out.clear();
        return false;
    }
    return true;
}

void write_file(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << content;
}

std::string read_file(const fs::path& p)
{
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

class GitSubmoduleTest : public ::testing::Test
{
protected:
    fs::path root;       // 本用例独占的临时根
    fs::path work_root;  // 克隆目标父目录

    static constexpr const char* kParentRepo = "parent-repo";

    void SetUp() override
    {
        init_localization();
        // 先定 root：GTEST_SKIP 之后 TearDown 仍会跑，那时 root 必须是合法路径
        root = fs::current_path() / "tmp_git_submodules_test";
        fs::remove_all(root);

        if (!git_binary_available()) {
            GTEST_SKIP() << "测试环境里没有 git 二进制，无法造本地 fixture 仓库";
        }

        work_root = root / "work";
        fs::create_directories(work_root);
    }

    void TearDown() override
    {
        fs::remove_all(root);
    }

    // ── fixture 零件 ────────────────────────────────────────────────────────

    void must_run(const std::string& cmd) const
    {
        ASSERT_EQ(run_cmd(cmd), 0) << "fixture 命令失败: " << cmd;
    }

    std::string git(const fs::path& repo, const std::string& args) const
    {
        return "git -C \"" + repo.string() + "\" " + args;
    }

    /// 建一个已 init + 配好身份/签名开关的仓库（fixture 提交绝不依赖宿主 git 配置）
    void make_repo(const fs::path& repo) const
    {
        must_run("git init -q -b main \"" + repo.string() + "\"");
        must_run(git(repo, "config user.email test@example.com"));
        must_run(git(repo, "config user.name \"Lpkg Test\""));
        must_run(git(repo, "config commit.gpgsign false"));
        must_run(git(repo, "config tag.gpgsign false"));
    }

    void commit_file(const fs::path& repo, const std::string& rel, const std::string& content,
                     const std::string& msg) const
    {
        write_file(repo / rel, content);
        must_run(git(repo, "add -A"));
        must_run(git(repo, "commit -q -m \"" + msg + "\""));
    }

    /// 把 sub_upstream 作为 submodule 挂到 parent 的 sub_path 上，并提交父仓库。
    ///
    /// `-c protocol.file.allow=always` 是**必需**的（见文件头）：git 默认拒绝把 `file://`
    /// 当子模块传输。libgit2 侧无此限制。
    void add_submodule(const fs::path& parent, const fs::path& sub_upstream,
                       const std::string& sub_path) const
    {
        must_run("git -C \"" + parent.string() +
                 "\" -c protocol.file.allow=always submodule add -q \"file://" +
                 sub_upstream.string() + "\" " + sub_path);
        must_run(git(parent, "commit -q -m \"add submodule " + sub_path + "\""));
    }

    /// 父仓库 HEAD 里记录的 submodule commit（= 索引里的 gitlink）
    std::string pinned_commit(const fs::path& repo, const std::string& sub_path) const
    {
        std::string out;
        EXPECT_TRUE(run_capture(git(repo, "rev-parse HEAD:" + sub_path), out))
            << "取不到 " << repo << " 里 " << sub_path << " 的 gitlink";
        return out;
    }

    /// 克隆结果里子模块**实际** checkout 出来的 commit（update_one_submodule 置的 detached HEAD）
    std::string checked_out_commit(const std::string& sub_path) const
    {
        std::string out;
        EXPECT_TRUE(run_capture(git(dest() / sub_path, "rev-parse HEAD"), out))
            << "子模块 " << sub_path << " 不是可读的 git 仓库（没被拉下来？）";
        return out;
    }

    /// 父仓库的克隆结果目录（`work_root/<仓库名>`）
    fs::path dest() const
    {
        return work_root / kParentRepo;
    }

    /// 子模块仓库里 `origin/main` 上**可达的 commit 数**（用 git 二进制数）。
    /// 浅拉（depth=1）会把历史截断在浅边界上 ⇒ 只数到 1；完整拉则数到全部。
    /// 这是"某一轮到底是浅拉还是完整拉"的**直接机械判据**。
    /// （`sub/.git/shallow` 不是判据 —— 实测 libgit2 在这条路上不留下它。）
    int reachable_commit_count(const std::string& sub_path) const
    {
        std::string out;
        EXPECT_TRUE(run_capture(git(dest() / sub_path, "rev-list --count origin/main"), out))
            << "数不了 " << sub_path << " 里 origin/main 的历史（浅边界坏了？）";
        return out.empty() ? -1 : std::atoi(out.c_str());
    }

    std::string parent_url(const fs::path& upstream) const
    {
        return "file://" + upstream.string();
    }

    /// 父仓库的标准布局：`root/<name>`
    fs::path upstream_path(const std::string& name) const
    {
        return root / name;
    }

    /// 跑一次失败路径并交出异常文案；**没抛**则 FAIL（返回空串）。
    std::string clone_expect_failure(const std::string& url) const
    {
        try {
            clone_git_source(url, work_root);
        } catch (const LpkgException& e) {
            return e.what();
        } catch (const std::exception& e) {
            ADD_FAILURE() << "抛的不是 LpkgException: " << e.what();
            return {};
        }
        ADD_FAILURE() << "克隆竟然成功了: " << url;
        return {};
    }
};

// ── ① 有子模块：拉下来、内容正确、且停在**父仓库钉住的那个 commit** 上 ──────────

TEST_F(GitSubmoduleTest, SubmoduleIsFetchedAtTheCommitPinnedByParent)
{
    // fixture：leaf 有 c1(leaf-v1) → c2(leaf-v2)；父仓库在 c2 处挂上它；
    // 之后 leaf 又前进到 c3(leaf-v3) —— 父仓库的 gitlink **仍钉在 c2**。
    //
    // 于是这条用例同时压两件事：
    //   1. 子模块不是"拉个默认分支就完事"，而是检出父仓库记录的 **index id**；
    //   2. `prepare_repo` 的**浅拉未命中 → 完整拉兜底**：depth=1 只拿得到分支 tip(c3)，
    //      钉住的 c2 不在里面 → 必须退到 depth=0 才拿得到（这是该兜底存在的理由）。
    const fs::path leaf = upstream_path("leaf-repo");
    make_repo(leaf);
    commit_file(leaf, "leaf.txt", "leaf-v1\n", "c1");
    commit_file(leaf, "leaf.txt", "leaf-v2\n", "c2");

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, leaf, "sub");

    // 父仓库已经提交；现在让 leaf 前进到 c3（父仓库的 gitlink 不动）
    commit_file(leaf, "leaf.txt", "leaf-v3\n", "c3");

    const std::string pinned = pinned_commit(parent, "sub");
    ASSERT_FALSE(pinned.empty());

    clone_git_source("git+" + parent_url(parent), work_root);

    EXPECT_EQ(read_file(dest() / "parent.txt"), "parent\n") << "父仓库本身没 checkout 对";
    EXPECT_TRUE(fs::is_directory(dest() / "sub")) << "子模块目录没有被拉下来";
    EXPECT_EQ(read_file(dest() / "sub/leaf.txt"), "leaf-v2\n")
        << "子模块内容不是父仓库钉住的那个 commit（拉成了 tip？）";
    EXPECT_EQ(checked_out_commit("sub"), pinned) << "子模块的 HEAD 不在父仓库记录的 gitlink 上";
}

// ── ② 子模块停在它自己的 tip 上（浅拉即命中，不走完整拉兜底）────────────────────

TEST_F(GitSubmoduleTest, SubmodulePinnedAtItsOwnTipUsesShallowFetch)
{
    const fs::path leaf = upstream_path("leaf-repo");
    make_repo(leaf);
    commit_file(leaf, "leaf.txt", "leaf-v1\n", "c1");

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, leaf, "sub");

    const std::string pinned = pinned_commit(parent, "sub");

    clone_git_source("git+" + parent_url(parent), work_root);

    EXPECT_EQ(read_file(dest() / "sub/leaf.txt"), "leaf-v1\n");
    EXPECT_EQ(checked_out_commit("sub"), pinned);
}

// ── ③ 嵌套子模块：leaf 自己还挂着 subsub（--recursive 的等价）──────────────────

TEST_F(GitSubmoduleTest, NestedSubmodulesAreFetchedRecursively)
{
    // parent → sub(leaf) → inner(subsub)：钉住 `update_one_submodule` 里
    // "checkout 成功后再 update_submodules(sub) 递归" 那一跳（必须在 free(sub) 之前）。
    const fs::path subsub = upstream_path("subsub-repo");
    make_repo(subsub);
    commit_file(subsub, "subsub.txt", "subsub-v1\n", "s1");

    const fs::path leaf = upstream_path("leaf-repo");
    make_repo(leaf);
    commit_file(leaf, "leaf.txt", "leaf-v1\n", "c1");
    add_submodule(leaf, subsub, "inner");

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, leaf, "sub");

    const std::string pinned_inner = pinned_commit(leaf, "inner");

    clone_git_source("git+" + parent_url(parent), work_root);

    EXPECT_EQ(read_file(dest() / "sub/leaf.txt"), "leaf-v1\n") << "第一层子模块没拉下来";
    EXPECT_EQ(read_file(dest() / "sub/inner/subsub.txt"), "subsub-v1\n")
        << "嵌套子模块没有被递归拉下来";
    EXPECT_EQ(checked_out_commit("sub/inner"), pinned_inner)
        << "嵌套子模块没停在父仓库钉住的 commit 上";
}

// ── ④ 无子模块：不报错、正常通过 ──────────────────────────────────────────────

TEST_F(GitSubmoduleTest, RepoWithoutSubmodulesClonesWithoutError)
{
    // `git_submodule_foreach` 在无 `.gitmodules` 的仓库上返回 0 + 空列表 —— 这条是
    // "submodule 更新不该把普通 git+ 源搞坏" 的兜底（真实配方里绝大多数 git+ 源都没有子模块）。
    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");

    ASSERT_NO_THROW(clone_git_source("git+" + parent_url(parent), work_root));

    EXPECT_EQ(read_file(dest() / "parent.txt"), "parent\n");
    EXPECT_FALSE(fs::exists(dest() / ".gitmodules")) << "凭空冒出了 .gitmodules";
}

// ── ⑤ 指定 ref（tag）的路径同样会更新子模块，且发生在 checkout 之后 ────────────

TEST_F(GitSubmoduleTest, SubmodulesAreUpdatedOnTheTagRefPathToo)
{
    // 指定 ref 走的是 prepare_ref_repo + checkout_rev（**不是** git_clone），
    // 所以这条用例钉的是调用顺序：必须**先** checkout（`.gitmodules` 落地）
    // **再** update_submodules —— 顺序反了子模块就一个都找不到。
    const fs::path leaf = upstream_path("leaf-repo");
    make_repo(leaf);
    commit_file(leaf, "leaf.txt", "leaf-v1\n", "c1");

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, leaf, "sub");
    must_run(git(parent, "tag v1.0"));

    // 打 tag 之后再前进一个 commit：确认 tag 路径检出的是 tag 那个树
    commit_file(parent, "parent.txt", "parent-2\n", "p2");

    const std::string pinned = pinned_commit(parent, "sub");

    clone_git_source("git+" + parent_url(parent) + "@v1.0", work_root);

    EXPECT_EQ(read_file(dest() / "parent.txt"), "parent\n") << "没有检出 v1.0";
    EXPECT_EQ(read_file(dest() / "sub/leaf.txt"), "leaf-v1\n") << "tag 路径上子模块没被更新";
    EXPECT_EQ(checked_out_commit("sub"), pinned);
}

// ── ⑥ 子模块的落盘形态：独立仓库，不是 git 的 gitfile + .git/modules 布局 ───────

TEST_F(GitSubmoduleTest, SubmoduleIsASelfContainedRepoNotAGitfileIntoModulesDir)
{
    // libgit2 路径下每个 submodule 是**独立仓库**（`sub/.git` 是真目录，由
    // `prepare_repo` 的 git_repository_init 建出来），不是 git 二进制那种
    // `.git` 文件 + 父仓库 `.git/modules/<name>` 的记账布局。
    //
    // 影响面（钉住现状，供以后判断）：从子模块目录里跑 `git status/log/fetch` 都正常
    // （origin 是 prepare_repo 建的，指向 .gitmodules 里那个 URL），但父仓库侧没有
    // submodule 的记账 —— 依赖 `git submodule update --init` 之类的下游脚本拿不到东西。
    // lpkg 的场景只是"把源码树拉下来"，构建脚本不跑这些。
    const fs::path leaf = upstream_path("leaf-repo");
    make_repo(leaf);
    commit_file(leaf, "leaf.txt", "leaf-v1\n", "c1");

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, leaf, "sub");

    clone_git_source("git+" + parent_url(parent), work_root);

    EXPECT_TRUE(fs::is_directory(dest() / "sub/.git")) << "子模块的 .git 不是真目录";
    EXPECT_FALSE(fs::exists(dest() / ".git/modules"))
        << "父仓库里出现了 git 二进制的 .git/modules 记账布局";
}

// ── ⑦ 相对 URL（../xxx）：按父仓库 origin 解析 —— 含 file:// origin 的已知坑 ────

TEST_F(GitSubmoduleTest, RelativeSubmoduleUrlOnAFileOriginIsBrokenByLibgit2UrlRewriting)
{
    // ⚠️ **实测（2026-09-26，libgit2 1.7.2）**：`.gitmodules` 里写相对 URL、且父仓库
    // origin 是 `file://` 形态时，`git_submodule_resolve_url` 产出的 URL 是**畸形的**：
    //     file:///…/parent-repo  +  ../leaf-repo   →   file://…/leaf-repo
    // （绝对路径的第一个 '/' 被吃掉），fetch 于是以
    //     failed to resolve path 'file://…/leaf-repo': No such file or directory
    // 失败。**这是 libgit2 的 URL 重写行为，不是 lpkg 的代码** —— lpkg 只是把
    // `.gitmodules` 里的 URL 原样交给 libgit2 解析。
    //
    // 真实配方里的 git+ 源是 https / ssh 形态，不受影响；对照见下一条用例
    // （RelativeUrlResolutionAnchorsAtParentOriginHost）。
    //
    // 顺带钉住 `../` 的**语义**：libgit2 是"弹出父仓库 URL 的最后一个路径段"——
    // `../leaf-repo` = 父仓库的**兄弟仓库**（与 git 一致），不是父目录的兄弟。
    const fs::path leaf = upstream_path("leaf-repo");
    make_repo(leaf);
    commit_file(leaf, "leaf.txt", "leaf-v1\n", "c1");

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, leaf, "sub");

    // 把 .gitmodules 的 URL 改成相对形态并提交（gitlink 不动）
    must_run(git(parent, "config -f .gitmodules submodule.sub.url ../leaf-repo"));
    must_run(git(parent, "add .gitmodules"));
    must_run(git(parent, "commit -q -m relative-url"));
    ASSERT_NE(read_file(parent / ".gitmodules").find("../leaf-repo"), std::string::npos)
        << "fixture 没写成相对 URL";

    const std::string msg = clone_expect_failure("git+" + parent_url(parent));
    ASSERT_FALSE(msg.empty());

    // 父仓库本身克隆成功了（报错来自 submodule 阶段，不是父仓库）
    EXPECT_EQ(read_file(dest() / "parent.txt"), "parent\n") << "父仓库克隆也一起失败了";
    EXPECT_FALSE(fs::exists(dest() / "sub/leaf.txt")) << "子模块竟然被拉下来了";

    EXPECT_NE(msg.find(parent_url(parent)), std::string::npos) << "报错没有点名父仓库 URL: " << msg;
    // 报错里的那个 URL 就是被 libgit2 拼坏的那一个：兄弟仓库名对，但 scheme 少了一个 '/'
    const std::string broken = "file://" + (root / "leaf-repo").string().substr(1);
    EXPECT_NE(msg.find(broken), std::string::npos)
        << "报错没有点名被解析出来的子模块 URL（期望形如 " << broken << "）: " << msg;
}

TEST_F(GitSubmoduleTest, RelativeUrlResolutionAnchorsAtParentOriginHost)
{
    // 直接问 libgit2 要解析结果（不经 lpkg），把 update_one_submodule 里
    // `git_submodule_resolve_url` 那一步在**两种父 origin 形态**下的行为钉住：
    //   https 形态（真实配方里的形态）→ `../leaf-repo` 解析正确（同 host 同 group）；
    //   file:// 形态（本文件的 fixture 形态）→ 拼坏（见上一条用例）。
    // 这两个断言合起来说明：上一条的失败是 **file:// 空 host** 特有的，不是相对 URL 本身。
    const fs::path leaf = upstream_path("leaf-repo");
    make_repo(leaf);
    commit_file(leaf, "leaf.txt", "leaf-v1\n", "c1");

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, leaf, "sub");

    // 先把父仓库克隆下来（子模块 URL 是绝对的，这一步必成功），拿到一个真实克隆
    clone_git_source("git+" + parent_url(parent), work_root);

    git_libgit2_init();
    git_repository* repo = nullptr;
    ASSERT_EQ(git_repository_open(&repo, (dest() / ".git").string().c_str()), 0);

    auto resolve = [&](const std::string& base, const char* rel) {
        EXPECT_EQ(git_remote_set_url(repo, "origin", base.c_str()), 0);
        git_buf out = GIT_BUF_INIT;
        const int rc = git_submodule_resolve_url(&out, repo, rel);
        const std::string resolved = (rc == 0 && out.ptr != nullptr) ? std::string(out.ptr) : "";
        git_buf_dispose(&out);
        return resolved;
    };

    EXPECT_EQ(resolve("https://example.invalid/group/parent-repo", "../leaf-repo"),
              "https://example.invalid/group/leaf-repo")
        << "https origin 下的相对解析（真实配方形态）";
    EXPECT_EQ(resolve(parent_url(parent), "../leaf-repo"),
              "file://" + (root / "leaf-repo").string().substr(1))
        << "file:// origin 下的相对解析（libgit2 少拼一个 '/'）";

    git_repository_free(repo);
    git_libgit2_shutdown();
}

// ── ⑧ 失败路径：子模块 URL 不可达 ─────────────────────────────────────────────

TEST_F(GitSubmoduleTest, UnreachableSubmoduleUrlFailsAndNamesTheBadUrl)
{
    const fs::path leaf = upstream_path("leaf-repo");
    make_repo(leaf);
    commit_file(leaf, "leaf.txt", "leaf-v1\n", "c1");

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, leaf, "sub");

    // 让 .gitmodules 指向一个不存在的仓库（submodule 的 gitlink 仍有效 —— 失败只可能来自 fetch）
    const fs::path gone = root / "no-such-sub-upstream";
    must_run(git(parent, "config -f .gitmodules submodule.sub.url file://" + gone.string()));
    must_run(git(parent, "add .gitmodules"));
    must_run(git(parent, "commit -q -m bad-url"));

    const std::string msg = clone_expect_failure("git+" + parent_url(parent));
    ASSERT_FALSE(msg.empty());

    EXPECT_NE(msg.find(parent_url(parent)), std::string::npos)
        << "报错没有点名**父仓库** URL: " << msg;
    EXPECT_EQ(msg.find("[MISSING_STRING"), std::string::npos)
        << "l10n 缺少 error.git_submodule_failed 键，报错成了占位符: " << msg;
    // 多子模块的仓库里，光有父 URL 不够：必须能看出是**哪个**子模块出了问题。
    // 这条路上 libgit2 的消息里带着子模块的 URL（= .gitmodules 里那个），够用。
    EXPECT_NE(msg.find(gone.string()), std::string::npos)
        << "报错没有点名不可达的子模块 URL，用户无法定位: " << msg;
}

// ── ⑨ 失败路径：钉住的 commit 在子模块仓库里已不可达 ──────────────────────────

TEST_F(GitSubmoduleTest, SubmodulePinnedToUnreachableCommitNamesTheSubmoduleAndCommit)
{
    // fixture：父仓库钉住 leaf 上一个**只存在于已删除分支**的 commit（再 gc 掉）。
    // 于是 fetch 本身成功（leaf 的默认分支 tip 拿得到），但钉住的那个 object
    // 无论浅拉还是完整拉都拿不到 → prepare_repo 返回 GIT_ENOTFOUND。
    const fs::path leaf = upstream_path("leaf-repo");
    make_repo(leaf);
    commit_file(leaf, "leaf.txt", "leaf-v1\n", "c1");
    must_run(git(leaf, "checkout -q -b doomed"));
    commit_file(leaf, "leaf.txt", "doomed\n", "c2-on-doomed");

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, leaf, "sub");  // 钉在 doomed 分支的 c2 上
    const std::string pinned = pinned_commit(parent, "sub");

    // 让 c2 变成不可达：切回 main、删分支、清 reflog + gc
    must_run(git(leaf, "checkout -q main"));
    must_run(git(leaf, "branch -D doomed"));
    must_run(git(leaf, "reflog expire --expire=now --all"));
    must_run(git(leaf, "gc --prune=now -q"));

    const std::string msg = clone_expect_failure("git+" + parent_url(parent));
    ASSERT_FALSE(msg.empty());

    EXPECT_NE(msg.find(parent_url(parent)), std::string::npos) << "报错没有点名父仓库 URL: " << msg;
    // **已修（2026-09-26）**：这条路径原先的 detail 是 l10n 的通用兜底文案（`error.unknown`）
    // —— 因为 `prepare_repo` 在"fetch 成功但目标 rev 不存在"（`GIT_ENOTFOUND`）时不写
    // `prog->err`，`clone_error_detail` 于是回落成 "Unknown error"，既不知道是哪个子模块、
    // 也不知道它钉的是哪个 commit（而 name / oid_hex / final_url 在 `update_one_submodule`
    // 手里全都有）。现在那条分支会写 `error.git_submodule_rev_missing`，本用例钉住它。
    const std::string detail_tmpl = get_string("error.git_submodule_rev_missing");
    const auto cut = detail_tmpl.find("{}");
    const std::string detail_prefix =
        detail_tmpl.substr(0, cut == std::string::npos ? detail_tmpl.size() : cut);
    ASSERT_FALSE(detail_prefix.empty()) << "l10n 键缺失: " << detail_tmpl;
    EXPECT_NE(msg.find(detail_prefix), std::string::npos)
        << "detail 没有点名『钉住的 commit 拿不到』（回落到通用兜底了？）: " << msg;
    // 三样都必须点到：子模块 URL、钉住的 commit、子模块名。
    // （不能用子模块的**路径** "sub" 当标记 —— 临时目录名 tmp_git_submodules_test 本身含 "sub"。）
    EXPECT_NE(msg.find("leaf-repo"), std::string::npos)
        << "报错没有点名子模块 URL，用户无法定位: " << msg;
    EXPECT_NE(msg.find(pinned), std::string::npos) << "报错没有点名钉住的那个 commit: " << msg;
}

// ── ⑩ 失败路径：`.gitmodules` 有条目但索引里没有对应 gitlink ──────────────────

TEST_F(GitSubmoduleTest, GitmodulesEntryWithoutGitlinkNamesTheSubmodule)
{
    // 半损坏状态：`.gitmodules` 声明了一个 submodule，索引里却没有对应的 gitlink
    // （例如上游手改过 .gitmodules、或源码树是半截的）。此时
    // `git_submodule_index_id` 返回 NULL → update_one_submodule 走
    // "url/path/oid/wd 有空" 那条早退分支（这个分支只有这条路径能撞到：
    // 名字来自 git_submodule_foreach，所以 lookup 不会失败）。
    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    write_file(parent / ".gitmodules", "[submodule \"ghost\"]\n\tpath = ghost\n\turl = file://" +
                                           (root / "ghost-upstream").string() + "\n");
    must_run(git(parent, "add -A"));
    must_run(git(parent, "commit -q -m ghost-submodule"));

    const std::string msg = clone_expect_failure("git+" + parent_url(parent));
    ASSERT_FALSE(msg.empty());

    EXPECT_NE(msg.find(parent_url(parent)), std::string::npos) << "报错没有点名父仓库 URL: " << msg;
    // **已修（2026-09-26）**：这条早退分支原先只写 `error.unknown`，**连入参 `name` 都不报**。
    // 现在写 `error.git_submodule_entry_incomplete`，点名是哪个子模块、缺的是什么
    // （`.gitmodules` 声明了条目、父仓库索引里没有 gitlink）。
    const std::string tmpl = get_string("error.git_submodule_entry_incomplete");
    const auto cut = tmpl.find("{}");
    const std::string prefix = tmpl.substr(0, cut == std::string::npos ? tmpl.size() : cut);
    ASSERT_FALSE(prefix.empty()) << "l10n 键缺失: " << tmpl;
    EXPECT_NE(msg.find(prefix), std::string::npos)
        << "detail 没有点名『条目不完整』（回落到通用兜底了？）: " << msg;
    EXPECT_NE(msg.find("ghost"), std::string::npos)
        << "报错没有点名是哪个 submodule，用户无法定位: " << msg;
}

// ── ⑪ 失败路径：**嵌套**子模块失败时，报错归属给谁 ────────────────────────────
//
// 这条是"报错文案归属"的探针（也是决定 update_one_submodule 能不能拆的依据，见测试报告）：
// 最内层在 `prepare_repo` 失败时**早退**（prog->err = libgit2 的消息，点名坏 URL），
// 然后一路原样返回 —— 但最外层 `update_one_submodule` 在尾巴上**又写了一次**
// prog->err = last_git_error()（`if (err != 0)` 那一段），把内层留下的消息**覆盖**掉。
// 覆盖后还剩不剩有用的信息，取决于 libgit2 的 last-error 是线程局部的、且成功调用不重置它。
// 本用例把实测结果钉住：内层点名的那条消息**活了下来**（外层覆盖写入的是同一条）。
TEST_F(GitSubmoduleTest, NestedSubmoduleFailureStillNamesTheBadInnerUrl)
{
    const fs::path subsub = upstream_path("subsub-repo");
    make_repo(subsub);
    commit_file(subsub, "subsub.txt", "subsub-v1\n", "s1");

    const fs::path leaf = upstream_path("leaf-repo");
    make_repo(leaf);
    commit_file(leaf, "leaf.txt", "leaf-v1\n", "c1");
    add_submodule(leaf, subsub, "inner");

    // leaf 的 `.gitmodules` 里，inner 的 URL 改成不可达
    const fs::path gone = root / "no-such-subsub-upstream";
    must_run(git(leaf, "config -f .gitmodules submodule.inner.url file://" + gone.string()));
    must_run(git(leaf, "add .gitmodules"));
    must_run(git(leaf, "commit -q -m bad-inner-url"));

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, leaf, "sub");

    const std::string msg = clone_expect_failure("git+" + parent_url(parent));
    ASSERT_FALSE(msg.empty());

    // 报错文案的第一段永远是**顶层**父仓库 URL（l10n 的 error.git_submodule_failed）
    EXPECT_NE(msg.find(parent_url(parent)), std::string::npos) << "报错没有点名父仓库 URL: " << msg;
    // detail 里必须还留着"是哪个（嵌套）子模块的 URL 不可达"
    EXPECT_NE(msg.find(gone.string()), std::string::npos)
        << "嵌套子模块的失败原因被外层覆盖没了（只剩父仓库 URL，用户无从定位）: " << msg;
}

// ── ⑬ 并列多个子模块：**按序逐个拉**，首个失败即返回 ──────────────────────────
//
// 此前所有用例的子模块都是**单个**或**嵌套**（一个套一个），没有"并列兄弟"这一形态。
// `update_submodules()` 是先 `git_submodule_foreach` 收名字、再逐个 `update_one_submodule`
// 并在**首个非 0 返回处** `return err` —— 下面两条把这两半各钉一遍：
//   · ⑬ 前面的兄弟**已经拉下来**（checkout 到父仓库钉住的 commit），后面那个才失败
//     ⇒ 证明是"按序逐个做"，不是"先探测全部再一起拉"；
//   · ⑭ 第一个就失败 ⇒ 排在后面的兄弟**压根没被尝试**（"首个失败即返回"）。
//
// 名字取 `sib_a` / `sib_z`：它们在 `.gitmodules` 里的顺序、父仓库索引顺序、字典序三者
// 一致，所以断言不受 `git_submodule_foreach` 具体走哪张表影响。

TEST_F(GitSubmoduleTest, EarlierSiblingIsFetchedBeforeALaterOneFails)
{
    const fs::path good = upstream_path("sib-good-repo");
    make_repo(good);
    commit_file(good, "good.txt", "good-v1\n", "g1");

    const fs::path bad_upstream = upstream_path("sib-bad-repo");
    make_repo(bad_upstream);
    commit_file(bad_upstream, "bad.txt", "bad-v1\n", "b1");

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, good, "sib_a");          // 好：排在前面
    add_submodule(parent, bad_upstream, "sib_z");  // 坏：排在后面

    // 只把 sib_z 的 URL 改成不可达（gitlink 仍有效 ⇒ 失败只可能来自 fetch）
    const fs::path gone = root / "no-such-sib-z-upstream";
    must_run(git(parent, "config -f .gitmodules submodule.sib_z.url file://" + gone.string()));
    must_run(git(parent, "add .gitmodules"));
    must_run(git(parent, "commit -q -m bad-sib-z-url"));

    const std::string msg = clone_expect_failure("git+" + parent_url(parent));
    ASSERT_FALSE(msg.empty());
    EXPECT_NE(msg.find(parent_url(parent)), std::string::npos) << "报错没有点名父仓库 URL: " << msg;
    EXPECT_NE(msg.find(gone.string()), std::string::npos)
        << "报错没有点名不可达的那个**兄弟**子模块 URL: " << msg;

    // 排在前面的兄弟必须已经落地并停在钉住的 commit 上 —— 这才说明是"按序逐个拉"
    ASSERT_TRUE(fs::exists(dest() / "sib_a" / ".git"))
        << "sib_a 排在 sib_z 之前且本身没问题，却不曾被拉下来";
    EXPECT_EQ(checked_out_commit("sib_a"), pinned_commit(parent, "sib_a"))
        << "sib_a 拉下来了却没 checkout 到父仓库钉住的 commit";
    // 坏的那个留下一堆残骸也无妨（整棵 work 树在构建失败后会被重建），关键是**没有静默成功**
}

TEST_F(GitSubmoduleTest, FirstSiblingFailureStopsBeforeLaterSiblings)
{
    const fs::path good = upstream_path("sib-good-repo");
    make_repo(good);
    commit_file(good, "good.txt", "good-v1\n", "g1");

    const fs::path bad_upstream = upstream_path("sib-bad-repo");
    make_repo(bad_upstream);
    commit_file(bad_upstream, "bad.txt", "bad-v1\n", "b1");

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, bad_upstream, "sib_a");  // 坏：排在前面
    add_submodule(parent, good, "sib_z");          // 好：排在后面

    const fs::path gone = root / "no-such-sib-a-upstream";
    must_run(git(parent, "config -f .gitmodules submodule.sib_a.url file://" + gone.string()));
    must_run(git(parent, "add .gitmodules"));
    must_run(git(parent, "commit -q -m bad-sib-a-url"));

    const std::string msg = clone_expect_failure("git+" + parent_url(parent));
    ASSERT_FALSE(msg.empty());
    EXPECT_NE(msg.find(gone.string()), std::string::npos)
        << "报错没有点名不可达的那个子模块 URL: " << msg;

    // **首个失败即返回**：排在后面的兄弟压根没被尝试。
    // 判据用"没有 `.git`"而不是"目录不存在"：libgit2 的 checkout 可能为 gitlink 先建出空
    // 目录，那样"目录不存在"会变成一个与语义无关的假红。
    EXPECT_FALSE(fs::exists(dest() / "sib_z" / ".git"))
        << "sib_a 已经失败，却还把 sib_z 拉了下来 —— 「首个失败即返回」的语义变了";
}

// ── ⑮ gitlink 钉在**祖先** commit 上（上游已往前走）─────────────────────────────
//
// 子模块的 gitlink 可以钉在分支历史上的任意 commit 上，而上游随后继续前进。这个形态此前
// 没有被覆盖：装出来的内容与 HEAD 都必须是**钉住那个祖先**，而不是分支尖端。
//
// ⚠️ **本用例不覆盖 `{1, 0}` 兜底轮**（尽管它长得像那个场景）。`prepare_repo()` 的兜底轮
// 要跑起来，前提是 depth=1 那一轮 revparse 不到目标；而实测本地 `file://` 传输**不认
// depth**（机制见 ⑯），第一轮就拿到全部对象 ⇒ 第二轮永远不跑。所以这里 `rev-list` 数到
// 2（完整历史）在"走了兜底"与"第一轮就没浅"两种情形下都成立，**不是**兜底轮的证据。
// 它证明的只有一件事：钉祖先这条安装路径本身是对的。
TEST_F(GitSubmoduleTest, PinnedAncestorCommitIsCheckedOut)
{
    const fs::path leaf = upstream_path("leaf-repo");
    make_repo(leaf);
    commit_file(leaf, "leaf.txt", "v1\n", "c1");
    std::string c1;
    ASSERT_TRUE(run_capture(git(leaf, "rev-parse HEAD"), c1)) << "取不到 c1";
    commit_file(leaf, "leaf.txt", "v2\n", "c2");  // 上游往前走了：tip = c2

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, leaf, "sub");  // gitlink 先落在 tip(c2)

    // 把 gitlink **回退**到 c1：子模块 checkout 到 c1，父仓库据此提交
    must_run(git(parent / "sub", "checkout -q " + c1));
    must_run(git(parent, "add sub"));
    must_run(git(parent, "commit -q -m pin-ancestor"));
    ASSERT_EQ(pinned_commit(parent, "sub"), c1) << "父仓库没钉到 c1";

    clone_git_source("git+" + parent_url(parent), work_root);

    EXPECT_EQ(checked_out_commit("sub"), c1) << "子模块没停在父仓库钉住的那个**祖先** commit 上";
    EXPECT_EQ(read_file(dest() / "sub/leaf.txt"), "v1\n")
        << "拿到的是尖端的内容（v2）而不是钉住那个祖先的内容（v1）";
}

// ── ⑯ 实测：本地 file:// 传输**不认 depth** ⇒ 兜底轮在本地 fixture 里不可达 ────────
//
// `prepare_repo()` 是 `for (int depth : {1, 0})`：先浅拉，目标 revparse 不到就删掉重建、
// 完整拉。要让它跑到第二轮，得有一份"depth=1 拿不到目标 commit"的源。
//
// **本地 fixture 造不出这份源** —— 实测（本用例）libgit2 的本地传输不做浅取：即便钉住的
// 就是分支尖端，落地仓库里 `origin/main` 的整条历史也在（`rev-list --count` 数到 2，
// 且**没有** `.git/shallow`）。于是第一轮 `any_rev_exists` 就成功，第二轮永远不跑。
//
// 这就是 lpkg/CLAUDE.md §7.4 那句"构造不出场景"背后的**机制**：不是"钉祖先"这种状态造不
// 出来（造得出来，见 ⑮），而是**本地传输不认 depth**。真网络传输（https/ssh）认，但测试
// 一律不碰网（§8 第 3 条），所以这一轮在测试里确实覆盖不到。
//
// 这条把**实测**钉成字面量，兼作绊线：哪天本地传输开始认 depth（或换到真传输），它会变红
// —— 那时才第一次有可能为 `{1, 0}` 的第二轮写真用例。
TEST_F(GitSubmoduleTest, LocalTransportIgnoresDepthSoTheFallbackRoundStaysUnreachable)
{
    const fs::path leaf = upstream_path("leaf-repo");
    make_repo(leaf);
    commit_file(leaf, "leaf.txt", "v1\n", "c1");
    commit_file(leaf, "leaf.txt", "v2\n", "c2");

    const fs::path parent = upstream_path(kParentRepo);
    make_repo(parent);
    commit_file(parent, "parent.txt", "parent\n", "p1");
    add_submodule(parent, leaf, "sub");  // gitlink = tip(c2)：**depth=1 本该足够**

    const std::string pinned = pinned_commit(parent, "sub");
    clone_git_source("git+" + parent_url(parent), work_root);

    EXPECT_EQ(checked_out_commit("sub"), pinned);
    EXPECT_EQ(reachable_commit_count("sub"), 2)
        << "本地 file:// 传输开始遵守 depth 了（历史被截断成 1）—— 兜底轮从此可以在本地 "
           "fixture 里触发，请给 `{1, 0}` 的第二轮补一条真正断言它的用例"
           "（⑮ 现在只能证明「钉祖先能装上」）；同时 §7.4 的这条缺口可以划掉";
    EXPECT_FALSE(fs::exists(dest() / "sub/.git/shallow"))
        << "出现 .git/shallow 说明真的做了浅取 —— 与上面那条一起看，本地传输的行为变了";
}
