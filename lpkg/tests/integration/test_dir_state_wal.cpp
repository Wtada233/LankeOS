/**
 * test_dir_state_wal.cpp — 目录的"改前状态"必须进 WAL，且 xattr 键要有归属（2026-09-26 新增）
 *
 * ── 两个缺陷（本文件各钉一组）──────────────────────────────────────────────────
 *
 * **① 目录元数据与 xattr 的写入不在 WAL 里（可达的回滚漏洞）**
 * 普通文件是安全的：先写 `<dst>.lpkgtmp`、再由 `COPY` rename 覆盖，**旧 inode 由 BACKUP
 * 保住**。**目录不是** —— `write_dir_entry()` / `let_go_make_dir()` 直接对**活着的**目录
 * `lchown`/`chmod`/`lsetxattr`，一个 WAL 行都不写。于是注入失败回滚后，那个目录保留
 * **新**版本的 mode/uid/xattr，与 `ARCH.md` §4 不变量 3（终态 == 事务开始时的盘面）冲突。
 * **这不是潜伏问题**：任何"新版本改了某个已存在目录的 mode"的批次失败都会踩到。
 * 修法：`OpSink::dir_meta()` / `set_xattr()` / `unset_xattr()`（write-ahead，行类型见下）。
 *
 * **② xattr 键"不再声明"时撤不掉（安全性质）**
 * 写入侧只写"我有的键"、从不删（`copy_xattrs` 的语义），于是新版本**撤掉**一个键时它留在
 * 盘上继续生效 —— 陈旧的 `system.posix_acl_default`（目录下新建文件的继承权限）会继续
 * 放权限，陈旧的 `security.selinux` 会继续打旧标签。修法：新增 `xattrkeys.db` 记
 * "**哪个包在哪个目录上声明过哪个键**"，升级/移除时按"这个**键**还有没有别的属主"撤。
 *
 * ── 为什么"归属"必须按**键**记 ────────────────────────────────────────────────
 * xattr 是按目录共用的：一个目录被多个包持有是常态（`/usr/share` 就有一堆包），每个包各自
 * 声明自己那几个键。按"这个**目录**还有没有别的属主"判，会让 A 包升级时撤掉 **B 包的**键
 * —— 那是**改坏别人的东西**，比"留一份陈旧 xattr"严重得多。本文件有一条用例专门钉它。
 */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/install_common.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

namespace
{
/// 目录的权限位（lstat，不跟随末段链接）
mode_t dir_mode(const fs::path& p)
{
    struct stat st{};
    if (::lstat(p.c_str(), &st) != 0) return static_cast<mode_t>(-1);
    return st.st_mode & 07777;
}
}  // namespace

class DirStateWalTest : public IntegrationTestBase
{
protected:
    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        IntegrationTestBase::TearDown();
    }

    static void write_file(const fs::path& p, const std::string& content)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }

    static std::string read_file(const fs::path& p)
    {
        std::ifstream f(p);
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    /// 盘上还剩几个 stash 痕迹（`.lpkg_bak_*`，目录与文件都算）
    int count_bak_residue() const
    {
        int n = 0;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(test_root, ec)) {
            if (ec) break;
            if (e.path().filename().string().find(".lpkg_bak_") != std::string::npos) ++n;
        }
        return n;
    }

    /// 读整份 WAL（`wal::log_wal_line` 每写一行都 open/write/fsync/close ⇒ 断点回调里读得到）
    static std::string read_wal()
    {
        std::ifstream f(wal::wal_log_path());
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    /// 设一个 xattr；失败直接 FAIL（不支持的文件系统**不许静默跳过** —— 那会假绿）
    static void set_xattr(const fs::path& p, const std::string& name, const std::string& value)
    {
        ASSERT_EQ(::lsetxattr(p.c_str(), name.c_str(), value.data(), value.size(), 0), 0)
            << "设 xattr 失败 " << p << " " << name;
    }

    /// 读一个 xattr；键不存在 → nullopt（**与"值是空串"区分开**）
    static std::optional<std::string> get_xattr(const fs::path& p, const std::string& name)
    {
        char buf[512];
        const ssize_t n = ::lgetxattr(p.c_str(), name.c_str(), buf, sizeof(buf));
        if (n < 0) return std::nullopt;
        return std::string(buf, static_cast<size_t>(n));
    }

    /// 打一个包：`fill` 往 `content/` 里填东西
    template <typename F>
    std::string pack(const std::string& name, const std::string& ver, F fill) const
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::create_directories(work / "content");
        fill(work / "content");
        const std::string path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(path, work.string(), name, ver, {}, {}, "man " + name, {});
        return path;
    }

    /// 让共享目录在包里带上指定的权限（打包时会记进归档条目）
    static void set_dir(const fs::path& dir, mode_t mode)
    {
        fs::create_directories(dir);
        ASSERT_EQ(::chmod(dir.c_str(), mode), 0);
    }

    /// 触发一次"批次中途失败"：断点命中即抛 → 整批回滚
    void fail_at(const std::string& breakpoint)
    {
        BreakpointManager::instance().set(breakpoint,
                                          [] { throw LpkgException("injected: 批次中途失败"); });
    }
};

// ═══════════════════════════════════════════════════════════════════════════
//  ① 既有目录的元数据：回滚必须还原（缺陷 ① 的本体）
// ═══════════════════════════════════════════════════════════════════════════

/**
 * 盘上先有一个 0700 的目录，包声明它是 0755；批次中途失败 → 回滚后必须是 **0700**。
 *
 * 断点取 `copy_after_wal_<pkg>`（写入趟里普通文件落位那一刻）：目录条目的元数据在它**之前**
 * 已经刷过（`scan_content_files` 先给目录条目、后给它的子文件），所以断点命中时盘上已经是
 * **新** mode —— 这个"当场取证"是本用例的要害：不证明这一点，"回滚后是 0700"可能只是因为
 * 压根没人改过它（恒真）。
 */
TEST_F(DirStateWalTest, ExistingDirModeIsRestoredOnRollback)
{
    const std::string pkg = "ds_mode";
    const fs::path target = test_root / "usr/share/dsmode";
    set_dir(target, 0700);
    ASSERT_EQ(dir_mode(target), 0700u);

    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dsmode", 0755);
        write_file(c / "usr/share/dsmode/a.txt", "v1\n");
    });

    mode_t mode_at_breakpoint = 0;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&] {
        mode_at_breakpoint = dir_mode(test_root / "usr/share/dsmode");
        throw LpkgException("injected: 目录元数据已改、批次中途失败");
    });
    EXPECT_THROW(install_packages({v1}), LpkgException);
    BreakpointManager::instance().clear_all();

    EXPECT_EQ(mode_at_breakpoint, 0755u)
        << "断点时刻目录还不是新 mode ⇒ 本用例没考到「元数据已改」那一刻，回滚断言是恒真的";
    EXPECT_EQ(dir_mode(target), 0700u)
        << "回滚后目录 mode 停在**新**值 —— 目录是就地改活对象，改前值不在 WAL 里就回滚不回来"
           "（ARCH §4 不变量 3：终态 == 事务开始时的盘面）";
}

/**
 * 盘上 mode 与包内不一致时**必须留下可见告警**（`warning.dir_perm_mismatch`）：接下来那句
 * "已修正"是配套动作，不是它的替代品 —— 用户得能知道 lpkg 改了这个目录的权限。
 *
 * 为什么补这一条：这条告警此前**没有任何用例钉住**，于是"它到底响没响"只能靠翻日志猜，本轮
 * 就真出过一次误判 —— 拿**键名** `dir_perm_mismatch` 去 grep 整份全量日志得 **0 次**，被读成
 * "分支没跑"；实际 `log_warning(string_format(键, …))` 打的是 **l10n 渲染后的译文**，键名永远
 * 不出现在输出里（实测那里有恰好一次：`... permissions differ: current 448, package wants
 * 493 — corrected`，448/493 就是 0700/0755）。
 *
 * 所以断言一律锚在**渲染文本**上，且取模板里第一个 `{}` 之前的前缀 ⇒ 与语言无关
 * （同 `tests/unit/test_repo_index_parsing.cpp` 的手法）。
 */
TEST_F(DirStateWalTest, ExistingDirModeMismatchIsReportedBeforeItIsCorrected)
{
    const std::string pkg = "ds_warn";
    const fs::path target = test_root / "usr/share/dswarn";
    set_dir(target, 0700);
    ASSERT_EQ(dir_mode(target), 0700u);

    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dswarn", 0755);
        write_file(c / "usr/share/dswarn/a.txt", "v1\n");
    });

    std::ostringstream cap;
    auto* old = std::cerr.rdbuf(cap.rdbuf());
    ASSERT_NO_THROW(install_packages({v1}));
    std::cerr.rdbuf(old);

    // **锚要取模板里最长的字面片段**，不能像别处那样简单"取第一个 `{}` 之前的前缀"：本键的
    // 模板是 `Directory {} permissions differ: …`，第一个 `{}` 之前只有 `"Directory "` 一个词
    // —— 太泛（别处也可能出现），拿它当锚等于没断言。取最长片段（en 是
    // `" permissions differ: current "`、zh 是 `" 权限不一致：当前 "`）才真的只可能来自这一句。
    const std::string tmpl = get_string("warning.dir_perm_mismatch");
    std::string needle;
    for (size_t b = 0; b <= tmpl.size();) {
        const size_t e = tmpl.find("{}", b);
        const size_t end = (e == std::string::npos) ? tmpl.size() : e;
        if (end - b > needle.size()) needle = tmpl.substr(b, end - b);
        if (e == std::string::npos) break;
        b = e + 2;
    }
    ASSERT_GE(needle.size(), 8u) << "l10n 模板里没有足够长的字面片段可作锚：" << tmpl;
    EXPECT_NE(cap.str().find(needle), std::string::npos)
        << "盘上 0700、包内 0755 ⇒ 必须告警（否则静默改权限）。要找的锚是：\n"
        << needle << "\n捕获到的 stderr：\n"
        << cap.str();
    // 与告警配对的另一半：确实**纠正**了 —— 只告警不修同样是缺陷
    EXPECT_EQ(dir_mode(target), 0755u) << "告警说了「已修正」却没真的改过来";
}

// ═══════════════════════════════════════════════════════════════════════════
//  ② 既有目录的 xattr：覆盖 / 新建 / 删除三种改法的回滚
// ═══════════════════════════════════════════════════════════════════════════

/// 键本来有值 → 写入覆盖它 → 回滚必须还原**旧值**（行类型 `XATTR_SET` 的逆）
TEST_F(DirStateWalTest, OverwrittenKeyValueIsRestoredOnRollback)
{
    const std::string pkg = "ds_ovw";
    const fs::path dir = test_root / "usr/share/dsovw";
    set_dir(dir, 0755);
    set_xattr(dir, "user.k", "old-value");

    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dsovw", 0755);
        set_xattr(c / "usr/share/dsovw", "user.k", "new-value");
        write_file(c / "usr/share/dsovw/a.txt", "v1\n");
    });

    std::string value_at_breakpoint;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&] {
        value_at_breakpoint = get_xattr(dir, "user.k").value_or("<缺失>");
        throw LpkgException("injected");
    });
    EXPECT_THROW(install_packages({v1}), LpkgException);
    BreakpointManager::instance().clear_all();

    EXPECT_EQ(value_at_breakpoint, "new-value") << "断点时刻还没写上新值 ⇒ 本用例没考到那次覆盖";
    EXPECT_EQ(get_xattr(dir, "user.k").value_or("<缺失>"), "old-value")
        << "回滚没有把 xattr 的**改前值**写回去（旧值已经丢了）";
}

/// 键本来**不存在** → 写入新建它 → 回滚必须把它**删掉**（行类型 `XATTR_NEW` 的逆）
TEST_F(DirStateWalTest, NewlyAddedKeyIsRemovedOnRollback)
{
    const std::string pkg = "ds_new";
    const fs::path dir = test_root / "usr/share/dsnew";
    set_dir(dir, 0755);
    ASSERT_FALSE(get_xattr(dir, "user.added").has_value()) << "前置：键本来不存在";

    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dsnew", 0755);
        set_xattr(c / "usr/share/dsnew", "user.added", "v");
        write_file(c / "usr/share/dsnew/a.txt", "v1\n");
    });

    bool present_at_breakpoint = false;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&] {
        present_at_breakpoint = get_xattr(dir, "user.added").has_value();
        throw LpkgException("injected");
    });
    EXPECT_THROW(install_packages({v1}), LpkgException);
    BreakpointManager::instance().clear_all();

    EXPECT_TRUE(present_at_breakpoint) << "断点时刻键还没写上 ⇒ 本用例没考到那次新建";
    EXPECT_FALSE(get_xattr(dir, "user.added").has_value())
        << "回滚没有删掉这次新建的键（`XATTR_NEW` 的逆操作没生效）—— 盘上会留下一份"
           "本批次凭空造出来的 xattr";
}

// ═══════════════════════════════════════════════════════════════════════════
//  ③ 撤销：新版本不再声明的键、以及多包共用
// ═══════════════════════════════════════════════════════════════════════════

/**
 * v1 声明 `user.old`，v2 不再声明 → 升级后该键**必须消失**（缺陷 ② 的本体）。
 *
 * 为什么这是安全性质：陈旧的 `system.posix_acl_default` 会继续决定该目录下新建文件的继承
 * 权限、陈旧的 `security.selinux` 会继续打旧标签 —— 上游删掉它是有意的。
 */
TEST_F(DirStateWalTest, KeyNoLongerDeclaredIsRevokedOnUpgrade)
{
    const std::string pkg = "ds_rev";
    const fs::path dir = test_root / "usr/share/dsrev";
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dsrev", 0755);
        set_xattr(c / "usr/share/dsrev", "user.old", "v1");
        write_file(c / "usr/share/dsrev/a.txt", "v1\n");
    })}));
    ASSERT_EQ(get_xattr(dir, "user.old").value_or("<缺失>"), "v1") << "前置：v1 的键没落上";

    // v2：同一个目录、同一个文件，但**不再声明** user.old
    ASSERT_NO_THROW(install_packages({pack(pkg, "2.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dsrev", 0755);
        write_file(c / "usr/share/dsrev/a.txt", "v2\n");
    })}));

    EXPECT_EQ(read_file(dir / "a.txt"), "v2\n") << "前置：内容换代了";
    EXPECT_FALSE(get_xattr(dir, "user.old").has_value())
        << "v2 不再声明 user.old，它却还在盘上继续生效 —— 陈旧的 posix_acl_default / "
           "security.selinux 属于**安全性质**，不是清理洁癖";
    // 归属记录也要跟着撤：留着会让下一次审计以为"这个键还是我们的"
    EXPECT_TRUE(Cache::instance().get_xattr_key_owners("/usr/share/dsrev/", "user.old").empty())
        << "键撤了，归属记录却还在";
}

/// v2 **仍然声明**这个键（换了值）→ 不许撤，且值要更新
TEST_F(DirStateWalTest, KeyStillDeclaredIsUpdatedNotRevoked)
{
    const std::string pkg = "ds_keep";
    const fs::path dir = test_root / "usr/share/dskeep";
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dskeep", 0755);
        set_xattr(c / "usr/share/dskeep", "user.k", "v1");
        write_file(c / "usr/share/dskeep/a.txt", "v1\n");
    })}));
    ASSERT_NO_THROW(install_packages({pack(pkg, "2.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dskeep", 0755);
        set_xattr(c / "usr/share/dskeep", "user.k", "v2");
        write_file(c / "usr/share/dskeep/a.txt", "v2\n");
    })}));

    EXPECT_EQ(get_xattr(dir, "user.k").value_or("<缺失>"), "v2")
        << "v2 仍声明的键被撤掉/没更新 —— 撤销判据（\"新版本还声明它吗\"）判反了";
}

/**
 * 两个包共用同一个目录、都声明**同名键** → A 升级到"不再声明它"时**不许**把键撤掉
 * （B 还持有它）。判据必须是"这个**键**还有没有别的属主"，不是"这个目录还有没有别的属主"。
 */
TEST_F(DirStateWalTest, KeyOwnedByAnotherPackageIsNotRevoked)
{
    const std::string a = "ds_own_a";
    const std::string b = "ds_own_b";
    const fs::path dir = test_root / "usr/share/dsown";
    ASSERT_NO_THROW(install_packages({pack(a, "1.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dsown", 0755);
        set_xattr(c / "usr/share/dsown", "user.shared", "from-a");
        write_file(c / "usr/share/dsown/a.txt", "a\n");
    })}));
    ASSERT_NO_THROW(install_packages({pack(b, "1.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dsown", 0755);
        set_xattr(c / "usr/share/dsown", "user.shared", "from-b");
        write_file(c / "usr/share/dsown/b.txt", "b\n");
    })}));
    ASSERT_EQ(get_xattr(dir, "user.shared").value_or("<缺失>"), "from-b") << "前置：b 后装、值归 b";

    // A 升级到不再声明 user.shared 的版本
    ASSERT_NO_THROW(install_packages({pack(a, "2.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dsown", 0755);
        write_file(c / "usr/share/dsown/a.txt", "a2\n");
    })}));

    EXPECT_EQ(get_xattr(dir, "user.shared").value_or("<缺失>"), "from-b")
        << "A 升级时把**B 仍然持有**的键撤掉了 —— 归属判据按目录判了（那是改坏别人的东西）";
    EXPECT_FALSE(
        Cache::instance().get_xattr_key_owners("/usr/share/dsown/", "user.shared").contains(a))
        << "A 的归属登记应当被摘掉（它确实不再声明这个键了）";
    EXPECT_TRUE(
        Cache::instance().get_xattr_key_owners("/usr/share/dsown/", "user.shared").contains(b))
        << "B 的归属登记被误摘 —— 下一次 B 升级时它就撤不掉自己的键了";
}

/// 撤销动作本身也要可回滚：v2 撤掉键之后批次失败 → 键必须**逐字节**回来
TEST_F(DirStateWalTest, RevokedKeyIsRestoredOnRollback)
{
    const std::string pkg = "ds_revrb";
    const fs::path dir = test_root / "usr/share/dsrevrb";
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dsrevrb", 0755);
        set_xattr(c / "usr/share/dsrevrb", "user.old", "v1-value");
        write_file(c / "usr/share/dsrevrb/a.txt", "v1\n");
    })}));
    ASSERT_EQ(get_xattr(dir, "user.old").value_or("<缺失>"), "v1-value");

    // v2 撤掉 user.old，但批次在写入趟中途失败 → 整批回滚
    const std::string v2 = pack(pkg, "2.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dsrevrb", 0755);
        write_file(c / "usr/share/dsrevrb/a.txt", "v2\n");
    });
    bool gone_at_breakpoint = false;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&] {
        gone_at_breakpoint = !get_xattr(dir, "user.old").has_value();
        throw LpkgException("injected");
    });
    EXPECT_THROW(install_packages({v2}), LpkgException);
    BreakpointManager::instance().clear_all();

    EXPECT_TRUE(gone_at_breakpoint) << "断点时刻键还在 ⇒ 本用例没考到那次撤销";
    EXPECT_EQ(get_xattr(dir, "user.old").value_or("<缺失>"), "v1-value")
        << "撤销动作不可回滚：键没有还原（撤之前必须把旧值写进 WAL —— `XATTR_SET`）";
    EXPECT_EQ(read_file(dir / "a.txt"), "v1\n") << "前置：整批确实回滚了";
}

/// 移除整包：本包声明过的键要撤（与 `--purge-config` 无关 —— 那是 /etc **文件**的政策）
TEST_F(DirStateWalTest, RemovedPackageRevokesItsKeys)
{
    const std::string pkg = "ds_rm";
    const fs::path dir = test_root / "usr/share/dsrm";
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dsrm", 0755);
        set_xattr(c / "usr/share/dsrm", "user.mine", "x");
        write_file(c / "usr/share/dsrm/a.txt", "v1\n");
    })}));
    ASSERT_EQ(get_xattr(dir, "user.mine").value_or("<缺失>"), "x") << "前置";

    // ⚠️ **这个前置是本用例的命门**：目录里必须有**无主内容**，否则移除时阶段 B 会把这个
    // "最后持有者 + 此刻为空"的目录整个 `rmdir` 掉 —— 键随着目录一起消失，**与撤销代码
    // 毫无关系**。第一版用例就是那样写的，于是把撤销接线整段注释掉它**照样绿**（假绿）。
    // 加了无主文件之后目录会因"非空"被保留（安全边界：无主内容一律不碰），撤销代码才成为
    // 唯一能让那个键消失的东西。
    write_file(dir / "user-file.txt", "不属于任何包\n");

    ASSERT_NO_THROW(remove_package(pkg, /*force=*/true));

    ASSERT_TRUE(fs::is_directory(dir))
        << "目录被整个删掉了 ⇒ 本用例又退回假绿（键的消失不再由撤销代码负责）";
    EXPECT_FALSE(get_xattr(dir, "user.mine").has_value())
        << "移除整包后本包声明过的键还在盘上（陈旧的 ACL/标签会继续生效）";
    EXPECT_TRUE(Cache::instance().get_xattr_key_owners("/usr/share/dsrm/", "user.mine").empty())
        << "键撤了，归属记录却还在";
}

// ═══════════════════════════════════════════════════════════════════════════
//  ④ ELOOP：中间段成环时，判定不得抛 raw filesystem_error
// ═══════════════════════════════════════════════════════════════════════════

/**
 * 盘上 `/usr/share/dspylib -> dspylib`（自环），包里带 `usr/share/dspylib/real.txt`
 * （**普通文件**，毫无特殊）→ 批次预检要对每个归档条目调 `probe_path()`，而它在
 * **中间段**成环时必抛（`fs::exists(phys, ec) || fs::is_symlink(phys)` 的短路方向决定
 * 右操作数必被求值），抛出来的还是 **raw `filesystem_error`**（不是 LpkgException、
 * 无 l10n 文案），整批中止、异常穿透到 CLI。
 *
 * 断言的不是"这次安装一定成功"（环上的父目录本来就装不进去），而是**失败也得是干净的
 * 失败**：要么成功，要么抛 `LpkgException`。raw `filesystem_error` 穿透 = 缺陷。
 */
TEST_F(DirStateWalTest, MiddleComponentLoopFailsCleanlyNotWithRawFsError)
{
    const std::string pkg = "ds_loop";
    const fs::path share = test_root / "usr/share";
    fs::create_directories(share);
    fs::create_symlink("dspylib", share / "dspylib");  // 自环
    ASSERT_TRUE(fs::is_symlink(share / "dspylib"));

    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        write_file(c / "usr/share/dspylib/real.txt", "payload\n");
    });

    std::string what;
    bool clean = true;
    try {
        install_packages({v1});
    } catch (const LpkgException& e) {
        what = e.what();  // 干净的失败：有 l10n 文案、有定位信息
    } catch (const std::exception& e) {
        clean = false;
        what = e.what();  // raw filesystem_error：没有 l10n、没有定位
    }
    EXPECT_TRUE(clean)
        << "中间段成环时抛的不是 LpkgException 而是 raw 异常（判定类调用不该有能力打断事务）："
        << what;
}

// ═══════════════════════════════════════════════════════════════════════════
//  ⑤ 归属表本身接进了 DB 生命周期
// ═══════════════════════════════════════════════════════════════════════════

/**
 * `xattrkeys.db` 必须与 `files.db`/`confhashes.db` **同族**：`init_filesystem()` 预建、
 * `Cache::write(milestone)` 走 WAL + 里程碑备份。少任何一环，这张表就会在崩溃/回滚后与
 * 盘面脱节（例如：回滚把归属还原了、但盘上的键没还原，或者反过来）。
 *
 * 这条只钉最容易被漏的一环 —— **预建**（`ensure_file_exists`）：不预建时
 * `write_db_file_wal` 会走 `DBNEW`、**不产生** `:batch-start` 备份，于是"每里程碑一份备份"
 * 对这个库不成立，而崩溃恢复的判据 `batch_start_db_still_in_place` 依赖它。
 */
TEST_F(DirStateWalTest, XattrKeysDbIsPrecreatedWithTheRestOfTheFamily)
{
    Config::instance().init_filesystem();
    EXPECT_TRUE(fs::exists(Config::instance().xattr_keys_db()))
        << "xattrkeys.db 没有被 init_filesystem 预建 —— 它会以 DBNEW 路径首次写入，"
           "从而**没有** :batch-start 备份（与兄弟库不同族）";
    // 与兄弟库同一目录（state_dir），且 cleanup_db_backups 的两个扫描根覆盖它
    EXPECT_EQ(Config::instance().xattr_keys_db().parent_path(),
              Config::instance().files_db().parent_path());
}

// ═══════════════════════════════════════════════════════════════════════════
//  ⑦ 三个新原语的 write-ahead **窗口**：WAL 行已落、动作未做
// ═══════════════════════════════════════════════════════════════════════════
//
// `dir_meta` / `set_xattr` / `unset_xattr` 都收 `after_wal_breakpoint`，但此前**没有任何
// 调用点传它、也没有用例用它** —— 而本仓库的断点纪律是"每个窗口都要有用例"：一个"接了断点
// 却传不下去"的调用点，在测试里没有任何东西会说话（上一轮 `record_let_go_facts` 那条正是
// 同一个教训）。
//
// 三条用例钉同一个形状，**三件断言缺一不可**：
//   ① 断点真的命中（否则后面两条恒真）；
//   ② 命中时刻 **WAL 里已有那一行**（write-ahead 的"行在前"）；
//   ③ 命中时刻 **盘面还没被改**（"动作在后"）。
// ② + ③ 合起来才是 write-ahead：只钉 ② 的话，"先改盘、再补写行"的实现照样绿。
// 然后再从断点抛出去 → 整批回滚 → 终态 == 批次前。**回滚那一条同时反向验证了行里记的值**
// —— 旧值记错了，回滚就会把它还原成错的。

/// `DIR_META` 的窗口：行已落、`lchown`/`chmod` 未做
///
/// ⚠️ 目标目录取**顶层**（`<root>/dswm`），不用 `usr/share/dswm`：归档里含**祖先目录
/// 条目**（`usr/`、`usr/share/`），它们各自也会刷元数据、各写一行 `DIR_META` —— 而
/// `BreakpointManager` 的断点是**一次性**的（头文件的契约："每个断点只触发一次，之后自动
/// 清除"）。于是第一次命中（父目录）就把断点消耗掉了，目标那一格**永远轮不到**。
/// 第一版用例就是这么写的：`hit` 为真、WAL 里也有 `DIR_META`（只是父目录那一行），而断言
/// 找的是目标那一行 —— 实测把这个坑量了出来。顶层目录没有祖先条目，第一次命中就是它。
TEST_F(DirStateWalTest, DirMetaWindowWritesRowBeforeTouchingTheDisk)
{
    const std::string pkg = "ds_wm";
    const fs::path dir = test_root / "dswm";  // 顶层：归档里没有祖先目录条目
    set_dir(dir, 0700);
    ASSERT_EQ(dir_mode(dir), 0700u) << "前置：盘上是 0700";

    bool hit = false;
    std::string wal_at_hit;
    mode_t mode_at_hit = 0;
    BreakpointManager::instance().set("dirmeta_after_wal_" + pkg, [&] {
        hit = true;
        wal_at_hit = read_wal();
        mode_at_hit = dir_mode(dir);
        throw LpkgException("injected: DIR_META 行已落、lchown/chmod 未做");
    });

    EXPECT_THROW(install_packages({pack(pkg, "1.0",
                                        [](const fs::path& c) {
                                            set_dir(c / "dswm", 0755);
                                            write_file(c / "dswm/a.txt", "v1\n");
                                        })}),
                 LpkgException);
    BreakpointManager::instance().clear_all();

    EXPECT_TRUE(hit) << "断点没命中 ⇒ 调用点没把 after_wal_breakpoint 传下去（窗口仍注入不进去）";
    EXPECT_NE(wal_at_hit.find("DIR_META " + dir.string() + " "), std::string::npos)
        << "命中时刻 WAL 里没有目标目录的 DIR_META 行 ⇒ 不是 write-ahead（行在后）";
    EXPECT_EQ(mode_at_hit, 0700u) << "命中时刻盘面已是新 mode ⇒ 动作做在行之前，不是 write-ahead";
    EXPECT_EQ(dir_mode(dir), 0700u)
        << "回滚后没还原（这一条同时验证行里记的旧 mode 是对的：记错了就会还原成错的）";
}

/// `XATTR_NEW` 的窗口：行已落、`lsetxattr` 未做
TEST_F(DirStateWalTest, SetXattrWindowWritesRowBeforeTouchingTheDisk)
{
    const std::string pkg = "ds_wx";
    const fs::path dir = test_root / "usr/share/dswx";
    set_dir(dir, 0755);
    ASSERT_FALSE(get_xattr(dir, "user.added").has_value()) << "前置：键本来不存在";

    bool hit = false;
    std::string wal_at_hit;
    bool key_present_at_hit = true;
    BreakpointManager::instance().set("xattrset_after_wal_" + pkg, [&] {
        hit = true;
        wal_at_hit = read_wal();
        key_present_at_hit = get_xattr(dir, "user.added").has_value();
        throw LpkgException("injected: XATTR_NEW 行已落、lsetxattr 未做");
    });

    EXPECT_THROW(install_packages({pack(pkg, "1.0",
                                        [](const fs::path& c) {
                                            set_dir(c / "usr/share/dswx", 0755);
                                            set_xattr(c / "usr/share/dswx", "user.added", "v");
                                            write_file(c / "usr/share/dswx/a.txt", "v1\n");
                                        })}),
                 LpkgException);
    BreakpointManager::instance().clear_all();

    EXPECT_TRUE(hit) << "断点没命中 ⇒ 调用点没把 after_wal_breakpoint 传下去";
    EXPECT_NE(wal_at_hit.find("XATTR_NEW " + dir.string() + " "), std::string::npos)
        << "命中时刻 WAL 里没有 XATTR_NEW 行 ⇒ 不是 write-ahead";
    EXPECT_FALSE(key_present_at_hit) << "命中时刻键已经写上 ⇒ 动作做在行之前，不是 write-ahead";
    EXPECT_FALSE(get_xattr(dir, "user.added").has_value())
        << "回滚后那个「本来不存在」的键必须重新不存在";
}

/// `XATTR_SET`（撤销分支）的窗口：行已落、`lremovexattr` 未做
TEST_F(DirStateWalTest, UnsetXattrWindowWritesRowBeforeTouchingTheDisk)
{
    const std::string pkg = "ds_wr";
    const fs::path dir = test_root / "usr/share/dswr";
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dswr", 0755);
        set_xattr(c / "usr/share/dswr", "user.old", "v1-value");
        write_file(c / "usr/share/dswr/a.txt", "v1\n");
    })}));
    ASSERT_EQ(get_xattr(dir, "user.old").value_or("<缺失>"), "v1-value") << "前置：v1 的键落上了";

    // v2 不再声明 user.old ⇒ 撤销趟（让开趟）会删它；断点钉在"行已落、lremovexattr 未做"
    const std::string v2 = pack(pkg, "2.0", [](const fs::path& c) {
        set_dir(c / "usr/share/dswr", 0755);
        write_file(c / "usr/share/dswr/a.txt", "v2\n");
    });
    bool hit = false;
    std::string wal_at_hit;
    std::string value_at_hit;
    BreakpointManager::instance().set("xattrrm_after_wal_" + pkg, [&] {
        hit = true;
        wal_at_hit = read_wal();
        value_at_hit = get_xattr(dir, "user.old").value_or("<缺失>");
        throw LpkgException("injected: XATTR_SET 行已落、lremovexattr 未做");
    });
    EXPECT_THROW(install_packages({v2}), LpkgException);
    BreakpointManager::instance().clear_all();

    EXPECT_TRUE(hit) << "断点没命中 ⇒ 撤销那个调用点没把 after_wal_breakpoint 传下去";
    // 撤销一个"本来有值"的键 ⇒ 行类型是 XATTR_SET（逆操作 = 把旧值写回去）
    EXPECT_NE(wal_at_hit.find("XATTR_SET " + dir.string() + " "), std::string::npos)
        << "命中时刻 WAL 里没有 XATTR_SET 行 ⇒ 撤之前没记旧值，回滚就还原不回来";
    EXPECT_EQ(value_at_hit, "v1-value") << "命中时刻键已被删 ⇒ 动作做在行之前，不是 write-ahead";
    EXPECT_EQ(get_xattr(dir, "user.old").value_or("<缺失>"), "v1-value")
        << "回滚后键与值都要逐字节回来";
    EXPECT_EQ(read_file(dir / "a.txt"), "v1\n") << "前置：整批确实回滚了";
}

// ═══════════════════════════════════════════════════════════════════════════
//  ⑧⑨ 归属表本身可回滚 / 成功提交后不留 stash
// ═══════════════════════════════════════════════════════════════════════════

/**
 * `xattrkeys.db` 的**归属记录本身**也要能回滚。
 *
 * 它走的是与 `confhashes.db` 逐字相同的 `write_db_file_wal` 里程碑链（`:batch-start` 备份 →
 * `reverse_execute` 的 DB 分支还原），但这条链**此前没有任何用例直接钉住** —— 只钉了
 * "盘上的键回来了"（那是 xattr 那一半）。这一条钉的是**表**。
 *
 * 现场：v1 声明 `user.k`（归属记下来）→ v2 撤掉它、改声明 `user.k2`，批次中途失败 →
 * 回滚后**归属表必须回到批次前**：`user.k` 仍属本包、`user.k2` 不属于任何人。
 * 只断言前者不够 —— "撤销趟压根没跑"也会让它成立；两条一起才排除了"这一批的两处改动
 * 都没被记下来"。
 */
TEST_F(DirStateWalTest, XattrOwnershipTableIsRestoredOnRollback)
{
    const std::string pkg = "ds_own";
    const fs::path dir = test_root / "dsown";
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", [](const fs::path& c) {
        set_dir(c / "dsown", 0755);
        set_xattr(c / "dsown", "user.k", "v1");
        write_file(c / "dsown/a.txt", "v1\n");
    })}));
    Cache::instance().load();
    ASSERT_EQ(Cache::instance().get_xattr_key_owners("/dsown/", "user.k").size(), 1u)
        << "前置：v1 的键归属没记下来（那 ⑧ 就无从谈起）";

    // v2：撤掉 user.k、改声明 user.k2 —— 归属表这一批要变两处
    const std::string v2 = pack(pkg, "2.0", [](const fs::path& c) {
        set_dir(c / "dsown", 0755);
        set_xattr(c / "dsown", "user.k2", "v2");
        write_file(c / "dsown/a.txt", "v2\n");
    });
    fail_at("copy_after_wal_" + pkg);
    EXPECT_THROW(install_packages({v2}), LpkgException);
    BreakpointManager::instance().clear_all();

    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_xattr_key_owners("/dsown/", "user.k").size(), 1u)
        << "回滚后归属表没回到批次前：user.k 的属主丢了（撤销趟改了表，而表没被还原）";
    EXPECT_TRUE(Cache::instance().get_xattr_key_owners("/dsown/", "user.k2").empty())
        << "回滚后 user.k2 的属主还在 —— 那一批没装上，归属不该留下";
    EXPECT_EQ(get_xattr(dir, "user.k").value_or("<缺失>"), "v1") << "前置：整批确实回滚了";
}

/**
 * `cleanup_stashes` 的契约：**成功提交后 stash 一个不剩**。
 *
 * 它在 `package_manager.cpp` 的匿名 namespace 里、测试取不到，所以这条从它**唯一的调用
 * 入口**（`finish_committed_batch`，post-commit）的**可观察效果**来钉 —— 不为了覆盖率给它
 * 开一个测试缝（一个到不了那行的测试比没有更糟）。
 *
 * 为什么值得钉：这里正是 `exists_no_follow` 那条修复（原写法
 * `!fs::exists(p) && !fs::is_symlink(p)` 在**中间段成环**的路径上抛 ELOOP）与 `CLEANUP`
 * write-ahead 唯一的落地处；而 `finish_committed_batch` 把它的异常吞成
 * `warning.cleanup_deferred` —— 也就是说**它失败是静默的**，只有"盘上有没有残留"能说话。
 */
TEST_F(DirStateWalTest, SuccessfulBatchLeavesNoStashResidue)
{
    const std::string pkg = "ds_clean";
    ASSERT_NO_THROW(install_packages({pack(pkg, "1.0", [](const fs::path& c) {
        set_dir(c / "dsclean", 0755);
        write_file(c / "dsclean/a.txt", "v1\n");
    })}));

    // 升级：`a.txt` 被替换 ⇒ 让开趟必然把它搬进 stash（这一步产生 `.lpkg_bak_*`）
    ASSERT_NO_THROW(install_packages({pack(pkg, "2.0", [](const fs::path& c) {
        set_dir(c / "dsclean", 0755);
        write_file(c / "dsclean/a.txt", "v2\n");
    })}));

    EXPECT_EQ(read_file(test_root / "dsclean/a.txt"), "v2\n") << "前置：升级成功";
    EXPECT_EQ(count_bak_residue(), 0)
        << "成功提交后仍残留 `.lpkg_bak_*` ⇒ cleanup_stashes 没跑完（它的异常被吞成 "
           "warning.cleanup_deferred，所以只有残留能说话）";
}
