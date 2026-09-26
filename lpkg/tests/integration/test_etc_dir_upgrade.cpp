/**
 * test_etc_dir_upgrade.cpp — `/etc` 下的**文件→目录**升级必须能成功（TODO E4 的 /etc 腿）
 *
 * ── 缺陷 ────────────────────────────────────────────────────────────────────
 * `InstallationTask::backup_existing_files()` 开头有一句
 *   `const bool is_config = f.starts_with("etc/"); if (is_config) continue;`
 * 它排在**目录分支之前**，于是 `/etc` 下的**目录条目**也被一并早退：既不备份挡路物、也不写
 * NEW_DIR、不建目录、不套元数据。而冲突预检按 ARCH §3.6.1 第 4 条（E4）**明确放行**"同一
 * 包的文件→目录升级"（`collect_content_conflicts` 的 `ours_exempts`），于是升级一路走到拷贝
 * 阶段，`copy_package_files` 的 `ensure_dir_exists(/etc/foo)` 撞在盘上那个同名普通文件上
 * （`fs::exists("/etc/foo/")` 对普通文件返回 false → `create_directories` EEXIST）抛
 * `error.create_dir_failed`，整批回滚 —— 这条升级路径**永远不可能成功**（`--overwrite` 也
 * 救不了：预检本来就没拦）。`usr/share/...` 那一腿由 test_dir_entry_over_symlink.cpp 的 E4
 * 用例覆盖，`/etc` 这一腿此前无人覆盖。
 *
 * 修法：把 `is_config` 早退**移到目录分支之后** —— 目录条目照目录分支处理；非目录的 `/etc`
 * 条目仍然早退（配置保护语义在 `copy_package_files` 的三哈希分流里，一字未变）。
 *
 * 两份测试：① 升级真的成功且 /etc/foo 变成目录；② 同一条路径的**回滚**自洽（中途失败 →
 * /etc/foo 逐字节回到 v1 那个普通文件）。② 不是修复的判别项（改前也会因别的错回滚），
 * 它钉的是"新打开的这条目录分支也进了事务、可回滚"。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class EtcDirUpgradeTest : public IntegrationTestBase
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

    /** 打一个包：content 由 fill 回调填（路径相对 content/） */
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

/** ① v1 发 `etc/foo`（普通文件）→ v2 发 `etc/foo/bar.conf`：升级必须接管成功 */
TEST_F(EtcDirUpgradeTest, EtcFileBecomesDirectoryOnUpgrade)
{
    const std::string v1 =
        pack("etcpkg", "1.0", [](const fs::path& c) { write_file(c / "etc/foo", "v1 file\n"); });
    ASSERT_NO_THROW(install_packages({v1}));
    ASSERT_TRUE(fs::is_regular_file(test_root / "etc/foo"));
    ASSERT_EQ(read_file(test_root / "etc/foo"), "v1 file\n");

    // 模拟"用户改过这个配置"：这是本用例的关键——`/etc` 条目被**目录**接管时，
    // 三哈希判定表压根不参与（它只在"归档条目是普通文件"那条路上跑），所以保护
    // 用户改动只能靠"把挡路物改名保留"，而不是靠判定表。
    write_file(test_root / "etc/foo", "user edited\n");

    const std::string v2 = pack("etcpkg", "2.0", [](const fs::path& c) {
        write_file(c / "etc/foo/bar.conf", "v2 conf\n");
    });

    // 改前：E4 预检放行 → 拷贝阶段的 ensure_dir_exists 撞上同名普通文件 → 抛
    // error.create_dir_failed 整批回滚（安装永远不可能成功）。
    EXPECT_NO_THROW(install_packages({v2}))
        << "同包 /etc 文件→目录升级被拒（E4 预检放行、拷贝阶段却撞 error.create_dir_failed）";

    EXPECT_TRUE(fs::is_directory(test_root / "etc/foo")) << "/etc/foo 应被接管成目录";
    EXPECT_FALSE(fs::is_symlink(test_root / "etc/foo"));
    EXPECT_EQ(read_file(test_root / "etc/foo/bar.conf"), "v2 conf\n");
    EXPECT_EQ(Cache::instance().get_installed_version("etcpkg"), "2.0");

    // **用户改过的那份必须留下来**：`/etc/` 的挡路物改成 save_config（`.lpkgsave`），
    // 与移除侧对 /etc 的政策一致。曾经走 `sink.backup()` 进 stash —— stash 在提交后
    // 被整目录 remove_all，用户配置就此无声蒸发，且连 `.lpkgnew` 都没有。
    EXPECT_TRUE(fs::is_regular_file(test_root / "etc/foo.lpkgsave"))
        << "被目录接管的 /etc 配置必须改名保留成 .lpkgsave，不能进 stash 后删掉";
    EXPECT_EQ(read_file(test_root / "etc/foo.lpkgsave"), "user edited\n")
        << ".lpkgsave 里必须是用户改过的那份内容（逐字节）";

    // 非 /etc 路径仍走 stash：提交后清干净，不该出现 .lpkgsave
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(test_root, ec))
        EXPECT_EQ(e.path().filename().string().find(".lpkg_bak_"), std::string::npos)
            << "残留备份: " << e.path();
}

/**
 * ② 回滚自洽：v2 升级在"目录已建、新配置正落位"处失败 → `/etc/foo` 逐字节回到 v1 的普通文件。
 *
 * 断点 `copy_after_wal_<pkg>` 落在 COPY 的 WAL 行与 rename **之间**（op_sink.cpp），此刻
 * `/etc/foo` 已是目录、`etc/foo/bar.conf` 还没落位。逆序回滚应当是：
 * 删 COPY 的 tmp → rmdir 新目录（NEW_DIR 逆操作）→ 把旧文件从 stash rename 回 `/etc/foo`。
 */
TEST_F(EtcDirUpgradeTest, EtcFileBecomesDirectoryUpgradeRollsBackCleanly)
{
    const std::string v1 =
        pack("etcpkg2", "1.0", [](const fs::path& c) { write_file(c / "etc/foo", "v1 file\n"); });
    ASSERT_NO_THROW(install_packages({v1}));
    ASSERT_TRUE(fs::is_regular_file(test_root / "etc/foo"));

    const std::string v2 = pack("etcpkg2", "2.0", [](const fs::path& c) {
        write_file(c / "etc/foo/bar.conf", "v2 conf\n");
    });

    BreakpointManager::instance().set("copy_after_wal_etcpkg2",
                                      [] { throw LpkgException("injected copy failure"); });
    EXPECT_THROW(install_packages({v2}), LpkgException);
    BreakpointManager::instance().clear_all();

    EXPECT_FALSE(fs::is_directory(test_root / "etc/foo")) << "回滚后 /etc/foo 不该还是目录";
    ASSERT_TRUE(fs::is_regular_file(test_root / "etc/foo"));
    EXPECT_EQ(read_file(test_root / "etc/foo"), "v1 file\n") << "回滚没把旧文件还原回来";
    Cache::instance().load();
    EXPECT_EQ(Cache::instance().get_installed_version("etcpkg2"), "1.0");
}

/**
 * ③ 对照：**非 `/etc`** 路径的"文件→目录"接管仍走 stash，提交后清干净、不留 `.lpkgsave`。
 *
 * 钉的是"我没有把 save_config 用过头"：`.lpkgsave` 是 /etc 配置的语义，别的路径上的
 * 挡路文件是包自己的内容，进 stash（回滚要用）并在提交后清理即可。
 */
TEST_F(EtcDirUpgradeTest, NonEtcFileBecomesDirectoryStillUsesStash)
{
    const std::string v1 =
        pack("usrpkg", "1.0", [](const fs::path& c) { write_file(c / "usr/share/thing", "v1\n"); });
    ASSERT_NO_THROW(install_packages({v1}));
    ASSERT_TRUE(fs::is_regular_file(test_root / "usr/share/thing"));

    const std::string v2 = pack("usrpkg", "2.0", [](const fs::path& c) {
        write_file(c / "usr/share/thing/inner.conf", "v2\n");
    });
    EXPECT_NO_THROW(install_packages({v2}));

    EXPECT_TRUE(fs::is_directory(test_root / "usr/share/thing"));
    EXPECT_EQ(read_file(test_root / "usr/share/thing/inner.conf"), "v2\n");
    EXPECT_FALSE(fs::exists(test_root / "usr/share/thing.lpkgsave"))
        << "非 /etc 路径不该产生 .lpkgsave（那是 /etc 配置的语义）";
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(test_root, ec))
        EXPECT_EQ(e.path().filename().string().find(".lpkg_bak_"), std::string::npos)
            << "残留备份: " << e.path();
}
