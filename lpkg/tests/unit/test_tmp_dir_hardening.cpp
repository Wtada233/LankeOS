/**
 * test_tmp_dir_hardening.cpp — 进程暂存目录必须**只属于本进程**
 *
 * 暂存目录（`/tmp/lpkg_<pid>_<rand>`）此前是
 * "`exists()` 探测 + `fs::create_directories()`"，而 `create_directories` 对**已存在**的路径
 * （攻击者预建的目录、或指向 `/etc` 的符号链接）**静默成功** ⇒ 本地无权用户可以劫持 root
 * 的暂存目录，root 把包内容解压进去、再拷进系统 ⇒ 任意内容以 root 安装。
 * 另：当时 mode 是 `0777 & ~umask`（umask 022 时 = **0755**，别人能读）。
 *
 * 修法是 `mkdtemp`（原子建、0700）+ 建后 lstat 复核。本文件断言**结果属性**，不断言实现：
 *   · 它必须是**真目录**（不是符号链接）—— 这条正是"预建符号链接"那条攻击路径的反面；
 *   · 属主必须是本进程 euid；
 *   · mode 必须是 **0700** —— 这一条对修前实现**必红**（0755），是本套件的主要证据；
 *   · 名字必须保留 `lpkg_<pid>_` 前缀 —— `cleanup_tmp_dirs()` 靠它在**首个 `_`** 处切出
 *     PID 判活（改了形状 = 崩溃残留的临时目录永远不被回收，那是另一个可达缺陷）。
 *
 * 测不到的：TOCTOU 本身（名字含随机串，单测里无法稳定抢在 mkdtemp 之前占名）。那一半靠
 * `mkdir(2)`/`mkdtemp` 的原子性**从构造上**保证，见 config.cpp 的实现注释 —— 这里不为它写
 * 一条永远绿的用例。
 */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <string>

#include "../../main/src/config/config.hpp"

namespace fs = std::filesystem;

TEST(TmpDirHardeningTest, IsPrivateDirectoryOwnedByThisProcess)
{
    const fs::path dir = Config::get_tmp_dir();

    struct stat st{};
    ASSERT_EQ(::lstat(dir.c_str(), &st), 0) << "暂存目录不存在: " << dir;

    EXPECT_TRUE(S_ISDIR(st.st_mode)) << "必须是**真目录**（lstat 语义），不能是符号链接: " << dir;
    EXPECT_EQ(st.st_uid, ::geteuid()) << "属主必须是本进程 euid: " << dir;
    EXPECT_EQ(st.st_mode & 07777, 0700u)
        << "必须是 0700（只有本进程能进/能写）—— 修前是 0777 & ~umask（实测 0755，别人能读、"
           "umask 为 0 时甚至能写 = 可以替换刚落进去的内容）: "
        << dir;
}

TEST(TmpDirHardeningTest, NameKeepsTheShapeCleanupDependsOn)
{
    // `cleanup_tmp_dirs()` 的判活解析：`lpkg_` 前缀 → 首个 `_` 之前那段当 PID。
    // 名字形状一改（例如换成纯随机、把 pid 挪到后面），崩溃残留就永远回收不掉。
    const fs::path dir = Config::get_tmp_dir();
    const std::string name = dir.filename().string();
    const std::string expected_prefix = "lpkg_" + std::to_string(::getpid()) + "_";

    EXPECT_EQ(name.rfind(expected_prefix, 0), 0u) << "名字必须是 `lpkg_<pid>_<随机后缀>`: " << name;
    EXPECT_GT(name.size(), expected_prefix.size()) << "随机后缀不能为空: " << name;
}

TEST(TmpDirHardeningTest, SameProcessGetsTheSameDirectory)
{
    // 进程内是常量（static）：初始化一次，各调用点拿到同一份，别在每次调用时现建一个新目录。
    EXPECT_EQ(Config::get_tmp_dir(), Config::get_tmp_dir());
}

TEST(TmpDirHardeningTest, RecreationAfterRemovalIsStillPrivate)
{
    // **全量测试里暴露出来的洞**：`~TmpDirManager()` 会 `remove_all` 掉这个根，而之后任何一句
    // `create_directories(<根>/…)` 都会把它**重新建出来 —— 用 0777 & ~umask（0755）**，
    // 于是加固在进程活着的中途就没了；更要紧的是那一格又回到"静默接受一个已存在的同名路径"
    // （= 最初那条可劫持缺陷的形态）。修法：`get_tmp_dir()` 每次取用都复核并修复（见 config.cpp）。
    const fs::path dir = Config::get_tmp_dir();

    // ① 真实事故形状：根被删掉、随后被**别处以 `create_directories` 重建**（= 0777 & ~umask）。
    //    这正是全量测试里暴露出来的那一格 —— `ensure_tmp_dir_usable` 的 **chmod 回修**分支。
    //    ⚠️ 前提是 umask 不放宽（测试容器 umask 022）：umask=077 时任何一种实现都会得到 0700，
    //    那种环境下本用例对这条分支没有区分力（下面还有"删掉"那一格，不依赖 umask）。
    // ① 真实事故形状：根被删掉、随后被**别处以 `create_directories` 重建**（= 0777 & ~umask）
    //    —— 这一格走的才是 `ensure_tmp_dir_usable` 的 **chmod 回修**分支（全量测试里暴露的洞）。
    //    ⚠️ 前提是 umask 不放宽（容器 umask 022）：umask=077 时旧实现也会得到 0700，那种环境下
    //    这一条没有区分力 —— 所以下面还留着不依赖 umask 的"整个删掉"那一格。
    std::error_code ec;
    ASSERT_GT(fs::remove_all(dir, ec), 0u)
        << "fixture：先把它删掉（模拟 `~TmpDirManager`），失败则本用例没测到东西";
    fs::create_directories(dir / "stale-child", ec);  // create_directories ⇒ 0755
    ASSERT_FALSE(ec) << ec.message();
    struct stat mid{};
    ASSERT_EQ(::lstat(dir.c_str(), &mid), 0);
    if ((mid.st_mode & 07777) == 0700) {
        GTEST_SKIP() << "umask 把 create_directories 也限成 0700（本环境无区分力）";
    }
    EXPECT_EQ((mid.st_mode & 07777), 0755u) << "fixture 自检：create_directories 应给出 0755";

    EXPECT_EQ(Config::get_tmp_dir(), dir) << "重建必须复用同一个名字";
    struct stat fixed{};
    ASSERT_EQ(::lstat(dir.c_str(), &fixed), 0);
    EXPECT_EQ(fixed.st_mode & 07777, 0700u)
        << "取用后必须把被放宽的 mode 收回来（否则加固在进程活着的中途就没了）";

    // ② 整个删掉（不依赖 umask）：再次取用必须重建为 0700。
    ASSERT_GT(fs::remove_all(dir, ec), 0u) << "fixture：删掉整个根";

    const fs::path again = Config::get_tmp_dir();
    EXPECT_EQ(again, dir) << "重建必须复用同一个名字（清理侧靠 pid 形状判活）";

    struct stat st{};
    ASSERT_EQ(::lstat(again.c_str(), &st), 0) << "取用后必须重新存在";
    EXPECT_TRUE(S_ISDIR(st.st_mode)) << again;
    EXPECT_EQ(st.st_uid, ::geteuid()) << again;
    EXPECT_EQ(st.st_mode & 07777, 0700u)
        << "重建后必须是 0700 —— 0755 说明走的是 create_directories（0777 & ~umask）: " << again;
}
