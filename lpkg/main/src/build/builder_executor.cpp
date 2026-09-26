#include "builder_executor.hpp"

#include <git2.h>
#include <unistd.h>

#include <array>
#include <fstream>
#include <iostream>

#include "archive.hpp"
#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "downloader.hpp"
#include "i18n/localization.hpp"

namespace fs = std::filesystem;

// ── git src 支持（git+<url>@<ref>）───────────────────────────────────────────
// 用 libgit2（链接库，保持 lpkg 绑定优先），支持 clone at tag/commit/branch + submodule 更新。
//
// 关键约束（libgit2 v1.7.2）：
//  - `git_transfer_progress_cb` 是 deprecated.h 里的 typedef 别名，函数不能叫这个名字；
//  - `git_indexer_progress` 只有 `received_bytes`，旧 API 的 `total_bytes` 已不存在；
//  - 浅克隆（depth>0）默认只拉默认分支，`refs/tags/*` 不保证存在 → 指定 tag 需显式 fetch。
//
// 显示走 l10n，进度刷新在 stdout（与 downloader 的 log_progress 同侧），tty 才刷新。

/** 是否为 git 源：URL 以 `git+` 开头。 */
bool is_git_url(const std::string& url)
{
    return url.rfind("git+", 0) == 0;
}

std::string safe_name_from_url(const std::string& url)
{
    const std::string name = fs::path(url).filename().string();
    // "." / ".." / 空 都会让 `work_root / name` 退化成 work_root 自己或其父目录；
    // 分隔符检查是兜底（filename() 正常不会带分隔符）。
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        throw LpkgException(string_format("error.invalid_source_url", url));
    }
    return name;
}

/** 解析 `git+<git_url>@<ref>` → (git_url, ref)。ref 缺省为 HEAD。 */
void parse_git_url(const std::string& url, std::string& git_url, std::string& ref)
{
    std::string rest = url.substr(4);  // strip "git+"
    // ref 只可能在**最后一个 '/' 之后**——否则 URL 自带的认证信息里的 '@'
    // （git+ssh://git@host/repo.git、git+https://user@host/repo.git）会被误当分隔符
    const auto at = rest.rfind('@');
    const auto slash = rest.rfind('/');
    const bool has_ref = (at != std::string::npos && (slash == std::string::npos || at > slash));
    if (has_ref) {
        git_url = rest.substr(0, at);
        ref = rest.substr(at + 1);
    } else {
        git_url = rest;
        ref = "HEAD";
    }
}

// git 传输进度状态（shallow clone + 下载进度）
struct GitProgress {
    bool is_tty;             // 输出是否为终端
    std::string current;     // 正在下载的仓库名（主仓库名 / submodule 名），进度行显示
    uint64_t last_received;  // 当前 fetch 已接收字节
    uint64_t cumulative;     // 已完成的 fetch 累计（主 clone + tag fetch + 各 submodule）
    size_t last_line_len;    // 上一条进度行长度（\r 刷新时补空格清残留，避免叠字）
    std::string err;         // submodule 更新失败时记录的错误（回调里不能安全抛异常）
};

/** 清掉 tty 上 \r 刷新出来的进度行。 */
void clear_progress_line(GitProgress* p)
{
    if (!p->is_tty) {
        return;
    }
    size_t len = p->last_line_len ? p->last_line_len : 64;
    std::cout << "\r" << std::string(len, ' ') << "\r" << std::flush;
    p->last_line_len = 0;
}

/** git 传输进度回调：tty 上 \r 刷新下载 MiB；非 tty 不显示，结束后统一输出。 */
int transfer_progress_cb(const git_indexer_progress* stats, void* payload)
{
    auto* p = static_cast<GitProgress*>(payload);
    // 检测新 fetch 开始（received_bytes 重置）→ 累加上一个 fetch 的字节
    if (stats->received_bytes < p->last_received) {
        p->cumulative += p->last_received;
    }
    p->last_received = stats->received_bytes;
    if (!p->is_tty) {
        return 0;  // 非 tty：不刷新，结束后 log_info 一次性输出
    }
    // 显示累计字节（跨 fetch 不归零），并标注当前正在下载的仓库
    double total_mb = (p->cumulative + p->last_received) / (1024.0 * 1024.0);
    std::string line = string_format("info.git_progress", p->current, total_mb);
    // \r 刷新：新行比旧行短时补空格清掉旧行残留（否则出现 "MiB MiB" 这类叠字）
    if (line.size() < p->last_line_len) {
        line.append(p->last_line_len - line.size(), ' ');
    }
    p->last_line_len = line.size();
    std::cout << "\r" << line << std::flush;
    return 0;
}

/** 在 remote 上拉取 refspec。depth>0 浅拉、0 完整拉。失败时把真实错误记进 prog->err（不吞错）。 */
int fetch_refspecs(git_remote* remote, const git_strarray* refspecs, int depth, GitProgress* prog)
{
    git_fetch_options fo = GIT_FETCH_OPTIONS_INIT;
    fo.callbacks.transfer_progress = transfer_progress_cb;
    fo.callbacks.payload = prog;
    fo.depth = depth;
    int err = git_remote_fetch(remote, refspecs, &fo, nullptr);
    if (err != 0) {
        const git_error* e = git_error_last();
        prog->err = (e && e->message) ? e->message : get_string("error.unknown");
    }
    return err;
}

/** 这些 rev 里能否至少 revparse 出一个对象。 */
bool any_rev_exists(git_repository* repo, const std::vector<std::string>& revs)
{
    for (const auto& r : revs) {
        git_object* obj = nullptr;
        if (git_revparse_single(&obj, repo, r.c_str()) == 0) {
            git_object_free(obj);
            return true;
        }
    }
    return false;
}

/**
 * 在 dest 建立仓库并拉取，直到 revs 之一可 revparse。
 * 先浅拉（depth=1）；浅层没有则删掉重建、完整拉（depth=0）——换全新仓库，避免
 * libgit2 在浅层仓库上做 depth=0 完整拉取（unshallow）不可靠导致目标 object 缺失。
 * 返回 0=目标存在；GIT_ENOTFOUND=fetch 成功但目标缺失；其他=真实 fetch 错误（prog->err 已记录）。
 */
int prepare_repo(const fs::path& dest, const std::string& url,
                 const std::vector<std::string>& refspec_strs, const std::vector<std::string>& revs,
                 GitProgress* prog, git_repository** out)
{
    int last_err = GIT_ENOTFOUND;
    for (int depth : {1, 0}) {  // 先浅拉，再完整拉
        std::error_code ec;
        fs::remove_all(dest, ec);  // 每轮全新仓库，避免浅层状态干扰完整拉
        git_repository* repo = nullptr;
        git_remote* remote = nullptr;
        int err = git_repository_init(&repo, dest.string().c_str(), 0);
        if (err == 0) {
            err = git_remote_create(&remote, repo, "origin", url.c_str());
        }
        if (err == 0) {
            std::vector<char*> rp;
            std::vector<std::string> storage = refspec_strs;
            for (auto& s : storage) {
                rp.push_back(s.data());
            }
            git_strarray refspecs{rp.data(), rp.size()};
            err = fetch_refspecs(remote, &refspecs, depth, prog);
        }
        if (remote != nullptr) {
            git_remote_free(remote);
        }
        if (err == 0 && any_rev_exists(repo, revs)) {
            *out = repo;
            return 0;
        }
        if (repo != nullptr) {
            git_repository_free(repo);
        }
        last_err = (err != 0) ? err : GIT_ENOTFOUND;
    }
    return last_err;
}

/** 前向声明：update_one_submodule 递归子模块时调用（--recursive 的等价）。定义在下方。 */
int update_submodules(git_repository* repo, GitProgress* prog);

/** 更新单个 submodule：手动建仓库 → 浅拉（refspec = **全部分支 + 全部 tag**，不是"默认分支"；
 *  见下面的 `refspecs`）→ 锁定 commit 不在浅层则完整拉兜底 → checkout
 * 到锁定 commit。 */
int update_one_submodule(git_repository* parent, const std::string& name, GitProgress* prog)
{
    git_submodule* sm = nullptr;
    int err = git_submodule_lookup(&sm, parent, name.c_str());
    if (err != 0) {
        const git_error* e = git_error_last();
        prog->err = (e && e->message) ? e->message : get_string("error.unknown");
        return err;
    }
    const char* url = git_submodule_url(sm);
    const char* path = git_submodule_path(sm);
    const git_oid* oid = git_submodule_index_id(sm);
    const char* wd = git_repository_workdir(parent);
    if (url == nullptr || path == nullptr || oid == nullptr || wd == nullptr) {
        // 点名是**哪个**子模块、以及缺的是什么 —— 原先只写 `error.unknown`，连入参 `name`
        // 都不报（实测可达：`.gitmodules` 声明了条目但父仓库索引里没有 gitlink）。
        prog->err = string_format("error.git_submodule_entry_incomplete", name);
        git_submodule_free(sm);
        return -1;
    }

    // 相对 URL（../xxx）按父仓库 origin 解析
    git_buf resolved = GIT_BUF_INIT;
    std::string final_url = url;
    if (git_submodule_resolve_url(&resolved, parent, url) == 0 && resolved.ptr) {
        final_url = resolved.ptr;
    }

    fs::path sub_dir = fs::path(wd) / path;

    char oid_hex[GIT_OID_HEXSZ + 1];
    git_oid_tostr(oid_hex, sizeof(oid_hex), oid);

    prog->current = name;
    git_repository* sub = nullptr;
    std::vector<std::string> refspecs = {"+refs/heads/*:refs/remotes/origin/*",
                                         "+refs/tags/*:refs/tags/*"};
    err = prepare_repo(sub_dir, final_url, refspecs, {oid_hex}, prog, &sub);
    if (err != 0) {
        // `GIT_ENOTFOUND` = **fetch 成功但目标 commit 不在**子模块仓库里（浅拉与完整拉都没拿到）。
        // `prepare_repo` 在这条路上不写 `prog->err`，于是最终文案回落到 `error.unknown` ——
        // 用户看到 "Failed to update submodules of <父仓库>: Unknown error"，既不知道是哪个
        // 子模块、也不知道它钉的是哪个 commit（而这三样这里全都有）。
        if (err == GIT_ENOTFOUND)
            prog->err = string_format("error.git_submodule_rev_missing", name, oid_hex, final_url);
        git_submodule_free(sm);
        git_buf_dispose(&resolved);
        return err;
    }

    // checkout 到锁定 commit（detached HEAD）
    git_object* target = nullptr;
    git_object* commit_obj = nullptr;
    if (git_revparse_single(&target, sub, oid_hex) == 0) {
        git_object_peel(&commit_obj, target, GIT_OBJECT_COMMIT);
    }
    git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
    co.checkout_strategy = GIT_CHECKOUT_FORCE;
    if (commit_obj != nullptr) {
        err = git_checkout_tree(sub, commit_obj, &co);
        if (err == 0) {
            err = git_repository_set_head_detached(sub, git_object_id(commit_obj));
        }
    } else {
        err = -1;
    }
    // 递归：该 submodule 自身可能还有 submodule（--recursive 等价，如
    // mbedtls→tf-psa-crypto→framework）。 必须在 free(sub) 之前调用；git submodule 结构是
    // DAG（无环），递归天然终止。
    if (err == 0) {
        err = update_submodules(sub, prog);
    }
    if (commit_obj != nullptr) {
        git_object_free(commit_obj);
    }
    if (target != nullptr) {
        git_object_free(target);
    }
    if (err != 0) {
        const git_error* e = git_error_last();
        prog->err = (e && e->message) ? e->message : get_string("error.unknown");
    }
    git_repository_free(sub);
    git_submodule_free(sm);
    git_buf_dispose(&resolved);
    return err;
}

/** 更新所有 submodule（--recurse-submodules 的等价）。 */
int update_submodules(git_repository* repo, GitProgress* prog)
{
    // 先收集名字（避免在迭代中改 WD）
    std::vector<std::string> names;
    int err = git_submodule_foreach(
        repo,
        []([[maybe_unused]] git_submodule* sm, const char* name, void* payload) -> int {
            static_cast<std::vector<std::string>*>(payload)->emplace_back(name ? name : "");
            return 0;
        },
        &names);
    if (err != 0) {
        const git_error* e = git_error_last();
        prog->err = (e && e->message) ? e->message : get_string("error.unknown");
        return err;
    }
    for (const auto& n : names) {
        err = update_one_submodule(repo, n, prog);
        if (err != 0) {
            return err;
        }
    }
    return 0;
}

// ── clone_git_source 的零件（2026-09-26 从 128 行的函数里按自然缝抽出）──────────
// 全都是**纯搬移 + 参数化**：语义、调用顺序、资源释放顺序与抽之前逐行一致。

/** 取 libgit2 最后一条错误消息；没有则回落到通用文案（绝不吐空串）。 */
std::string last_git_error()
{
    const git_error* e = git_error_last();
    return (e && e->message) ? e->message : get_string("error.unknown");
}

/** 上报用文案：prog->err 为空时回落到通用文案。 */
std::string clone_error_detail(const GitProgress& prog)
{
    return prog.err.empty() ? get_string("error.unknown") : prog.err;
}

/**
 * libgit2 全局初始化的 RAII 门：构造 `git_libgit2_init()`、析构 `git_libgit2_shutdown()`。
 *
 * 抽之前是三处"各自 `git_libgit2_shutdown()` 再 throw"；用门之后**每条**退出路径
 * （含异常展开）都恰好 shutdown 一次，不会漏也不会重。shutdown 相对 throw 的时机只差
 * 在"异常对象已构造"之后 —— 异常文案读的是 `prog.err`（std::string 副本）与 l10n 表，
 * 都不碰 libgit2 全局态，所以可观测行为不变。
 */
class GitLibGuard
{
public:
    GitLibGuard()
    {
        git_libgit2_init();
    }
    ~GitLibGuard()
    {
        git_libgit2_shutdown();
    }
    GitLibGuard(const GitLibGuard&) = delete;
    GitLibGuard& operator=(const GitLibGuard&) = delete;
};

/**
 * 准备克隆目标：`work_root/<repo>`（URL 最后一段，剥掉 `.git` 后缀）。
 * **目标已存在时先整体删除** —— 语义是"清空再全新克隆"，既不报错也不增量复用。
 */
fs::path prepare_clone_destination(const std::string& git_url, const fs::path& work_root)
{
    std::string name = safe_name_from_url(git_url);
    if (name.ends_with(".git")) {
        name.resize(name.size() - 4);
    }
    fs::path dest = work_root / name;
    // `exists_no_follow`（2026-09-26 修）：悬空链接也占着这个名字 ⇒ 必须清掉，否则随后的
    // clone 撞 EEXIST，而报错只说"目标已存在"、定位不到真实原因（一个悬空链接）。
    if (exists_no_follow(dest)) {
        fs::remove_all(dest);  // 对链接按名字删（`remove_all` 不跟随**末段**）
    }
    return dest;
}

/** 默认分支克隆：git_clone 的默认分支处理最可靠（浅克隆）。返回 0 或 libgit2 错误码。 */
int clone_default_head(const std::string& git_url, const fs::path& dest, GitProgress* prog,
                       git_repository** out)
{
    git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
    opts.checkout_opts.checkout_strategy = 0;  // 先不 checkout，下面统一处理
    opts.fetch_opts.depth = 1;
    opts.fetch_opts.callbacks.transfer_progress = transfer_progress_cb;
    opts.fetch_opts.callbacks.payload = prog;
    int err = git_clone(out, git_url.c_str(), dest.string().c_str(), &opts);
    if (err != 0) {
        prog->err = last_git_error();
    }
    return err;
}

/** 默认分支的收尾 checkout：git_clone 已把 HEAD 指向默认分支，只差这一步。 */
int checkout_default_head(git_repository* repo, GitProgress* prog)
{
    git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
    co.checkout_strategy = GIT_CHECKOUT_FORCE;
    int err = git_checkout_head(repo, &co);
    if (err != 0) {
        prog->err = last_git_error();
    }
    return err;
}

/** 依次 revparse 这些 rev，取第一个**能解开**的并剥到 commit 对象；都解不开返回 nullptr。 */
git_object* peel_first_rev_to_commit(git_repository* repo, const std::vector<std::string>& revs)
{
    git_object* obj = nullptr;
    for (const auto& r : revs) {
        if (git_revparse_single(&obj, repo, r.c_str()) == 0) {
            break;
        }
    }
    git_object* commit_obj = nullptr;
    if (obj != nullptr) {
        git_object_peel(&commit_obj, obj, GIT_OBJECT_COMMIT);
        git_object_free(obj);
    }
    return commit_obj;
}

/**
 * checkout 到 rev（annotated tag → 剥到 commit）并置 detached HEAD。
 * revs 依次尝试（`ref` 本身 → `refs/remotes/origin/<ref>`，分支 ref 走后者）。
 * 失败时记录 prog->err，返回非 0。
 */
int checkout_rev(git_repository* repo, const std::vector<std::string>& revs, GitProgress* prog)
{
    git_object* commit_obj = peel_first_rev_to_commit(repo, revs);
    git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
    co.checkout_strategy = GIT_CHECKOUT_FORCE;
    int err = -1;
    if (commit_obj != nullptr) {
        err = git_checkout_tree(repo, commit_obj, &co);
        if (err == 0) {
            err = git_repository_set_head_detached(repo, git_object_id(commit_obj));
        }
        git_object_free(commit_obj);
    }
    if (err != 0) {
        prog->err = last_git_error();
    }
    return err;
}

/**
 * 指定 ref（tag/branch）的拉取：fresh repo + 只拉该 ref（浅拉，失败完整兜底）。
 * 返回 0=成功（*out 可用）；GIT_ENOTFOUND=ref 不存在；其他=真实 fetch 错误（prog->err 已记录）。
 */
int prepare_ref_repo(const fs::path& dest, const std::string& git_url, const std::string& ref,
                     GitProgress* prog, git_repository** out)
{
    std::vector<std::string> refspecs = {"+refs/tags/" + ref + ":refs/tags/" + ref,
                                         "+refs/heads/" + ref + ":refs/remotes/origin/" + ref};
    std::vector<std::string> revs = {ref, "refs/tags/" + ref, "refs/remotes/origin/" + ref};
    return prepare_repo(dest, git_url, refspecs, revs, prog, out);
}

/**
 * 拉取阶段的失败上报（两条不同的 l10n 键）：`ref` 真不存在 vs 真实 fetch 错误。
 * 两者都点名 URL；前者额外点名 ref。
 */
[[noreturn]] void throw_fetch_error(int fetch_err, const std::string& git_url,
                                    const std::string& ref, const GitProgress& prog)
{
    if (fetch_err == GIT_ENOTFOUND) {
        throw LpkgException(
            string_format("error.git_ref_not_found", ref, git_url, clone_error_detail(prog)));
    }
    throw LpkgException(string_format("error.git_clone_failed", git_url, clone_error_detail(prog)));
}

/** 克隆 git 源到 work_root/<repo>，checkout 指定 ref，并更新 submodule。 */
void clone_git_source(const std::string& url, const fs::path& work_root)
{
    std::string git_url, ref;
    parse_git_url(url, git_url, ref);

    const fs::path dest = prepare_clone_destination(git_url, work_root);

    GitProgress prog{};
    prog.is_tty = isatty(STDOUT_FILENO) == 1;  // 进度刷在 stdout，与 downloader 一致
    prog.current = dest.filename().string();   // 主仓库下载时进度行显示仓库名

    GitLibGuard libgit2;
    git_repository* repo = nullptr;
    const bool head_default = (ref.empty() || ref == "HEAD");
    int err = 0;

    if (head_default) {
        err = clone_default_head(git_url, dest, &prog, &repo);
    } else {
        // 指定 ref（tag/branch）：拉取失败只能出在这里（此时 repo 尚未建立）
        const int fetch_err = prepare_ref_repo(dest, git_url, ref, &prog, &repo);
        if (fetch_err != 0) {
            clear_progress_line(&prog);
            throw_fetch_error(fetch_err, git_url, ref, prog);
        }
        // checkout 到目标 ref；失败则落进下面统一的失败收尾（与 clone 失败同一条路）
        err = checkout_rev(repo, {ref, "refs/remotes/origin/" + ref}, &prog);
    }

    if (err == 0 && head_default) {
        // 默认分支：git_clone 已把 HEAD 指向默认分支，只差 checkout
        err = checkout_default_head(repo, &prog);
    }

    if (err != 0) {
        clear_progress_line(&prog);
        if (repo != nullptr) {
            git_repository_free(repo);
        }
        throw LpkgException(
            string_format("error.git_clone_failed", git_url, clone_error_detail(prog)));
    }

    // 更新 submodule（--recurse-submodules 的等价）
    err = update_submodules(repo, &prog);
    if (err != 0) {
        clear_progress_line(&prog);
        git_repository_free(repo);
        throw LpkgException(
            string_format("error.git_submodule_failed", git_url, clone_error_detail(prog)));
    }

    git_repository_free(repo);

    // 结束输出：清掉进度行，统一走 l10n 的 log_info
    const double total_mb = (prog.cumulative + prog.last_received) / (1024.0 * 1024.0);
    clear_progress_line(&prog);
    log_info(string_format("info.git_download", total_mb));
}

/**
 * 下载并准备构建所需的源码
 * 将 sources 中的归档文件自动解压到工作目录，
 * 将 work_sources 中的文件直接复制到工作目录
 */
std::vector<fs::path> download_and_prepare_sources(const std::vector<std::string>& sources,
                                                   const std::vector<std::string>& work_sources,
                                                   const fs::path& build_dir,
                                                   const fs::path& work_root)
{
    std::vector<fs::path> downloaded_files;

    auto download_one = [&](const std::string& url) -> fs::path {
        fs::path filename = safe_name_from_url(url);
        fs::path dest = build_dir / filename;
        if (!fs::exists(dest)) {
            // **先下到 .part 再 rename**：被中断（SIGKILL/断电）的构建只会留下不完整的
            // .part，正式文件仅在下载完整后出现。否则 `if (!fs::exists(dest))` 会把上次
            // 留下的**截断源码包永久当成"已下载好"**，错误延后到某个无关的构建阶段
            // 才以看不懂的形式爆出来（TODO.md C4）。
            const fs::path part = dest.string() + ".part";
            std::error_code ec;
            fs::remove(part, ec);  // 清掉上次残留的半截
            download_with_retries(url, part, 3, true);
            safe_rename(part, dest);
            downloaded_files.push_back(dest);
        } else {
            log_info(string_format("info.source_exists", filename.string()));
        }
        return dest;
    };

    for (const auto& url : sources) {
        // git 源：git+<url>@<ref>，libgit2 clone + submodule
        if (is_git_url(url)) {
            clone_git_source(url, work_root);
            continue;
        }
        fs::path dest = download_one(url);
        fs::path filename = dest.filename();

        std::string ext = dest.extension().string();
        if (ext == ".gz" || ext == ".bz2" || ext == ".xz" || ext == ".zst" || ext == ".tgz" ||
            ext == ".tar" || ext == ".zip") {
            log_info(string_format("info.auto_extracting", filename.string()));
            try {
                // 标签用源码归档文件名：构建期"在解压谁"就是哪个源码包（与上一行的
                // info.auto_extracting 同一口径），包名在这一层已经拿不到了。
                extract_tar_zst(dest, work_root, filename.string());
            } catch (const std::exception& e) {
                log_warning(
                    string_format("warning.auto_extract_failed", filename.string(), e.what()));
            }
        }
    }

    for (const auto& url : work_sources) {
        fs::path dest = download_one(url);
        fs::path filename = dest.filename();
        fs::path target_path = work_root / filename;

        log_info(string_format("info.copying_to_workdir", filename.string()));
        try {
            // **`exists_no_follow`（2026-09-26 修）**：判据要的是"**这个名字**被占着"，
            // 而 `fs::exists` 跟随末段链接 ⇒ **悬空链接判 false**（不让开），紧接着
            // `fs::copy_file` 也**跟随** ⇒ 内容被写到**链接目标**上（父目录存在时），
            // 即以 root 写到 `work_root` **之外**。可达路径：源码归档里一个悬空链接
            // （`xxx.jar -> ../../etc/ld.so.preload`）+ 同名 work_source。
            // 判据换成 lstat 语义后，悬空链接会被 `fs::remove` 按名字删掉（`remove` 对链接
            // 本来就是不跟随的），随后的 copy_file 落在**新文件**上。
            if (exists_no_follow(target_path)) {
                fs::remove(target_path);
            }
            fs::copy_file(dest, target_path, fs::copy_options::overwrite_existing);
        } catch (const std::exception& e) {
            throw LpkgException(string_format("error.copy_work_source_failed", filename.string(),
                                              std::string(e.what())));
        }
    }

    return downloaded_files;
}

/**
 * 检测工作目录中的源码树结构
 * 如果工作目录中只有一个子目录，则返回该子目录作为源码根目录（常见的 tarball
 * 解压后单目录结构） 否则返回工作目录本身
 */
fs::path detect_source_tree(const fs::path& work_root)
{
    // 不抛判定：work_root 下就是解压出来的**上游源码树**，里面可能有符号链接环；
    // 判定类调用不该有能力把构建打死（见 base/utils.hpp 的谓词族）。
    if (!is_directory_follow(work_root)) {
        return work_root;
    }

    int dir_count = 0;
    fs::path lone_dir;

    for (const auto& entry : fs::directory_iterator(work_root)) {
        // 保持"跟随"语义、只把"抛"换成"判否"（`entry.is_directory()` 走 status()，
        // 对源码树里的符号链接环会抛 ELOOP）
        std::error_code fec;
        if (fs::is_directory(entry.path(), fec) && !fec) {
            lone_dir = entry.path();
            ++dir_count;
        } else {
            // 顶层有文件说明不是单目录结构
            return work_root;
        }
    }

    if (dir_count == 1) {
        log_info(string_format("info.detected_source_tree", lone_dir.filename().string()));
        return lone_dir;
    }
    return work_root;
}

/**
 * 读取构建脚本内容，并进行变量替换
 * 将脚本中的 {PKG_NAME}、{SRC_DIR} 等占位符替换为实际值
 */
std::string process_build_script(const fs::path& script_path,
                                 const std::map<std::string, std::string>& vars)
{
    std::string content;
    {
        std::ifstream f(script_path);
        content.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    for (const auto& [from, to] : vars) {
        string_replace_all(content, from, to);
    }
    return content;
}

/**
 * 执行构建阶段的 shell 脚本
 * source 处理后的构建脚本，然后调用指定的 phase_name 函数
 * 构建失败时清理临时脚本并抛出异常
 *
 * 执行前 export CFLAGS/CXXFLAGS/LDFLAGS/MAKEFLAGS：保证默认不出现
 * -march=native（见 build_defaults.hpp），configure/make/cmake 自动继承；
 * 脚本也可引用 {CFLAGS} 等模板变量获得同一组值。
 */
void execute_build_phase(const std::string& phase_name, const fs::path& work_dir,
                         const fs::path& processed_script_path,
                         const build_defaults::BuildFlags& flags)
{
    log_info(string_format("info.executing_phase", phase_name));

    // 空字段回退到 build_defaults 默认值（直接调用方如测试可不传 flags）
    auto eff = [](const std::string& s, std::string_view dflt) {
        return s.empty() ? std::string(dflt) : s;
    };
    const std::string cflags = eff(flags.cflags, build_defaults::CFLAGS);
    const std::string cxxflags = eff(flags.cxxflags, build_defaults::CXXFLAGS);
    const std::string ldflags = eff(flags.ldflags, build_defaults::LDFLAGS);
    const std::string makeflags =
        flags.makeflags.empty() ? build_defaults::default_makeflags() : flags.makeflags;

    // 单引号包裹 export 值（共用 base/utils 的 shell_quote：与 hook 执行路径同一实现）
    std::string cmd = "export CFLAGS=" + shell_quote(cflags) +
                      " CXXFLAGS=" + shell_quote(cxxflags) + " LDFLAGS=" + shell_quote(ldflags) +
                      " MAKEFLAGS=" + shell_quote(makeflags) + "; set -e; . " +
                      shell_quote(fs::absolute(processed_script_path).string()) + " && " +
                      phase_name;
    int ret = run_shell(cmd, work_dir);
    if (ret != 0) {
        fs::remove(processed_script_path);
        throw LpkgException(
            string_format("error.build_phase_failed", phase_name, std::to_string(ret)));
    }
}
