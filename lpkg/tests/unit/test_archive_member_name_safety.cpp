/**
 * test_archive_member_name_safety.cpp — 危险归档成员名必须**整包拒绝**
 *
 * 归档成员名是不可信输入，而它会经 `scan_content_files` → file_db → 安装期 OpSink 变成
 * **WAL 行的字面内容**（`op + " " + src.string() + " → " + bak.string()`，op_sink.cpp）。
 * WAL 是行式协议：写侧 `line + "\n"`，读侧 std::getline 逐行再 parse_op 分帧，两侧都不转义。
 * 于是：
 *   · 名字里的 `\n` 会把一行切成两行，第二行成为**独立可解析**的 WAL 行 —— 成员名
 *     `usr/share/x\nDIR_RM /etc 511 0 0` 造出的第二行是一条合法 DIR_RM，而
 *     `reverse_execute()` 的 DIR_RM 分支**没有任何路径 confinement**（照行里的绝对路径
 *     create_directories + chmod/lchown），崩溃恢复/回滚会以 root 改任意绝对路径的权限/属主，
 *     `--root` 隔离失效；
 *   · 名字里的字面 `" → "` 会破坏箭头分帧，非箭头类型的 arg1 被截断成前缀，回滚作用到
 *     "前缀同名"的**别的路径**上。
 * 两种伤害都发生在解压**之后**（WAL 层无从分辨），唯一能挡的地方就是名字进入系统之前，
 * 所以守卫在解压侧：命中即拒绝整个归档（而不是跳过该成员 —— 静默跳过只会留下一个"装了一半、
 * 某文件莫名消失"的包）。
 *
 * 反向也要钉住：只是"看起来可疑"的名字（含空格、箭头两侧没空格、`.lpkgnew` 不在末尾）
 * 必须照常解出来 —— 守卫过严会把合法包一起拒了。
 */

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/archive/archive.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/i18n/localization.hpp"

namespace fs = std::filesystem;

class ArchiveMemberNameSafetyTest : public ::testing::Test
{
protected:
    fs::path suite_dir;
    fs::path out_dir;
    fs::path pkg;

    void SetUp() override
    {
        init_localization();
        suite_dir = fs::absolute("tmp_archive_member_name_test");
        fs::remove_all(suite_dir);
        out_dir = suite_dir / "out";
        fs::create_directories(out_dir);
        pkg = suite_dir / "evil.lpkg";
    }

    void TearDown() override
    {
        fs::remove_all(suite_dir);
    }

    struct Member {
        std::string name;
        std::string data;
        std::string hardlink;  // 非空则写成硬链接（内容是"目标"）
        bool is_dir = false;
    };

    /** 用 libarchive 写一个 tar（读取侧 support_filter_all，不压缩也读得动） */
    void write_archive(const std::vector<Member>& members)
    {
        struct archive* a = archive_write_new();
        archive_write_set_format_pax_restricted(a);
        archive_write_open_filename(a, pkg.c_str());
        for (const auto& m : members) {
            struct archive_entry* e = archive_entry_new();
            archive_entry_set_pathname(e, m.name.c_str());
            if (m.is_dir) {
                archive_entry_set_filetype(e, AE_IFDIR);
                archive_entry_set_perm(e, 0755);
            } else {
                archive_entry_set_filetype(e, AE_IFREG);
                archive_entry_set_perm(e, 0644);
                archive_entry_set_size(e, static_cast<la_int64_t>(m.data.size()));
                if (!m.hardlink.empty()) archive_entry_set_hardlink(e, m.hardlink.c_str());
            }
            archive_write_header(a, e);
            if (!m.data.empty() && m.hardlink.empty() && !m.is_dir)
                archive_write_data(a, m.data.data(), m.data.size());
            archive_entry_free(e);
        }
        archive_write_close(a);
        archive_write_free(a);
    }

    /** 解压，返回异常消息；没抛则返回空串 */
    std::string extract_error()
    {
        try {
            extract_tar_zst(pkg, out_dir, pkg.filename().string());
        } catch (const LpkgException& e) {
            return e.what();
        }
        return {};
    }

    static size_t count_entries(const fs::path& dir)
    {
        size_t n = 0;
        for (const auto& e : fs::directory_iterator(dir)) {
            (void)e;
            ++n;
        }
        return n;
    }

    /**
     * 某个 l10n 键的**字面前缀**（模板在第一个占位符处截断）。
     *
     * 用来断言"拒绝的**原因**是守卫"，而不是碰巧因为别的失败而中止 —— 否则用例只是
     * "不抛就行/抛了就行"的空转（实测：硬链接用例在修复前的代码上也能通过，因为
     * libarchive 自己会因"硬链接目标不存在"而失败，异常一样非空）。
     * 取前缀而不是整条：语言无关，且不必把 strerror/路径拼进期望值。
     */
    static std::string key_head(const char* key)
    {
        const std::string tmpl = get_string(key);
        return tmpl.substr(0, tmpl.find("{}"));
    }
};

// ============================================================================
// 1. 换行：会把 WAL 行切成两行，第二行可成为合法 DIR_RM → 整包拒绝
// ============================================================================

TEST_F(ArchiveMemberNameSafetyTest, NewlineInMemberNameIsRejectedAndNothingIsExtracted)
{
    // 成员名里的 `\n` 之后跟一条**语法完全合法**的 DIR_RM 行（mode=511，即八进制 0777 ——
    // 写成十进制是因为 WAL 行里记的是十进制）。 若它进了 WAL，回滚/崩溃恢复就会 chmod + chown 掉
    // /etc —— 与 --root 无关的绝对路径。
    write_archive({
        {"usr/share/x\nDIR_RM /etc 511 0 0", "pwned", "", false},
        {"usr/share/ok.txt", "ok", "", false},
    });

    const std::string err = extract_error();
    ASSERT_FALSE(err.empty()) << "含换行的成员名必须被拒绝（throw），而不是照解不误";
    // 恶意成员排在**第一个**，所以拒绝发生在写出任何东西之前（解压是流式的：已经写出的成员
    // 不会回滚，见硬链接用例对"中止"语义的断言）。
    EXPECT_EQ(count_entries(out_dir), 0u)
        << "恶意成员之前不该有东西落盘（它是第一个成员）：拒绝必须发生在写任何文件之前";
    // 拒绝的**原因**必须是守卫（而不是碰巧别的失败中止）：见 key_head 的说明
    EXPECT_NE(err.find(key_head("error.unsafe_member_control")), std::string::npos)
        << "拒绝原因不是控制字符守卫：" << err;
    // 报错消息自己也不能被换行切碎 —— 否则日志/CLI 的行式消费会重演同一个 bug。
    EXPECT_EQ(err.find('\n'), std::string::npos) << "异常消息被成员名里的换行切成了多行：" << err;
    EXPECT_NE(err.find(pkg.string()), std::string::npos)
        << "错误消息必须点名是哪个归档（否则用户无从定位坏包）：" << err;
}

// ============================================================================
// 2. 回车：同样会破坏行式协议 → 拒绝
// ============================================================================

TEST_F(ArchiveMemberNameSafetyTest, CarriageReturnInMemberNameIsRejected)
{
    write_archive({
        {"usr/share/y\rDIR_RM /etc 511 0 0", "pwned", "", false},
    });

    EXPECT_FALSE(extract_error().empty()) << "含回车的成员名必须被拒绝";
    EXPECT_EQ(count_entries(out_dir), 0u);
}

// ============================================================================
// 3. 字面 " → "：破坏箭头分帧，arg1 被截断成前缀 → 回滚作用到别的路径 → 拒绝
// ============================================================================

TEST_F(ArchiveMemberNameSafetyTest, ArrowInMemberNameIsRejected)
{
    write_archive({
        {"usr/share/one \xe2\x86\x92 usr/share/two", "pwned", "", false},
    });

    const std::string err = extract_error();
    EXPECT_NE(err.find(key_head("error.unsafe_member_arrow")), std::string::npos)
        << "含字面 \" → \" 的成员名必须被**守卫**拒绝（而不是别的失败）：" << err;
    EXPECT_EQ(count_entries(out_dir), 0u);
}

// ============================================================================
// 4. 硬链接的**目标名**走同一条守卫（同一次解压的另一个入口）→ 处理在此中止
//
// 钉住的是"守卫在 member_path_relative 里、对所有调用点生效"这一点：若将来谁把它挪到
// 只判成员名的那一处，这条会红。
//
// 语义说明（写这条时踩过，故钉清楚）：解压是**流式**的，"拒绝整个归档"= 在命中处**中止**，
// 不是"回滚已写出的成员"。所以这里刻意让恶意成员**排在合法成员之后**：
//   · 它前面的 `usr/share/real` 已落盘（断言它存在，把"流式"这个既有语义钉住）；
//   · 它自己与它**之后**的成员都不许落盘 —— 后者才是"中止"的证据。
// 调用方（安装/构建）把解压失败当致命错误、临时目录整个作废，所以不会出现"装出半个包"。
// ============================================================================

TEST_F(ArchiveMemberNameSafetyTest, ArrowInHardlinkTargetAbortsExtraction)
{
    // 目标文件**预先存在**（解压根内）：这样"目标名被放行"时硬链接能真的建出来，两个世界
    // 才给出不同结果。若目标不存在，libarchive 自己也会因为"硬链接目标不存在"而失败 →
    // 异常一样非空 → 用例在**修复前**也照样通过（实测踩到：那是恒绿的空转）。
    // 目标名里的箭头必须**两侧带空格**才是被禁的那三个字节（`A→B` 不带空格是合法名字，
    // 见下面的正面对照用例）。踩过：写成 `tar→get` 时守卫（正确地）放行，用例断言的是
    // 一个根本不存在的危险名 —— 两个世界都红。
    const std::string target = "usr/share/tar \xe2\x86\x92 get";
    fs::create_directories(out_dir / "usr/share");
    std::ofstream(out_dir / target) << "pre-existing\n";

    write_archive({
        {"usr/share/real", "data", "", false},  // 恶意成员之前的合法成员
        // 注意 `"…\x92" "get"` 的**字面量拼接**：C++ 的 `\x` 转义会一路吃十六进制数字，
        // 写成 `"\x92g"` 会被解析成 `\x92g` 之外的字节（越界 → 编译警告 + 字节变样），
        // 名字就不是我们要测的那一个了。拼接（或后接空格）才能终止转义。
        {"usr/share/link", "",
         "usr/share/tar \xe2\x86\x92"
         " get",
         false},                                      // 硬链接目标名含 " → "
        {"usr/share/after.txt", "after", "", false},  // 恶意成员之后的成员
    });

    const std::string err = extract_error();
    EXPECT_NE(err.find(key_head("error.unsafe_member_arrow")), std::string::npos)
        << "硬链接**目标名**里的 \" → \" 未被守卫拦下（修复前这里会被放行、链接照建）：" << err;
    EXPECT_FALSE(fs::exists(out_dir / "usr/share/link")) << "含危险目标名的硬链接成员被写出来了";
    EXPECT_FALSE(fs::exists(out_dir / "usr/share/after.txt"))
        << "命中守卫后解压没有中止（它之后的成员照样被解出来了）";
    EXPECT_TRUE(fs::exists(out_dir / "usr/share/real"))
        << "流式语义：恶意成员**之前**已写出的成员不回滚（调用方负责作废整个临时目录）";
}

// ============================================================================
// 5. `.lpkgtmp` / `.lpkgnew`：lpkg 自用后缀，包声明同名成员会与落位撞名 → 拒绝
//    （目录条目带尾斜杠，判据要剥掉斜杠再比末段）
// ============================================================================

TEST_F(ArchiveMemberNameSafetyTest, ReservedLpkgSuffixesAreRejected)
{
    const std::vector<std::string> bad_names = {
        "usr/share/data.txt.lpkgtmp",  // 与 "<dst>.lpkgtmp → <dst>" 的 rename 撞名
        "usr/share/app.conf.lpkgnew",  // 与"配置冲突落 .lpkgnew 请用户审阅"撞名
        "usr/share/d.lpkgnew/",        // 目录条目（带尾斜杠）：末段判据必须剥掉斜杠
    };

    for (const auto& name : bad_names) {
        fs::remove_all(out_dir);
        fs::create_directories(out_dir);
        const bool dir = name.ends_with('/');
        write_archive({Member{name, dir ? "" : "x", "", dir}});

        const std::string err = extract_error();
        EXPECT_NE(err.find(key_head("error.unsafe_member_suffix")), std::string::npos)
            << "成员名 " << name << " 用了 lpkg 自用后缀，必须被**守卫**拒绝：" << err;
        EXPECT_EQ(count_entries(out_dir), 0u) << "成员名 " << name << " 被拒后仍有文件落盘";
    }
}

// ============================================================================
// 6. 正面对照：只是"看起来可疑"的名字必须照常解出来
//
// 守卫过严会拒掉合法包，所以每条规则都要有一个"不该命中"的对照：
//   · 含空格的名字（tar 成员名允许空格，WAL 分帧靠箭头/tail 字段，不靠空格）
//   · 箭头两侧**没有**空格的 `a→b`（不构成 " → "，不破坏分帧）
//   · `.lpkgnew`/`.lpkgtmp` 出现在**中间**或**后接字符**（只有末段整体以它结尾才算撞名）
//   · 多级子目录
// ============================================================================

TEST_F(ArchiveMemberNameSafetyTest, SuspiciousLookingButLegalNamesStillExtract)
{
    write_archive({
        {"usr/share/has space.txt", "1", "", false},
        {"usr/share/a\xe2\x86\x92"
         "b.txt",
         "2", "", false},  // 拼接：见硬链接用例的 `\x92a` 坑
        {"usr/share/lpkgnew.txt", "3", "", false},
        {"usr/share/x.lpkgnewx", "4", "", false},
        {"usr/share/deep/dir/file.txt", "5", "", false},
        {"usr/share/d.lpkgnewx/", "", "", true},
    });

    EXPECT_NO_THROW(extract_tar_zst(pkg, out_dir, pkg.filename().string()));

    const auto content = [](const fs::path& p) {
        std::ifstream f(p);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    EXPECT_EQ(content(out_dir / "usr/share/has space.txt"), "1");
    EXPECT_EQ(content(out_dir / "usr/share/a\xe2\x86\x92"
                                "b.txt"),
              "2");
    EXPECT_EQ(content(out_dir / "usr/share/lpkgnew.txt"), "3");
    EXPECT_EQ(content(out_dir / "usr/share/x.lpkgnewx"), "4");
    EXPECT_EQ(content(out_dir / "usr/share/deep/dir/file.txt"), "5");
    EXPECT_TRUE(fs::is_directory(out_dir / "usr/share/d.lpkgnewx"))
        << "末段后缀不匹配的目录名不该被拒";
}
