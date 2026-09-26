/**
 * test_soname_links_loop.cpp —— `apply_soname_links()` 必须能跳过符号链接环
 *
 * ── 缺陷（实测，2026-09-26）────────────────────────────────────────────────
 * `main/src/elf/lib_utils.cpp` 的扫描趟曾写 `if (!entry.is_regular_file()) continue;`，
 * 而 `directory_entry::is_regular_file()` 走 `status()`（**跟随**末段链接），对符号链接环
 * 抛 `filesystem_error code=40 (ELOOP)`（前提见 `tests/unit/test_symlink_loop_paths.cpp`
 * 的 Premise* 用例）。**它的两个调用点传的都是包内容**：
 *   · `trigger/trigger.cpp:117` → 目标 root 的 `<root>/usr/lib`（**提交之后**才跑）
 *   · `build/builder.cpp:169`   → 构建 staging 的 `<staging>/usr/lib`
 * 后果不是"某个操作失败"，而是：盘上 `/usr/lib` 只要有一个环（两个包各出一个悬空链接互指
 * 就能造出来，无需 root 手工干预），任何提供 `<root>/usr/lib` 下共享库的包**装完之后**在触发器里抛
 * —— 包已落地、DB 已提交，命令却报失败。构建侧同理（打包时 staging 里的环）。
 *
 * ── 本文件钉两件事 ─────────────────────────────────────────────────────────
 *   ① 有环时**不抛**（回归项）；
 *   ② 有环时**扫描不中断**：同一个目录里那个正常的库仍要被处理、SONAME 链接仍要建出来。
 *      这一条防的是"用 lstat 语义一刀切"的过度修复 —— `libfoo.so -> libfoo.so.1.2.3`
 *      这类链接本就该被本函数处理（修正指错的 SONAME 链接正是它的职责），
 *      把"跟随"换成"不跟随"会静默丢掉一整段行为。
 */

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../../main/src/elf/lib_utils.hpp"

namespace fs = std::filesystem;

class SonameLinksLoopTest : public ::testing::Test
{
protected:
    fs::path work;

    void SetUp() override
    {
        work = fs::temp_directory_path() / ("lpkg_soname_loop_" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(work, ec);
        fs::create_directories(work);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(work, ec);
    }

    static void write_file(const fs::path& p, const std::string& content)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << content;
    }
};

TEST_F(SonameLinksLoopTest, LoopEntryIsSkippedAndScanContinues)
{
    const fs::path lib = work / "lib";
    fs::create_directories(lib);

    // 正常的库：真编译一个带 SONAME 的 .so。文件名（libprobe.so.1.2.3）与 SONAME
    // （libprobe.so.1）不同 → 本函数应当建出 `libprobe.so.1 -> libprobe.so.1.2.3`。
    write_file(work / "probe.c", "int probe(void) { return 42; }\n");
    const fs::path so = lib / "libprobe.so.1.2.3";
    const std::string cmd = "gcc -shared -fPIC -Wl,-soname,libprobe.so.1 -o " + so.string() + " " +
                            (work / "probe.c").string() + " 2>/dev/null";
    ASSERT_EQ(std::system(cmd.c_str()), 0) << "前置：编译 .so 失败（缺 gcc？）";
    ASSERT_TRUE(fs::is_regular_file(so));

    // 环：自指。它解不开（ELOOP），但**只是个环**，不该有能力打断扫描。
    fs::create_symlink("self", lib / "self");

    // ① 不抛
    EXPECT_NO_THROW(apply_soname_links(lib))
        << "盘上有符号链接环时不该抛：本函数在**提交之后**的触发器里跑，"
           "抛了就是「包已落地、DB 已提交，命令却报失败」";

    // ② 扫描没被中断：正常库的 SONAME 链接仍应被建出来
    const fs::path link = lib / "libprobe.so.1";
    ASSERT_TRUE(fs::is_symlink(link))
        << "环不该让扫描中断：同目录里正常的库仍要被处理，SONAME 链接仍要建出来";
    EXPECT_EQ(fs::read_symlink(link).string(), "libprobe.so.1.2.3");

    // 环本身原样留着（本函数只生成/修正 SONAME 链接，不清理无关条目）
    std::error_code ec;
    EXPECT_TRUE(fs::is_symlink(lib / "self", ec) && !ec) << "无关的环不该被本函数删掉";
}
