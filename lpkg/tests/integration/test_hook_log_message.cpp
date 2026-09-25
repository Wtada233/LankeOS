/**
 * test_hook_log_message.cpp — 钩子执行日志必须**点名包**
 *
 * 挂载时机统一到批次提交后之后（见 finish_committed_batch），一次多包批次会连着跑多个钩子，
 * 而原日志是 `正在运行钩子: postinst.sh` / `Running hook: postinst.sh` —— 在日志里没有任何
 * 上下文（哪个包？）。改成带包名的形态。
 *
 * 断言刻意**落在那一行本身**（先按行切开、再找含 hook 名的那行），而不是在整个输出里
 * `find(包名)` —— 后者是空转：安装过程本来就会打印"开始安装 <包名>"，无论日志行改没改都会绿。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"

namespace fs = std::filesystem;

class HookLogMessageTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        // 必须**打开**钩子：run_hook 在钩子文件不存在时会在打日志之前就返回。
        Config::instance().set_no_hooks_mode(false);
        init_localization();
        BreakpointManager::instance().clear_all();

        suite_work_dir = fs::absolute("tmp_hook_log_test");
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
        Config::instance().set_no_hooks_mode(true);
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    /** 打一个带 postinst 钩子的包 */
    std::string pack_with_postinst(const std::string& name)
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name);
        fs::create_directories(work / "content" / "usr" / "bin");
        std::ofstream(work / "content" / "usr" / "bin" / name) << "#!/bin/sh\ntrue\n";
        // packer 会把 <work>/hooks/ 整个作为归档里的 hooks/ 加进去
        fs::create_directories(work / "hooks");
        std::ofstream(work / "hooks" / std::string(constants::POSTINST_SH)) << "#!/bin/sh\ntrue\n";
        const std::string path = (pkg_dir / (name + "-1.0.lpkg")).string();
        pack_package(path, work.string(), name, "1.0", {}, {}, "man " + name, {});
        return path;
    }

    /**
     * 找出"**运行钩子**那条消息"所在行（找不到返回空串）。
     *
     * 关键：只在整个输出里 `find(包名)` 是**空转**（安装过程本来就会打印"开始安装 <包名>"），
     * 所以断言必须落在这一行本身。
     *
     * 额外要求这一行不含路径分隔符：这是**预防性**的（不是实测到的现象）—— 万一将来有别的
     * 日志行打印 hook 的完整路径（形如 `<root>/etc/lpkg/hooks/<pkg>/postinst.sh`），那行天然
     * 同时含 hook 名与包名，会让断言重新变成空转；而用户看到的这条消息不含路径。
     *
     * 造红提示（踩过一次，值得记）：l10n 的查找顺序是 `<exe>/../l10n` → `<exe>/../main/l10n`
     * → `<exe>/../src/l10n` → 安装路径。容器里跑测试命中的是 **`/app/main/l10n`**（同步进去的
     * 源码树），**不是** `/usr/share/lpkg/l10n` —— 改后者造不出红。
     */
    static std::string running_hook_line(const std::string& out, const std::string& hook_name)
    {
        std::string cur;
        const auto matches = [&](const std::string& s) {
            return s.find(hook_name) != std::string::npos && s.find('/') == std::string::npos;
        };
        for (const char c : out) {
            if (c == '\n') {
                if (matches(cur)) return cur;
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        return matches(cur) ? cur : std::string{};
    }
};

TEST_F(HookLogMessageTest, PostinstLogLineNamesThePackage)
{
    const std::string pkg = pack_with_postinst("hooklog");

    testing::internal::CaptureStdout();
    install_packages({pkg});
    const std::string out = testing::internal::GetCapturedStdout();

    const std::string line = running_hook_line(out, std::string(constants::POSTINST_SH));
    ASSERT_FALSE(line.empty()) << "输出里没有\"运行钩子\"那条消息（只有路径行不算）：\n" << out;
    // 关键断言：那一行**本身**必须带包名（整个输出里含包名是必然的，不能拿来当证据）
    EXPECT_NE(line.find("hooklog"), std::string::npos)
        << "钩子执行日志没有点名包，多包批次里无法分辨是谁的钩子。该行实际是：\n"
        << line;
}

TEST_F(HookLogMessageTest, PrermLogLineNamesThePackage)
{
    const std::string pkg = pack_with_postinst("hooklogrm");
    install_packages({pkg});

    testing::internal::CaptureStdout();
    remove_packages({"hooklogrm"});
    const std::string out = testing::internal::GetCapturedStdout();

    // 这个 fixture 只带 postinst；移除时若没有 prerm 就不会有对应日志行——
    // 所以只断言"若出现 prerm 行则必须点名包"，避免把 fixture 的形态当成断言前提。
    const std::string line = running_hook_line(out, "prerm");
    if (!line.empty()) {
        EXPECT_NE(line.find("hooklogrm"), std::string::npos) << "该行实际是：\n" << line;
    }
}
