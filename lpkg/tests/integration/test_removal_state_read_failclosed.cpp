/**
 * test_removal_state_read_failclosed.cpp — 移除路径读「反向依赖状态文件」失败必须 fail-closed
 *
 * 背景（2026-10-03 改）：`remove_package_files`（`main/src/pkg/package_manager.cpp`）清理本包
 * 写下的**反向依赖边**时，要读两个以包名命名的状态文件：
 *   · `deps/<pkg>`      —— 本包声明的依赖（用于把它从别包的 reverse-dep 表里摘掉）；
 *   · `needed_so/<pkg>` —— 本包需要的 SONAME（用于把它从提供者的 reverse-dep 表里摘掉）。
 *
 * 改动前守卫**只判存在**（`exists_follow`）：文件存在却**打不开**（FIFO / 设备 / 权限）或
 * **读不出**（是目录 —— Linux 下 `ifstream` 打开目录成功、随后 `getline` 才失败；或 EIO）时，
 * 读取循环静默跑零次 ⇒ 被当成"这个包没有反向依赖" ⇒ 那些边**永远留在 DB 里**（陈旧反向依赖，
 * 之后会错误地挡住别的包卸载）。这与**升级侧**读同一个文件做同一件事
 * （`installation_task_register.cpp` 摘除旧反向依赖）的处置必须一致。
 *
 * 现行为（本文件钉住）：fail-closed —— 点名**文件路径**报错
 * （`error.open_file_failed` 或 `error.read_file_failed`）、整批回滚、**包仍是已安装状态**。
 *
 * ── 每条断言"在什么缺陷下会红" ──
 *   · `EXPECT_THROW(..., LpkgException)` —— 若守卫退回"只判存在、读不出当空"，移除会**成功**，
 *     断言红（这正是本次要钉的旧行为）。
 *   · 报错锚点（文件路径 + l10n 模板字面）—— 若抛的是**别的**原因（包没找到 / 路径写错），
 *     路径锚对不上，断言红；只断言"抛了"无法把两者区分开。
 *   · "包仍已安装" —— 若移除**部分生效却没回滚**（边被摘了、包却还在或反之），或整批未回滚，
 *     断言红。
 *   · 对照组（状态文件正常 → 移除必须成功且包消失）—— 若 fixture 本身在这套环境里根本卸不掉
 *     这个包，上面的"抛错"就会以**错误的原因**变绿；对照组正是排除这种假绿。
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

namespace
{
/** 取 l10n 模板里最长的一段**字面**文本（`{}` 之间）—— 用作与语言无关的断言锚。 */
std::string longest_template_literal(const std::string& tmpl)
{
    std::string needle;
    for (size_t b = 0; b <= tmpl.size();) {
        const size_t e = tmpl.find("{}", b);
        const size_t end = (e == std::string::npos) ? tmpl.size() : e;
        if (end - b > needle.size()) needle = tmpl.substr(b, end - b);
        if (e == std::string::npos) break;
        b = e + 2;
    }
    return needle;
}
}  // namespace

class RemovalStateReadFailClosedTest : public IntegrationTestBase
{
protected:
    /**
     * 把某个状态文件位置换成**目录**：这是"文件存在、名字对、但读不出来"最现实的可构造形态
     * —— `ifstream` 打开目录会成功（`is_open()` 为真），随后第一次 `getline` 才失败
     * （`error.read_file_failed`）。若某些平台 `open` 就失败，则落到 `error.open_file_failed`，
     * 两条腿都由本用例的断言覆盖。
     */
    static void turn_into_directory(const fs::path& p)
    {
        std::error_code ec;
        fs::remove_all(p, ec);  // 装包可能已建了一个普通文件（也可能是空的），先清掉
        ASSERT_FALSE(ec) << "清理旧状态文件失败: " << ec.message();
        ASSERT_TRUE(fs::create_directories(p, ec))
            << "无法把 " << p << " 建成目录: " << ec.message();
        ASSERT_FALSE(ec);
    }

    /** 跑一次"本该被拒绝"的移除并取回报错文本（只断言 THROW 不足以区分拒绝原因）。 */
    static std::string removal_refusal(const std::string& pkg)
    {
        try {
            remove_packages({pkg});
        } catch (const LpkgException& e) {
            return e.what();
        } catch (const std::exception& e) {
            ADD_FAILURE() << "移除抛的不是 LpkgException：" << e.what();
            return {};
        }
        ADD_FAILURE() << "读不出反向依赖状态文件时，移除本该 fail-closed 却成功了";
        return {};
    }

    /** 报错必须**点名那个文件路径**，且措辞必须是"打开/读取失败"之一（不是别的原因）。 */
    void expect_names_file_and_reason(const std::string& msg, const fs::path& file) const
    {
        EXPECT_NE(msg.find(file.string()), std::string::npos)
            << "报错没点名读状态文件的路径 " << file << "：\n"
            << msg;
        // 取**值**再取锚（`get_string` 返回引用、以字面量临时量调用它绑引用会撞 GCC 的
        // `-Wdangling-reference` 误报 —— 见 installation_task.cpp 里同款订正）。
        const std::string open_tmpl = get_string("error.open_file_failed");
        const std::string read_tmpl = get_string("error.read_file_failed");
        const std::string open_anchor = longest_template_literal(open_tmpl);
        const std::string read_anchor = longest_template_literal(read_tmpl);
        ASSERT_GE(open_anchor.size(), 8u);
        ASSERT_GE(read_anchor.size(), 8u);
        EXPECT_TRUE(msg.find(open_anchor) != std::string::npos ||
                    msg.find(read_anchor) != std::string::npos)
            << "报错理由不是 open/read 文件失败（锚: '" << open_anchor << "' / '" << read_anchor
            << "'）：\n"
            << msg;
    }
};

// ============================================================================
// deps/<pkg> 存在但读不出来 → fail-closed
// ============================================================================
TEST_F(RemovalStateReadFailClosedTest, UnreadableDepStateFailsClosed)
{
    const std::string pkg = "rmfc_dep";
    const std::string pkg_path = create_pkg(pkg, "1.0");
    ASSERT_NO_THROW(install_packages({pkg_path}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed(pkg)) << "前置：包应已安装";

    const fs::path dep_file = Config::instance().dep_dir() / pkg;
    turn_into_directory(dep_file);
    ASSERT_TRUE(fs::is_directory(dep_file)) << "前置：状态文件已换成目录";

    const std::string msg = removal_refusal(pkg);

    // ① 报错点名**那个文件** + ② 理由是"打开/读取失败"（不是别的失败）
    expect_names_file_and_reason(msg, dep_file);

    // ③ 整批回滚：包仍是已安装状态（陈旧边也不会被静默摘掉）
    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed(pkg))
        << "拒绝时包必须原封不动（整批回滚），实际已被移除：\n"
        << msg;
}

// ============================================================================
// needed_so/<pkg> 存在但读不出来 → fail-closed（第二条腿；dep_file 正常，所以先走到这里）
// ============================================================================
TEST_F(RemovalStateReadFailClosedTest, UnreadableNeededSoStateFailsClosed)
{
    const std::string pkg = "rmfc_nso";
    const std::string pkg_path = create_pkg(pkg, "1.0");
    ASSERT_NO_THROW(install_packages({pkg_path}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed(pkg)) << "前置：包应已安装";

    // 只破坏 needed_so 那条腿：dep_file 保持正常（若不存在即"无依赖"，读取被跳过），
    // 于是读 dep_file 这一步必然通过，报错只可能来自 needed_so 这次读取。
    const fs::path nso_file = Config::instance().needed_so_dir() / pkg;
    turn_into_directory(nso_file);
    ASSERT_TRUE(fs::is_directory(nso_file)) << "前置：状态文件已换成目录";

    const std::string msg = removal_refusal(pkg);

    expect_names_file_and_reason(msg, nso_file);

    Cache::instance().load();
    EXPECT_TRUE(Cache::instance().is_installed(pkg)) << "拒绝时包必须原封不动（整批回滚）：\n"
                                                     << msg;
}

// ============================================================================
// 对照组：状态文件正常 → 移除必须成功、包必须消失
//
// 没有这一条，"上面那条抛错了"在"这个 fixture 本来就在这套环境里卸不掉这个包"时也会绿。
// ============================================================================
TEST_F(RemovalStateReadFailClosedTest, NormalStateRemovesCleanly)
{
    const std::string pkg = "rmfc_ok";
    const std::string pkg_path = create_pkg(pkg, "1.0");
    ASSERT_NO_THROW(install_packages({pkg_path}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed(pkg)) << "前置：包应已安装";

    EXPECT_NO_THROW(remove_packages({pkg})) << "状态文件正常时移除不得报错";

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed(pkg)) << "成功移除后包不应仍在册";
}
