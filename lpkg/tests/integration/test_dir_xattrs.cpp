/**
 * test_dir_xattrs.cpp — **目录**的 xattr 必须跟着包落位（2026-09-26 新增）
 *
 * ── 缺陷 ──────────────────────────────────────────────────────────────────────
 * `copy_xattrs` 全仓只有两个调用点，都是**普通文件**（`stage_regular_file` 的 `.lpkgtmp`、
 * `install_hook_files` 的 hook 脚本）。`write_dir_entry()`（刷新既有目录）与
 * `let_go_make_dir()`（新建目录）只做 `lchown`/`chmod`，**目录从不复制 xattr**。
 *
 * 目录的 xattr 不是小事，它正是 **POSIX ACL 的存放处**：
 *   · `system.posix_acl_default` —— 目录的**默认 ACL**，决定"这个目录下新建的文件/子目录
 *     继承什么权限"。丢了它，包声明的那套继承权限就静默变成 umask 默认值；
 *   · `security.selinux` —— SELinux 标签（本发行版当前未启用 SELinux，但同样经这条路）。
 * 而打包/解包两侧**本来就是全的**：`pack_package` 用 libarchive 的 disk reader（会带上
 * xattr 与真正的 ACL 记录），解包用了 `ARCHIVE_EXTRACT_XATTR | ARCHIVE_EXTRACT_ACL`
 * —— 所以包内 `content/` 下那份目录**有** xattr，只是写入系统时没人把它拷过去。
 *
 * ── 两条边界（有意，均由用例钉住）──────────────────────────────────────────────
 *   1. **目标目录是符号链接 → 整个 xattr 块跳过**。与相邻的元数据块（`lchown`/`chmod`）
 *      同源：`symlink→目录`（usr-merge 的 `/lib -> usr/lib`、`/var/run -> ../run`）时内容是
 *      **穿过**链接写进真实目录的，而 xattr 写下去改的是**别的包持有的**那个目录。
 *   2. **已被多个包持有的目录 → 只写包内声明的键，不整份覆盖**。本文件里的实现方式已经
 *      是**逐键** `lsetxattr`（只写 `from` 有的键、从不删 `to` 上别的键），所以第二个包装
 *      同一个目录时不会抹掉第一个包设的键。
 *      ⚠️ **订正 2026-09-26（同日傍晚）**：本行原写"**代价（有意）**：包在新版本里不再声明的
 *      键不会被撤掉 —— xattr 的'撤'没有归属记账可依据，宁留不删"。**那个前提已经不成立**：
 *      现在有了按**键**的归属记账（`xattrkeys.db`），"新版本不再声明"的键会被撤（判据是
 *      "这个**键**还有没有别的属主"，见 `test_dir_state_wal.cpp`）。撤销不是清理洁癖而是
 *      **安全性质** —— 陈旧的 `system.posix_acl_default` 会继续决定该目录下新建文件的继承
 *      权限、陈旧的 `security.selinux` 会继续打旧标签。
 */

#include <gtest/gtest.h>
#include <sys/xattr.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

namespace
{
constexpr const char* ACL_DEFAULT = "system.posix_acl_default";
constexpr uint16_t TAG_USER_OBJ = 0x01;
constexpr uint16_t TAG_GROUP_OBJ = 0x04;
constexpr uint16_t TAG_MASK = 0x10;
constexpr uint16_t TAG_OTHER = 0x20;

/**
 * 手工拼一份 `system.posix_acl_default` 的值。
 *
 * 容器里**没有任何 xattr 工具**（`setfacl`/`setfattr`/`getfattr` 全无，实测），所以只能直接发
 * 系统调用；而这个 xattr 的值不是任意字节 —— 内核会**校验**它是不是合法的 ACL 记录。
 * 格式（小端）：`struct posix_acl_xattr_header { __le32 a_version; }`（=2）后跟若干
 * `struct posix_acl_xattr_entry { __le16 e_tag; __le16 e_perm; __le32 e_id; }`。
 */
std::vector<char> default_acl_blob()
{
    struct Entry {
        uint16_t tag;
        uint16_t perm;
        uint32_t id;
    };
    const uint32_t version = 2;
    const Entry entries[] = {
        {TAG_USER_OBJ, 07, 0xFFFFFFFFu},
        {TAG_GROUP_OBJ, 05, 0xFFFFFFFFu},
        {TAG_MASK, 05, 0xFFFFFFFFu},
        {TAG_OTHER, 05, 0xFFFFFFFFu},
    };
    std::vector<char> blob;
    blob.resize(sizeof(version) + sizeof(entries));
    std::memcpy(blob.data(), &version, sizeof(version));
    std::memcpy(blob.data() + sizeof(version), entries, sizeof(entries));
    return blob;
}
}  // namespace

class DirXattrTest : public IntegrationTestBase
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

    /// 设一个 `user.*` xattr；失败直接 FAIL（容器/宿主文件系统不支持时**不许静默跳过**）
    static void set_xattr(const fs::path& p, const std::string& name, const std::string& value)
    {
        ASSERT_EQ(::lsetxattr(p.c_str(), name.c_str(), value.data(), value.size(), 0), 0)
            << "设 xattr 失败 " << p << " " << name << "：" << std::strerror(errno);
    }

    /// 设目录的默认 ACL（值由 default_acl_blob 拼，内核会校验）
    static void set_default_acl(const fs::path& dir)
    {
        const std::vector<char> blob = default_acl_blob();
        ASSERT_EQ(::lsetxattr(dir.c_str(), ACL_DEFAULT, blob.data(), blob.size(), 0), 0)
            << "设默认 ACL 失败 " << dir << "：" << std::strerror(errno);
    }

    /// 读一个 xattr 的值；不存在返回 nullopt
    static std::optional<std::string> get_xattr(const fs::path& p, const std::string& name)
    {
        char buf[512];
        const ssize_t n = ::lgetxattr(p.c_str(), name.c_str(), buf, sizeof(buf));
        if (n < 0) return std::nullopt;
        return std::string(buf, static_cast<size_t>(n));
    }

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
};

// ── ① 新建目录：包内那份目录的 xattr（含默认 ACL）必须落到系统上 ──────────────
TEST_F(DirXattrTest, NewDirectoryKeepsPackageXattrs)
{
    const std::string pkg = "dx_new";
    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        const fs::path d = c / "usr/share/dxnew";
        fs::create_directories(d);
        set_xattr(d, "user.probe", "hello");
        set_default_acl(d);
        write_file(d / "a.txt", "content\n");
    });
    ASSERT_NO_THROW(install_packages({v1}));

    const fs::path target = test_root / "usr/share/dxnew";
    ASSERT_TRUE(fs::is_directory(target));
    EXPECT_EQ(get_xattr(target, "user.probe").value_or("<缺失>"), "hello")
        << "包在目录上声明的 xattr 没落到系统上（copy_xattrs 漏了目录这一路）";
    EXPECT_TRUE(get_xattr(target, ACL_DEFAULT).has_value())
        << "目录的默认 ACL（system.posix_acl_default）丢了 —— 该目录下新建文件的继承权限"
        << "会静默退回 umask 默认值";
    // 逐字节比对：这条链是 打包（libarchive disk reader 读 ACL → PAX 记录）→ 解包
    // （ARCHIVE_EXTRACT_ACL 写回）→ `copy_xattrs`（原样搬 xattr 字节），中途**没有**任何一步
    // 会把 ACL 重新序列化成别的等价形态（实测落位的 36 字节与设进去的那份逐字节相同）。
    {
        const std::vector<char> expect = default_acl_blob();
        ASSERT_TRUE(get_xattr(target, ACL_DEFAULT).has_value()) << "（上一条已失败，跳过比对）";
        EXPECT_EQ(get_xattr(target, ACL_DEFAULT).value(), std::string(expect.data(), expect.size()))
            << "落位的默认 ACL 与包内那份不是同一份记录";
    }
}

// ── ② 刷新既有目录：新版本声明的键要写上 ─────────────────────────────────────
TEST_F(DirXattrTest, ExistingDirectoryGetsNewVersionKeys)
{
    const std::string pkg = "dx_refresh";
    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        const fs::path d = c / "usr/share/dxr";
        fs::create_directories(d);
        set_xattr(d, "user.v1", "one");
        write_file(d / "a.txt", "v1\n");
    });
    ASSERT_NO_THROW(install_packages({v1}));
    const fs::path target = test_root / "usr/share/dxr";
    ASSERT_EQ(get_xattr(target, "user.v1").value_or("<缺失>"), "one")
        << "前置：v1 的 xattr 都没落上，本格其余断言无意义";

    const std::string v2 = pack(pkg, "2.0", [](const fs::path& c) {
        const fs::path d = c / "usr/share/dxr";
        fs::create_directories(d);
        set_xattr(d, "user.v2", "two");
        write_file(d / "a.txt", "v2\n");
    });
    ASSERT_NO_THROW(install_packages({v2}));

    EXPECT_EQ(read_file(target / "a.txt"), "v2\n") << "前置：内容换代了";
    EXPECT_EQ(get_xattr(target, "user.v2").value_or("<缺失>"), "two")
        << "刷新既有目录时新版本声明的 xattr 没写上";
    // **语义已改（2026-09-26 傍晚，不是放宽断言）**：v2 不再声明 `user.v1` ⇒ 它**被撤掉**。
    // 原期望写的是"留着不删"，依据是"xattr 没有归属记账可依据，宁留不删"——那个依据已经
    // 不成立：现在 `xattrkeys.db` 按**键**记归属，能精确判断"这个键还有没有别的属主"，
    // 于是可以只撤自己的、不碰别人的（多包共用的边界由
    // `test_dir_state_wal.KeyOwnedByAnotherPackageIsNotRevoked` 钉住）。
    // 为什么必须撤：陈旧的 `system.posix_acl_default` 是**该目录下新建文件的继承权限**，
    // 留着 = 上游删掉的授权继续生效（安全性质，不是清理洁癖）。
    EXPECT_FALSE(get_xattr(target, "user.v1").has_value())
        << "v2 不再声明 user.v1，它却还在盘上 —— 撤销没生效（或归属判据判反了）";
}

// ── ③ 两个包共用一个目录：后装的**不许**抹掉先装的键 ───────────────────────────
TEST_F(DirXattrTest, SecondPackageDoesNotWipeFirstPackageKeys)
{
    const std::string a = "dx_a";
    const std::string b = "dx_b";
    ASSERT_NO_THROW(install_packages({pack(a, "1.0", [](const fs::path& c) {
        const fs::path d = c / "usr/share/dxshared";
        fs::create_directories(d);
        set_xattr(d, "user.from_a", "aaa");
        write_file(d / "a.txt", "a\n");
    })}));
    const fs::path target = test_root / "usr/share/dxshared";
    ASSERT_EQ(get_xattr(target, "user.from_a").value_or("<缺失>"), "aaa") << "前置：a 的键没落上";

    ASSERT_NO_THROW(install_packages({pack(b, "1.0", [](const fs::path& c) {
        const fs::path d = c / "usr/share/dxshared";
        fs::create_directories(d);
        set_xattr(d, "user.from_b", "bbb");
        write_file(d / "b.txt", "b\n");
    })}));

    EXPECT_EQ(get_xattr(target, "user.from_b").value_or("<缺失>"), "bbb") << "b 声明的键没写上";
    EXPECT_EQ(get_xattr(target, "user.from_a").value_or("<缺失>"), "aaa")
        << "b 装同一个目录时把 a 设的键**抹掉**了 —— xattr 落位必须只写自己声明的键";
}

// ── ④ 边界：目标目录是符号链接 → 一个 xattr 都不许碰 ───────────────────────────
//
// 走**直连写入趟**（`task.copy_package_files()`，与 `test_tmp_path_symlink_guard.cpp`
// 同一手法）：这条边界要求"写入趟那一刻目标路径**仍是**符号链接"，而经 `install_packages`
// 走完整流程时让开趟会先把盘上那条链接搬走/改名（盘上无主的链接还会被冲突预检直接拒绝，
// 实测：`File conflict detected`）—— 构造不出这个形态。直连写入趟（无让开趟 ⇒ 无记录 ⇒
// 回退路径）才是它的现场。
TEST_F(DirXattrTest, SymlinkedDirectoryTargetXattrsAreNotTouched)
{
    // 盘上：`usr/share/dxreal` 是真目录（带自己的键），`usr/share/dxlink -> dxreal`
    const fs::path real_dir = test_root / "usr/share/dxreal";
    fs::create_directories(real_dir);
    set_xattr(real_dir, "user.keep", "must-survive");

    const fs::path link = test_root / "usr/share/dxlink";
    fs::create_directories(link.parent_path());
    fs::create_symlink("dxreal", link);
    ASSERT_TRUE(fs::is_symlink(link));

    // 包内：`usr/share/dxlink/` 是**目录**，带一个 xattr；另有 metadata.json 供直连调用
    const std::string pkg = "dx_symtarget";
    const fs::path work = suite_work_dir / "_pkg_dx_symtarget";
    const fs::path content = work / "content";
    fs::create_directories(content / "usr/share/dxlink");
    set_xattr(content / "usr/share/dxlink", "user.from_pkg", "pkg-value");
    write_file(content / "usr/share/dxlink/inside.txt", "pkg\n");
    {
        std::ofstream(work / "metadata.json")
            << "{\"name\":\"" << pkg
            << "\",\"version\":\"1.0\",\"deps\":[],\"provides\":[],\"man\":\"\"}";
    }

    InstallationTask task(pkg, "1.0", true);
    task.set_tmp_dir(work);
    ASSERT_NO_THROW(task.copy_package_files());

    EXPECT_TRUE(fs::is_symlink(link)) << "前置：目标仍应是符号链接（本格考的是它）";
    EXPECT_EQ(read_file(real_dir / "inside.txt"), "pkg\n")
        << "内容照旧**穿过**链接写进真实目录（这条行为不变）";
    EXPECT_FALSE(get_xattr(real_dir, "user.from_pkg").has_value())
        << "把包的 xattr 写到了**链接目标**上 —— 那可能是别的包持有的目录（如 /usr/lib）";
    EXPECT_EQ(get_xattr(real_dir, "user.keep").value_or("<缺失>"), "must-survive")
        << "链接目标上原有的键被动了";
}

// ── ⑤ 目录当废弃删掉后回滚：xattr 必须逐键回来（2026-09-26 修的缺口）────────────
/**
 * **缺陷**：`OpSink::remove_empty_dir` 写的 `DIR_RM <path> <mode> <uid> <gid>` **不含 xattr**，
 * 而回滚侧的 `Undo::RecreateDir` 只 `create_directories` + `lchown`/`chmod` ⇒ **被 rmdir 又由
 * 回滚重建的目录，那份 xattr 全丢**（目录 xattr 正是 POSIX ACL 与 SELinux 标签的存放处）。
 * 这与刚为目录元数据补的 `DIR_META` 是**同一族缺口**，只是长在**移除侧**；由属性测试新加的
 * xattr 维度当场抓到（实测 2/32 种子）。
 *
 * **修法**：`remove_empty_dir` 在写 `DIR_RM` **之前**，把该目录的 xattr 逐键记成 `XATTR_SET` 行
 * （复用现成行类型；只记行、不动盘 —— 目录马上要删）。**行序是承重的**：回滚是逆序的 ⇒
 * 先撤 `DIR_RM`（把目录重建出来）、**再**撤各 `XATTR_SET`（把旧值写回去），正好落在重建好的
 * 目录上；顺序反了的话写回会打在还不存在的路径上，`Guard::TakenNotSymlink` 判否跳过 ⇒ 静默丢失。
 *
 * 本用例钉住这一整条：v1 的目录带一个 `user.*` 键 + 默认 ACL，v2 把它整个丢掉（里面的文件被当
 * 废弃搬走 ⇒ 目录空 ⇒ `DIR_RM`），写入趟注入失败 ⇒ 回滚后目录**连同两个 xattr 逐字节**回来。
 * 另在断点时刻钉一条：那个目录**确实已经不在盘上** —— 否则"回滚后 xattr 还在"可能只是因为
 * 它压根没被删过（恒真废话）。
 */
TEST_F(DirXattrTest, ObsoleteDirectoryXattrsSurviveRollback)
{
    const std::string pkg = "dx_rm";
    const fs::path dir = test_root / "usr/share/dx_rm";

    // v1：目录带两个 xattr（普通键 + 默认 ACL），里面有一个文件；另有一个普通文件让写入趟有活干
    const std::string v1 = pack(pkg, "1.0", [](const fs::path& c) {
        const fs::path d = c / "usr/share/dx_rm";
        fs::create_directories(d);
        set_xattr(d, "user.rmk", "keep-me");
        set_default_acl(d);
        write_file(d / "payload.txt", "v1\n");
        write_file(c / "usr/bin/dx_rm", "bin v1\n");
    });
    ASSERT_NO_THROW(install_packages({v1}));
    ASSERT_TRUE(fs::is_directory(dir)) << "前置：v1 的目录已装到盘上";
    ASSERT_EQ(get_xattr(dir, "user.rmk").value_or("<缺失>"), "keep-me") << "前置：键已在盘上";
    ASSERT_TRUE(get_xattr(dir, ACL_DEFAULT).has_value()) << "前置：默认 ACL 已在盘上";

    // v2：不再提供那个目录与里面的文件（⇒ 废弃搬运 + 目录空 ⇒ DIR_RM），但仍有普通文件可写
    //     ⇒ 断点落在**废弃清除之后**（写入趟跑在 ②b 之后），窗口正好覆盖"目录已被 rmdir"。
    const std::string v2 =
        pack(pkg, "2.0", [](const fs::path& c) { write_file(c / "usr/bin/dx_rm", "bin v2\n"); });

    bool hit = false;
    bool dir_gone_at_hit = false;
    BreakpointManager::instance().set("copy_after_wal_" + pkg, [&] {
        hit = true;
        dir_gone_at_hit = !fs::exists(dir);
        throw LpkgException("injected: 废弃清除已跑完（目录已被 rmdir）、写入趟中途失败");
    });
    EXPECT_THROW(install_packages({v2}), LpkgException);
    BreakpointManager::instance().clear_all();

    ASSERT_TRUE(hit) << "断点没命中 ⇒ 本用例没考到「目录已被 rmdir 之后的回滚」";
    ASSERT_TRUE(dir_gone_at_hit)
        << "断点时刻那个目录还在盘上 ⇒ 它压根没被删过，下面「xattr 回来了」是恒真废话";

    // 回滚后：目录回来，且两个 xattr 逐字节回来
    ASSERT_TRUE(fs::is_directory(dir)) << "回滚没把被 rmdir 的目录重建出来";
    EXPECT_EQ(get_xattr(dir, "user.rmk").value_or("<缺失>"), "keep-me")
        << "回滚重建的目录丢了 user.* 键 —— `DIR_RM` 没把 xattr 记进 WAL（本用例要修的那个缺口）";
    const std::vector<char> expect = default_acl_blob();
    const auto got_acl = get_xattr(dir, ACL_DEFAULT);
    ASSERT_TRUE(got_acl.has_value())
        << "回滚重建的目录丢了默认 ACL —— 该目录下新建文件的继承权限会静默退回 umask 默认值";
    EXPECT_EQ(*got_acl, std::string(expect.data(), expect.size()))
        << "回滚写回的默认 ACL 与批次前那份不是同一份记录";
    // 里面那个文件也该回来（废弃搬运的逆操作）
    EXPECT_EQ(read_file(dir / "payload.txt"), "v1\n") << "废弃文件没被搬回来";
    EXPECT_EQ(Cache::instance().get_installed_version(pkg), "1.0");
}
