/**
 * test_query_exit_code.cpp — `lpkg query -p <未安装的包>` 必须**非零退出**且用 query 语境的文案
 *
 * 背景（2026-10-03 改）：`query_package()`（`main/src/pkg/package_manager.cpp`）此前在包未安装时
 * 复用 `info.package_not_installed`（那是 **remove** 语境的文案，意思是"…没有可移除的"）并
 * **静默返回** ⇒ 退出码 0。脚本/farm 拿到的信号是"查询成功、只是这个包没文件"，与"这包根本没装"
 * 无法区分。改动后：抛 `LpkgException`（文案 `error.query_package_not_installed`），由 `run_cli`
 * 的 catch 落成**退出码 1** —— 不需要 `main_cli` 侧改动（`run_query_command` 直接调本函数、
 * 不吞异常）。
 *
 * ── 每条断言"在什么缺陷下会红" ──
 *   · 未安装 query 退出码 `== 1` —— 若退回"静默返回 0"（旧行为）则红。
 *   · 输出含 `error.query_package_not_installed` 的**字面片段** —— 若又复用 remove 语境的
 *     `info.package_not_installed`（"…no need to remove"），query 的模板锚对不上则红；
 *     只断言"非零退出"无法把"报对了原因"与"报了别的错"区分开。
 *   · 输出含**包名** —— 报错必须能定位到具体是哪个包（CLAUDE.md §8 第 7 条）。
 *   · 对照组：已安装包的 query 必须退 **0** —— 防"query 一律退 1"也能让上面那条绿。
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <format>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/main_cli.hpp"
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

/** 跑一次 CLI，捕获两个流（消息走 stdout/stderr 都是实现细节）。 */
struct CliRun {
    int code = 0;
    std::string out;
    std::string err;
    std::string all() const
    {
        return out + err;
    }
};

CliRun run_cli_captured(const std::vector<std::string>& argv)
{
    std::ostringstream out_buf;
    std::ostringstream err_buf;
    auto* old_out = std::cout.rdbuf(out_buf.rdbuf());
    auto* old_err = std::cerr.rdbuf(err_buf.rdbuf());
    int code = 0;
    try {
        code = run_cli(argv);
    } catch (...) {
        std::cout.rdbuf(old_out);
        std::cerr.rdbuf(old_err);
        throw;
    }
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    return {code, out_buf.str(), err_buf.str()};
}
}  // namespace

class QueryExitCodeTest : public IntegrationTestBase
{
};

TEST_F(QueryExitCodeTest, QueryingUninstalledPackageExitsNonZero)
{
    const std::string pkg = "qec_absent";

    const CliRun r = run_cli_captured({"lpkg", "--root", test_root.string(), "query", "-p", pkg});

    // ① 退出码 1（此前是 0）
    EXPECT_EQ(r.code, 1) << "查询未安装的包必须非零退出，实际为 0：\n" << r.all();

    // ② 文案来自 query 语境（锚取模板里最长字面片段，与语言无关）。
    //    取**值**再取锚：`get_string` 返回引用，以临时字面量调用它绑引用会撞 GCC 的
    //    `-Wdangling-reference` 误报（本仓库已有同款订正）。
    const std::string query_tmpl = get_string("error.query_package_not_installed");
    // ⚠️ `string_format()` 收的是**key**（内部 `get_string(key)` 再 `vformat`），不是模板 ——
    // 拿已经取出来的模板去调它会走成"查一个不存在的 key"，返回 `[MISSING_STRING: …]`（而占位符
    // 里恰好还留着 `{}`，会被 vformat 填上，看起来像"渲染成功了"，极具误导）。
    // 这里要的是"模板 + 实参"，所以直接 `std::vformat`。
    const std::string rendered = std::vformat(query_tmpl, std::make_format_args(pkg));
    const std::string anchor = longest_template_literal(query_tmpl);
    ASSERT_GE(anchor.size(), 8u) << "l10n 模板里没有足够长的字面片段可作锚";
    EXPECT_NE(r.all().find(rendered), std::string::npos)
        << "应打印 error.query_package_not_installed 的渲染结果：\n"
        << "  期望片段: [" << rendered << "]\n"
        << "  实际输出: [" << r.all() << "]";
    EXPECT_NE(r.all().find(anchor), std::string::npos)
        << "  锚: [" << anchor << "]\n  实际输出: [" << r.all() << "]";

    // ③ 点名包
    EXPECT_NE(r.all().find(pkg), std::string::npos) << "报错必须点名是哪个包：\n" << r.all();

    // ④ 不得泄漏 remove 语境的文案（那是本次改掉的耦合）
    const std::string remove_tmpl = get_string("info.package_not_installed");
    const std::string remove_anchor = longest_template_literal(remove_tmpl);
    if (!remove_anchor.empty()) {
        EXPECT_EQ(r.all().find(remove_anchor), std::string::npos)
            << "查询报错不该复用 remove 语境的文案：\n"
            << r.all();
    }
}

TEST_F(QueryExitCodeTest, QueryingInstalledPackageExitsZero)
{
    // 对照组：防"query 一律退 1"也能让上一条绿。
    const std::string pkg = "qec_installed";
    const std::string pkg_path = create_pkg(pkg, "1.0");
    ASSERT_NO_THROW(install_packages({pkg_path}));
    Cache::instance().load();
    ASSERT_TRUE(Cache::instance().is_installed(pkg)) << "前置：包应已安装";

    const CliRun r = run_cli_captured({"lpkg", "--root", test_root.string(), "query", "-p", pkg});

    EXPECT_EQ(r.code, 0) << "查询已安装的包必须退 0：\n" << r.all();

    const std::string files_tmpl = get_string("info.package_files");
    const std::string anchor = longest_template_literal(files_tmpl);
    ASSERT_GE(anchor.size(), 8u);
    EXPECT_NE(r.all().find(anchor), std::string::npos) << "已安装包应打印文件清单表头：\n"
                                                       << r.all();
    EXPECT_NE(r.all().find(pkg), std::string::npos) << r.all();
}
