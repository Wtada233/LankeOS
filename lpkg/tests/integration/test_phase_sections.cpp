/**
 * test_phase_sections.cpp — 四个阶段 + 一条 summary 的**可执行规格**
 *
 * 2026-10-03 把批次流水线拆成四段并对用户可见（`ui::section`）：
 *
 *     :: 下载 → :: 解压 → :: 安装 → :: 运行安装后钩子 → :: 系统触发器
 *
 * 本文件钉四件事：
 *   ① 阶段标题按这个**顺序**出现（且"准备"那个旧标题没了）；
 *   ② **不带 hooks 的包不出"运行安装后钩子"这一节** —— `hook_sets` 对每个被处理的包都会记一条
 *      （没 hook 的包记的是空表，而空表在下游是"本版本没有 hooks"的硬信号），所以判据必须是
 *      "某个成员带了 postinst.sh"，不能是"表非空"；
 *   ③ 批次结束后**只有一条** summary（数量 + 名单），逐包的"X 已成功安装!"不复存在；
 *   ④ 元数据与索引不符时**事务根本没开**（`install_after_begin_<pkg>` 断点不命中）——
 *      校验搬到"下载"阶段之后，不一致是在任何文件落地之前报出来的。
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/constants.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/crypto/hash.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/db/test_breakpoints.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"

namespace fs = std::filesystem;

class PhaseSectionTest : public ::testing::Test
{
protected:
    fs::path suite_work_dir;
    fs::path test_root;
    fs::path pkg_dir;
    fs::path mirror_dir;

    void SetUp() override
    {
        // 依赖声明：本用例要看的正是"钩子阶段出不出声"，所以钩子必须**开着**。
        // （进程级开关的复位由 tests/test_hygiene.hpp 的全局 listener 兜底；这里再显式写一遍
        //   是让依赖就地可见 —— 少了它，整段会静默，而"没有钩子就不该出现该阶段"那条断言
        //   会退化成**两种情形都给绿**的空转断言。）
        Config::instance().set_no_hooks_mode(false);
        Config::instance().set_non_interactive_mode(NonInteractiveMode::YES);
        Config::instance().set_testing_mode(true);
        init_localization();
        BreakpointManager::instance().clear_all();

        suite_work_dir = fs::absolute("tmp_phase_section_test_" + std::to_string(::getpid()));
        fs::remove_all(suite_work_dir);
        test_root = suite_work_dir / "root";
        pkg_dir = suite_work_dir / "pkgs";
        mirror_dir = suite_work_dir / "mirror" / "x86_64";
        fs::create_directories(test_root);
        fs::create_directories(pkg_dir);
        fs::create_directories(mirror_dir);

        Config::instance().set_root_path(test_root.string());
        Config::instance().set_architecture("x86_64");
        Config::instance().init_filesystem();
        std::ofstream(test_root / "etc/lpkg/mirror.conf")
            << "file://" << (suite_work_dir / "mirror").string() << "/" << std::endl;
        // 触发器规则是**文件驱动**的：不写这份配置，`TriggerManager` 一条规则都没有，
        // "系统触发器"那一段就永远不出现（run_all() 在没有待执行触发器时直接返回）。
        std::ofstream(test_root / "etc/lpkg/triggers.conf") << "^/usr/lib/.*\\.so.*\tldconfig\n";
        Cache::instance().load();
    }

    void TearDown() override
    {
        BreakpointManager::instance().clear_all();
        Config::instance().set_root_path("/");
        fs::remove_all(suite_work_dir);
    }

    /** 打一个包（可选带 postinst 钩子），返回 .lpkg 路径 */
    std::string create_pkg(const std::string& name, const std::string& ver,
                           const std::vector<std::string>& deps = {},
                           const std::vector<std::string>& provides = {},
                           const std::vector<std::string>& provides_soname = {},
                           bool with_postinst = false)
    {
        const fs::path work = suite_work_dir / ("_pkg_" + name + "-" + ver);
        fs::create_directories(work / "content" / "usr" / "bin");
        std::ofstream(work / "content" / "usr" / "bin" / name) << "#!/bin/sh\ntrue\n";
        // 再放一个 usr/lib 下的 .so：让 ldconfig 触发器真的入队 —— 于是"系统触发器"那一段
        // 会出现（`run_all()` 在没有待执行触发器时直接返回、连 section 都不打）。
        fs::create_directories(work / "content" / "usr" / "lib");
        std::ofstream(work / "content" / "usr" / "lib" / ("lib" + name + ".so.1")) << "x\n";
        if (with_postinst) {
            fs::create_directories(work / "hooks");
            std::ofstream(work / "hooks" / std::string(constants::POSTINST_SH))
                << "#!/bin/sh\ntrue\n";
        }
        const std::string path = (pkg_dir / std::format("{}-{}.lpkg", name, ver)).string();
        pack_package(path, work.string(), name, ver, deps, provides, provides_soname, "man " + name,
                     {});
        fs::remove_all(work);
        return path;
    }

    void add_to_mirror(const std::string& name, const std::string& ver)
    {
        fs::create_directories(mirror_dir / name);
        fs::copy_file(pkg_dir / (std::format("{}-{}.lpkg", name, ver)),
                      mirror_dir / name / (ver + ".lpkg"), fs::copy_options::overwrite_existing);
    }

    /** 索引行：`name|ver:hash:deps:provides:needed_so;ver2:…|` */
    void update_index(const std::vector<std::tuple<std::string, std::string, std::string,
                                                   std::string, std::string>>& entries)
    {
        std::ofstream index(mirror_dir / "index.txt");
        for (const auto& [name, ver, deps, provides, provides_soname] : entries) {
            const fs::path p = pkg_dir / (std::format("{}-{}.lpkg", name, ver));
            const std::string hash = fs::exists(p) ? calculate_sha256(p) : "unknown";
            index << name << "|" << ver << ":" << hash << ":" << deps << ":" << provides << ":"
                  << provides_soname << ":|\n";
        }
    }

    /// 渲染后的阶段标题那一行（`:: <标题>`；非 TTY 测试里日志前缀就是裸文本）
    static std::string section_line(const char* key)
    {
        return get_string("info.log_prefix") + " " + get_string(key);
    }

    /** RAII 捕获 stdout（异常路径下也不泄漏 gtest 的捕获器） */
    struct CaptureOut {
        std::string out;
        bool stopped = false;
        CaptureOut()
        {
            testing::internal::CaptureStdout();
        }
        void stop()
        {
            if (!stopped) {
                out = testing::internal::GetCapturedStdout();
                stopped = true;
            }
        }
        ~CaptureOut()
        {
            stop();
        }
        CaptureOut(const CaptureOut&) = delete;
        CaptureOut& operator=(const CaptureOut&) = delete;
    };
};

TEST_F(PhaseSectionTest, SectionsAppearInOrderAndOneSummaryAtTheEnd)
{
    create_pkg("ps_lib", "1.0", {}, {}, {"libps.so.1"});
    create_pkg("ps_app", "1.0", {"ps_lib"}, {});
    add_to_mirror("ps_lib", "1.0");
    add_to_mirror("ps_app", "1.0");
    update_index({{"ps_lib", "1.0", "", "", "libps.so.1"}, {"ps_app", "1.0", "ps_lib", "", ""}});

    CaptureOut cap;
    ASSERT_NO_THROW(install_packages({"ps_app"}));
    cap.stop();
    const std::string& out = cap.out;

    // ① 四个阶段按序出现（"准备"那个旧标题已不存在 —— 它的 key 已删，编译期就钉住了）
    const auto at = [&](const char* key) { return out.find(section_line(key)); };
    const auto download = at("ui.section_download");
    const auto extract = at("ui.section_extract");
    const auto install = at("ui.section_install");
    const auto triggers = at("ui.section_triggers");
    ASSERT_NE(download, std::string::npos) << out;
    ASSERT_NE(extract, std::string::npos) << out;
    ASSERT_NE(install, std::string::npos) << out;
    ASSERT_NE(triggers, std::string::npos) << out;
    EXPECT_LT(download, extract) << "阶段顺序：下载 先于 解压";
    EXPECT_LT(extract, install) << "阶段顺序：解压 先于 安装";
    EXPECT_LT(install, triggers) << "阶段顺序：安装 先于 触发器";

    // ③ 只有一条 summary，且名单里两个包都在（模板里 `{0}` 是数量）
    const std::string summary_head = string_format("info.install_summary", 2, "");
    const std::size_t first = out.find(summary_head);
    ASSERT_NE(first, std::string::npos) << "没有那条 summary：\n" << out;
    EXPECT_EQ(out.find(summary_head, first + 1), std::string::npos) << "summary 不该出现两次";
    EXPECT_NE(out.find("ps_app 1.0", first), std::string::npos) << out;
    EXPECT_NE(out.find("ps_lib 1.0", first), std::string::npos) << out;
}

TEST_F(PhaseSectionTest, PostInstallSectionOnlyWhenAPackageShipsOne)
{
    // ② 不带 hooks 的包：**不出**"运行安装后钩子"这一节
    create_pkg("ps_nohook", "1.0");
    add_to_mirror("ps_nohook", "1.0");
    update_index({{"ps_nohook", "1.0", "", "", ""}});

    CaptureOut cap1;
    ASSERT_NO_THROW(install_packages({"ps_nohook"}));
    cap1.stop();
    EXPECT_EQ(cap1.out.find(section_line("ui.section_postinst")), std::string::npos)
        << "没有包带 postinst.sh 时不该出现该阶段：\n"
        << cap1.out;

    // 带 postinst 的包：出现该阶段，且钩子行落在它之后
    create_pkg("ps_hook", "1.0", {}, {}, {}, /*with_postinst=*/true);
    add_to_mirror("ps_hook", "1.0");
    update_index({{"ps_nohook", "1.0", "", "", ""}, {"ps_hook", "1.0", "", "", ""}});

    CaptureOut cap2;
    ASSERT_NO_THROW(install_packages({"ps_hook"}));
    cap2.stop();
    const std::size_t sec = cap2.out.find(section_line("ui.section_postinst"));
    ASSERT_NE(sec, std::string::npos) << "带 postinst.sh 的包必须出该阶段：\n" << cap2.out;
    EXPECT_NE(cap2.out.find(get_string("hook.name.postinst"), sec), std::string::npos)
        << "钩子行必须落在该阶段之后：\n"
        << cap2.out;
}

TEST_F(PhaseSectionTest, MetadataMismatchAbortsBeforeTheTransactionStarts)
{
    // ④ 索引说 app 没有依赖，归档里却有 → 在"下载"阶段就拒绝；事务**根本没开**
    create_pkg("ps_dep", "1.0");
    create_pkg("ps_drift", "1.0", {"ps_dep"});
    add_to_mirror("ps_dep", "1.0");
    add_to_mirror("ps_drift", "1.0");
    update_index(
        {{"ps_dep", "1.0", "", "", ""}, {"ps_drift", "1.0", "", "", ""}});  // deps 故意写空

    bool began = false;
    BreakpointManager::instance().set("install_after_begin_ps_drift", [&] { began = true; });

    CaptureOut cap;
    try {
        install_packages({"ps_drift"});
        FAIL() << "元数据不一致必须拒绝安装";
    } catch (const LpkgException& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("deps"), std::string::npos) << "差异字段要点名：" << msg;
    }
    cap.stop();

    EXPECT_FALSE(began)
        << "事务不该开始（`install_after_begin_*` 命中 = BEGIN 已写、文件即将落地）\n"
        << cap.out;
    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("ps_drift"));
    BreakpointManager::instance().clear_all();
}
