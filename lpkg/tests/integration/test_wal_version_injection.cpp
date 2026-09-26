/**
 * test_wal_version_injection.cpp — 版本号不得把内容注入 WAL（WAL 行注入）
 *
 * ── 威胁模型 ────────────────────────────────────────────────────────────────
 * 包版本号是**不可信**输入：本地 .lpkg 的版本取自归档 `metadata.json`
 * （package_manager.cpp 用 `detail::read_archive_metadata` 读出来塞进 targets → InstallPlan
 * 的 `actual_version`），远端索引与 CLI `pkg:版本` 同理。而它被**原样**拼进
 * `wal::log_wal_line("BEGIN " + pkg + " " + ver)` —— WAL 是**按行、空格分帧**的文本协议
 * （`wal::parse_op` 逐行解析），于是版本里的一个 `\n` 就能把 BEGIN 行截断，第二行成为
 * **攻击者可控的合法 WAL 行**（如 `NEW /etc/sudoers`）。回滚（`batch_rollback`）与
 * `lpkg rec`（`recover_packages`）都会把这些行交给 `reverse_execute`，而该函数**没有任何
 * 路径 confinement**：它按 WAL 里的绝对路径直接 rename / remove / chmod。
 *
 * 不变量：`actual_version_` 必须通过 `is_safe_path_component` 才能进 WAL。校验点在
 * `InstallationTask::run()` 里写 BEGIN 行**之前**（所有来源的汇合点）—— 不在
 * `download_and_verify_package()` 里，因为那里对**本地包**提前 return（`archive_path_` 直接
 * 取本地路径，版本号不参与拼路径），够不到；而 COMMIT/END/ROLLBACK 行用的是同一个
 * `actual_version_`，一个校验点覆盖全部出口。
 *
 * ── 为什么断言"WAL 里没有注入行"而不是只看异常 ─────────────────────────────
 * 只 EXPECT_THROW 无法区分"被我们的校验拦下"与"因为别的原因（求解器不认这个版本号）失败"
 * —— 后者同样会绿，却是**假绿**。故本文件两个测试各自带**阳性对照**：
 *   · 直连测试：先断言带 `\n` 的版本确实随 metadata.json 往返（不是测试自己写错）；
 *   · 端到端测试：断点 `install_after_begin_<pkg>` 只在"批次真的把这个包带到了 BEGIN 之后"
 *     时命中，其回调把**此刻的 WAL** 抓下来 —— 修复前它必然命中且抓到注入行，修复后压根
 *     不会命中（校验在 BEGIN 之前抛），而"抛的是我们的校验消息"这条断言同时排除了
 *     "求解器提前失败"这一类假绿。
 *
 * 注意：批次结束后 WAL 会被 `trim_completed()` 清空（正常路径），所以**事后**读 WAL 在端到端
 * 场景里是没有判别力的 —— 这也是端到端那份必须用断点抓现场、而不是事后读文件的原因。
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/db/wal_op.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/install_common.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

class WalVersionInjectionTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;

    void SetUp() override
    {
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        Config::instance().set_no_hooks_mode(true);
        init_localization();
        BreakpointManager::instance().clear_all();

        suite_work_dir = fs::absolute("tmp_wal_version_injection_test");
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
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    /** 打一个包（版本号可以带任何字节，包括 `\n`） */
    std::string pack(const std::string& name, const std::string& ver) const
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name);
        fs::create_directories(work / "content" / "usr" / "bin");
        std::ofstream(work / "content" / "usr" / "bin" / name) << "#!/bin/sh\ntrue\n";

        // 归档文件名不能用版本号拼（带 `\n` 的名字连构造都失败）—— 用固定名，版本只在
        // metadata.json 里（与"远端索引/本地包各自带版本"的实际形态一致）。
        const std::string path = (pkg_dir / (name + ".lpkg")).string();
        pack_package(path, work.string(), name, ver, {}, {}, "man " + name, {});
        return path;
    }

    static std::string read_wal()
    {
        std::ifstream f(wal::wal_log_path());
        if (!f.is_open()) return "";
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }
};

/**
 * 直连 `InstallationTask`（确定性，不经求解器）：版本号带 `\n` 时必须在写 BEGIN 行**之前**被拒。
 *
 * 这条直接钉住被修的那一行；`actual_version_` 的来路与生产完全一致（`version_` 就是
 * package_manager 从 metadata.json 读出来喂给 InstallTask 的那个值）。
 */
TEST_F(WalVersionInjectionTest, NewlineVersionIsRejectedBeforeBeginWalLine)
{
    // 攻击载荷：第二行是一条**合法** WAL 行。`NEW <绝对路径>` 的逆操作就是 `fs::remove(路径)`
    // —— 回滚或 `lpkg rec` 时删掉任意绝对路径。
    const std::string victim = "/tmp/lpkg_wal_inject_victim";
    const std::string evil_ver = "1.0\nNEW " + victim;
    const std::string pkg = pack("walinj", evil_ver);

    // 阳性对照：带 \n 的版本确实随 metadata.json 完整往返（否则下面的"被拒绝"是测试写错）
    const json meta = detail::read_archive_metadata(pkg);
    ASSERT_EQ(meta.at(std::string(constants::J_VERSION)).get<std::string>(), evil_ver)
        << "构造出的恶意包没把换行带进 metadata.json，本测试的前提不成立";

    InstallationTask task("walinj", evil_ver, true, "", pkg);
    std::string msg;
    try {
        task.run(nullptr);
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    // 必须是**我们**的版本号校验拦下的 —— 别的什么原因（如预检阶段先炸）也能让 "没写 BEGIN"
    // 成立，但那种"绿"与本条不变量无关（消息与 error.unsafe_path_component 的格式化结果比对）
    const std::string expected = string_format("error.unsafe_path_component", "version", evil_ver);
    EXPECT_NE(msg.find(expected), std::string::npos)
        << "抛的不是版本号校验的错（假绿？）\n  got: " << msg << "\n  want substring: " << expected;

    // 关键断言：BEGIN 行从未被写出。修复前这里会读到
    //   BEGIN walinj 1.0
    //   NEW /tmp/lpkg_wal_inject_victim
    const std::string wal = read_wal();
    EXPECT_EQ(wal.find(victim), std::string::npos)
        << "版本号里的换行把内容注入了 WAL（第二行就是攻击者拼的 WAL 行）:\n"
        << wal;
    EXPECT_EQ(wal.find("BEGIN walinj"), std::string::npos) << "BEGIN 行不该被写出:\n" << wal;
}

/** 端到端：同样的恶意本地包走真实入口 `install_packages`（版本号由 metadata.json 提供） */
TEST_F(WalVersionInjectionTest, LocalPackageWithNewlineVersionIsRejected)
{
    const std::string victim = "/tmp/lpkg_wal_inject_victim_e2e";
    const std::string evil_ver = "1.0\nNEW " + victim;
    const std::string pkg = pack("walinj2", evil_ver);

    // 断点写在 run() 的 BEGIN 行之后：命中 ⟺ 恶意版本**真的**被带进了 WAL（= 修复前）。
    // 回调把此刻的 WAL 抓下来（批次结束时 WAL 会被 trim 清空，事后读没有判别力）。
    std::string captured_wal;
    BreakpointManager::instance().set("install_after_begin_walinj2", [&captured_wal] {
        std::ifstream f(wal::wal_log_path());
        if (f.is_open())
            captured_wal.assign(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
    });

    std::string msg;
    try {
        install_packages({pkg});
    } catch (const LpkgException& e) {
        msg = e.what();
    }
    BreakpointManager::instance().clear_all();

    // 断点命中 ⟺ 注入行已经落进 WAL
    EXPECT_EQ(captured_wal.find(victim), std::string::npos) << "恶意版本进了 WAL:\n"
                                                            << captured_wal;

    // 必须是**我们的**校验拦下的（消息与 error.unsafe_path_component 的格式化结果逐字一致）
    // —— 否则"抛异常"可能来自求解器/别的什么，与本次修复无关（假绿）。
    const std::string expected = string_format("error.unsafe_path_component", "version", evil_ver);
    EXPECT_NE(msg.find(expected), std::string::npos)
        << "抛的不是版本号校验的错（假绿？）\n  got: " << msg << "\n  want substring: " << expected;

    // 被拒 = 一个文件都没落、DB 里也没有它
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().get_installed_version("walinj2").empty());
    EXPECT_FALSE(fs::exists(test_root / "usr/bin/walinj2"));
}
