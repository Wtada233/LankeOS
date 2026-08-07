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

/** 解析 `git+<git_url>@<ref>` → (git_url, ref)。ref 缺省为 HEAD。 */
void parse_git_url(const std::string& url, std::string& git_url, std::string& ref)
{
    std::string rest = url.substr(4);  // strip "git+"
    auto at = rest.rfind('@');
    if (at != std::string::npos) {
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

/** 更新单个 submodule：手动建仓库 → 浅拉默认分支 → 锁定 commit 不在浅层则完整拉兜底 → checkout
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
        prog->err = get_string("error.unknown");
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

/** 克隆 git 源到 work_root/<repo>，checkout 指定 ref，并更新 submodule。 */
void clone_git_source(const std::string& url, const fs::path& work_root)
{
    std::string git_url, ref;
    parse_git_url(url, git_url, ref);

    std::string name = fs::path(git_url).filename().string();
    if (name.ends_with(".git")) {
        name.resize(name.size() - 4);
    }
    fs::path dest = work_root / name;
    if (fs::exists(dest)) {
        fs::remove_all(dest);
    }

    GitProgress prog{};
    prog.is_tty = isatty(STDOUT_FILENO) == 1;  // 进度刷在 stdout，与 downloader 一致
    prog.current = name;                       // 主仓库下载时进度行显示仓库名

    git_libgit2_init();
    git_repository* repo = nullptr;
    int err = 0;
    bool head_default = false;

    if (ref.empty() || ref == "HEAD") {
        // 默认分支：git_clone 的默认分支处理最可靠（浅克隆）
        head_default = true;
        git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
        opts.checkout_opts.checkout_strategy = 0;  // 先不 checkout，下面统一处理
        opts.fetch_opts.depth = 1;
        opts.fetch_opts.callbacks.transfer_progress = transfer_progress_cb;
        opts.fetch_opts.callbacks.payload = &prog;
        err = git_clone(&repo, git_url.c_str(), dest.string().c_str(), &opts);
        if (err != 0) {
            const git_error* e = git_error_last();
            prog.err = (e && e->message) ? e->message : get_string("error.unknown");
        }
    } else {
        // 指定 ref（tag/branch）：fresh repo + 只拉该 ref（浅拉，失败完整兜底）
        std::vector<std::string> rs = {"+refs/tags/" + ref + ":refs/tags/" + ref,
                                       "+refs/heads/" + ref + ":refs/remotes/origin/" + ref};
        std::vector<std::string> revs = {ref, "refs/tags/" + ref, "refs/remotes/origin/" + ref};
        err = prepare_repo(dest, git_url, rs, revs, &prog, &repo);
        if (err != 0) {
            clear_progress_line(&prog);
            git_libgit2_shutdown();
            if (err == GIT_ENOTFOUND) {
                // ref 真不存在
                throw LpkgException(
                    string_format("error.git_ref_not_found", ref, git_url,
                                  prog.err.empty() ? get_string("error.unknown") : prog.err));
            }
            // fetch 失败：上报真实错误
            throw LpkgException(
                string_format("error.git_clone_failed", git_url,
                              prog.err.empty() ? get_string("error.unknown") : prog.err));
        }
        if (err == 0) {
            // checkout 到目标 ref（annotated tag → 剥到 commit）
            git_object* obj = nullptr;
            git_object* commit_obj = nullptr;
            if (git_revparse_single(&obj, repo, ref.c_str()) != 0) {
                git_revparse_single(&obj, repo, ("refs/remotes/origin/" + ref).c_str());
            }
            if (obj != nullptr) {
                git_object_peel(&commit_obj, obj, GIT_OBJECT_COMMIT);
            }
            git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
            co.checkout_strategy = GIT_CHECKOUT_FORCE;
            if (commit_obj != nullptr) {
                err = git_checkout_tree(repo, commit_obj, &co);
                if (err == 0) {
                    err = git_repository_set_head_detached(repo, git_object_id(commit_obj));
                }
            } else {
                err = -1;
            }
            if (commit_obj != nullptr) {
                git_object_free(commit_obj);
            }
            if (obj != nullptr) {
                git_object_free(obj);
            }
            if (err != 0) {
                const git_error* e = git_error_last();
                prog.err = (e && e->message) ? e->message : get_string("error.unknown");
            }
        }
    }

    if (err == 0 && head_default) {
        // 默认分支：git_clone 已把 HEAD 指向默认分支，只差 checkout
        git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
        co.checkout_strategy = GIT_CHECKOUT_FORCE;
        err = git_checkout_head(repo, &co);
        if (err != 0) {
            const git_error* e = git_error_last();
            prog.err = (e && e->message) ? e->message : get_string("error.unknown");
        }
    }

    if (err != 0) {
        clear_progress_line(&prog);
        if (repo != nullptr) {
            git_repository_free(repo);
        }
        git_libgit2_shutdown();
        throw LpkgException(
            string_format("error.git_clone_failed", git_url,
                          prog.err.empty() ? get_string("error.unknown") : prog.err));
    }

    // 更新 submodule（--recurse-submodules 的等价）
    err = update_submodules(repo, &prog);
    if (err != 0) {
        clear_progress_line(&prog);
        git_repository_free(repo);
        git_libgit2_shutdown();
        throw LpkgException(
            string_format("error.git_submodule_failed", git_url,
                          prog.err.empty() ? get_string("error.unknown") : prog.err));
    }

    git_repository_free(repo);
    git_libgit2_shutdown();

    // 结束输出：清掉进度行，统一走 l10n 的 log_info
    double total_mb = (prog.cumulative + prog.last_received) / (1024.0 * 1024.0);
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
        fs::path filename = fs::path(url).filename();
        fs::path dest = build_dir / filename;
        if (!fs::exists(dest)) {
            download_with_retries(url, dest, 3, true);
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
                extract_tar_zst(dest, work_root);
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
            if (fs::exists(target_path)) {
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
    if (!fs::exists(work_root) || !fs::is_directory(work_root)) {
        return work_root;
    }

    int dir_count = 0;
    fs::path lone_dir;

    for (const auto& entry : fs::directory_iterator(work_root)) {
        if (entry.is_directory()) {
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
 */
void execute_build_phase(const std::string& phase_name, const fs::path& work_dir,
                         const fs::path& processed_script_path)
{
    log_info(string_format("info.executing_phase", phase_name));
    std::string cmd =
        "set -e; . " + fs::absolute(processed_script_path).string() + " && " + phase_name;
    int ret = run_shell(cmd, work_dir);
    if (ret != 0) {
        fs::remove(processed_script_path);
        throw LpkgException(
            string_format("error.build_phase_failed", phase_name, std::to_string(ret)));
    }
}
