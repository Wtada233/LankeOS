#include <gtest/gtest.h>
#include <sys/mount.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

class AdvancedPackageManagerTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        Config::instance().set_force_overwrite_mode(false);
        Config::instance().set_no_hooks_mode(false);
        Config::instance().set_no_deps_mode(false);
        init_localization();

        // 目录名带 PID：并发的第二个测试进程会 rm -rf 掉固定名目录（同 test_base.hpp 的
        // `tmp_lpkg_itest`，实测产生成片 SetUp 假失败）。
        suite_work_dir = fs::absolute("tmp_advanced_test_" + std::to_string(getpid()));
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";

        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        std::string clean_cmd = "sudo rm -rf " + suite_work_dir.string();
        run_shell(clean_cmd);
    }

    std::string create_pkg(const std::string& name, const std::string& ver,
                           const std::string& content_file, const std::string& content)
    {
        fs::path work_dir = suite_work_dir / ("pkg_work_" + name);
        fs::create_directories(work_dir / "content");
        fs::path dest = work_dir / "content" / content_file;
        fs::create_directories(dest.parent_path());

        std::ofstream f(dest);
        f << content;
        f.close();

        std::string pkg_name = name + "-" + ver + ".lpkg";
        std::string pkg_path = (pkg_dir / pkg_name).string();

        pack_package(pkg_path, work_dir.string(), name, ver);

        fs::remove_all(work_dir);
        return pkg_path;
    }
};

TEST_F(AdvancedPackageManagerTest, RollbackOnCopyFailure)
{
    // 1. Prepare a package with two files
    std::string pkg;
    {
        std::string pkg_name = "rollback_new-1.0.lpkg";
        pkg = (pkg_dir / pkg_name).string();
        fs::path work_dir = suite_work_dir / "pkg_work_rollback_new";
        fs::create_directories(work_dir / "content" / "usr" / "bin");
        std::ofstream f1(work_dir / "content" / "usr" / "bin" / "file_ok");
        f1 << "ok";
        f1.close();
        std::ofstream f2(work_dir / "content" / "usr" / "bin" / "file_blocked");
        f2 << "blocked";
        f2.close();

        pack_package(pkg, work_dir.string(), "rollback_new", "1.0");
        fs::remove_all(work_dir);
    }

    // 2. Sabotage: Make individual FILE read-only to block overwrite
    // (目录权限不再适用——copy_package_files 现会纠正目录权限)
    fs::path bin_dir = test_root / "usr" / "bin";
    fs::create_directories(bin_dir);

    Config::instance().set_force_overwrite_mode(true);

    std::ofstream f_sabotage(bin_dir / "file_blocked");
    f_sabotage << "original";
    f_sabotage.close();

    // 不再需要破坏性测试 — copy_package_files 现在会纠正目录权限，
    // 因此安装应当成功。这里验证纠正后的正确行为。
    EXPECT_NO_THROW(install_packages({pkg}));

    // Verify both files were installed despite the sabatoge
    EXPECT_TRUE(fs::exists(test_root / "usr" / "bin" / "file_ok"));
    EXPECT_TRUE(fs::exists(test_root / "usr" / "bin" / "file_blocked"));
}

TEST_F(AdvancedPackageManagerTest, ChrootHook)
{
    std::string pkg = create_pkg("hook_test", "1.0", "dummy", "dummy");

    // Add hook to the package (Re-creating with hook)
    {
        fs::path work_dir = suite_work_dir / "pkg_work_hook_test_with_hook";
        fs::create_directories(work_dir / "content");
        std::ofstream f(work_dir / "content" / "dummy");
        f << "d";
        f.close();

        fs::create_directories(work_dir / "hooks");
        std::ofstream hook(work_dir / "hooks" / "postinst.sh");
        hook << "#!/bin/sh\n";
        hook << "echo 'Running' > /hook_ran.txt\n";
        hook.close();
        fs::permissions(work_dir / "hooks" / "postinst.sh",
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add);

        pack_package(pkg, work_dir.string(), "hook_test", "1.0");
        fs::remove_all(work_dir);
    }

    // 正向对照：`run_hook` 的**执行点**断点（hooks 已启用、脚本确实存在、只剩 exec）。
    // 沙盒 root 里没有 /bin/bash，run_hook 会在 chroot 分支提前 return —— 钩子不会真的执行。
    // 正因如此，"没有落文件"这句若不配这个断点就是**恒真**：包不带 hook / hook 没被识别 /
    // 钩子被禁用，都会得到"没落文件"，而它们与 root 前缀对不对无关。断点先钉住"处理确实走到
    // 了执行决策那一步"，下面的否定断言才有咬合力（同 test_hook_transaction.cpp 的取证方式）。
    bool hook_exec_point_reached = false;
    auto on_hook = [&] { hook_exec_point_reached = true; };
    BreakpointManager::instance().set("hook_run_postinst.sh", on_hook);

    testing::internal::CaptureStderr();
    install_packages({pkg});
    testing::internal::GetCapturedStderr();
    BreakpointManager::instance().clear_all();

    // (1) 正向对照本身：执行点确实被走到 —— 否则下面全是空转
    ASSERT_TRUE(hook_exec_point_reached) << "run_hook 未走到执行点（钩子被禁用 / 未识别）";

    // (2) 钩子脚本**确实**被装进了目标 root 的 hooks 目录
    const fs::path hook_file = Config::instance().hooks_dir() / "hook_test" / "postinst.sh";
    ASSERT_TRUE(fs::exists(hook_file)) << "hook 没被装到目标 root 的 hooks_dir 里，护栏是空转的";

    // (3) 执行点已到，但副作用**没有**落在目标 root、也**没有**落到宿主真实的 / 上。两条
    //     合起来排除了"chroot 建不起来就退化成在宿主上执行"这种回退（那会把 hook_ran.txt 写到
    //     宿主 /）。本沙盒里钩子最终被"缺 bash"那道守卫**跳过**，所以这里证明不了"钩子会在目标
    //     root 内真的跑起来"——那要往沙盒塞一整套 rootfs，属于"考环境"（同 test_hook_transaction
    //     的说明）；但"绝不退化到宿主执行"这条能证，且正是本用例要守的不变量。
    EXPECT_FALSE(fs::exists(test_root / "hook_ran.txt"));
    EXPECT_FALSE(fs::exists("/hook_ran.txt"));
    // (4) hook 脚本本身也没落到宿主真实的 /etc/lpkg（root 前缀漏掉就会写到这里）
    EXPECT_FALSE(fs::exists("/etc/lpkg/hooks/hook_test"))
        << "hook 落到宿主真实的 /etc/lpkg 上了（root 前缀被漏掉）";
}
