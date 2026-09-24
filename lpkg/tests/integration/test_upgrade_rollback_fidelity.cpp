/**
 * test_upgrade_rollback_fidelity.cpp — 升级批次**中途失败 → 整批回滚**的保真度
 *
 * 目标：把"回滚后文件系统与 DB 必须逐字节回到升级之前"钉死。为此每个失败场景都取两份
 * **完整快照**（文件系统 manifest + DB 文件内容）逐条 EXPECT_EQ；另配一条正向对照，
 * 证明同一套快照机制**真的能看见变化**（否则"逐条相等"可能只是恒等空转）。
 *
 * ── 为什么用 install_packages(本地 .lpkg) 而不是 upgrade_packages() ──────────────
 * upgrade_packages() 需要仓库索引（mirror + index.txt）才能算出"最新版"，而本测试要的是
 * "同一批里若干已装包一起换成新版本、其中最后一个失败"。对已装包来说，把新版本 .lpkg
 * **本地路径**喂给 install_packages 走的就是升级路径本身：InstallationTask 以
 * Cache::get_installed_version() 作为 old_version_to_replace_，同一批次里的所有包共用
 * 一个 BEGIN_PKGS…COMMIT_PKGS 事务（与 upgrade_packages 内部用的 run_batch_transaction
 * 是同一套代码）。因此这里不引入仓库索引，快照里也就没有索引/缓存文件这类无关噪音。
 *
 * ── 场景（见各测试注释）：alpha/beta/gamma 三个包 v1 → v2，另加不参与升级的 fshelper，
 *    它发一个**符号链接目录** `var/run -> ../run`（`<root>/run` 是真目录）。
 *
 * ── 关键前置发现（决定了本文件为何拆成 5 个测试）─────────────────────────────
 * 需求中"批次在 alpha/beta 已改过文件之后因 gamma 的**类型变更冲突**失败"这一组合，
 * 在当前代码里**不可满足**，两条约束互相排斥：
 *   其一：alpha v2 与 beta v2 都发 `usr/share/shared.txt`（跨包同路径），要让**两个都装上**
 *      必须 `--force-overwrite`：第二个包安装时 owners={第一个包} 且第一个包已在
 *      ctx.installed_set 里，`all_upgrading` 不成立 → 非 force 下必判文件冲突
 *      （installation_task.cpp 的 collect_content_conflicts 所有权分支）。
 *   其二：gamma 的"目录条目撞上 fshelper 的符号链接"要判冲突，又**必须不带 force**：类型变更
 *      分支的条件是 `!ours && !Config::instance().force_overwrite_mode()` —— force 把它
 *      整个豁免掉。实测（本文件 ForceOverwrite… 测试钉住）：force 下 gamma 会**接管**那个
 *      符号链接（链接被 rename 进 stash、原路径建出实体目录），整批**成功**而非失败。
 * 于是拆成：① 类型变更冲突真触发的那一份（不带 force）—— 冲突由**整批预检**在进入事务之前
 * 拦下，因此这一份改钉"一个文件都没动"；② 带 force 的**三包**批次"两个包都已完全落地
 * （含跨包所有权接管）之后才失败"，失败点由**断点注入**给出（`install_after_begin_gamma`）
 * —— 这类"两个包已落地的中途证据"只有确定性注入才拿得到（类型变更冲突本身已经拦不到那么晚，
 * 见下）；③ 正向对照；④ 把 force 下类型变更被豁免的**现状**钉住，使这处与 ARCH §3.6.1 第
 * 3/4 条的偏差可见；⑤ 补"**失败的那个包自己**已经在文件操作中途"的维度
 * （`copy_after_wal_<pkg>`：backup_existing_files 已跑完、拷贝只做了一半），④⑤ 与 ② 一样
 * 覆盖"整批回滚"这条路径，① 覆盖"连事务都没进"那条。
 *
 * ── 冲突检出在**整批预检**里（进入事务之前），逐包检查是第二道防线 ─────────────
 * package_manager.cpp 的批次循环**之外**先跑一次 check_batch_file_conflicts：把本批次所有
 * 包的 content 清单 + 当前所有权 + 本批次内的接管顺序一起算，冲突则**在一个文件都没动**的
 * 情况下拒绝（WAL 里连 BEGIN_PKGS 都不写）。这与上游 libalpm 一致：`alpm_trans_commit` 的
 * 第一步就把整笔事务（trans->add）的冲突检完，要么全不动要么全动。逐包的
 * `prepare() → check_for_file_conflicts` **保留为第二道防线**（预检算漏的、预检之后中途状态
 * 变了的由它兜底），正常路径下它不再触发。
 * 语义后果：① 里 gamma 的类型变更冲突**不再**发生在"alpha 已完全落地之后"—— 预检直接拒绝，
 * alpha 一个文件都没落（本文件 ① 现在钉的正是这个），因此"两个包已落地之后才失败"的取证只能
 * 靠 ② 的断点注入。
 *
 * 钩子（postinst.sh / prerm.sh）曾经是"不会回滚的副作用"的典型：postinst 原先在
 * `commit_without_file_ops()` 末尾执行、属于**批次内**的一步，钩子写入也不进 WAL，回滚没有
 * 东西可撤（本文件的 ① 走的正是这条路径）。现已修掉，两个维度都变得可回滚：
 *   · **执行**时机：postinst 只在批次**提交之后**跑（finish_committed_batch），批次回滚则
 *     一个 postinst 都不跑；
 *   · **脚本文件**：hooks_dir/<pkg>/ 的脚本落位改走写入层原语（BACKUP + COPY，可回滚），
 *     回滚后目录里仍是旧版本的内容。
 * 由 tests/integration/test_hook_transaction.cpp 钉住（本文件的快照与此无关：fixture 的包
 * 都不带 `hooks/` 目录，hooks_dir 下没有它们的实体；SetUp 里的 no_hooks_mode 只额外关掉执行）。
 */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/crypto/hash.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"  // db_family_files()：DB 一族的清单只有这一份

namespace fs = std::filesystem;

class UpgradeRollbackFidelityTest : public ::testing::Test
{
protected:
    static constexpr const char* V1 = "1.0";
    static constexpr const char* V2 = "2.0";

    // ── 快照新维度（uid/gid、xattr）的**探针**──────────────────────────────────
    // 探针必须满足：v1 与 v2 的**字节内容相同**，差异**只**来自一个新维度。否则"快照看见
    // 了变化"可以由 sha 解释，新维度依然可能是恒真的摆设。两个探针各隔离一个维度：
    //   xattr 探针：字节/uid/gid/mode 全同，唯一差别是沙盒里手工造的 `user.*` 属性
    //   uid   探针：字节/xattr/mode 全同，唯一差别是沙盒里手工 chown 出的非 root 属主
    // 两者都在 install_initial_state() 里安装**之后**在沙盒里造出来（不依赖打包器是否
    // 保留 xattr / 保留属主 —— 那样测试就变成了在考打包器，而不是在考回滚）。
    static constexpr const char* XATTR_PROBE = "usr/share/alpha-xattr-probe.txt";
    static constexpr const char* UID_PROBE = "usr/share/alpha-uid-probe.txt";
    static constexpr const char* XATTR_NAME = "user.lpkg_probe";
    static constexpr const char* XATTR_VALUE = "probe-v1";
    static constexpr uid_t PROBE_UID = 12345;
    static constexpr gid_t PROBE_GID = 12345;

    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        Config::instance().set_no_hooks_mode(true);
        // force-overwrite 是**进程级**开关：本文件里有的测试开、有的测试必须关。
        // 若漏了这句 reset，后面的测试（含同进程里其它文件的 EXPECT_THROW）会被污染。
        Config::instance().set_force_overwrite_mode(false);
        init_localization();
        BreakpointManager::instance().clear_all();

        suite_work_dir = fs::absolute("tmp_upgrade_rollback_fidelity");
        if (fs::exists(suite_work_dir)) fs::remove_all(suite_work_dir);
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
        Cache::instance().load();
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        Config::instance().set_force_overwrite_mode(false);
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    // ================= fixture =================

    static void write_file(const fs::path& p, const std::string& content = "x\n")
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }

    /** 造一个 `user.*` xattr。失败即断言失败：没有它，快照的 xattr 维度会退化成恒真。 */
    static void set_user_xattr(const fs::path& p, const std::string& value)
    {
        ASSERT_EQ(::lsetxattr(p.c_str(), XATTR_NAME, value.data(), value.size(), 0), 0)
            << "沙盒不支持 user.* xattr，xattr 维度无法取证：" << p;
    }

    /** 任意字节串 → 十六进制（xattr 值是裸字节，capability/ACL 都是二进制） */
    static std::string to_hex(std::string_view bytes)
    {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (const unsigned char c : bytes) {
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0f]);
        }
        return out;
    }

    /** 属主 uid（lstat 语义，不跟随链接）；失败返回 -1 */
    static int64_t owner_uid(const fs::path& p)
    {
        struct stat st{};
        if (lstat(p.c_str(), &st) != 0) return -1;
        return static_cast<int64_t>(st.st_uid);
    }

    /** 打一个包。tag 用于区分同 name+version 的多个 fixture 变体（不同内容/不同冲突点）。 */
    template <typename F>
    std::string pack(const std::string& name, const std::string& ver, const std::string& tag,
                     const std::vector<std::string>& deps, const std::vector<std::string>& provides,
                     const std::vector<std::string>& needed_so, F fill)
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "_" + ver + "_" + tag);
        fs::create_directories(work / "content");
        fill(work / "content");
        const std::string path = (pkg_dir / (name + "-" + ver + "-" + tag + ".lpkg")).string();
        // man 内容对 v1/v2 相同 → 不进 fast 快照的差异集，避免噪音
        pack_package(path, work.string(), name, ver, deps, provides, "man " + name, needed_so);
        return path;
    }

    /** 初始状态：fshelper + alpha/beta/gamma 的 v1，一个批次装好 */
    void install_initial_state()
    {
        const std::string fsh = pack("fshelper", V1, "base", {}, {}, {}, [&](const fs::path& c) {
            write_file(c / "run" / "fshelper-run.txt", "run dir content\n");
            fs::create_directories(c / "var");
            fs::create_symlink("../run", c / "var" / "run");  // 符号链接目录（不参与升级）
            write_file(c / "srv" / "fshelper-srv.txt", "srv dir content\n");
            write_file(c / "usr" / "bin" / "fshelper", "#!/bin/sh\necho fshelper\n");
        });
        const std::string a1 = pack("alpha", V1, "base", {}, {}, {}, [&](const fs::path& c) {
            write_file(c / "usr" / "bin" / "alpha", "alpha v1\n");
            write_file(c / "usr" / "share" / "alpha-v1-only.txt", "alpha v1 only\n");
            // 两个探针：v1/v2 内容**逐字节相同**，只为让"新维度看得见差异"有据可依
            write_file(c / "usr" / "share" / "alpha-xattr-probe.txt", "probe\n");
            write_file(c / "usr" / "share" / "alpha-uid-probe.txt", "probe\n");
        });
        const std::string b1 = pack("beta", V1, "base", {}, {}, {}, [&](const fs::path& c) {
            write_file(c / "usr" / "bin" / "beta", "beta v1\n");
        });
        const std::string g1 = pack("gamma", V1, "base", {}, {}, {}, [&](const fs::path& c) {
            write_file(c / "usr" / "bin" / "gamma", "gamma v1\n");
            write_file(c / "usr" / "share" / "gamma-v1-only.txt", "gamma v1 only\n");
        });

        ASSERT_NO_THROW(install_packages({fsh, a1, b1, g1}));
        ASSERT_TRUE(fs::is_symlink(test_root / "var/run")) << "fshelper 的符号链接目录没装上";
        ASSERT_TRUE(fs::is_directory(test_root / "run"));

        // 在**已安装**的文件上造出两个新维度的差异（见探针常量处的说明）。必须在 install
        // 之后做：这两处状态不属于包内容，属于"安装期被别人改过的现场"，正是回滚要还原的
        // 那类状态。
        set_user_xattr(test_root / XATTR_PROBE, XATTR_VALUE);
        ASSERT_EQ(::chown((test_root / UID_PROBE).c_str(), PROBE_UID, PROBE_GID), 0)
            << "沙盒无法 chown 出非 root 属主，uid 维度无法取证";

        // fixture 自检：快照必须**真的看得见**这些对象 —— 否则"升级前后逐条相同"可能只是
        // 两边都是空表，整文件变成空转。
        const auto fs0 = fs_state();
        ASSERT_TRUE(fs0.contains("var/run")) << "文件系统快照漏掉了符号链接";
        EXPECT_NE(fs0.at("var/run").find("l mode="), std::string::npos) << fs0.at("var/run");
        EXPECT_NE(fs0.at("var/run").find("target=../run"), std::string::npos) << fs0.at("var/run");
        ASSERT_TRUE(fs0.contains("srv")) << "文件系统快照漏掉了目录";
        ASSERT_TRUE(fs0.contains("usr/bin/alpha"));
        EXPECT_NE(fs0.at("usr/bin/alpha").find("sha="), std::string::npos)
            << fs0.at("usr/bin/alpha");
        // 新增两个维度的自检：快照必须**真的看得见** xattr 与 uid/gid，否则"回滚后逐条相同"
        // 对这两个维度就是恒真的摆设。
        ASSERT_TRUE(fs0.contains(XATTR_PROBE)) << "文件系统快照漏掉了 xattr 探针";
        EXPECT_NE(
            fs0.at(XATTR_PROBE).find(std::string(" ") + XATTR_NAME + "=" + to_hex(XATTR_VALUE)),
            std::string::npos)
            << "快照的 xattr 维度看不见 user.* 属性：" << fs0.at(XATTR_PROBE);
        ASSERT_TRUE(fs0.contains(UID_PROBE)) << "文件系统快照漏掉了 uid 探针";
        EXPECT_NE(fs0.at(UID_PROBE).find(" uid=" + std::to_string(PROBE_UID)), std::string::npos)
            << "快照的 uid 维度看不见属主：" << fs0.at(UID_PROBE);
        EXPECT_NE(fs0.at(UID_PROBE).find(" gid=" + std::to_string(PROBE_GID)), std::string::npos)
            << "快照的 gid 维度看不见属组：" << fs0.at(UID_PROBE);

        const auto db0 = db_state();
        ASSERT_GE(db0.size(), 8u)
            << "DB 快照条目太少（deps/needed_so/man/pkgs/files.db 都该在里面）";
        EXPECT_TRUE(db0.contains(Config::instance().files_db().string()));
        EXPECT_NE(db0.at(Config::instance().files_db().string()).find("/usr/bin/alpha"),
                  std::string::npos)
            << "DB 快照里没有文件归属记录";
        EXPECT_NE(read_text(Config::instance().pkgs_file()).find("alpha:1.0"), std::string::npos);
    }

    /** alpha v2：与 beta v2 共享 usr/share/shared.txt；删掉 v1 独有的文件（废弃文件路径）。
     *  两个探针与 v1 **逐字节相同**（差异只来自沙盒里造出的 xattr / uid，见探针常量）。 */
    std::string alpha_v2()
    {
        return pack("alpha", V2, "up", {}, {}, {}, [&](const fs::path& c) {
            write_file(c / "usr" / "bin" / "alpha", "alpha v2\n");
            write_file(c / "usr" / "share" / "alpha-v2-only.txt", "alpha v2 only\n");
            write_file(c / "usr" / "share" / "shared.txt", "shared from alpha v2\n");
            write_file(c / "usr" / "share" / "alpha-xattr-probe.txt", "probe\n");
            write_file(c / "usr" / "share" / "alpha-uid-probe.txt", "probe\n");
        });
    }

    /** beta v2：后装的这个包在 force 下会把 shared.txt 的所有权从 alpha 手上接管过来 */
    std::string beta_v2()
    {
        return pack("beta", V2, "up", {}, {}, {}, [&](const fs::path& c) {
            write_file(c / "usr" / "bin" / "beta", "beta v2\n");
            write_file(c / "usr" / "share" / "shared.txt", "shared from beta v2\n");
        });
    }

    /** gamma v2（冲突版）：目录条目 `var/run/` 正是 fshelper 持有的**符号链接**路径 */
    std::string gamma_v2_dir_over_symlink()
    {
        return pack("gamma", V2, "dir-over-symlink", {}, {}, {}, [&](const fs::path& c) {
            write_file(c / "usr" / "bin" / "gamma", "gamma v2\n");
            fs::create_directories(c / "var" / "run");
            write_file(c / "var" / "run" / "gamma.pid", "4242\n");
        });
    }

    /** gamma v2（正向对照用）：不含任何冲突条目，升级应当成功 */
    std::string gamma_v2_clean()
    {
        return pack("gamma", V2, "clean", {}, {}, {}, [&](const fs::path& c) {
            write_file(c / "usr" / "bin" / "gamma", "gamma v2\n");
            write_file(c / "usr" / "share" / "gamma-v2-only.txt", "gamma v2 only\n");
        });
    }

    // ================= 快照 =================

    /**
     * xattr 指纹：`名字=十六进制值` 的空格分隔列表（名字排序，保证稳定）；一个都没有 → 空串。
     * 用 `l*` 变体（`llistxattr`/`lgetxattr`）：快照要看的是**链接本身**的属性，不能穿透到
     * 目标。读失败记 `<err>` 而**不是**静默跳过 —— "读不到"和"没有"是两回事，前者会让这个
     * 维度悄悄退化成恒真（例如权限不足时全部读空）。
     */
    static std::string xattr_fingerprint(const fs::path& p)
    {
        const ssize_t len = ::llistxattr(p.c_str(), nullptr, 0);
        if (len <= 0) return {};
        std::vector<char> names(static_cast<size_t>(len));
        const ssize_t got = ::llistxattr(p.c_str(), names.data(), names.size());
        if (got <= 0) return {};
        std::vector<std::string> sorted;
        for (const char* n = names.data(); n < names.data() + got; n += std::strlen(n) + 1) {
            if (*n == '\0') continue;
            sorted.emplace_back(n);
        }
        std::sort(sorted.begin(), sorted.end());
        std::ostringstream fp;
        for (const auto& n : sorted) {
            const ssize_t vlen = ::lgetxattr(p.c_str(), n.c_str(), nullptr, 0);
            std::string value;
            if (vlen < 0) {
                fp << " " << n << "=<err>";
                continue;
            }
            if (vlen > 0) {
                std::vector<char> val(static_cast<size_t>(vlen));
                if (::lgetxattr(p.c_str(), n.c_str(), val.data(), val.size()) != vlen) {
                    fp << " " << n << "=<err>";
                    continue;
                }
                value.assign(val.data(), val.size());
            }
            fp << " " << n << "=" << to_hex(value);
        }
        return fp.str();
    }

    /**
     * 文件系统快照：`<root>` 下每个条目的"相对路径 → 形态指纹"。
     * 指纹含：类型（f/d/l）、mode、普通文件的 sha256、符号链接的 target，外加**所有条目**的
     * uid/gid 与 xattr —— **目录本身也要覆盖**（只比文件会漏掉"链接被换成实体目录"这类形态
     * 变化）。
     *
     * uid/gid 与 xattr 是独立于"内容是否变过"的状态：包被重装/接管会换属主，capability 与
     * ACL 都存在 xattr 里（`copy_xattrs` 就是为此而生）—— 回滚必须把它们一起还原，而旧快照
     * （只比 mode/sha/target）看不见这两类差异。**目录 mtime 故意不比**：任何子项改动都会改
     * 它，回滚后合法地不同，加了只会 flaky。
     *
     * 排除 `<state_dir>`（= `<root>/var/lib/lpkg`：pkgs/files.db/wal/DB 备份）：那一族的
     * 保真由 db_state() **逐字节**钉，且 WAL 按约定不做字节比较（回滚会写 ROLLBACK/END、
     * trim 也会改它）。stash（`.lpkg_bak_*`）与 `.lpkgtmp` **不排除**：它们出现在快照里
     * 就等于"回滚没清干净"，本来就要失败。
     */
    std::map<std::string, std::string> fs_state() const
    {
        std::map<std::string, std::string> out;
        const fs::path state_dir = Config::instance().state_dir();
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(test_root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const fs::path p = it->path();
            if (p == state_dir) {
                it.disable_recursion_pending();  // 只跳过子树，不跟随
                continue;
            }
            const std::string rel = p.lexically_relative(test_root).generic_string();

            struct stat st{};
            if (lstat(p.c_str(), &st) != 0) {
                out[rel] = "lstat-failed";
                continue;
            }
            std::ostringstream fp;
            if (S_ISLNK(st.st_mode)) {
                fp << "l mode=" << std::oct << (st.st_mode & 07777)
                   << " target=" << fs::read_symlink(p, ec).string();
            } else if (S_ISDIR(st.st_mode)) {
                fp << "d mode=" << std::oct << (st.st_mode & 07777);
            } else if (S_ISREG(st.st_mode)) {
                fp << "f mode=" << std::oct << (st.st_mode & 07777)
                   << " sha=" << calculate_sha256(p);
            } else {
                fp << "other mode=" << std::oct << (st.st_mode & 07777);
            }
            // 十进制（`std::dec` 显式收回上面 `std::oct` 的影响，否则 uid 会按八进制打印）
            fp << std::dec << " uid=" << static_cast<unsigned long long>(st.st_uid)
               << " gid=" << static_cast<unsigned long long>(st.st_gid) << xattr_fingerprint(p);
            out[rel] = fp.str();
        }
        return out;
    }

    /**
     * DB 快照：绝对路径 → **逐字节**文件内容。覆盖安装状态（pkgs / holdpkgs / essential）、
     * 文件归属（files.db）、providers（provides.db）、**配置文件三哈希记录（confhashes.db）**，
     * 以及 register_package 逐包写下的 deps/ needed_so/ man 三个目录里的全部条目（它们也是
     * DB 的一部分，走 DBNEW/DBRM 回滚）。
     *
     * DB 一族的清单取自 test_base.hpp 的 db_family_files()（与 test_db_backup_chain.cpp
     * 同一份）——一族有 5 个库，逐处硬编码的 4 元素清单正是漏掉第 5 个（confhashes.db）的
     * 地方："回滚后逐字节回到批次前"这条不变量必须对**整族**成立。
     */
    std::map<std::string, std::string> db_state() const
    {
        std::map<std::string, std::string> out;
        auto add_file = [&](const fs::path& p) {
            std::ifstream f(p, std::ios::binary);
            if (!f.is_open()) {
                out[p.string()] = "<无法打开>";
                return;
            }
            std::stringstream ss;
            ss << f.rdbuf();
            out[p.string()] = ss.str();
        };
        for (const fs::path& p : db_family_files()) add_file(p);
        add_file(Config::instance().essential_file());
        for (const fs::path& dir :
             {Config::instance().dep_dir(), Config::instance().needed_so_dir(),
              Config::instance().docs_dir()}) {
            std::error_code ec;
            if (!fs::exists(dir, ec)) continue;
            for (auto it = fs::recursive_directory_iterator(dir, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->is_regular_file(ec)) add_file(it->path());
            }
        }
        return out;
    }

    /** 逐条 EXPECT_EQ：两份快照必须完全相同；失败信息里带上路径，便于直接定位差异 */
    void expect_same_snapshot(const std::map<std::string, std::string>& before,
                              const std::map<std::string, std::string>& after,
                              const std::string& what) const
    {
        std::set<std::string> keys;
        for (const auto& [k, v] : before) keys.insert(k);
        for (const auto& [k, v] : after) keys.insert(k);
        for (const auto& k : keys) {
            const auto ib = before.find(k);
            const auto ia = after.find(k);
            const std::string b = ib == before.end() ? "<不存在>" : ib->second;
            const std::string a = ia == after.end() ? "<不存在>" : ia->second;
            EXPECT_EQ(b, a) << what << "：路径 " << k << "（升级前 → 回滚后）";
        }
    }

    /** 未清理的 stash / 临时文件残留数 */
    int count_residue() const
    {
        int n = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(test_root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const std::string name = it->path().filename().string();
            if (name.find(".lpkg_bak_") != std::string::npos || name.ends_with(".lpkgtmp")) ++n;
        }
        return n;
    }

    /** 盘上的 `.lpkgtmp`（"已写出、还没 rename 到位"的中间态）个数 */
    int count_tmp_files() const
    {
        int n = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(test_root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->path().filename().string().ends_with(".lpkgtmp")) ++n;
        }
        return n;
    }

    /** 未清理的 DB 备份数（回滚已消费 + 清理过的批次应为 0） */
    int count_db_backups() const
    {
        int n = 0;
        std::error_code ec;
        const fs::path state = Config::instance().state_dir();
        if (!fs::exists(state, ec)) return n;
        for (auto it = fs::recursive_directory_iterator(state, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->path().filename().string().find(".lpkg_db_bak_before:") != std::string::npos)
                ++n;
        }
        return n;
    }

    std::string read_text(const fs::path& p) const
    {
        std::ifstream f(p, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    /** 失败批次收尾后可观测的形态：WAL 里不再有未提交批次（BEGIN_PKGS） */
    void expect_no_open_batch() const
    {
        EXPECT_EQ(read_text(wal::wal_log_path()).find("BEGIN_PKGS"), std::string::npos)
            << "WAL 里仍有未提交批次（批次未收尾 / 未被 trim）";
    }

    /** 四个包的安装版本必须都在 v1（回滚后） */
    void expect_all_at_v1(const std::string& ctx) const
    {
        EXPECT_EQ(Cache::instance().get_installed_version("alpha"), V1) << ctx;
        EXPECT_EQ(Cache::instance().get_installed_version("beta"), V1) << ctx;
        EXPECT_EQ(Cache::instance().get_installed_version("gamma"), V1) << ctx;
        EXPECT_EQ(Cache::instance().get_installed_version("fshelper"), V1) << ctx;
    }

    /** fshelper 的符号链接目录 + 它指向的目录必须原封不动 */
    void expect_fshelper_layout_intact(const std::string& ctx) const
    {
        std::error_code ec;
        EXPECT_TRUE(fs::is_symlink(test_root / "var/run", ec))
            << ctx << "：符号链接没了/被实体目录取代";
        EXPECT_EQ(fs::read_symlink(test_root / "var/run", ec).string(), "../run")
            << ctx << "：链接 target 变了";
        EXPECT_TRUE(fs::is_directory(test_root / "run", ec)) << ctx << "：链接目标目录没了";
        EXPECT_EQ(read_text(test_root / "run" / "fshelper-run.txt"), "run dir content\n")
            << ctx << "：链接目标目录的内容被动过";
        EXPECT_FALSE(fs::exists(test_root / "run" / "gamma.pid", ec))
            << ctx << "：内容穿透符号链接写进了目标目录";
    }
};

// ============================================================================
// ① 类型变更冲突（不带 force）：**整批预检**在进入事务之前拦下 → 一个文件都没动
// ============================================================================

TEST_F(UpgradeRollbackFidelityTest, TypeChangeConflictFailsBatchAndRollsEverythingBack)
{
    install_initial_state();
    const auto a2 = alpha_v2();
    const auto g2 = gamma_v2_dir_over_symlink();

    const auto fs_before = fs_state();
    const auto db_before = db_state();

    // 断点取"批次中途"的证据：alpha 已 COMMIT（文件 + DB 都落地）之后、批次失败之前。
    // **新语义下这两条都不该命中**：类型变更冲突由整批预检（check_batch_file_conflicts）
    // 在**进入事务之前**拦下，批次循环一次都没进 —— alpha 连 WAL BEGIN 都没写。
    // 命中 ⟺ 批次真的动过盘/进过事务（旧行为：逐包检出→整批回滚），那时下面的
    // "alpha 仍是 v1、文件未被碰"就不成立了。回调里照旧留证，失败时直接打印。
    std::map<std::string, std::string> mid;
    BreakpointManager::instance().set("after_commit_alpha", [&] {
        mid["alpha_ver"] = Cache::instance().get_installed_version("alpha");
        mid["alpha_bin"] = read_text(test_root / "usr/bin/alpha");
        mid["shared"] = read_text(test_root / "usr/share/shared.txt");
        {
            std::string owners;
            for (const auto& o : Cache::instance().get_file_owners("/usr/share/shared.txt"))
                owners += o + ",";
            mid["shared_owners"] = owners;
        }
        mid["pkgs"] = read_text(Config::instance().pkgs_file());
        mid["xattr_probe_attr"] =
            xattr_fingerprint(test_root / XATTR_PROBE).empty() ? "none" : "present";
        mid["uid_probe_uid"] = std::to_string(owner_uid(test_root / UID_PROBE));
    });
    BreakpointManager::instance().set("install_after_begin_alpha",
                                      [&] { mid["alpha_began"] = "yes"; });

    // gamma 的目录条目 `var/run/` 撞上 fshelper 持有的 symlink → 预检的类型变更分支判冲突
    // → 在**一个文件都没动**的情况下拒绝（alpha 的 v2 一个字节都没落地）
    //
    // 拒绝的判据是"点名了谁"：只断言"抛了异常"的话，"包压根没找到 / 路径写错 / 别的什么
    // 原因"同样能让它绿 —— 那正是这条断言此前的问题。
    std::string refusal;
    try {
        install_packages({a2, g2});
        FAIL() << "归档目录条目撞上别的包持有的 symlink→目录，必须判冲突中止";
    } catch (const LpkgException& e) {
        refusal = e.what();
    }
    BreakpointManager::instance().clear_all();

    EXPECT_NE(refusal.find("var/run"), std::string::npos) << "拒绝信息没点名冲突路径：" << refusal;
    EXPECT_NE(refusal.find("fshelper"), std::string::npos)
        << "拒绝信息必须点名真实持有者 fshelper：" << refusal;

    // 本段的价值：**证明预检真的没动文件**（不是"装完再回滚"）。
    // 命中任何一条断点都说明批次已经进过事务/动过盘，那就不再是预检的收益了。
    EXPECT_FALSE(mid.contains("alpha_began"))
        << "alpha 已经写了自己的 WAL BEGIN（批次进过事务）—— 冲突不是预检拦下的";
    EXPECT_TRUE(mid.empty()) << "断点命中，批次中途留下过证据（预检没拦在事务之前）："
                             << "alpha_ver=" << mid["alpha_ver"]
                             << " alpha_bin=" << mid["alpha_bin"] << " shared=" << mid["shared"]
                             << " shared_owners=" << mid["shared_owners"]
                             << " xattr_probe_attr=" << mid["xattr_probe_attr"]
                             << " uid_probe_uid=" << mid["uid_probe_uid"];

    // (a) 预检拦下时 alpha 仍是 v1：盘上内容、DB 记录、文件所有权逐条未变
    EXPECT_EQ(Cache::instance().get_installed_version("alpha"), V1)
        << "alpha 被预检拒绝时不该有任何版本变化";
    EXPECT_EQ(read_text(test_root / "usr/bin/alpha"), "alpha v1\n") << "alpha 的 v1 内容被覆盖过";
    {
        std::string owners;
        for (const auto& o : Cache::instance().get_file_owners("/usr/bin/alpha")) owners += o + ",";
        EXPECT_EQ(owners, "alpha,") << "alpha 的文件所有权被动过";
    }
    // 新维度（xattr / uid）**根本没被碰过** —— 这与 ②⑤ 的"改过再还原"互补：
    // 那两例考的是撤销保真度，这一例考的是"压根没动"。
    EXPECT_NE(xattr_fingerprint(test_root / XATTR_PROBE)
                  .find(std::string(" ") + XATTR_NAME + "=" + to_hex(XATTR_VALUE)),
              std::string::npos)
        << "预检拒绝时 xattr 探针被覆盖过：" << xattr_fingerprint(test_root / XATTR_PROBE);
    EXPECT_EQ(owner_uid(test_root / UID_PROBE), static_cast<int64_t>(PROBE_UID))
        << "预检拒绝时 uid 探针的属主被改过";
    // 失败包（gamma）同样连目录都没碰。
    // ⚠ **本断言不具判别力**：成功批次后的 trim_completed（以及进批次前那次）会把整个
    // WAL 日志清空，批次回滚成功时同样如此 —— "预检在事务之外拒绝"与"批次内拒绝→整批
    // 回滚"两条路径终点都是空 WAL，所以它在两种实现下都恒真。它钉得住的只是"拒绝后没有
    // 留下未提交批次"。**判别力来自上面的断点取证**：`install_after_begin_alpha` /
    // `after_commit_alpha` 都没命中（`mid.empty()`）—— 命中 ⟺ 批次真的进过事务。
    EXPECT_EQ(read_text(wal::wal_log_path()).find("BEGIN"), std::string::npos)
        << "预检拒绝后残留了 WAL 行（未提交批次没被收干净）";

    // (b) 文件系统保真：逐条相同（含目录本身、符号链接的 target、uid/gid、xattr）
    expect_same_snapshot(fs_before, fs_state(), "文件系统 manifest");
    // (c) DB 保真：逐字节相同
    expect_same_snapshot(db_before, db_state(), "DB 文件");
    // 无 stash/临时残留、无 DB 备份残留、WAL 无未提交批次
    EXPECT_EQ(count_residue(), 0) << "预检拒绝后仍有 .lpkg_bak_* / .lpkgtmp 残留";
    EXPECT_EQ(count_db_backups(), 0) << "预检拒绝却残留 DB 备份";
    expect_no_open_batch();

    // (e) fshelper 的符号链接与目标目录原封不动
    expect_fshelper_layout_intact("类型变更冲突被拒绝时");
    // 失败包没留下任何痕迹；alpha 的 v1 独有文件照旧在
    EXPECT_FALSE(fs::exists(test_root / "usr/share/shared.txt"));
    EXPECT_FALSE(fs::exists(test_root / "usr/share/alpha-v2-only.txt"));
    EXPECT_TRUE(fs::exists(test_root / "usr/share/alpha-v1-only.txt")) << "v1 独有文件不见了";
    EXPECT_TRUE(Cache::instance().get_file_owners("/usr/share/shared.txt").empty());
    expect_all_at_v1("类型变更冲突被拒绝后");
}

// ============================================================================
// ② 带 force 的三包批次：alpha/beta 都已完全落地（含跨包所有权接管）后 gamma 失败
// ============================================================================

TEST_F(UpgradeRollbackFidelityTest, ThreePackageUpgradeFailureRollsBackFilesAndOwnership)
{
    install_initial_state();
    const auto a2 = alpha_v2();
    const auto b2 = beta_v2();
    // gamma 用**不带冲突条目**的 gamma v2：本 case 要的是"两个包连跨包所有权接管都完全落地
    // 之后，批次才失败"的保真度，失败点由下面的断点注入。
    // 不靠 gamma 的类型变更冲突：那条冲突现在由**整批预检**在进入事务之前拦下
    // （package_manager.cpp 的 check_batch_file_conflicts → 冲突时连 BEGIN_PKGS 都不写），
    // 那时 alpha/beta 一个文件都没落 —— 本 case 要的"已落地两个包"的证据只能靠断点注入
    // （冲突本身由 ① 与 DirEntryOverSymlinkTest.FileEntryOverRealDirIsRefused 覆盖）。
    const auto g2 = gamma_v2_clean();

    const auto fs_before = fs_state();
    const auto db_before = db_state();

    // 批次中途取证：gamma 的 WAL BEGIN 之后（= alpha/beta 都已 COMMIT）、gamma 碰文件之前。
    // 此刻 alpha/beta 的 v2 已落盘、且 shared.txt 的**内存所有权接管**（force 分支只改
    // Cache，不写盘）已经发生 —— 正是回滚最容易漏掉的那部分状态。
    std::map<std::string, std::string> mid;
    BreakpointManager::instance().set("install_after_begin_gamma", [&] {
        mid["alpha_ver"] = Cache::instance().get_installed_version("alpha");
        mid["beta_ver"] = Cache::instance().get_installed_version("beta");
        mid["gamma_ver"] = Cache::instance().get_installed_version("gamma");
        mid["shared"] = read_text(test_root / "usr/share/shared.txt");
        {
            std::string owners;
            for (const auto& o : Cache::instance().get_file_owners("/usr/share/shared.txt"))
                owners += o + ",";
            mid["shared_owners"] = owners;
        }
        mid["v1_only_gone"] = fs::exists(test_root / "usr/share/alpha-v1-only.txt") ? "no" : "yes";
        // 落盘 DB（pkgs 里程碑 + files.db 归属）此刻也必须已经变了
        mid["pkgs"] = read_text(Config::instance().pkgs_file());
        mid["files_db"] = read_text(Config::instance().files_db());
        // 失败点用**断点注入**（确定性）：断点取在 gamma 的 WAL BEGIN 之后，此刻 alpha/beta
        // 已完全落地（含跨包所有权接管），正是回滚最容易漏掉的那部分状态。冲突本身由 ① 与
        // DirEntryOverSymlinkTest.FileEntryOverRealDirIsRefused 覆盖（含 force 不放行）。
        throw LpkgException("interrupt after alpha/beta committed");
    });

    Config::instance().set_force_overwrite_mode(true);
    EXPECT_THROW(install_packages({a2, b2, g2}), LpkgException)
        << "批次中途注入的失败必须让整批中止并回滚";
    BreakpointManager::instance().clear_all();

    // 中途取证：两个包确实已完全落地，且所有权接管真的发生过
    ASSERT_FALSE(mid.empty()) << "断点没命中：gamma 的 BEGIN 之后没有留证";
    EXPECT_EQ(mid["alpha_ver"], V2);
    EXPECT_EQ(mid["beta_ver"], V2);
    EXPECT_EQ(mid["gamma_ver"], V1) << "失败的 gamma 不该已经算装上";
    EXPECT_EQ(mid["shared"], "shared from beta v2\n") << "后装的 beta 没有覆盖 alpha 的文件";
    EXPECT_EQ(mid["shared_owners"], "beta,") << "跨包同路径的所有权接管没有发生";
    EXPECT_EQ(mid["v1_only_gone"], "yes") << "alpha 的废弃文件没有被移除（REMOVE_OLD 没走）";
    EXPECT_NE(mid["pkgs"].find("beta:2.0"), std::string::npos);
    EXPECT_NE(mid["files_db"].find("/usr/share/shared.txt"), std::string::npos)
        << "files.db 里没有 shared.txt 的归属记录（DB 相等断言就没有证明力）";

    // (b) 文件系统保真
    expect_same_snapshot(fs_before, fs_state(), "文件系统 manifest");
    // (c) DB 保真（含 files.db 的所有权：接管只改了 Cache 内存，回滚必须连内存一起回到旧值）
    expect_same_snapshot(db_before, db_state(), "DB 文件");
    EXPECT_EQ(count_residue(), 0) << "回滚后仍有 .lpkg_bak_* / .lpkgtmp 残留";
    EXPECT_EQ(count_db_backups(), 0);
    expect_no_open_batch();

    // (e) fshelper 的符号链接与目标目录**未被触碰**。这一条在本 case 里**不是**"被接管后再
    // 还原"：gamma_v2_clean 一次都没碰 `var/run` / `srv`，所以它证明的是"整批回滚没有波及
    // 无关的邻居"，以及"回滚不会把别人持有的链接/目录顺手搬来搬去"。
    // （"链接真被接管"的现状由 ④ 钉住；"接管被拒绝"由 ① 钉住 —— 两者都不要在这里重复。）
    expect_fshelper_layout_intact("三包批次失败回滚后");
    EXPECT_FALSE(fs::is_symlink(test_root / "srv")) << "fshelper 的实体目录被换成了符号链接";
    EXPECT_EQ(read_text(test_root / "srv" / "fshelper-srv.txt"), "srv dir content\n");
    // 失败批次留下的东西全部撤销
    EXPECT_FALSE(fs::exists(test_root / "usr/share/shared.txt"));
    EXPECT_TRUE(Cache::instance().get_file_owners("/usr/share/shared.txt").empty())
        << "所有权接管没有被回滚（ARCH §4.5 OWNER_OVERRIDE：回滚必须 cache.load() 回旧值）";
    expect_all_at_v1("三包批次失败回滚后");
}

// ============================================================================
// ③ 正向对照：同一套 fixture、同一套快照，升级成功时**必须能看出差异**
// ============================================================================

TEST_F(UpgradeRollbackFidelityTest, SameSnapshotSeesASuccessfulUpgrade)
{
    install_initial_state();
    const auto a2 = alpha_v2();
    const auto b2 = beta_v2();
    const auto g2 = gamma_v2_clean();  // 不含冲突条目的 gamma v2

    const auto fs_before = fs_state();
    const auto db_before = db_state();

    // 跨包同路径（alpha/beta 都发 usr/share/shared.txt）在同一批次里只有 force 才放行
    // —— 这正是 CLI 的 --force-overwrite，也是 ② 用的同一条路径。
    Config::instance().set_force_overwrite_mode(true);
    ASSERT_NO_THROW(install_packages({a2, b2, g2})) << "不冲突的批次必须能成功";
    Config::instance().set_force_overwrite_mode(false);

    const auto fs_after = fs_state();
    const auto db_after = db_state();

    // 差异集必须**恰好**是这些（证明快照机制真的能看见变化，不是恒等通过）
    std::set<std::string> added, removed, changed;
    for (const auto& [k, v] : fs_after)
        if (!fs_before.contains(k)) added.insert(k);
    for (const auto& [k, v] : fs_before)
        if (!fs_after.contains(k)) removed.insert(k);
    for (const auto& [k, v] : fs_before)
        if (fs_after.contains(k) && fs_after.at(k) != v) changed.insert(k);

    const std::set<std::string> exp_added = {"usr/share/alpha-v2-only.txt",
                                             "usr/share/gamma-v2-only.txt", "usr/share/shared.txt"};
    const std::set<std::string> exp_removed = {"usr/share/alpha-v1-only.txt",
                                               "usr/share/gamma-v1-only.txt"};
    // 两个探针文件也出现在 changed 里，但它们的**字节内容、类型、mode 前后完全相同**（下面
    // 逐条断言），差异**只**来自本文件新增的两个快照维度：xattr 探针的 `user.*` 属性在升级
    // 中被包内副本（不带该属性）覆盖掉、uid 探针的属主从沙盒里手工 chown 出的非 root 变回
    // 包内属主。少了这两条，"新维度看得见变化"就无从证明 —— 旧快照（只比 mode/sha/target）
    // 会把它们判成"未变化"，正向对照对新增维度就失去了证明力。
    const std::set<std::string> exp_changed = {"usr/bin/alpha", "usr/bin/beta", "usr/bin/gamma",
                                               XATTR_PROBE, UID_PROBE};
    EXPECT_EQ(added, exp_added) << "快照没能看见新增文件";
    EXPECT_EQ(removed, exp_removed) << "快照没能看见被移除的旧文件";
    EXPECT_EQ(changed, exp_changed) << "快照没能看见被替换的文件";

    // 探针的隔离性证明。指纹布局固定为「类型/mode/sha」+「 uid=… gid=…」+「xattr 段」，
    // 逐段对比：每个探针前后**只有一段**变化，且就是它要隔离的那个维度。
    const auto split = [](const std::string& fp) {
        const size_t u = fp.find(" uid=");
        const size_t g = fp.find(" gid=");
        const size_t g_end = (g == std::string::npos) ? std::string::npos : fp.find(' ', g + 1);
        return std::array<std::string, 3>{
            fp.substr(0, u), (g_end == std::string::npos) ? fp.substr(u) : fp.substr(u, g_end - u),
            (g_end == std::string::npos) ? std::string{} : fp.substr(g_end)};
    };
    const auto xb = split(fs_before.at(XATTR_PROBE));
    const auto xa = split(fs_after.at(XATTR_PROBE));
    const auto ub = split(fs_before.at(UID_PROBE));
    const auto ua = split(fs_after.at(UID_PROBE));

    // 两个探针的字节内容必须前后一致，否则"差异来自新维度"这句话就不成立（内容、类型、
    // mode 段也被逐段钉死：都不许变）
    for (const char* probe : {XATTR_PROBE, UID_PROBE}) {
        EXPECT_EQ(read_text(test_root / probe), "probe\n")
            << probe << "：探针内容必须前后一致，否则差异可以归因于内容而不是新维度";
    }
    EXPECT_EQ(xb[0], xa[0]) << "xattr 探针的类型/mode/sha 变了";
    EXPECT_EQ(ub[0], ua[0]) << "uid 探针的类型/mode/sha 变了";
    EXPECT_EQ(xb[1], xa[1]) << "xattr 探针的 uid/gid 变了 → 差异不再只归因于 xattr";
    EXPECT_EQ(ub[2], ua[2]) << "uid 探针的 xattr 段变了 → 差异不再只归因于 uid/gid";
    EXPECT_NE(xb[2], xa[2]) << "xattr 探针的 xattr 段没变 → xattr 维度没被考到";
    EXPECT_NE(ub[1], ua[1]) << "uid 探针的 uid/gid 段没变 → uid 维度没被考到";
    // 变化方向也要对：包内副本带走 root 属主、且不带 `user.*` 属性
    EXPECT_EQ(ua[1], " uid=0 gid=0") << "uid 探针的属主应被包内副本（root）取代";
    EXPECT_TRUE(xa[2].empty()) << "xattr 探针的属性应被不带该属性的包内副本覆盖掉：" << xa[2];

    // 后装的 beta 覆盖了 alpha 的同路径文件：磁盘内容与 DB 所有权都属于 beta
    EXPECT_EQ(read_text(test_root / "usr/share/shared.txt"), "shared from beta v2\n");
    std::set<std::string> owners;
    for (const auto& o : Cache::instance().get_file_owners("/usr/share/shared.txt"))
        owners.insert(o);
    EXPECT_EQ(owners, std::set<std::string>({"beta"}))
        << "跨包同路径接管后，所有权必须归后安装的那个包";

    // 成功批次：DB 与文件系统都前进了（对照 ①/② 的"完全相等"）
    EXPECT_NE(db_before, db_after) << "升级成功后 DB 竟然没有变化";
    EXPECT_EQ(Cache::instance().get_installed_version("alpha"), V2);
    EXPECT_EQ(Cache::instance().get_installed_version("beta"), V2);
    EXPECT_EQ(Cache::instance().get_installed_version("gamma"), V2);
    EXPECT_EQ(count_residue(), 0);
    EXPECT_EQ(count_db_backups(), 0);
    expect_no_open_batch();
    expect_fshelper_layout_intact("成功批次之后");
}

// ============================================================================
// ④ 现状钉：--force-overwrite 下"目录条目撞别人的符号链接"会被接管 —— **有意为之**。
//    ARCH §3.6.1 第 3 条写明 force 只豁免这一方向（归档目录撞盘上非目录），反方向
//    （归档文件撞真目录）永不覆盖；上游 libalpm 的闸门 `!S_ISDIR(lsbuf) &&
//    _alpm_can_overwrite_file(...)` 与之同构（pacman 的 --overwrite 覆盖的正是"symlink
//    挡路"这一类）。旧注释曾称它与 ARCH 不一致，已过时。
// ============================================================================

TEST_F(UpgradeRollbackFidelityTest, ForceOverwriteStillTakesOverForeignSymlinkDir)
{
    install_initial_state();
    const auto a2 = alpha_v2();
    const auto b2 = beta_v2();
    const auto g2 = gamma_v2_dir_over_symlink();

    // ⚠ 本测试钉的是**现状**，不是 ARCH 写的语义：
    //   ARCH §3.6.1 第 3 条"类型变更 = 冲突"，第 4 条"唯一豁免：该路径由**本包**以另一形态
    //   持有"。但 check_for_file_conflicts 的类型变更分支里还有第二个豁免
    //   `!Config::instance().force_overwrite_mode()`：带 --force-overwrite 时整段跳过，
    //   于是 gamma 的 `var/run/` 目录条目会**接管** fshelper 的符号链接 —— 链接被 rename
    //   进 stash（CLEANUP 时删掉）、原路径建出实体目录，正是 §3.6.1 开头描述的那起事故形态。
    //   若维护者判定该豁免违反 ARCH 并收紧（force 也不放行类型变更），把本测试翻成
    //   EXPECT_THROW + expect_fshelper_layout_intact() 即可。
    Config::instance().set_force_overwrite_mode(true);
    ASSERT_NO_THROW(install_packages({a2, b2, g2}))
        << "force 下类型变更被豁免 → 整批成功（实测行为）";
    Config::instance().set_force_overwrite_mode(false);

    EXPECT_EQ(Cache::instance().get_installed_version("gamma"), V2);
    std::error_code ec;
    EXPECT_FALSE(fs::is_symlink(test_root / "var/run", ec))
        << "现状：符号链接已被实体目录取代（ARCH §3.6.1 第 3 条期望它保留）";
    EXPECT_TRUE(fs::is_directory(test_root / "var/run", ec));
    EXPECT_EQ(read_text(test_root / "var/run" / "gamma.pid"), "4242\n");
    // 即便在这种接管下，"内容不许穿过符号链接写进别的包持有的目录"这条仍然成立：
    // gamma.pid 落在新的实体目录里，而不是 `<root>/run/`
    EXPECT_FALSE(fs::exists(test_root / "run" / "gamma.pid", ec)) << "内容穿透了符号链接";
    EXPECT_EQ(read_text(test_root / "run" / "fshelper-run.txt"), "run dir content\n");
    EXPECT_TRUE(Cache::instance().is_installed("fshelper"));
    // 被搬走的链接不留在盘上（stash 已被 CLEANUP）
    EXPECT_EQ(count_residue(), 0);
    EXPECT_EQ(count_db_backups(), 0);
    expect_no_open_batch();
}

// ============================================================================
// ⑤ 失败的包**自己在文件操作中途**失败（backup_existing_files 已跑完、拷贝只做了一半）
// ============================================================================

TEST_F(UpgradeRollbackFidelityTest, MidCopyFailureOfTheFailingPackageRollsBackWholeBatch)
{
    install_initial_state();
    const auto a2 = alpha_v2();
    const auto b2 = beta_v2();
    const auto g2 = gamma_v2_clean();

    const auto fs_before = fs_state();
    const auto db_before = db_state();

    // 断点取在 gamma **自己的** COPY write-ahead 窗口（WAL 行已落、`.lpkgtmp`→目标的 rename
    // 未做）。这是 ①/② 都没覆盖的维度：那两例的失败都发生在失败包碰任何文件**之前**（① 是
    // 整批预检在事务之前抛、② 是 WAL BEGIN 之后立刻注入），本例里失败包**自己已经动过盘** ——
    // 它的 v1 文件已被 backup_existing_files 整阶段搬进 stash、新内容已写成 `.lpkgtmp`。
    // 回滚因此要同时撤销"前面已成功包的"和"它自己的"文件操作。
    //
    // 取证只用**与 readdir 顺序无关**的事实（`scan_content_files` 走
    // recursive_directory_iterator，顺序不定）：backup_existing_files 是先整阶段跑完的，所以
    // gamma 的 v1 文件此刻**一定**已不在原位；而断点卡在第一个文件的 rename 之前，所以
    // gamma 的 v2 内容**一个都没落地**。废弃文件（v1 独有、v2 不再发）的移除属于 REMOVE_OLD，
    // 那是 commit_without_ops 的事、在拷贝**之后** —— 它此刻还在，正好反证断点落在"拷贝中途"
    // 而不是更晚的阶段。
    std::map<std::string, std::string> mid;
    BreakpointManager::instance().set("copy_after_wal_gamma", [&] {
        mid["alpha_ver"] = Cache::instance().get_installed_version("alpha");
        mid["beta_ver"] = Cache::instance().get_installed_version("beta");
        mid["gamma_ver"] = Cache::instance().get_installed_version("gamma");
        mid["gamma_bin_gone"] = fs::exists(test_root / "usr/bin/gamma") ? "no" : "yes";
        mid["gamma_v1_only_gone"] =
            fs::exists(test_root / "usr/share/gamma-v1-only.txt") ? "no" : "yes";
        mid["gamma_new_landed"] =
            fs::exists(test_root / "usr/share/gamma-v2-only.txt") ? "yes" : "no";
        mid["tmp_files"] = std::to_string(count_tmp_files());
        mid["shared"] = read_text(test_root / "usr/share/shared.txt");
        {
            std::string owners;
            for (const auto& o : Cache::instance().get_file_owners("/usr/share/shared.txt"))
                owners += o + ",";
            mid["shared_owners"] = owners;
        }
        // 新增维度的中途取证：alpha/beta 已落地 → 两个探针的 xattr 已消失、uid 已变回 root。
        mid["xattr_probe_attr"] =
            xattr_fingerprint(test_root / XATTR_PROBE).empty() ? "none" : "present";
        mid["uid_probe_uid"] = std::to_string(owner_uid(test_root / UID_PROBE));
        throw LpkgException("injected failure in the middle of gamma's own copy");
    });

    Config::instance().set_force_overwrite_mode(true);
    EXPECT_THROW(install_packages({a2, b2, g2}), LpkgException)
        << "拷贝中途注入的失败必须让整批中止并回滚";
    BreakpointManager::instance().clear_all();

    // 中途取证：批次真的在"两个包已完全落地 + 失败包自己已动过盘"的状态下被打断
    ASSERT_FALSE(mid.empty()) << "断点没命中：gamma 的 COPY 窗口没有留证";
    EXPECT_EQ(mid["alpha_ver"], V2) << "批次失败前 alpha 并没有真的装上 v2";
    EXPECT_EQ(mid["beta_ver"], V2);
    EXPECT_EQ(mid["gamma_ver"], V1) << "失败的 gamma 不该已经算装上";
    EXPECT_EQ(mid["gamma_bin_gone"], "yes")
        << "gamma 的 v1 二进制还在原位 —— 断点没落在 backup_existing_files 之后";
    EXPECT_EQ(mid["gamma_v1_only_gone"], "no")
        << "v1 独有文件不该在拷贝阶段就被移除 —— 那是 commit 阶段 REMOVE_OLD 的事，"
           "在这里被移除说明断点落到了更晚的阶段";
    EXPECT_EQ(mid["gamma_new_landed"], "no") << "断点取得太晚：gamma 的新内容已经落地了";
    EXPECT_GE(std::stoi(mid["tmp_files"]), 1)
        << "盘上没有 .lpkgtmp —— 断点没落在拷贝途中（COPY 的中间态）";
    EXPECT_EQ(mid["shared"], "shared from beta v2\n") << "后装的 beta 没有覆盖 alpha 的文件";
    EXPECT_EQ(mid["shared_owners"], "beta,") << "跨包同路径的所有权接管没有发生";
    EXPECT_EQ(mid["xattr_probe_attr"], "none")
        << "中期取证：alpha v2 的副本应已覆盖掉 `user.*` 属性 —— 否则回滚的 xattr 维度没被考到";
    EXPECT_EQ(mid["uid_probe_uid"], "0")
        << "中期取证：alpha v2 的副本属主应是 root —— 否则回滚的 uid 维度没被考到";

    // (b) 文件系统保真：逐条相同（含目录本身、符号链接 target，以及新增的 uid/gid、xattr）
    expect_same_snapshot(fs_before, fs_state(), "文件系统 manifest");
    // (c) DB 保真：逐字节相同
    expect_same_snapshot(db_before, db_state(), "DB 文件");
    // 批次已收尾：stash / .lpkgtmp / DB 备份都无残留、WAL 无未提交批次
    EXPECT_EQ(count_residue(), 0) << "回滚后仍有 .lpkg_bak_* / .lpkgtmp 残留";
    EXPECT_EQ(count_tmp_files(), 0) << "回滚后仍残留 .lpkgtmp（COPY 的中间态没被撤销）";
    EXPECT_EQ(count_db_backups(), 0) << "批次已收尾却残留 DB 备份";
    expect_no_open_batch();

    // 失败包**自己**的文件操作被完整撤销：v1 的内容与属主都从 stash 回来了
    EXPECT_EQ(read_text(test_root / "usr/bin/gamma"), "gamma v1\n") << "v1 二进制没被还原";
    EXPECT_EQ(read_text(test_root / "usr/share/gamma-v1-only.txt"), "gamma v1 only\n");
    EXPECT_FALSE(fs::exists(test_root / "usr/share/gamma-v2-only.txt")) << "gamma 的新文件没被撤销";
    // 探针：被覆盖文件的**原属主与原 xattr** 也必须回来 —— 这两样是"安装期被别人改过的
    // 现场"，最容易在"从 stash 还原"这条路径上悄悄丢掉
    struct stat st{};
    ASSERT_EQ(lstat((test_root / UID_PROBE).c_str(), &st), 0);
    EXPECT_EQ(static_cast<uid_t>(st.st_uid), PROBE_UID) << "还原后 uid 没回来";
    EXPECT_EQ(static_cast<gid_t>(st.st_gid), PROBE_GID) << "还原后 gid 没回来";
    EXPECT_NE(xattr_fingerprint(test_root / XATTR_PROBE)
                  .find(std::string(" ") + XATTR_NAME + "=" + to_hex(XATTR_VALUE)),
              std::string::npos)
        << "还原后 xattr 没回来：" << xattr_fingerprint(test_root / XATTR_PROBE);
    // 前面已成功包（alpha/beta）的痕迹同样全部撤销
    EXPECT_FALSE(fs::exists(test_root / "usr/share/shared.txt"));
    EXPECT_FALSE(fs::exists(test_root / "usr/share/alpha-v2-only.txt"));
    EXPECT_TRUE(fs::exists(test_root / "usr/share/alpha-v1-only.txt")) << "v1 独有文件没被还原";
    EXPECT_TRUE(Cache::instance().get_file_owners("/usr/share/shared.txt").empty())
        << "所有权接管没有被回滚（ARCH §4.5 OWNER_OVERRIDE：回滚必须 cache.load() 回旧值）";
    expect_all_at_v1("拷贝中途失败回滚后");
    expect_fshelper_layout_intact("拷贝中途失败回滚后");
}
