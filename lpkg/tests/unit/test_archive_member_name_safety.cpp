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
 * 反向也要覆盖：只是"看起来可疑"的名字（含空格、箭头两侧没空格、`.lpkgnew` 不在末尾）
 * 必须照常解出来 —— 守卫过严会把合法包一起拒了。
 */

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/archive/archive.hpp"
#include "../../main/src/archive/packer.hpp"
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
        suite_dir = fs::absolute("tmp_archive_member_name_test_" + std::to_string(::getpid()));
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
     * "不抛就行/抛了就行"的空转（硬链接用例在修复前的代码上也能通过，因为
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
// 固定的是"守卫在 member_path_relative 里、对所有调用点生效"这一点：若将来谁把它挪到
// 只判成员名的那一处，这条会红。
//
// 语义说明（写这条时踩过，故钉清楚）：解压是**流式**的，"拒绝整个归档"= 在命中处**中止**，
// 不是"回滚已写出的成员"。所以这里刻意让恶意成员**排在合法成员之后**：
//   · 它前面的 `usr/share/real` 已落盘（断言它存在，把"流式"这个既有语义固定下来）；
//   · 它自己与它**之后**的成员都不许落盘 —— 后者才是"中止"的证据。
// 调用方（安装/构建）把解压失败当致命错误、临时目录整个作废，所以不会出现"装出半个包"。
// ============================================================================

TEST_F(ArchiveMemberNameSafetyTest, ArrowInHardlinkTargetAbortsExtraction)
{
    // 目标文件**预先存在**（解压根内）：这样"目标名被放行"时硬链接能真的建出来，两个世界
    // 才给出不同结果。若目标不存在，libarchive 自己也会因为"硬链接目标不存在"而失败 →
    // 异常一样非空 → 用例在**修复前**也照样通过（踩到过：那是恒绿的空转）。
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
//    （判据按 `/` 切分量逐段判后缀；目录条目的尾斜杠由"空分量"覆盖）
// ============================================================================

TEST_F(ArchiveMemberNameSafetyTest, ReservedLpkgSuffixesAreRejected)
{
    const std::vector<std::string> bad_names = {
        "usr/share/data.txt.lpkgtmp",  // 与 "<dst>.lpkgtmp → <dst>" 的 rename 撞名
        "usr/share/app.conf.lpkgnew",  // 与"配置冲突落 .lpkgnew 请用户审阅"撞名
        "usr/share/d.lpkgnew/",        // 目录条目（带尾斜杠）：切分量后照样命中
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
//   · `.lpkgnew`/`.lpkgtmp` 出现在**任一命名分量里、但该分量不以它结尾**（判据是"某分量
//     以这三个后缀结尾"，不是子串匹配；分量整体以它结尾的另见"任一分量"那条用例）
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

// ============================================================================
// 6. 成员名消毒抛的是 **UnsafeArchiveException**（子类）—— 构建期自动解压据此区分
//    "普通失败（容忍）"与"安全拒绝（必须放行）"
// ============================================================================

TEST_F(ArchiveMemberNameSafetyTest, MemberNameRejectionIsAnUnsafeArchiveException)
{
    // `UnsafeArchiveException` 是 `LpkgException` 的**子类**：所有既有的
    // `catch (const LpkgException&)` 照旧接得住，而构建期"自动解压源码归档"额外加一条
    // 更具体的 catch 把它 rethrow（见 base/exception.hpp 与 builder_executor.cpp）。
    write_archive({{"content/evil\nname", "x", "", false}});

    EXPECT_THROW(extract_tar_zst(pkg, out_dir, pkg.filename().string()), UnsafeArchiveException);
}

// ============================================================================
// 7. 打包侧对称：`pack` 不得产出**自己的 extractor 会拒收**的包
//
// 解压侧的拒绝发生在下游（装包/建包时才发现），那时坏包已经传出去了 —— 该出声的地方是
// 产生它的那一刻。判据本体是两处**共用**的同一个函数，所以这里钉的是"打包侧真的调了它"，
// 以及"消息点名了是哪个输出文件"。
// ============================================================================

TEST_F(ArchiveMemberNameSafetyTest, PackerRefusesNamesItsOwnExtractorWouldReject)
{
    const fs::path src = suite_dir / "pkgsrc";
    fs::create_directories(src / "content/usr/share");
    // 换行是 Linux 上**合法的文件名字符**，但不含 `/`（含 `/` 的话 fs::path 会把它当分隔符，
    // ofstream 静默失败、文件根本没建起来 —— 踩到过：那样 pack 无物可打，用例变成恒真空转）。
    // 解压侧那条用例能直接用 libarchive 造任意名字，打包侧必须先有**真的文件**。
    const fs::path evil_rel = "usr/share/x\npwned";
    {
        std::ofstream f(src / "content" / evil_rel);
        ASSERT_TRUE(f.is_open()) << "测试场景没建起来（名字里只该有换行，不该有 /）";
        f << "pwned\n";
    }
    ASSERT_TRUE(fs::exists(src / "content" / evil_rel))
        << "测试场景没建起来 —— 没有这个文件，下面的「没抛」断言就成了恒真空转";

    const fs::path out = suite_dir / "evil_out.lpkg";
    std::string err;
    try {
        pack_package(out.string(), src.string(), "evil", "1.0");
    } catch (const LpkgException& e) {
        err = e.what();
    }

    ASSERT_FALSE(err.empty()) << "含换行的成员名必须在**打包**时就被拒绝，而不是留给下游";
    EXPECT_NE(err.find(key_head("error.unsafe_member_control")), std::string::npos)
        << "打包侧的拒绝原因不是控制字符守卫（共用的判据没生效？）：" << err;
    EXPECT_NE(err.find(out.string()), std::string::npos)
        << "消息必须点名**输出文件**（用户要知道哪个包没打成）：" << err;
    // 半成品必须丢弃：残留的截断 .lpkg 会被当成有效包，其哈希也算得出来，farm 会把它写进索引
    EXPECT_FALSE(fs::exists(out)) << "打包失败后留下了半成品 .lpkg";
}

// 正面对照：只是"看起来可疑"的名字必须照常打得出来 —— 否则守卫过严会把合法包一起拒了。
// 顺带证明**对称性真的成立**：产物能被自己的 extractor 原样读回来。
TEST_F(ArchiveMemberNameSafetyTest, PackerAcceptsLegalSuspiciousNames)
{
    const fs::path src = suite_dir / "pkgsrc_ok";
    fs::create_directories(src / "content/usr/share");
    {
        std::ofstream(src / "content/usr/share/has space.txt") << "1";
        std::ofstream(src / "content/usr/share/lpkgnew.txt") << "3";
        std::ofstream(src / "content/usr/share/x.lpkgnewx") << "4";
    }

    const fs::path out = suite_dir / "ok_out.lpkg";
    ASSERT_NO_THROW(pack_package(out.string(), src.string(), "ok", "1.0"));
    ASSERT_TRUE(fs::exists(out));

    const fs::path back = suite_dir / "back_from_pack";
    ASSERT_NO_THROW(extract_tar_zst(out, back, out.filename().string()));
    EXPECT_TRUE(fs::exists(back / "content/usr/share/has space.txt"));
    EXPECT_TRUE(fs::exists(back / "content/usr/share/lpkgnew.txt"));
    EXPECT_TRUE(fs::exists(back / "content/usr/share/x.lpkgnewx"));
}

// ============================================================================
// TAB：破坏**制表符分帧**的归属数据库（键在重载时被截断）
// ============================================================================

// ============================================================================
// `.lpkg_bak_`：**第四个**自用命名空间，而且是**前缀**形态 —— 守卫此前整类漏掉
//
// 前三个（`.lpkgtmp`/`.lpkgnew`/`.lpkgsave`）都是"追加在目标名**之后**"的后缀，判据是
// `component.ends_with(...)`；而 stash 名是 `.lpkg_bak_<pkg>_<pid>`（见
// `install_common.cpp` 的 `stash_parent_dir`），token 在**开头** ⇒ `ends_with` 结构上
// 就抓不到它，加进那张后缀清单也没用。
//
// 后果（可达且静默）：包发 `content/.lpkg_bak_x_999999/payload` 能合法装进
// `<root>/.lpkg_bak_x_999999/` 并登记进 files.db；而 `cleanup_orphan_stashes()`
// （`base/utils.cpp`）**正是**按这个前缀 + 尾段 pid 已死（kill ESRCH）就 `remove_all`
// —— 下次任意 lpkg 命令会把已安装的文件连同目录静默删掉，盘面与 DB 当场脱节
// （`query` 说文件属于某包、盘上已经没了）。
//
// 判据形态与另外三个一致：**按 `/` 切分量、逐分量判**（`.lpkg_bak_` 落在中间分量上同样
// 要命中，因为 libarchive 会为文件成员自动补建父目录 —— 与 `.lpkgtmp` 那条同因）。
// ⚠️ 仍然**不是**子串匹配：`x.lpkg_bak_1` 只是**含**该串、不以它开头，必须照常解出。
// ============================================================================

TEST_F(ArchiveMemberNameSafetyTest, ReservedStashNamespaceIsRejectedInAnyPathComponent)
{
    const std::vector<std::string> bad_names = {
        ".lpkg_bak_pkgx_999999/payload",    // 顶层（stash 真正的落点）：与孤儿回收的前缀判据撞名
        "content/.lpkg_bak_pkgx_999999/y",  // 包内容形态
        "content/usr/lib/.lpkg_bak_a_1/z",  // 中间分量（末段是 `z`）：父目录由 libarchive 自动补建
        "content/usr/bin/.lpkg_bak_b_2/",   // 目录条目形态（带尾斜杠）
    };

    for (const auto& name : bad_names) {
        fs::remove_all(out_dir);
        fs::create_directories(out_dir);
        const bool dir = name.ends_with('/');
        write_archive({Member{name, dir ? "" : "x", "", dir}});

        const std::string err = extract_error();
        ASSERT_FALSE(err.empty()) << "成员名 " << name
                                  << " 用了 lpkg 自用的 stash 命名空间，必须整包拒绝（未抛）";
        EXPECT_EQ(count_entries(out_dir), 0u) << "成员名 " << name << " 被拒后仍有文件落盘";
        // 拒绝的**原因**必须点名了这个命名空间 —— 文案与判据必须同步更新（只加判据、不改
        // 文案的话，用户看到的仍是"只列了三个后缀"的旧说明，指向不了真因）。
        EXPECT_NE(err.find(".lpkg_bak_"), std::string::npos)
            << "拒绝原因不是 stash 命名空间守卫（或 l10n 文案没跟上判据）：" << err;
    }

    // 正面对照：只是**含**该串、但分量不以 `.lpkg_bak_` **开头**的名字必须照常解出 ——
    // 与另外三个后缀的对照用例同一条纪律（判据是前缀，不是子串）。
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);
    write_archive({
        {"content/usr/share/x.lpkg_bak_1.txt", "1", "", false},  // 串在中间
        {"content/usr/share/foo.lpkg_bak.txt", "2", "", false},  // 尾部不同
        {"content/usr/share/d.lpkg_bak_1/", "", "", true},       // 目录名不以 token 开头
    });
    EXPECT_NO_THROW(extract_tar_zst(pkg, out_dir, pkg.filename().string()));
    EXPECT_TRUE(fs::exists(out_dir / "content/usr/share/x.lpkg_bak_1.txt"));
    EXPECT_TRUE(fs::exists(out_dir / "content/usr/share/foo.lpkg_bak.txt"));
    EXPECT_TRUE(fs::is_directory(out_dir / "content/usr/share/d.lpkg_bak_1"));
}

TEST_F(ArchiveMemberNameSafetyTest, TabInMemberNameIsRejected)
{
    // `\t` 是合法文件名字节、也不会伪造 WAL 行（WAL 是空格分帧），但它会破坏**制表符分帧**
    // 的归属数据库：files.db / provides.db 写成 `key\tvalues\n`、按**第一个** TAB 读回
    // （cache.cpp），带 TAB 的键在重载时被截断、属主串错位（误报共享文件 / 假孤儿）。
    // 守卫此前只拒 \n / \r / \0 / `→`，漏了 TAB。
    write_archive({{"usr/share/a\tb.txt", "x", "", false}});

    const std::string err = extract_error();
    ASSERT_FALSE(err.empty()) << "含制表符的成员名必须被拒绝（throw），而不是照解不误";
    EXPECT_NE(err.find(key_head("error.unsafe_member_control")), std::string::npos) << err;
    EXPECT_EQ(count_entries(out_dir), 0u) << "被拒后仍有文件落盘";
}

// ============================================================================
// 后缀判据必须按**任一路径分量**判，不能只看末段
//
// 只看末段会漏掉 `content/usr/bin/bash.lpkgtmp/x` 这类成员 —— 它的**末段是 `x`**，旧判据放行。
// 而 libarchive 的 disk writer 会为文件成员**自动补建缺失的父目录**（本仓库已记录：
// 一个只含 `content/usr/bin/foo`、不含任何目录成员的归档，解压后 `content/usr/bin` 真实存在）。
// 于是包里就"合法地"装出一个**目录** `usr/bin/bash.lpkgtmp/`。此后别的包安装 `bash` 时，
// `<dst>.lpkgtmp` 的落位路径正撞上那个目录：
//   · 全新安装 `rename(目录 → 不存在路径)` **成功** ⇒ `usr/bin/bash` 变成一个**目录**而 lpkg
//     报成功（用户拿到坏系统）；
//   · 升级 `rename(目录 → 已存在文件)` = `ENOTDIR` ⇒ 整批回滚，且报错定位不到真因。
// 冲突预检拦不住（它只比对清单里的路径，不比对 `<目标>.lpkgtmp`），故必须在名字进系统之前、
// 在解压侧按**分量**拒绝整个归档。
// ============================================================================

TEST_F(ArchiveMemberNameSafetyTest, ReservedSuffixInAnyPathComponentIsRejected)
{
    const std::vector<std::string> bad_names = {
        // 后缀落在**中间分量**上（末段是 `x`）：修复前旧判据（只看末段）会放行 —— 这条会红
        "content/usr/bin/bash.lpkgtmp/x",
        "content/usr/share/app.lpkgnew/y",
        "content/usr/lib/libfoo.lpkgsave/z",
        // 中间分量**整体**就是后缀（目录条目形态，带尾斜杠），后面还接了子文件
        "content/usr/bin/bash.lpkgtmp/",
        // 前导 `./`（member_path_relative 会先归一化掉）之后，中间分量仍必须命中
        "./content/usr/bin/bash.lpkgtmp/evil",
    };

    for (const auto& name : bad_names) {
        fs::remove_all(out_dir);
        fs::create_directories(out_dir);
        // 中间分量那几条写成**普通文件**成员：带后缀的父目录由 libarchive 自动补建 ——
        // 正是这条缺陷赖以成立的前提（末尾带 `/` 的那条本身是目录条目，照直写成目录）。
        const bool dir = name.ends_with('/');
        write_archive({Member{name, dir ? "" : "x", "", dir}});

        const std::string err = extract_error();
        // 断言**拒绝原因**（不是"抛了就行"）：修复前 err 为空（放行），`find` 返回 npos ⇒ 红。
        EXPECT_NE(err.find(key_head("error.unsafe_member_suffix")), std::string::npos)
            << "成员名 " << name << " 的中间分量带了 lpkg 自用后缀，必须被**后缀守卫**整包拒绝："
            << (err.empty() ? "<被放行、根本没抛>" : err);
        EXPECT_EQ(count_entries(out_dir), 0u) << "成员名 " << name << " 被拒后仍有文件落盘";
    }

    // 正面对照：分量里**含**后缀样串、但不以它**结尾**的名字必须照常解出 —— 判据是
    // "某分量以这三个后缀结尾"，不是子串匹配。若修复被写成 `member.find(".lpkgtmp") != npos`
    // 之类的子串匹配，下面这几个合法名字会被误拒 ⇒ EXPECT_NO_THROW 从"不抛"变"抛"。
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);
    write_archive({
        {"content/usr/share/x.lpkgnewx/y", "1", "", false},       // 后缀后还接了字符
        {"content/usr/share/foo.lpkgtmp.txt/z", "2", "", false},  // 后缀不在分量末尾
        {"content/usr/share/d.lpkgsavex/", "", "", true},         // 目录名后缀后接了字符
    });
    EXPECT_NO_THROW(extract_tar_zst(pkg, out_dir, pkg.filename().string()));
    EXPECT_TRUE(fs::exists(out_dir / "content/usr/share/x.lpkgnewx/y"));
    EXPECT_TRUE(fs::exists(out_dir / "content/usr/share/foo.lpkgtmp.txt/z"));
    EXPECT_TRUE(fs::is_directory(out_dir / "content/usr/share/d.lpkgsavex"))
        << "分量后缀不完整匹配的合法名字不该被拒";
}

// ============================================================================
// `.lpkgsave`（保留后缀）：与 `.lpkgtmp` / `.lpkgnew` 同一条守卫
//
// 上面第 5 节的后缀用例只列了 `.lpkgtmp` / `.lpkgnew`；`.lpkgsave` 是第三个。
// 它是"类型变化 / 废弃的配置改名保留"的落点（`OpSink::save_config`）：包声明同名成员会与它
// 撞名 —— `save_config` 发现目标名被占时会把旧 `.lpkgsave` **移位**成 `<dst>.lpkgsave.<N>`，
// 那会把**另一个包**的同名文件挤开、归属当场脱节。合法包里不该有这个名字，
// 故与另外两个一样在解压侧**整包拒绝**。目录条目形态（`x.lpkgsave/`）同样要判：切分量后空分量跳过。
// ============================================================================

TEST_F(ArchiveMemberNameSafetyTest, ReservedLpkgsaveSuffixIsRejected)
{
    const std::vector<std::string> bad_names = {
        "usr/share/app.conf.lpkgsave",  // 文件形态：与"保留用户配置"的落位撞名
        "usr/share/d.lpkgsave/",        // 目录条目形态：末段判据必须先剥掉尾斜杠
    };

    for (const auto& name : bad_names) {
        fs::remove_all(out_dir);
        fs::create_directories(out_dir);
        const bool dir = name.ends_with('/');
        write_archive({Member{name, dir ? "" : "x", "", dir}});

        const std::string err = extract_error();
        // 断言点名 l10n 键对应的**文案片段**（不是"抛了就行"）：缺陷下会红 —— 若守卫漏了
        // `.lpkgsave` 这一支，这个合法成员会被照常解出 ⇒ err 为空 ⇒ find 返回 npos。
        EXPECT_NE(err.find(key_head("error.unsafe_member_suffix")), std::string::npos)
            << "成员名 " << name << " 用了 .lpkgsave 保留后缀，必须被**守卫**拒绝：" << err;
        EXPECT_EQ(count_entries(out_dir), 0u) << "成员名 " << name << " 被拒后仍有文件落盘";
    }

    // 正面对照：`.lpkgsave` 出现在**中间**或**后接字符**时不命中（只有**末段整体**以它结尾
    // 才算）。缺陷下会红：若判据被写成 `member.find(".lpkgsave") != npos`（子串匹配而非末段
    // 后缀），这两个合法名字会被误拒 ⇒ EXPECT_NO_THROW 从"不抛"变"抛"。
    fs::remove_all(out_dir);
    fs::create_directories(out_dir);
    write_archive({
        {"usr/share/foo.lpkgsave.txt", "1", "", false},  // 含 `.lpkgsave` 但不在末尾
        {"usr/share/x.lpkgsavex", "2", "", false},       // 后缀后还接了字符
    });
    EXPECT_NO_THROW(extract_tar_zst(pkg, out_dir, pkg.filename().string()));
    EXPECT_TRUE(fs::exists(out_dir / "usr/share/foo.lpkgsave.txt"));
    EXPECT_TRUE(fs::exists(out_dir / "usr/share/x.lpkgsavex"));
}
