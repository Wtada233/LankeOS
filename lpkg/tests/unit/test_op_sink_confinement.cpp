/**
 * test_op_sink_confinement.cpp — 写入层原语的**祖先链闸**（`OpSink::confined`）
 *
 * 背景：归档内容那条腿已经在 `detail::confine_target_path()` 上
 * 挡过"经祖先符号链接写到 root 之外"，但**由 DB 键派生的路径**走的是另一条腿 ——
 * 升级废弃文件、移除趟、空目录回收、xattr 撤销。那些路径取自 `files.db` 的键，若某个祖先
 * 目录此刻是一条**逃出 root 的符号链接**，`rename`/`rmdir`/`chmod`/`lremovexattr` 就会落到
 * root 之外（把宿主文件搬进 stash、提交后随 stash 删掉 = 静默删除宿主文件）。
 *
 * 修法是把闸放进**唯一写盘入口**（`OpSink` 里每个碰文件系统的原语），而不是逐调用点补：
 * 任何将来的调用点自动被覆盖。语义是**跳过 + 告警**、绝不抛（这些路径出现在移除/回滚链上，
 * 抛了等于"包卸载不掉"；跳过才是安全方向 —— 那条路径按定义在 root 之外，本来就不该动）。
 *
 * 每格都配**对照**（同样的调用、路径不越界时必须照常动作）—— 没有对照，一个"永远返回空 /
 * 永远 return false"的实现也能全绿。
 *
 * ⚠️ 测试形态：`dir_meta` / `set_xattr` / `unset_xattr` 自身还有一道"目标必须是**真目录**
 * （lstat 语义）"的前置，所以对**那条逃逸链接本身**调用它们本来就是空转（不是本文件要测的闸）。
 * 要测闸，必须把目标放在**逃逸链接之后**的真实目录上（`outside/<子目录>`），这样前置通过、
 * 只剩闸能拦住。
 */

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/transaction_log.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/op_sink.hpp"

namespace fs = std::filesystem;
using detail::DirRemoval;
using detail::OpSink;

class OpSinkConfinementTest : public ::testing::Test
{
protected:
    fs::path suite_dir;
    fs::path root;
    fs::path outside;  ///< 模拟 root 之外的宿主目录
    std::vector<fs::path> stashes;

    void SetUp() override
    {
        init_localization();
        Config::instance().set_testing_mode(true);
        suite_dir = fs::absolute("tmp_op_sink_confinement_test_" + std::to_string(::getpid()));
        fs::remove_all(suite_dir);
        root = suite_dir / "root";
        outside = suite_dir / "outside";
        fs::create_directories(root / "usr");
        fs::create_directories(outside);
        Config::instance().set_root_path(root.string());
        Config::instance().init_filesystem();
        Cache::instance().load();

        // 逃逸形态：`<root>/usr/lib` 是一条指向 root **之外**的目录链接。
        // 末段不解析是有意的（§5.4 不变量 6）——建这条链接本身合法，非法的是"经它写出去"。
        std::error_code ec;
        fs::create_directory_symlink(outside, root / "usr" / "lib", ec);
        ASSERT_FALSE(ec) << ec.message();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        fs::remove_all(suite_dir);
    }

    /// 在 root 之外造一个真实文件（模拟"被逃逸链接罩住的宿主文件"）。
    fs::path make_outside_file(const std::string& name, const std::string& body = "host data\n")
    {
        std::ofstream(outside / name) << body;
        return outside / name;
    }

    static std::string slurp(const fs::path& p)
    {
        std::ifstream f(p);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    /// 词法上在 root 内、实际解析到 root 之外的路径（闸要拦的就是这种）。
    fs::path lexically_inside(const std::string& rel_to_outside) const
    {
        return root / "usr" / "lib" / rel_to_outside;
    }
};

TEST_F(OpSinkConfinementTest, BackupRefusesPathsBehindAnEscapingAncestor)
{
    const fs::path host_file = make_outside_file("payload", "host data\n");
    OpSink sink("pkg", &stashes);

    const fs::path bak = sink.backup(lexically_inside("payload"));

    EXPECT_TRUE(bak.empty()) << "越界路径不许备份（返回空 = 什么都没做）: " << bak;
    EXPECT_TRUE(fs::exists(host_file)) << "**root 之外的宿主文件被搬走了** —— 闸没生效";
    EXPECT_EQ(slurp(host_file), "host data\n") << "宿主文件内容也不许被动";
    EXPECT_TRUE(stashes.empty()) << "没有备份就不该往 stash 账本里记东西";
}

TEST_F(OpSinkConfinementTest, BackupStillWorksForPathsInsideTheRoot)
{
    // 对照：同样的调用，路径不越界时必须照常搬进 stash。没有这条，"永远返回空"也全绿。
    std::ofstream(root / "payload") << "ours\n";
    OpSink sink("pkg", &stashes);

    const fs::path bak = sink.backup(root / "payload");
    EXPECT_FALSE(bak.empty()) << "root 内的普通路径必须照常备份";
    EXPECT_FALSE(fs::exists(root / "payload")) << "备份 = 从原位搬走";
    EXPECT_TRUE(fs::exists(bak)) << "备份文件必须真的在 stash 里: " << bak;
}

TEST_F(OpSinkConfinementTest, SaveConfigRefusesPathsBehindAnEscapingAncestor)
{
    const fs::path host_file = make_outside_file("a-conf");
    OpSink sink("pkg", &stashes);

    const fs::path kept = sink.save_config(lexically_inside("a-conf"));

    EXPECT_TRUE(kept.empty()) << "越界路径不许改名保留: " << kept;
    EXPECT_TRUE(fs::exists(host_file)) << "宿主文件被改名了";
    EXPECT_FALSE(fs::exists(outside / "a-conf.lpkgsave")) << "`.lpkgsave` 落到了 root 之外的目录里";
}

TEST_F(OpSinkConfinementTest, RemoveEmptyDirRefusesPathsBehindAnEscapingAncestor)
{
    fs::create_directories(outside / "victim");
    OpSink sink("pkg", &stashes);

    EXPECT_EQ(sink.remove_empty_dir(lexically_inside("victim")), DirRemoval::NotRemoved);
    EXPECT_TRUE(fs::exists(outside / "victim")) << "root 之外的空目录被删掉了";
}

TEST_F(OpSinkConfinementTest, RemoveEmptyDirStillRemovesInsideTheRoot)
{
    // 对照：root 内的空目录必须照常删（否则上面那条在"函数永远 NotRemoved"时也绿）。
    fs::create_directories(root / "empty-dir");
    OpSink sink("pkg", &stashes);

    EXPECT_EQ(sink.remove_empty_dir(root / "empty-dir"), DirRemoval::Removed);
    EXPECT_FALSE(fs::exists(root / "empty-dir"));
}

TEST_F(OpSinkConfinementTest, DirMetaRefusesPathsBehindAnEscapingAncestor)
{
    // 目标必须是**真目录**（那是 dir_meta 自己的前置），所以造在逃逸链接之后。
    fs::create_directories(outside / "subdir");
    struct stat before{};
    ASSERT_EQ(::lstat((outside / "subdir").c_str(), &before), 0);
    const mode_t before_mode = before.st_mode & 07777;
    ASSERT_NE(before_mode, 0700u) << "fixture 自检：先让它不是 0700，改动才可观测";
    OpSink sink("pkg", &stashes);

    sink.dir_meta(lexically_inside("subdir"), 0700, 0, 0, /*record_previous=*/false);

    struct stat after{};
    ASSERT_EQ(::lstat((outside / "subdir").c_str(), &after), 0);
    EXPECT_EQ(after.st_mode & 07777, before_mode)
        << "root 之外那个目录的 mode 被就地改了（目录是活对象，回滚也救不回来）";
}

TEST_F(OpSinkConfinementTest, XattrPrimitivesRefusePathsBehindAnEscapingAncestor)
{
    fs::create_directories(outside / "xdir");
    OpSink sink("pkg", &stashes);
    const std::vector<char> value{'v'};

    EXPECT_FALSE(sink.set_xattr(lexically_inside("xdir"), "user.lpkg-test", value))
        << "越界路径不许设 xattr";
    EXPECT_FALSE(sink.unset_xattr(lexically_inside("xdir"), "user.lpkg-test"))
        << "越界路径不许删 xattr（这是**删数据**，方向最危险的一格）";

    // 对照：root 内的真目录照常能设（否则"永远 return false"也绿）。
    fs::create_directories(root / "xdir");
    EXPECT_TRUE(sink.set_xattr(root / "xdir", "user.lpkg-test", value));
}

TEST_F(OpSinkConfinementTest, CommitCopyRefusesAnEscapingDestinationAndDropsTheStagedFile)
{
    // 越界时**不许 rename**，但我们 staged 的那份要自己收掉（别把 `.lpkgtmp` 留在盘上）。
    const fs::path tmp = root / "staged.lpkgtmp";
    std::ofstream(tmp) << "new content\n";
    OpSink sink("pkg", &stashes);

    sink.commit_copy(tmp, lexically_inside("target"));

    EXPECT_FALSE(fs::exists(outside / "target")) << "内容落到了 root 之外";
    EXPECT_FALSE(fs::exists(tmp)) << "越界时我们 staged 的 `.lpkgtmp` 没被收掉";
}

TEST_F(OpSinkConfinementTest, UnStashRefusesAnEscapingDestination)
{
    // stash 一定在 root 内（`stash_parent_dir` 恒 clamp），越界的是"搬回的目标"。
    fs::create_directories(root / "stash");
    const fs::path bak = root / "stash" / "thing.lpkg_bak_pkg_1";
    std::ofstream(bak) << "stashed\n";
    const fs::path host_file = make_outside_file("thing", "host data\n");
    OpSink sink("pkg", &stashes);

    EXPECT_FALSE(sink.un_stash(bak, lexically_inside("thing")))
        << "搬回的目标越界时必须拒绝（返回 false = 没动盘）";
    EXPECT_TRUE(fs::exists(bak)) << "被拒绝时 bak 必须留在 stash 里（不许半路消失）";
    EXPECT_EQ(slurp(host_file), "host data\n") << "宿主文件被覆盖/搬走了";

    // 空 bak（= 上游把"被闸掉的备份"记成空路径时）也必须拒绝，不许拿空路径去 rename。
    EXPECT_FALSE(sink.un_stash({}, lexically_inside("thing")));
    EXPECT_EQ(slurp(host_file), "host data\n");
}
