/**
 * test_ultimate_multipkg.cpp — **终极**端到端：6 个包样本（3 包 × 2 版本）走完
 * 安装 / 升级 / 卸载 / 回滚 四条路径（2026-09-26 新增）
 *
 * ── 它与既有测试的分工 ──────────────────────────────────────────────────────
 *   · `test_type_transition_matrix.cpp`：**单格**语义，一格里只放一个待考的形状（读得懂，
 *     定位快）；
 *   · `test_upgrade_property.cpp`：**单包**的广谱抽样（种子 × 形态，抓"人没想到的组合"）；
 *   · **本文件**：**多包 + 固定场景表**。前面两者都不覆盖的东西在这里：
 *     跨包争用与合租、以及"一个批次里 3 个包各自带着不同类型的变更一起提交/一起回滚"。
 *     它是**读得懂的**（场景是常量表、每个用例一条路径），与随机组互补而非重复。
 *
 * ── 场景（3 包 × 2 版本，`usr/share/ult/` 与 `/etc/`）────────────────────────
 *
 * `alpha`（负责"同型三格" + `/etc` 三哈希三条分支 + xattr 撤销）：
 *   | 路径                    | v1        | v2        | 考什么                              |
 *   |-------------------------|-----------|-----------|-------------------------------------|
 *   | `ult/same-file`         | file a1   | file a2   | 同型：内容换（三哈希之外的普通路径）|
 *   | `ult/same-dir/`         | dir 0755  | dir 0700  | 同型：dir 元数据被改（必须可回滚）  |
 *   | `ult/same-link`         | →same-file| →a2       | 同型：链接目标换                    |
 *   | `ult/shared/`           | dir+xattr | dir+xattr | 与 **beta 合租**；xattr 见下        |
 *   | `etc/ult-installnew.conf`| file v1  | file v2   | 三哈希 ① 用户没改 → 就地静默换新    |
 *   | `etc/ult-keplocal.conf` | file v1   | file **同 v1** | 三哈希 ② 包没改 + 用户改过 → 保留 |
 *   | `etc/ult-savelpkgnew.conf`| file v1 | file v2   | 三哈希 ③ 两者都改 → 落 `.lpkgnew`    |
 *
 * `beta`（负责**六向互转**）+ `/etc` 的类型变化两格 + 在合租目录上声明自己的 xattr 键：
 *   | 路径               | v1            | v2            | 六向 / 政策                    |
 *   |--------------------|---------------|---------------|--------------------------------|
 *   | `ult/f2d`          | file          | dir/          | file → dir                     |
 *   | `ult/d2f/`         | dir           | file          | **dir → 文件**（缺陷形状腿 1） |
 *   | `ult/f2s`          | file          | symlink       | file → symlink                 |
 *   | `ult/s2f`          | symlink       | file          | symlink → file                 |
 *   | `ult/d2s/`         | dir           | symlink       | **dir → 符号链接**（腿 2）     |
 *   | `ult/s2d`          | symlink       | dir/          | symlink → dir                  |
 *   | `ult/shared/`      | dir + `from-beta.txt` + `user.k_beta` | 同左 | 合租；撤销时**不许碰** |
 *   | `etc/ult-f2s.conf` | file          | symlink       | `/etc` 类型变化：原物 `.lpkgsave` |
 *   | `etc/ult-s2f.conf` | symlink       | file          | `/etc` 类型变化：原物 `.lpkgsave` |
 *
 * `gamma`（负责**废弃**与**新增**）：
 *   | 路径                          | v1   | v2      | 考什么                                   |
 *   |-------------------------------|------|---------|------------------------------------------|
 *   | `ult/gone-file`               | file | （不发）| 废弃普通文件                             |
 *   | `ult/gone-tree/l1/l2/leaf.txt`| file | （不发）| 废弃**嵌套 ≥2 层**的 owned 目录整棵消失  |
 *   | `ult/added-file`              | 无   | file    | 新增文件                                 |
 *   | `ult/added-tree/a/b/leaf.txt` | 无   | file    | 新增嵌套目录树                           |
 *   | `etc/ult-gone.conf`           | file | （不发）| 废弃 `/etc` 配置 → `.lpkgsave`           |
 *
 * ── xattr 的互锁（本文件最值得存在的一组）────────────────────────────────────
 * 合租目录 `usr/share/ult/shared/` 上：
 *   · alpha v1 声明 `user.k_keep=v1`、`user.k_drop=dropme`、`user.k_change=c1`；
 *   · alpha v2 声明 `user.k_keep=v1`（不变）、`user.k_change=c2`（改值）、`user.k_new=new`
 *     （新增）——**不再声明 `k_drop`** ⇒ 升级时**必须撤掉它**（陈旧的
 *     `system.posix_acl_default` 会继续决定该目录下新建文件的继承权限，所以撤销是**安全性质**
 *     不是清理洁癖）；
 *   · beta 在同一目录上声明 `user.k_beta=b1` ⇒ alpha 升级/卸载时**一个字都不许动**。
 * 这一个目录同时考到了：改值、新增、撤销、以及"多包共用时不碰别人的键"。
 *
 * ── 反假绿（沿用 `test_upgrade_property.cpp` 的三条纪律）────────────────────────
 *   1. **注入类必须当场取证"断点时刻盘面 ≠ 基线"**。只断言"回滚后 == 基线"是**恒真废话**
 *      （什么都没动过也满足）。每个注入用例都在 lambda 里抓一条"此刻确实已经变了"的事实，
 *      并在 lambda **之外**断言它 —— 没命中（全等基线）就说明这个用例没考到东西；
 *   2. **注入的失败要落在预期的那条路径上**（不是"抛了就算"）：断言断点**真的命中**；
 *   3. 回滚断言用**整棵树 + 整族 DB 的逐字节快照**（`fs_state()` / `db_state()`），
 *      而不是挑几个路径看 —— 只看几个路径的话，"多出/少掉一个别的文件"会漏掉。
 *
 * ── 写这个文件时撞到的两处缺口（**未改产品代码**，只记录）──────────────────────
 *   1. **`REMOVE_OLD`（废弃搬运）没有断点**：`OpSink::backup_obsolete(phys)` 的签名里
 *      没有 `after_wal_breakpoint`（`backup` / `save_config` / `un_stash` / `commit_copy` /
 *      `dir_meta` / `set_xattr` / `unset_xattr` 都有）。所以"废弃搬运的 WAL 已写、rename
 *      未做"这个窗口**注入不进去**。本文件的替代做法见
 *      `UpgradeRollsBackObsoleteRemovalAlreadyDone`：在**写入趟**注入（它跑在废弃清除 ②b
 *      **之后**），并在断点时刻断言"废弃的那棵树已经不在了" —— 那正是"废弃清除已经发生
 *      才失败"的取证，回滚要把它整棵还回来。等价性：窗口不同，但**被考的回滚责任相同**。
 *   2. ~~`test_base.hpp` 的 `db_family_files()` 漏了 `xattrkeys.db`~~ —— **已修（2026-09-26）**：
 *      那个 helper 现在列全 **6** 个（pkgs/files/provides/confhashes/xattrkeys/holdpkgs）。
 *      本文件原先在 `db_state()` 里显式补了一行 `add_file(xattr_keys_db())`，helper 修好后
 *      已删（重复了）。保留这条记录，是因为它是个**反复发生**的坑：那个 helper 的注释自己
 *      预言过「加新库时几处硬编码清单会漏掉它」，而**预言并没有防住第二次**。
 */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
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
#include "../test_base.hpp"

namespace fs = std::filesystem;

namespace
{
constexpr const char* V1 = "1.0";
constexpr const char* V2 = "2.0";

/// 场景的路径基准（非 `/etc` 那一半）
constexpr const char* ULT = "usr/share/ult";

// ── /etc 的六个条目（三个包分摊）──────────────────────────────────────────────
constexpr const char* ETC_INSTALLNEW = "etc/ult-installnew.conf";    ///< 三哈希 ①
constexpr const char* ETC_KEPLOCAL = "etc/ult-keplocal.conf";        ///< 三哈希 ②
constexpr const char* ETC_SAVELPKGNEW = "etc/ult-savelpkgnew.conf";  ///< 三哈希 ③
constexpr const char* ETC_F2S = "etc/ult-f2s.conf";                  ///< 类型变化 file→link
constexpr const char* ETC_S2F = "etc/ult-s2f.conf";                  ///< 类型变化 link→file
constexpr const char* ETC_GONE = "etc/ult-gone.conf";                ///< 废弃

// ── 合租目录与它的 xattr 键 ───────────────────────────────────────────────────
constexpr const char* SHARED_DIR = "usr/share/ult/shared";
constexpr const char* KEEP = "user.k_keep";
constexpr const char* DROP = "user.k_drop";
constexpr const char* CHANGE = "user.k_change";
constexpr const char* NEWK = "user.k_new";
constexpr const char* BETAK = "user.k_beta";
}  // namespace

class UltimateMultiPkgTest : public IntegrationTestBase
{
protected:
    void SetUp() override
    {
        IntegrationTestBase::SetUp();
        setup_local_mirror();  // 空镜像：repo.load_index 快速且不联网
        BreakpointManager::instance().clear_all();
        // 刻意**不**改 `no_hooks_mode` / `force_overwrite_mode` 这类全局开关：它们是进程级
        // 的，而 `test_hygiene.hpp` 的 listener 只复位 `TriggerManager` 与 `sigint_graceful`
        // —— 在这里改了就会污染后面的套件。本文件的包都不带 `hooks/`，也不需要 force。
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        IntegrationTestBase::TearDown();
    }

    // ── 造现场的小工具 ──────────────────────────────────────────────────────

    static void write_file(const fs::path& p, const std::string& content)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }

    static std::string read_file(const fs::path& p)
    {
        std::ifstream f(p);
        if (!f.is_open()) return "<无法打开>";
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    static bool exists_l(const fs::path& p)
    {
        struct stat st{};
        return ::lstat(p.c_str(), &st) == 0;
    }

    /// 盘上形态（**lstat 语义**：符号链接算 symlink，不跟随）
    static std::string shape_of(const fs::path& p)
    {
        struct stat st{};
        if (::lstat(p.c_str(), &st) != 0) return "absent";
        if (S_ISLNK(st.st_mode)) return "symlink";
        if (S_ISDIR(st.st_mode)) return "dir";
        if (S_ISREG(st.st_mode)) return "file";
        return "other";
    }

    /// 设一个 `user.*` xattr；失败直接 FAIL（文件系统不支持时**不许静默跳过** —— 静默跳过
    /// 会让所有 xattr 断言变成恒真的摆设）
    static void set_xattr(const fs::path& p, const std::string& name, const std::string& value)
    {
        ASSERT_EQ(::lsetxattr(p.c_str(), name.c_str(), value.data(), value.size(), 0), 0)
            << "设 xattr 失败 " << p << " " << name << "：" << std::strerror(errno);
    }

    static std::optional<std::string> get_xattr(const fs::path& p, const std::string& name)
    {
        char buf[256];
        const ssize_t n = ::lgetxattr(p.c_str(), name.c_str(), buf, sizeof(buf));
        if (n < 0) return std::nullopt;
        return std::string(buf, static_cast<size_t>(n));
    }

    /// 打一个包：`content/` 由 fill 回调填（路径相对 `content/`）
    template <typename F>
    std::string pack(const std::string& name, const std::string& ver, F fill) const
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "_" + ver);
        fs::remove_all(work);
        fs::create_directories(work / "content");
        fill(work / "content");
        const std::string path = (pkg_dir / (name + "-" + ver + ".lpkg")).string();
        pack_package(path, work.string(), name, ver, {}, {}, "man " + name, {});
        return path;
    }

    // ── 三个包的场景（**唯一一处**描述"这个包发什么"的地方）─────────────────

    /**
     * alpha：同型三格 + 三哈希三条分支 + 合租目录的 xattr 所有者。
     *
     * `/etc` 三哈希的**原料**是刻意分开的（不是让三种情形碰运气）：
     *   · `ult-installnew.conf`：v1/v2 内容不同、**用户不改** ⇒ 盘上 == 记录 ⇒ ① 就地换新；
     *   · `ult-keplocal.conf`：v1/v2 内容**逐字相同**、用户改过 ⇒ ② 保留用户那份；
     *   · `ult-savelpkgnew.conf`：v1/v2 不同**且**用户改过 ⇒ ③ 落 `.lpkgnew`。
     */
    void fill_alpha(const std::string& ver, const fs::path& c) const
    {
        const bool v2 = (ver == V2);
        write_file(c / ULT / "same-file", v2 ? "a2\n" : "a1\n");
        fs::create_directories(c / ULT / "same-dir");
        ::chmod((c / ULT / "same-dir").c_str(), v2 ? 0700 : 0755);
        write_file(c / ULT / "same-dir" / "inner.txt", "alpha-same-dir\n");
        fs::create_directories((c / ULT / "same-link").parent_path());
        fs::create_symlink(v2 ? "a2" : "same-file", c / ULT / "same-link");

        // ── 合租目录：alpha 与 beta 都发它；xattr 是**逐键**叠加的 ──────────────
        fs::create_directories(c / ULT / "shared");
        write_file(c / ULT / "shared" / "from-alpha.txt", v2 ? "alpha-v2\n" : "alpha-v1\n");
        set_xattr(c / ULT / "shared", KEEP, "v1");                // 不变
        set_xattr(c / ULT / "shared", CHANGE, v2 ? "c2" : "c1");  // 改值
        if (v2) {
            set_xattr(c / ULT / "shared", NEWK, "new");  // 新增
            // v2 **不再声明** DROP ⇒ 升级时必须被撤销（alpha 是它唯一的属主）
        } else {
            set_xattr(c / ULT / "shared", DROP, "dropme");
        }

        write_file(c / ETC_INSTALLNEW, v2 ? "installnew-v2\n" : "installnew-v1\n");
        // ② 的原料：两个版本**逐字相同** —— "包没改这个配置"，判定才有资格保留用户那份
        write_file(c / ETC_KEPLOCAL, "keplocal-v1\n");
        write_file(c / ETC_SAVELPKGNEW, v2 ? "lpkgnew-v2\n" : "lpkgnew-v1\n");
    }

    /// beta：六向互转 + `/etc` 类型变化两格 + 合租目录上**自己的** xattr 键
    void fill_beta(const std::string& ver, const fs::path& c) const
    {
        const bool v2 = (ver == V2);

        // 六向互转。每格 v1/v2 形态相反，且**都**在 `ULT` 下（不是 `/etc`：那里另有政策）
        if (v2) {
            fs::create_directories(c / ULT / "f2d");
            write_file(c / ULT / "f2d" / "in.txt", "f2d-v2\n");
        } else {
            write_file(c / ULT / "f2d", "f2d-v1\n");
        }
        if (v2) {
            write_file(c / ULT / "d2f", "d2f-v2\n");
        } else {
            fs::create_directories(c / ULT / "d2f");
            write_file(c / ULT / "d2f" / "in.txt", "d2f-v1\n");
        }
        if (v2) {
            fs::create_symlink("f2d-v2-target", c / ULT / "f2s");
        } else {
            write_file(c / ULT / "f2s", "f2s-v1\n");
        }
        if (v2) {
            write_file(c / ULT / "s2f", "s2f-v2\n");
        } else {
            fs::create_symlink("s2f-v1-target", c / ULT / "s2f");
        }
        if (v2) {
            fs::create_symlink("d2s-v2-target", c / ULT / "d2s");
        } else {
            fs::create_directories(c / ULT / "d2s");
            write_file(c / ULT / "d2s" / "in.txt", "d2s-v1\n");
        }
        if (v2) {
            fs::create_directories(c / ULT / "s2d");
            write_file(c / ULT / "s2d" / "in.txt", "s2d-v2\n");
        } else {
            fs::create_symlink("s2d-v1-target", c / ULT / "s2d");
        }

        // 合租目录：beta 的文件 + **自己的** xattr 键（v1/v2 都声明 ⇒ 永不撤）
        fs::create_directories(c / ULT / "shared");
        write_file(c / ULT / "shared" / "from-beta.txt", "beta\n");
        set_xattr(c / ULT / "shared", BETAK, "b1");

        // `/etc` 的类型变化两格（现行政策：原物 `.lpkgsave` + 新物**就地**落位）
        if (v2) {
            // 造符号链接前**父目录必须先在**（create_symlink 的 ENOENT 说的是父目录，
            // 不是链接目标 —— 目标只是链接里存的那个字符串，不解析）
            fs::create_directories((c / ETC_F2S).parent_path());
            fs::create_symlink("f2s-conf-target", c / ETC_F2S);
            write_file(c / ETC_S2F, "s2f-conf-v2\n");
        } else {
            write_file(c / ETC_F2S, "f2s-conf-v1\n");
            fs::create_directories((c / ETC_S2F).parent_path());
            fs::create_symlink("s2f-conf-v1-target", c / ETC_S2F);
        }
    }

    /// gamma：废弃（文件 + 嵌套 ≥2 层的 owned 目录 + `/etc` 配置）与新增（文件 + 目录树）
    void fill_gamma(const std::string& ver, const fs::path& c) const
    {
        const bool v2 = (ver == V2);
        if (!v2) {
            write_file(c / ULT / "gone-file", "gone\n");
            // 嵌套 ≥2 层：升级后必须**整棵**消失（含中间目录），不能留空壳
            write_file(c / ULT / "gone-tree" / "l1" / "l2" / "leaf.txt", "leaf\n");
            write_file(c / ETC_GONE, "gone-conf\n");
        } else {
            write_file(c / ULT / "added-file", "added\n");
            write_file(c / ULT / "added-tree" / "a" / "b" / "leaf.txt", "added-leaf\n");
        }
    }

    // ── 版本无关的入口 ────────────────────────────────────────────────────────

    std::string alpha(const std::string& v) const
    {
        return pack("alpha", v, [&](const fs::path& c) { fill_alpha(v, c); });
    }
    std::string beta(const std::string& v) const
    {
        return pack("beta", v, [&](const fs::path& c) { fill_beta(v, c); });
    }
    std::string gamma(const std::string& v) const
    {
        return pack("gamma", v, [&](const fs::path& c) { fill_gamma(v, c); });
    }

    fs::path root(const std::string& rel) const
    {
        return test_root / rel;
    }

    /// 走真实入口，返回**实际错误消息**（成功返回空串）—— 失败时看得到 `what()`
    template <typename F>
    static std::string err_of(F&& action)
    {
        try {
            action();
        } catch (const LpkgException& e) {
            return e.what();
        } catch (const std::exception& e) {
            return std::string("（非 LpkgException）") + e.what();
        }
        return {};
    }

    static std::string owners_of(const std::string& logical)
    {
        std::string s;
        auto v = Cache::instance().get_file_owners(logical);
        std::vector<std::string> sorted(v.begin(), v.end());
        std::sort(sorted.begin(), sorted.end());
        for (const auto& o : sorted) s += o + ",";
        return s;
    }

    // ── 期望表（"这一版盘上该长什么样"）──────────────────────────────────────

    struct Expect {
        std::string path;    ///< 相对 test_root（用 std::string 才能拼出 `.lpkgsave` 这类派生名）
        std::string shape;   ///< file / dir / symlink / absent
        std::string detail;  ///< file=内容，symlink=链接目标，dir/absent=""
    };

    /// `/etc` 条目的**派生落点**（升级/移除会把原物改名成这两个后缀之一）
    static std::string kept(const char* etc_rel)
    {
        return std::string(etc_rel) + ".lpkgsave";
    }
    static std::string pending(const char* etc_rel)
    {
        return std::string(etc_rel) + ".lpkgnew";
    }

    /** 逐条断言；每条都带路径，失败时能一眼定位是哪一格 */
    void expect_tree(const std::vector<Expect>& rows, const std::string& what) const
    {
        for (const auto& r : rows) {
            const fs::path p = root(r.path);
            EXPECT_EQ(shape_of(p), r.shape) << what << "：路径 " << r.path;
            if (std::string(r.shape) == "file") {
                EXPECT_EQ(read_file(p), r.detail) << what << "：内容 " << r.path;
            } else if (std::string(r.shape) == "symlink") {
                std::error_code ec;
                EXPECT_EQ(fs::read_symlink(p, ec).string(), r.detail)
                    << what << "：链接目标 " << r.path;
            }
        }
    }

    /// v1 装完后的形状（三个包）
    std::vector<Expect> v1_tree() const
    {
        return {
            // alpha：同型三格的原形态
            {std::string(ULT) + "/same-file", "file", "a1\n"},
            {std::string(ULT) + "/same-dir", "dir", ""},
            {std::string(ULT) + "/same-dir/inner.txt", "file", "alpha-same-dir\n"},
            {std::string(ULT) + "/same-link", "symlink", "same-file"},
            {std::string(ULT) + "/shared/from-alpha.txt", "file", "alpha-v1\n"},
            {ETC_INSTALLNEW, "file", "installnew-v1\n"},
            {ETC_KEPLOCAL, "file", "keplocal-v1\n"},
            {ETC_SAVELPKGNEW, "file", "lpkgnew-v1\n"},
            // beta：六向互转的原形态 + 合租
            {std::string(ULT) + "/f2d", "file", "f2d-v1\n"},
            {std::string(ULT) + "/d2f", "dir", ""},
            {std::string(ULT) + "/d2f/in.txt", "file", "d2f-v1\n"},
            {std::string(ULT) + "/f2s", "file", "f2s-v1\n"},
            {std::string(ULT) + "/s2f", "symlink", "s2f-v1-target"},
            {std::string(ULT) + "/d2s", "dir", ""},
            {std::string(ULT) + "/d2s/in.txt", "file", "d2s-v1\n"},
            {std::string(ULT) + "/s2d", "symlink", "s2d-v1-target"},
            {std::string(ULT) + "/shared/from-beta.txt", "file", "beta\n"},
            {ETC_F2S, "file", "f2s-conf-v1\n"},
            {ETC_S2F, "symlink", "s2f-conf-v1-target"},
            // gamma：废弃与新增的原形态
            {std::string(ULT) + "/gone-file", "file", "gone\n"},
            {std::string(ULT) + "/gone-tree/l1/l2/leaf.txt", "file", "leaf\n"},
            {ETC_GONE, "file", "gone-conf\n"},
            {std::string(ULT) + "/added-file", "absent", ""},
            {std::string(ULT) + "/added-tree/a/b/leaf.txt", "absent", ""},
        };
    }

    /**
     * v2 升完后的形状 —— 这里就是"六向互转 + 废弃 + 新增 + `/etc` 三哈希"的**合计答案**。
     *
     * `/etc` 三种保留结果的落点按现行政策：
     *   · ① 用户没改过 → 就地换新，**不**留副本；
     *   · ② 包没改 + 用户改过 → 用户那份**原地**保留，**不**产生 `.lpkgnew`；
     *   · ③ 两者都改 → 用户那份原地保留 + 新内容落 `.lpkgnew`。
     * （用户改动由 `apply_user_edits()` 在 v1 装完之后施加，见那里的说明。）
     */
    std::vector<Expect> v2_tree() const
    {
        return {
            // alpha
            {std::string(ULT) + "/same-file", "file", "a2\n"},
            {std::string(ULT) + "/same-dir", "dir", ""},
            {std::string(ULT) + "/same-link", "symlink", "a2"},
            {std::string(ULT) + "/shared/from-alpha.txt", "file", "alpha-v2\n"},
            {ETC_INSTALLNEW, "file", "installnew-v2\n"},         // ① 就地换新
            {pending(ETC_INSTALLNEW), "absent", ""},             // ① 不留副本
            {ETC_KEPLOCAL, "file", "keplocal-user\n"},           // ② 用户那份原地
            {pending(ETC_KEPLOCAL), "absent", ""},               // ② 连副本都不产生
            {ETC_SAVELPKGNEW, "file", "lpkgnew-user\n"},         // ③ 用户那份原地
            {pending(ETC_SAVELPKGNEW), "file", "lpkgnew-v2\n"},  // ③ 新内容待审
            // beta：六向互转（每格的终态与 v1 相反）
            {std::string(ULT) + "/f2d", "dir", ""},
            {std::string(ULT) + "/f2d/in.txt", "file", "f2d-v2\n"},
            {std::string(ULT) + "/d2f", "file", "d2f-v2\n"},
            {std::string(ULT) + "/f2s", "symlink", "f2d-v2-target"},
            {std::string(ULT) + "/s2f", "file", "s2f-v2\n"},
            {std::string(ULT) + "/d2s", "symlink", "d2s-v2-target"},
            {std::string(ULT) + "/s2d", "dir", ""},
            {std::string(ULT) + "/s2d/in.txt", "file", "s2d-v2\n"},
            {std::string(ULT) + "/shared/from-beta.txt", "file", "beta\n"},
            // `/etc` 类型变化：原物 `.lpkgsave` + 新物**就地**
            {ETC_F2S, "symlink", "f2s-conf-target"},
            {kept(ETC_F2S), "file", "f2s-conf-v1\n"},
            {ETC_S2F, "file", "s2f-conf-v2\n"},
            {kept(ETC_S2F), "symlink", "s2f-conf-v1-target"},
            // gamma：废弃整棵消失 + 新增落地
            {std::string(ULT) + "/gone-file", "absent", ""},
            {std::string(ULT) + "/gone-tree", "absent", ""},  // 中间目录也不许留空壳
            {ETC_GONE, "absent", ""},
            {kept(ETC_GONE), "file", "gone-conf\n"},  // 废弃 `/etc` → `.lpkgsave`
            {std::string(ULT) + "/added-file", "file", "added\n"},
            {std::string(ULT) + "/added-tree/a/b/leaf.txt", "file", "added-leaf\n"},
        };
    }

    /**
     * **施加用户改动之后**的 v1 形状 —— 回滚类用例的基线就该是它。
     *
     * `v1_tree()` 描述的是"刚装完 v1"那一瞬；而回滚用例的基线是
     * `install_v1_and_edit()` 之后（两份配置已被用户改过）—— 用 `v1_tree()` 当基线会红在
     * "盘上是 keplocal-user，期望 keplocal-v1"上，那是我把**基线**写错了。
     */
    std::vector<Expect> v1_tree_edited() const
    {
        auto t = v1_tree();
        for (auto& r : t) {
            if (r.path == ETC_KEPLOCAL) r.detail = "keplocal-user\n";
            if (r.path == ETC_SAVELPKGNEW) r.detail = "lpkgnew-user\n";
        }
        return t;
    }

    /** 施加"用户改动"（v1 装完之后调用：此刻盘上 == 记录，改完就是"用户改过"） */
    void apply_user_edits() const
    {
        // ① 的那份**不动**（否则就考不到"用户没改 → 就地换新"）
        write_file(root(ETC_KEPLOCAL), "keplocal-user\n");    // ② 用户改过
        write_file(root(ETC_SAVELPKGNEW), "lpkgnew-user\n");  // ③ 用户改过
    }

    // ── 快照（含 xattr 与 xattrkeys.db —— 见文件头"缺口 2"）──────────────────

    static std::string xattr_fingerprint(const fs::path& p)
    {
        const ssize_t len = ::llistxattr(p.c_str(), nullptr, 0);
        if (len <= 0) return "";
        std::vector<char> names(static_cast<size_t>(len));
        if (::llistxattr(p.c_str(), names.data(), names.size()) != len) return "";
        std::vector<std::string> lines;
        for (const char* n = names.data(); n < names.data() + len; n += std::strlen(n) + 1) {
            if (*n == '\0') continue;
            char v[256];
            const ssize_t vn = ::lgetxattr(p.c_str(), n, v, sizeof(v));
            lines.push_back(std::string(n) + "=" +
                            (vn < 0 ? "<读失败>" : std::string(v, static_cast<size_t>(vn))));
        }
        std::sort(lines.begin(), lines.end());
        std::string out;
        for (const auto& l : lines) out += " x[" + l + "]";
        return out;
    }

    /// 文件系统快照：相对路径 → 形态/权限/目标/内容哈希/属主/xattr
    std::map<std::string, std::string> fs_state() const
    {
        std::map<std::string, std::string> out;
        std::error_code ec;
        // **排除 lpkg 自己的状态目录**（`state_dir()`：WAL / transaction.log / db_bak /
        // 各库与 man 备份都在里面）。理由与 `test_upgrade_rollback_fidelity.cpp` 的"WAL 按
        // 约定不做字节比较"同源：那是**记账**不是**被管理的系统镜像**，而它的**内容**由
        // `db_state()` 逐字节钉住。不排除的话，"拒绝时盘面一字未动"会被
        // `var/lib/lpkg/transaction.log` 的创建/改写误判成"盘面动了"。
        const fs::path state_dir = Config::instance().state_dir();
        for (auto it = fs::recursive_directory_iterator(test_root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const fs::path p = it->path();
            if (p == state_dir || p.string().starts_with(state_dir.string() + "/")) continue;
            const std::string rel = p.lexically_relative(test_root).generic_string();
            struct stat st{};
            if (::lstat(p.c_str(), &st) != 0) {
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
                fp << "other";
            }
            fp << std::dec << " uid=" << static_cast<unsigned long long>(st.st_uid)
               << " gid=" << static_cast<unsigned long long>(st.st_gid) << xattr_fingerprint(p);
            out[rel] = fp.str();
        }
        return out;
    }

    /**
     * DB 快照：**逐字节**。清单取自 `test_base.hpp::db_family_files()`（**6** 个 =
     * `Cache::write(milestone)` 真正写的整族（见文件头 "缺口 2"）。另加 essential 与
     * deps/needed_so/docs 三个目录。
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

    static void expect_same_snapshot(const std::map<std::string, std::string>& before,
                                     const std::map<std::string, std::string>& after,
                                     const std::string& what)
    {
        std::set<std::string> keys;
        for (const auto& [k, v] : before) keys.insert(k);
        for (const auto& [k, v] : after) keys.insert(k);
        for (const auto& k : keys) {
            const auto ib = before.find(k);
            const auto ia = after.find(k);
            const std::string b = ib == before.end() ? "<不存在>" : ib->second;
            const std::string a = ia == after.end() ? "<不存在>" : ia->second;
            EXPECT_EQ(b, a) << what << "：路径 " << k << "（改动前 → 回滚后）";
        }
    }

    /// WAL 里有没有 `BEGIN_PKGS`（"连事务都没进"的取证）
    bool wal_has_begin() const
    {
        const std::string t = read_file(wal::wal_log_path());
        return t.find("BEGIN_PKGS") != std::string::npos;
    }

    /// `.lpkg_bak*` / `.lpkgtmp` 残留计数（回滚后必须为 0）
    std::string residue() const
    {
        int bak = 0, tmp = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(test_root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const std::string n = it->path().filename().string();
            if (n.find(".lpkg_bak") != std::string::npos) ++bak;
            if (n.find(".lpkgtmp") != std::string::npos) ++tmp;
        }
        return "bak=" + std::to_string(bak) + " tmp=" + std::to_string(tmp);
    }

    /// 把三个包的 v1 装好并施加用户改动（多个用例共用的起点）
    void install_v1_and_edit() const
    {
        ASSERT_NO_THROW(install_packages({alpha(V1), beta(V1), gamma(V1)}));
        apply_user_edits();
    }
};

// ============================================================================
// ① 安装：一个批次装 3 个包，盘面 == v1 形态
// ============================================================================

TEST_F(UltimateMultiPkgTest, InstallThreePackagesInOneBatchLandsV1Shape)
{
    ASSERT_NO_THROW(install_packages({alpha(V1), beta(V1), gamma(V1)}));

    expect_tree(v1_tree(), "① 三包一批装完");
    for (const char* p : {"alpha", "beta", "gamma"}) {
        EXPECT_EQ(Cache::instance().get_installed_version(p), V1) << p;
    }

    // 归属：合租目录被**两个**包同时持有（这正是"卸载 A 时目录必须活着"的由来）
    EXPECT_EQ(owners_of("/" + std::string(ULT) + "/shared/"), "alpha,beta,");
    EXPECT_EQ(owners_of("/" + std::string(ULT) + "/shared/from-alpha.txt"), "alpha,");
    EXPECT_EQ(owners_of("/" + std::string(ULT) + "/shared/from-beta.txt"), "beta,");

    // xattr 归属：各包只登记**自己声明**的键 —— 这是"撤销不碰别人"的判据来源
    const std::string dir_key = "/" + std::string(ULT) + "/shared/";
    EXPECT_EQ(Cache::instance().get_xattr_key_owners(dir_key, DROP).count("alpha"), 1u);
    EXPECT_EQ(Cache::instance().get_xattr_key_owners(dir_key, BETAK).count("beta"), 1u);
    EXPECT_EQ(Cache::instance().get_xattr_key_owners(dir_key, BETAK).count("alpha"), 0u)
        << "alpha 没声明 k_beta，却把它登记成自己的 —— 撤销会去删别人的键";

    // 盘上确实有这些键（否则上面的归属断言是空转）
    EXPECT_EQ(get_xattr(root(ULT) / "shared", DROP), std::optional<std::string>("dropme"));
    EXPECT_EQ(get_xattr(root(ULT) / "shared", BETAK), std::optional<std::string>("b1"));

    EXPECT_EQ(residue(), "bak=0 tmp=0");
}

// ============================================================================
// ② 升级：一个批次升 3 个包 —— 六向互转 + 废弃 + 新增 + `/etc` 三哈希 + xattr 撤销
// ============================================================================

TEST_F(UltimateMultiPkgTest, UpgradeThreePackagesInOneBatchAppliesEveryTransition)
{
    install_v1_and_edit();
    ASSERT_EQ(shape_of(root(ETC_KEPLOCAL)), "file");

    ASSERT_NO_THROW(install_packages({alpha(V2), beta(V2), gamma(V2)}));

    expect_tree(v2_tree(), "② 三包一批升级");
    for (const char* p : {"alpha", "beta", "gamma"}) {
        EXPECT_EQ(Cache::instance().get_installed_version(p), V2) << p;
    }

    // ── xattr：改值 / 新增 / **撤销** / 不碰别人的 ────────────────────────────
    const fs::path shared = root(ULT) / "shared";
    EXPECT_EQ(get_xattr(shared, KEEP), std::optional<std::string>("v1")) << "不该被动的键被动了";
    EXPECT_EQ(get_xattr(shared, CHANGE), std::optional<std::string>("c2")) << "键值没被更新";
    EXPECT_EQ(get_xattr(shared, NEWK), std::optional<std::string>("new")) << "新声明的键没落地";
    EXPECT_EQ(get_xattr(shared, DROP), std::nullopt)
        << "新版本不再声明的键**必须被撤**（陈旧的 posix_acl_default 会继续决定继承权限）";
    EXPECT_EQ(get_xattr(shared, BETAK), std::optional<std::string>("b1"))
        << "beta 的键被 alpha 的撤销趟删了 —— 只许动本包持有的键";

    const std::string dir_key = "/" + std::string(ULT) + "/shared/";
    EXPECT_TRUE(Cache::instance().get_xattr_key_owners(dir_key, DROP).empty())
        << "键被撤了，归属记录却还在（下次撤销会对着一个不存在的键写 WAL 行）";
    EXPECT_EQ(Cache::instance().get_xattr_key_owners(dir_key, NEWK).count("alpha"), 1u);
    EXPECT_EQ(Cache::instance().get_xattr_key_owners(dir_key, BETAK).count("beta"), 1u);

    // ── 合租目录本身仍在（两个包都还持有）────────────────────────────────────
    EXPECT_EQ(owners_of("/" + std::string(ULT) + "/shared/"), "alpha,beta,");
    EXPECT_EQ(residue(), "bak=0 tmp=0");
}

// ============================================================================
// ③ 卸载 alpha：合租目录与 beta 的键都必须活下来
// ============================================================================

TEST_F(UltimateMultiPkgTest, RemoveAlphaKeepsSharedDirAndBetasXattrKey)
{
    install_v1_and_edit();
    ASSERT_NO_THROW(install_packages({alpha(V2), beta(V2), gamma(V2)}));

    ASSERT_NO_THROW(remove_packages({"alpha"}));

    EXPECT_EQ(Cache::instance().get_installed_version("alpha"), "") << "alpha 还在已装表里";
    // alpha 自己的文件没了
    EXPECT_EQ(shape_of(root(ULT) / "same-file"), "absent");
    EXPECT_EQ(shape_of(root(ULT) / "shared" / "from-alpha.txt"), "absent");
    // **合租目录必须活着**（beta 还持有），且 beta 的文件一字不动
    EXPECT_EQ(shape_of(root(ULT) / "shared"), "dir");
    EXPECT_EQ(read_file(root(ULT) / "shared" / "from-beta.txt"), "beta\n");
    EXPECT_EQ(owners_of("/" + std::string(ULT) + "/shared/"), "beta,");
    // beta 的 xattr 键不许跟着 alpha 一起走
    EXPECT_EQ(get_xattr(root(ULT) / "shared", BETAK), std::optional<std::string>("b1"));
    // alpha 自己声明的键要撤（它是唯一属主）
    EXPECT_EQ(get_xattr(root(ULT) / "shared", KEEP), std::nullopt);
    EXPECT_EQ(get_xattr(root(ULT) / "shared", NEWK), std::nullopt);
    EXPECT_TRUE(Cache::instance().get_package_xattr_keys("alpha").empty())
        << "alpha 卸载后 xattrkeys.db 里还留着它的登记";

    // alpha 的 `/etc` 配置按政策改名保留（默认不 purge）—— 内容逐字节在
    EXPECT_EQ(shape_of(root(kept(ETC_INSTALLNEW))), "file");
    EXPECT_EQ(read_file(root(kept(ETC_INSTALLNEW))), "installnew-v2\n");
    EXPECT_EQ(shape_of(root(ETC_INSTALLNEW)), "absent") << "原名不该还在（它已不归任何包）";

    // beta / gamma 完全没被牵连
    EXPECT_EQ(Cache::instance().get_installed_version("beta"), V2);
    EXPECT_EQ(Cache::instance().get_installed_version("gamma"), V2);
    EXPECT_EQ(residue(), "bak=0 tmp=0");
}

// ============================================================================
// ④ 卸载全部三个：只剩 `/etc` 政策允许保留的东西
// ============================================================================

TEST_F(UltimateMultiPkgTest, RemoveAllThreeLeavesOnlyEtcPolicyLeftovers)
{
    install_v1_and_edit();
    ASSERT_NO_THROW(install_packages({alpha(V2), beta(V2), gamma(V2)}));

    ASSERT_NO_THROW(remove_packages({"alpha", "beta", "gamma"}));

    for (const char* p : {"alpha", "beta", "gamma"}) {
        EXPECT_EQ(Cache::instance().get_installed_version(p), "") << p;
    }
    // 本包触碰面里的一切都不该留下（连合租目录也走了：无人持有 + 空）。
    // **只断言 `usr/share/ult`**：`usr/share` 本身是 lpkg 会保留的祖先/共享目录，
    // 断言它消失是把"清理祖先"当成契约（那是错的 —— 祖先可能被别的包或发行版自己用着）。
    EXPECT_EQ(shape_of(root(ULT)), "absent") << ULT;

    // `/etc`：被移除的配置都改名保留，内容逐字节在。
    // 注意 gamma 那份是**升级时**就已经改名的（v2 不再提供 ⇒ 废弃 ⇒ `.lpkgsave` + 撤所有权），
    // 所以移除趟不会再给它加一层后缀 —— 我第一版期望 `.lpkgsave.lpkgsave` 是错的。
    for (const auto& [kept_path, content] : std::vector<std::pair<std::string, std::string>>{
             {std::string(ETC_INSTALLNEW) + ".lpkgsave", "installnew-v2\n"},
             {std::string(ETC_KEPLOCAL) + ".lpkgsave", "keplocal-user\n"},
             {std::string(ETC_SAVELPKGNEW) + ".lpkgsave", "lpkgnew-user\n"},
             {std::string(ETC_GONE) + ".lpkgsave", "gone-conf\n"}}) {
        EXPECT_EQ(shape_of(root(kept_path)), "file") << kept_path;
        EXPECT_EQ(read_file(root(kept_path)), content) << kept_path;
    }
    // 类型变化那两格升级时已经留过一份 `.lpkgsave`；移除时原位那份再改名（`.lpkgsave.1`）
    EXPECT_EQ(shape_of(root(kept(ETC_F2S))), "symlink");
    EXPECT_EQ(shape_of(root(kept(ETC_S2F))), "file");

    EXPECT_EQ(residue(), "bak=0 tmp=0");
    for (const char* p : {"alpha", "beta", "gamma"}) {
        EXPECT_TRUE(Cache::instance().get_package_xattr_keys(p).empty()) << p;
    }
}

// ============================================================================
// ⑤ 回滚：安装批次（让开趟注入）
// ============================================================================

TEST_F(UltimateMultiPkgTest, InstallBatchRollsBackWhenFailureHitsInsideTheTransaction)
{
    // **安装**批次（不是升级）：沙盒里先只有 gamma v1，alpha/beta 是全新安装。
    // 断点取 `install_after_begin_<pkg>`（WAL `BEGIN_PKGS` 之后立刻）—— 这是"注册"那条
    // 路径上最靠前的窗口：此时批次刚进事务，什么都没落地，回滚必须把整批退回原样。
    //
    // ⚠️ 注意：**全新安装批次里 `backup_after_wal_<pkg>` 不会命中** —— 让开趟只在"盘面被
    // 占、需要搬走"时调 `backup`（新安装的路径本来就不存在 ⇒ 走 `RegisterNew`/`MakeDir`）。
    // 让开趟的注入因此放在**升级**批次上（见 `UpgradeRollsBackWhenLetGoPassFails`）。
    ASSERT_NO_THROW(install_packages({gamma(V1)}));
    const auto fs_before = fs_state();
    const auto db_before = db_state();

    bool hit = false;
    std::string alpha_etc_at_bp;
    BreakpointManager::instance().set("install_after_begin_beta", [&] {
        hit = true;
        // 断点时刻 ≠ 基线的取证：alpha 的 `/etc` 尚未落地（beta 在 alpha 之后才开始）
        // —— 这里抓的是"alpha 的条目已经在解压/准备阶段动过"，见下面的断言说明
        alpha_etc_at_bp = shape_of(root(ETC_INSTALLNEW)) + "/" + shape_of(root(ULT) / "same-file");
        throw LpkgException("injected: 注册路径上失败");
    });
    EXPECT_THROW(install_packages({alpha(V1), beta(V1)}), LpkgException);
    BreakpointManager::instance().clear_all();

    ASSERT_TRUE(hit) << "断点没命中：本用例没考到「安装批次中途失败」";
    // 回滚后：只剩 gamma v1 那一份（批次前的状态），alpha/beta 一个字节都不许留下
    EXPECT_EQ(shape_of(root(ETC_INSTALLNEW)), "absent");
    EXPECT_EQ(shape_of(root(ULT) / "same-file"), "absent");
    EXPECT_EQ(Cache::instance().get_installed_version("alpha"), "");
    EXPECT_EQ(Cache::instance().get_installed_version("beta"), "");
    EXPECT_EQ(Cache::instance().get_installed_version("gamma"), V1);
    expect_same_snapshot(fs_before, fs_state(), "⑤ 文件系统（应只剩 gamma v1）");
    expect_same_snapshot(db_before, db_state(), "⑤ DB 一族（含 xattrkeys.db）");
    EXPECT_EQ(residue(), "bak=0 tmp=0");
}

// ============================================================================
// ⑤b 回滚：**升级**批次的让开趟中途失败
//      （新安装批次里 `backup` 根本不发生，所以让开趟的注入必须落在升级上）
// ============================================================================

TEST_F(UltimateMultiPkgTest, UpgradeRollsBackWhenLetGoPassFails)
{
    install_v1_and_edit();
    const auto fs_before = fs_state();
    const auto db_before = db_state();

    bool hit = false;
    std::map<std::string, std::string> mid;
    // `backup_after_wal_beta`：beta 的 v1 文件已搬进 stash（WAL `BACKUP` 已落）、
    // 紧接的 rename 未做 —— 让开趟的 write-ahead 窗口。
    BreakpointManager::instance().set("backup_after_wal_beta", [&] {
        hit = true;
        // 此刻的"已动过盘"证据：批内 alpha 跑在 beta 之前，alpha 的 `/etc` ① 分支
        // （用户没改 → 就地静默换新）已经落盘 —— 所以"回滚 == 基线"不是恒真。
        // （**别**拿 `Cache::get_installed_version()` 当"DB 还没提交"的判据：Cache 是**内存**
        //   表，批内逐包注册时它就已经是 v2 了，提交是 COMMIT_PKGS 那一步的事。）
        mid["alpha_etc_now"] = read_file(root(ETC_INSTALLNEW));
        throw LpkgException("injected: 升级的让开趟中途失败");
    });
    EXPECT_THROW(install_packages({alpha(V2), beta(V2), gamma(V2)}), LpkgException);
    BreakpointManager::instance().clear_all();

    ASSERT_TRUE(hit) << "断点没命中：本用例没考到「让开趟中途失败」";
    EXPECT_EQ(mid["alpha_etc_now"], "installnew-v2\n")
        << "断点时刻盘面与基线相同 ⇒ 这个用例没考到任何东西（回滚断言是恒真的）";
    expect_tree(v1_tree_edited(), "⑤b 升级批次回滚后（应逐项回到改动后的 v1）");
    expect_same_snapshot(fs_before, fs_state(), "⑤b 文件系统");
    expect_same_snapshot(db_before, db_state(), "⑤b DB 一族（含 xattrkeys.db）");
    EXPECT_EQ(residue(), "bak=0 tmp=0");
}

// ============================================================================
// ⑥ 回滚：升级批次（写入趟注入）—— **废弃清除已经发生**才失败
// ============================================================================

TEST_F(UltimateMultiPkgTest, UpgradeRollsBackObsoleteRemovalAlreadyDone)
{
    install_v1_and_edit();
    const auto fs_before = fs_state();
    const auto db_before = db_state();

    bool hit = false;
    std::map<std::string, std::string> mid;
    // 断点取在**写入趟**：它跑在废弃清除（让开趟 ②b `remove_obsolete_files()`）**之后** ——
    // 所以此刻 gamma 的废弃痕迹（文件与整棵嵌套目录）**已经不在盘上**。
    // 这正是"废弃清除已发生、随后失败"的取证：回滚必须把它们整棵还回来。
    // （`REMOVE_OLD` 自身没有断点，见文件头"缺口 1"——所以用"其后的窗口"代替。）
    // 断点取 **gamma 自己**的写入趟：批内是**逐包**跑完"让开(②a+②b) → 写入(③) → 注册(④)"
    // 的，所以 gamma 的 `copy_after_wal_gamma` 一定发生在 **gamma 自己的废弃清除之后**
    // —— 取证只依赖 gamma 自己的事实，与批内包序无关（取 alpha 的写入趟就不成立：
    // 那一刻 gamma 的让开趟可能还没跑）。
    BreakpointManager::instance().set("copy_after_wal_gamma", [&] {
        hit = true;
        mid["gone_file"] = shape_of(root(ULT) / "gone-file");
        mid["gone_tree"] = shape_of(root(ULT) / "gone-tree");
        mid["gone_conf_new_name"] = shape_of(root(kept(ETC_GONE)));
        mid["added_file"] = shape_of(root(ULT) / "added-file");
        mid["v1_ver"] = Cache::instance().get_installed_version("gamma");
        throw LpkgException("injected: 写入趟中途失败（废弃清除已发生）");
    });
    EXPECT_THROW(install_packages({alpha(V2), beta(V2), gamma(V2)}), LpkgException);
    BreakpointManager::instance().clear_all();

    EXPECT_TRUE(hit) << "断点没命中：本用例没考到「升级中途失败」";
    EXPECT_EQ(mid["gone_file"], "absent") << "废弃文件此刻还在 ⇒ 没考到「废弃清除已发生」";
    EXPECT_EQ(mid["gone_tree"], "absent") << "废弃的嵌套目录此刻还在 ⇒ 同上";
    EXPECT_EQ(mid["gone_conf_new_name"], "file")
        << "废弃的 /etc 配置应已被改名成 `.lpkgsave`（没改名 ⇒ 废弃清除没跑到）";
    EXPECT_EQ(mid["added_file"], "absent")
        << "gamma 的 v2 新内容不该已经落地（断点卡在它的 COPY 窗口）";
    EXPECT_EQ(mid["v1_ver"], V1) << "DB 还没提交，版本不该变";

    // 回滚：废弃的整棵树（含中间目录）、`/etc` 配置、xattr 都要逐字节回来
    expect_tree(v1_tree_edited(), "⑥ 升级批次回滚后（应逐项回到改动后的 v1）");
    EXPECT_EQ(get_xattr(root(ULT) / "shared", DROP), std::optional<std::string>("dropme"))
        << "回滚没把被撤的键还回来";
    EXPECT_EQ(get_xattr(root(ULT) / "shared", NEWK), std::nullopt) << "回滚后多出了升级才声明的键";
    EXPECT_EQ(get_xattr(root(ULT) / "shared", CHANGE), std::optional<std::string>("c1"))
        << "被改的键值没回到批次前";
    expect_same_snapshot(fs_before, fs_state(), "⑥ 文件系统");
    expect_same_snapshot(db_before, db_state(), "⑥ DB 一族（含 xattrkeys.db）");
    EXPECT_EQ(residue(), "bak=0 tmp=0");
}

// ============================================================================
// ⑦⑧ 回滚：两个"新机制"自己的 write-ahead 窗口
//      （`DIR_META` 与 `XATTR_*` 的 WAL 行已落、动作未做）
// ============================================================================

TEST_F(UltimateMultiPkgTest, UpgradeRollsBackWhenDirMetaWindowFails)
{
    install_v1_and_edit();
    const auto fs_before = fs_state();
    const auto db_before = db_state();

    bool hit = false;
    std::string mode_at_bp;
    BreakpointManager::instance().set("dirmeta_after_wal_alpha", [&] {
        hit = true;
        struct stat st{};
        ::lstat((root(ULT) / "same-dir").c_str(), &st);
        mode_at_bp = std::to_string(static_cast<int>(st.st_mode & 07777));
        throw LpkgException("injected: DIR_META 行已落、lchown/chmod 未做");
    });
    EXPECT_THROW(install_packages({alpha(V2), beta(V2), gamma(V2)}), LpkgException);
    BreakpointManager::instance().clear_all();

    ASSERT_TRUE(hit) << "断点没命中";
    EXPECT_EQ(mode_at_bp, "493") << "断点时刻目录已是新 mode（0755=493）⇒ 没考到「元数据已改」";
    // 回滚后 mode 回到 v1 的 0755
    struct stat st{};
    ASSERT_EQ(::lstat((root(ULT) / "same-dir").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0755u) << "回滚没把目录 mode 还原（就地改活对象的老问题）";
    expect_same_snapshot(fs_before, fs_state(), "⑦ 文件系统");
    expect_same_snapshot(db_before, db_state(), "⑦ DB 一族");
}

TEST_F(UltimateMultiPkgTest, UpgradeRollsBackWhenXattrRevokeWindowFails)
{
    install_v1_and_edit();
    const auto fs_before = fs_state();
    const auto db_before = db_state();

    bool hit = false;
    std::optional<std::string> drop_at_bp;
    BreakpointManager::instance().set("xattrrm_after_wal_alpha", [&] {
        hit = true;
        drop_at_bp = get_xattr(root(ULT) / "shared", DROP);
        throw LpkgException("injected: XATTR 撤销行已落、lremovexattr 未做");
    });
    EXPECT_THROW(install_packages({alpha(V2), beta(V2), gamma(V2)}), LpkgException);
    BreakpointManager::instance().clear_all();

    ASSERT_TRUE(hit) << "断点没命中：撤销趟没走到（k_drop 的撤销窗口没被注入）";
    EXPECT_EQ(drop_at_bp, std::optional<std::string>("dropme"))
        << "断点时刻键已经没了 ⇒ 没考到「WAL 已写、lremovexattr 未做」这个窗口";
    EXPECT_EQ(get_xattr(root(ULT) / "shared", DROP), std::optional<std::string>("dropme"))
        << "回滚没把键还回来";
    expect_same_snapshot(fs_before, fs_state(), "⑧ 文件系统");
    expect_same_snapshot(db_before, db_state(), "⑧ DB 一族（含 xattrkeys.db）");
}

// ============================================================================
// ⑨ 回滚：注册之后（事务内、DB 里程碑之后）失败
// ============================================================================

TEST_F(UltimateMultiPkgTest, UpgradeRollsBackWhenFailureComesAfterRegistration)
{
    install_v1_and_edit();
    const auto fs_before = fs_state();
    const auto db_before = db_state();

    bool hit = false;
    std::string gamma_ver_at_bp;
    BreakpointManager::instance().set("install_after_begin_gamma", [&] {
        hit = true;
        gamma_ver_at_bp = Cache::instance().get_installed_version("gamma");
        throw LpkgException("injected: 注册之后失败");
    });
    EXPECT_THROW(install_packages({alpha(V2), beta(V2), gamma(V2)}), LpkgException);
    BreakpointManager::instance().clear_all();

    ASSERT_TRUE(hit) << "断点没命中";
    // 这个断点在 WAL BEGIN 之后立刻命中 ⇒ alpha/beta 的让开趟还没跑；DB 未提交
    EXPECT_EQ(gamma_ver_at_bp, V1) << "DB 不该已经变成 v2";
    expect_tree(v1_tree_edited(), "⑨ 回滚后（应逐项回到改动后的 v1）");
    expect_same_snapshot(fs_before, fs_state(), "⑨ 文件系统");
    expect_same_snapshot(db_before, db_state(), "⑨ DB 一族（含 xattrkeys.db）");
}

// ============================================================================
// ⑩⑪ 回滚：卸载侧的两个窗口
// ============================================================================

TEST_F(UltimateMultiPkgTest, RemoveBatchRollsBackWhenBackupWindowFails)
{
    install_v1_and_edit();
    ASSERT_NO_THROW(install_packages({alpha(V2), beta(V2), gamma(V2)}));
    const auto fs_before = fs_state();
    const auto db_before = db_state();

    bool hit = false;
    std::string alpha_bin_at_bp;
    // 移除侧：某个包的旧文件已搬进 stash（WAL `BACKUP` 已写）、后续步骤未做
    BreakpointManager::instance().set("rm_backup_after_wal_gamma", [&] {
        hit = true;
        // 该断点是"`BACKUP` 行已写、rename **未做**" ⇒ gamma 自己的那份**还在原位**；
        // 真正"已经动过盘"的证据是**批内前面的包**（alpha 的文件）已经被搬走了。
        alpha_bin_at_bp =
            shape_of(root(ULT) / "same-file") + "|" + shape_of(root(ULT) / "added-file");
        throw LpkgException("injected: 移除侧 BACKUP 行已落、后续未做");
    });
    EXPECT_THROW(remove_packages({"alpha", "beta", "gamma"}), LpkgException);
    BreakpointManager::instance().clear_all();

    ASSERT_TRUE(hit) << "断点没命中：本用例没考到「移除批次中途失败」";
    EXPECT_EQ(alpha_bin_at_bp, "absent|file")
        << "断点时刻 alpha 的文件该已不在、gamma 的该还在原位（`rm_backup_*` 是 rename 之前的"
           "窗口）—— 不满足 ⇒ 这个用例没考到「已经搬走一部分」";
    for (const char* p : {"alpha", "beta", "gamma"}) {
        EXPECT_EQ(Cache::instance().get_installed_version(p), V2) << "回滚后 " << p << " 应仍是 v2";
    }
    expect_tree(v2_tree(), "⑩ 移除批次回滚后（应完整回到 v2 形态）");
    EXPECT_EQ(get_xattr(root(ULT) / "shared", CHANGE), std::optional<std::string>("c2"));
    expect_same_snapshot(fs_before, fs_state(), "⑩ 文件系统");
    expect_same_snapshot(db_before, db_state(), "⑩ DB 一族（含 xattrkeys.db）");
    EXPECT_EQ(residue(), "bak=0 tmp=0");
}

TEST_F(UltimateMultiPkgTest, RemoveBatchRollsBackWhenConfSaveWindowFails)
{
    install_v1_and_edit();
    ASSERT_NO_THROW(install_packages({alpha(V2), beta(V2), gamma(V2)}));
    const auto fs_before = fs_state();
    const auto db_before = db_state();

    bool hit = false;
    std::string conf_at_bp;
    // 移除侧：`/etc` 配置改名成 `.lpkgsave` 的 write-ahead 窗口
    BreakpointManager::instance().set("rm_save_conf_after_wal_alpha", [&] {
        hit = true;
        conf_at_bp = shape_of(root(ETC_INSTALLNEW));
        throw LpkgException("injected: 移除侧 SAVE_CONF 行已落、rename 未做");
    });
    EXPECT_THROW(remove_packages({"alpha"}), LpkgException);
    BreakpointManager::instance().clear_all();

    ASSERT_TRUE(hit) << "断点没命中";
    EXPECT_EQ(conf_at_bp, "file") << "断点时刻原名已不在 ⇒ 没考到「WAL 已写、rename 未做」这个窗口";
    EXPECT_EQ(Cache::instance().get_installed_version("alpha"), V2);
    expect_tree(v2_tree(), "⑪ 移除回滚后（alpha 也应在）");
    expect_same_snapshot(fs_before, fs_state(), "⑪ 文件系统");
    expect_same_snapshot(db_before, db_state(), "⑪ DB 一族（含 xattrkeys.db）");
}

TEST_F(UltimateMultiPkgTest, RemoveBatchRollsBackWhenFailureComesBeforeCommit)
{
    install_v1_and_edit();
    ASSERT_NO_THROW(install_packages({alpha(V2), beta(V2), gamma(V2)}));
    const auto fs_before = fs_state();
    const auto db_before = db_state();

    bool hit = false;
    std::string tree_at_bp;
    // 移除批次：所有文件都删完了、COMMIT 之前失败（这是最难回滚的位置）
    BreakpointManager::instance().set("remove_batch_before_commit", [&] {
        hit = true;
        tree_at_bp = shape_of(root(ULT) / "same-file") + "/" + shape_of(root(ULT) / "shared");
        throw LpkgException("injected: 移除批次的文件都删完了、COMMIT 之前");
    });
    EXPECT_THROW(remove_packages({"alpha", "beta", "gamma"}), LpkgException);
    BreakpointManager::instance().clear_all();

    ASSERT_TRUE(hit) << "断点没命中";
    EXPECT_NE(tree_at_bp, "file/dir")
        << "断点时刻盘面与基线相同 ⇒ 没考到「文件都已删完、只差提交」";
    for (const char* p : {"alpha", "beta", "gamma"}) {
        EXPECT_EQ(Cache::instance().get_installed_version(p), V2) << p;
    }
    expect_tree(v2_tree(), "⑫ 移除批次回滚后");
    expect_same_snapshot(fs_before, fs_state(), "⑫ 文件系统");
    expect_same_snapshot(db_before, db_state(), "⑫ DB 一族（含 xattrkeys.db）");
    EXPECT_EQ(residue(), "bak=0 tmp=0");
}

// ============================================================================
// ⑬⑭ 跨包类型争用：拒绝时"盘面一字未动 + 报错点名持有者"；对照：单人持有照常
// ============================================================================

TEST_F(UltimateMultiPkgTest, CrossPackageTypeContentionIsRefusedAndNothingMoves)
{
    // alpha 发**文件** `ult/contend`、beta 发**目录** `ult/contend/` —— 同一逻辑路径两种类型。
    // 这是跨包争用，必须在**整批预检**（进入事务之前）被拒。
    const std::string a = pack("alpha", "1.0", [&](const fs::path& c) {
        fill_alpha("1.0", c);
        write_file(c / ULT / "contend", "alpha-file\n");
    });
    const std::string b = pack("beta", "1.0", [&](const fs::path& c) {
        fill_beta("1.0", c);
        fs::create_directories(c / ULT / "contend");
        write_file(c / ULT / "contend" / "in.txt", "beta-dir\n");
    });

    const auto fs_before = fs_state();
    const std::string msg = err_of([&] { install_packages({a, b}); });

    EXPECT_FALSE(msg.empty()) << "跨包类型争用竟然装成功了";
    // 报的是**批内**先认领那条路径的那个包（这里 alpha 发的是文件 `ult/contend`，
    // beta 发的是目录 `ult/contend/`；预检按批次顺序模拟，于是报 alpha 持有）——
    // 关键是**点名了真实持有者与路径**（§8 第 7 条：报错必须能定位）
    EXPECT_NE(msg.find("alpha"), std::string::npos) << "报错没点名**持有者**：" << msg;
    EXPECT_NE(msg.find("/usr/share/ult/contend"), std::string::npos)
        << "报错没点名**路径**：" << msg;
    EXPECT_FALSE(wal_has_begin()) << "预检在进入事务之前就该拒绝 —— WAL 里不该有 BEGIN_PKGS";
    expect_same_snapshot(fs_before, fs_state(), "⑬ 拒绝后（盘面必须一字未动）");
    for (const char* p : {"alpha", "beta"}) {
        EXPECT_EQ(Cache::instance().get_installed_version(p), "") << p << " 不该被装进去";
    }
}

TEST_F(UltimateMultiPkgTest, SinglyOwnedSamePathInstallsFineAsControl)
{
    // 对照组：同一条路径只由**一个**包声明时照常工作 —— 否则上面那条"必须拒绝"可能只是因为
    // 这一格压根装不上（那是把缺陷伪装成政策）。
    const std::string a = pack("alpha", "1.0", [&](const fs::path& c) {
        fill_alpha("1.0", c);
        write_file(c / ULT / "contend", "alpha-file\n");
    });
    ASSERT_NO_THROW(install_packages({a}));
    EXPECT_EQ(shape_of(root(ULT) / "contend"), "file");
    EXPECT_EQ(read_file(root(ULT) / "contend"), "alpha-file\n");
    EXPECT_EQ(owners_of("/" + std::string(ULT) + "/contend"), "alpha,");
}

// ============================================================================
// ⑮ 重装同版本：无变化（不引入新副本、不动 xattr）
// ============================================================================

TEST_F(UltimateMultiPkgTest, ReinstallSameVersionIsANoOp)
{
    install_v1_and_edit();
    ASSERT_NO_THROW(install_packages({alpha(V2), beta(V2), gamma(V2)}));

    const auto fs_before = fs_state();
    const auto db_before = db_state();
    ASSERT_NO_THROW(install_packages({alpha(V2), beta(V2), gamma(V2)}));
    expect_same_snapshot(fs_before, fs_state(), "⑮ 同版本重装后（应完全无变化）");
    expect_same_snapshot(db_before, db_state(), "⑮ DB 一族");
    EXPECT_EQ(residue(), "bak=0 tmp=0");
}
