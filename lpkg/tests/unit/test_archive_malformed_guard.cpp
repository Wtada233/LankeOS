/**
 * test_archive_malformed_guard.cpp — 畸形 / 敌意 tar 必须**整包拒绝**
 *
 * tar 是 1979 年的**纯顺序**格式：没有中央目录、没有成员索引、没有一个字段被强制校验。
 * 头就是 512 字节的定长块，`name` 只是一个 100 字节的字符串字段 —— 于是下面这些形态在
 * **格式上完全合法**，只能由消费者自己拒。判据本体在 `main/src/archive/tar_guard.hpp`
 * （那份抬头里同时列了"哪些通用加固建议**有意不采纳**"）。
 *
 * 本文件里最要紧的一条是**重名成员**：`metadata.json` 在归档里放两份（第一份照索引写、
 * 第二份写 payload），就能同时骗过"校验"与"使用"——因为
 *   · 校验（`extract_file_from_archive`）读到**第一份**，
 *   · 安装（解压后 `read_package_metadata`）用的是**最后一份**。
 * 用例 `DuplicateMemberCannotSmuggleASecondMetadataJson` 钉的就是这条链在**整包拒绝**
 * 处断掉，并且**盘上不留下**那份 payload。
 *
 * ## 怎么造样本
 * **手写 ustar 字节**，不用 libarchive 写侧。理由有两条，都是实测的：
 *   · GNU tar 会把"同一个文件列两次"塌成**硬链接条目**（实测输出
 *     `hrw-r--r-- … metadata.json 连接到 metadata.json`）——拿它造样本会造出**假样本**；
 *   · 要造"声明尺寸与实际不符"、"空名字"这类形态，写侧根本不给你写。
 * 写法照 `tests/fuzz/archive_name_fuzz.cpp` 的 `append_header`（同一套最小 ustar）。
 */

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../main/src/archive/archive.hpp"
#include "../../main/src/archive/packer.hpp"
#include "../../main/src/base/exception.hpp"
#include "../../main/src/i18n/localization.hpp"

namespace fs = std::filesystem;

namespace
{

constexpr std::size_t kBlock = 512;

void put_bytes(char* field, std::size_t width, std::string_view s)
{
    std::memcpy(field, s.data(), std::min(s.size(), width));
}

/// tar 的数值字段：宽度 -1 位八进制 + 结尾 NUL。
void put_octal(char* field, std::size_t width, uint64_t value)
{
    std::snprintf(field, width, "%0*llo", static_cast<int>(width - 1),
                  static_cast<unsigned long long>(value));
}

/// 校验和按规范算：**校验字段本身先当 8 个空格**参与求和。
uint64_t header_checksum(const char* h)
{
    uint64_t sum = 0;
    for (std::size_t i = 0; i < kBlock; ++i) {
        sum += (i >= 148 && i < 156) ? static_cast<unsigned char>(' ')
                                     : static_cast<unsigned char>(h[i]);
    }
    return sum;
}

}  // namespace

class ArchiveMalformedGuardTest : public ::testing::Test
{
protected:
    fs::path suite_dir;
    fs::path out_dir;
    fs::path pkg;

    void SetUp() override
    {
        init_localization();
        suite_dir = fs::absolute("tmp_archive_malformed_guard_test_" + std::to_string(::getpid()));
        fs::remove_all(suite_dir);
        out_dir = suite_dir / "out";
        fs::create_directories(out_dir);
        pkg = suite_dir / "evil.lpkg";
    }

    void TearDown() override
    {
        fs::remove_all(suite_dir);
    }

    // ── 手写 ustar ────────────────────────────────────────────────────────────

    void append_header(std::vector<char>& out, std::string_view name, char typeflag, uint64_t size)
    {
        std::array<char, kBlock> h{};
        put_bytes(h.data() + 0, 100, name);  // 超长走 GNU longlink
        put_octal(h.data() + 100, 8, 0644);
        put_octal(h.data() + 108, 8, 0);
        put_octal(h.data() + 116, 8, 0);
        put_octal(h.data() + 124, 12, size);
        put_octal(h.data() + 136, 12, 0);
        std::memset(h.data() + 148, ' ', 8);
        h[156] = typeflag;
        std::memcpy(h.data() + 257, "ustar", 5);
        h[262] = '\0';
        h[263] = '0';
        h[264] = '0';
        const uint64_t sum = header_checksum(h.data());
        std::snprintf(h.data() + 148, 7, "%06llo", static_cast<unsigned long long>(sum));
        h[154] = '\0';
        h[155] = ' ';
        out.insert(out.end(), h.begin(), h.end());
    }

    void append_padded(std::vector<char>& out, std::string_view data)
    {
        out.insert(out.end(), data.begin(), data.end());
        const std::size_t rem = data.size() % kBlock;
        if (rem != 0) out.insert(out.end(), kBlock - rem, '\0');
    }

    /// 追加一个普通文件成员（名字 >99 字节时自动补 GNU longlink）。
    void add_file(std::vector<char>& out, const std::string& name, std::string_view data)
    {
        if (name.size() > 99) {
            append_header(out, "././@LongLink", 'L', name.size() + 1);
            append_padded(out, name + '\0');
        }
        append_header(out, name, '0', data.size());
        append_padded(out, data);
    }

    /// 追加一个符号链接成员（`target` 是链接目标文本）。
    void add_symlink(std::vector<char>& out, const std::string& name, const std::string& target)
    {
        if (target.size() > 99) {
            append_header(out, "././@LongLink", 'K', target.size() + 1);
            append_padded(out, target + '\0');
        }
        append_header(out, name, '2', 0);
        // 符号链接的目标写在 header 的 linkname 字段（偏移 157，100 字节）——上面那个
        // append_header 没写它，这里补：重写整块太啰嗦，改成重新生成一次头。
        // 简化做法：直接改写刚追加进去的那 512 字节。
        char* h = out.data() + out.size() - kBlock;
        put_bytes(h + 157, 100, target);
        std::memset(h + 148, ' ', 8);
        const uint64_t sum = header_checksum(h);
        std::snprintf(h + 148, 7, "%06llo", static_cast<unsigned long long>(sum));
        h[154] = '\0';
        h[155] = ' ';
    }

    void write_raw(const std::vector<char>& bytes)
    {
        std::ofstream f(pkg, std::ios::binary);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    /// 加结束标记（两个零块）。
    static void finish(std::vector<char>& out)
    {
        out.insert(out.end(), 2 * kBlock, '\0');
    }

    // ── 断言工具 ──────────────────────────────────────────────────────────────

    std::string extract_error()
    {
        try {
            extract_tar_zst(pkg, out_dir, pkg.filename().string());
        } catch (const LpkgException& e) {
            return e.what();
        }
        return {};
    }

    static std::string key_head(const char* key)
    {
        const std::string tmpl = get_string(key);
        return tmpl.substr(0, tmpl.find("{}"));
    }

    /// 拒绝类断言的两个锚点：**原因**（l10n 键的字面前缀）+ **点名是哪个归档**。
    void expect_rejected_with(const std::string& err, const char* key) const
    {
        const std::string head = key_head(key);
        ASSERT_FALSE(head.empty()) << "l10n 键 " << key << " 没有字面前缀，锚点无效";
        EXPECT_NE(err.find(head), std::string::npos) << "拒绝原因不是 " << key << "：" << err;
        EXPECT_NE(err.find(pkg.string()), std::string::npos) << "必须点名是哪个归档：" << err;
    }

    bool file_exists(const fs::path& p) const
    {
        return fs::exists(fs::symlink_status(p));
    }
};

// ============================================================================
// 1. 重名成员 —— 本模块存在的首要理由
// ============================================================================

/**
 * 同一个 `metadata.json` 放两份：第一份"诚实"、第二份是 payload。
 *
 * 没有守卫时：校验读到第一份、盘上留下第二份 ⇒ 装进系统的是**从未被校验过的元数据**。
 * 有守卫时：第二份一出现就整包拒绝，且**盘上不留下** payload。
 */
TEST_F(ArchiveMalformedGuardTest, DuplicateMemberCannotSmuggleASecondMetadataJson)
{
    std::vector<char> tar;
    const std::string honest = R"({"name":"good","version":"1.0"})";
    const std::string payload = R"({"name":"good","version":"9.9","provides":["so:libfoo.so.1"]})";
    add_file(tar, "metadata.json", honest);
    add_file(tar, "metadata.json", payload);
    finish(tar);
    write_raw(tar);

    const std::string err = extract_error();
    expect_rejected_with(err, "error.archive_malformed_duplicate_member");

    // 关键断言：判据在**第二份**上命中，所以第一份已经落盘了（这是本设计有意的：
    // 拒绝发生在循环中途，解压目录里的半成品由调用方整批丢弃，见 base/exception.hpp）。
    // 真正要钉的是**盘上那份不是 payload** —— 否则"拒绝了"就成了空话。
    if (file_exists(out_dir / "metadata.json")) {
        std::ifstream f(out_dir / "metadata.json");
        const std::string on_disk((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
        EXPECT_EQ(on_disk, honest) << "盘上留下的不是第一份 —— 覆盖发生在判据之前？";
        EXPECT_EQ(on_disk.find("9.9"), std::string::npos) << "payload 已经写到盘上了";
    }
}

/// 重名在**不同目录层**上：`a/b` 与 `./a/b` 归一化后是同一个路径，同样是重名。
TEST_F(ArchiveMalformedGuardTest, NamesThatNormalizeToTheSamePathCountAsDuplicates)
{
    std::vector<char> tar;
    add_file(tar, "usr/bin/foo", "first");
    add_file(tar, "./usr/bin/foo", "second");
    finish(tar);
    write_raw(tar);

    expect_rejected_with(extract_error(), "error.archive_malformed_duplicate_member");
}

/// 正面对照：名字各不相同（含"看起来像重复"的兄弟路径）必须照常解出来。
TEST_F(ArchiveMalformedGuardTest, DistinctSiblingPathsStillExtract)
{
    std::vector<char> tar;
    add_file(tar, "usr/bin/foo", "a");
    add_file(tar, "usr/bin/foo2", "b");
    add_file(tar, "usr/lib/foo", "c");  // 同名不同目录：归一化路径不同，不算重名
    add_file(tar, "usr/bin/fo", "d");   // 前缀关系也不算重名
    finish(tar);
    write_raw(tar);

    EXPECT_EQ(extract_error(), "");
    EXPECT_TRUE(file_exists(out_dir / "usr/bin/foo"));
    EXPECT_TRUE(file_exists(out_dir / "usr/bin/foo2"));
    EXPECT_TRUE(file_exists(out_dir / "usr/lib/foo"));
    EXPECT_TRUE(file_exists(out_dir / "usr/bin/fo"));
}

// ============================================================================
// 2. `.` / `..` 分量
// ============================================================================

TEST_F(ArchiveMalformedGuardTest, DotDotComponentIsRejected)
{
    std::vector<char> tar;
    add_file(tar, "usr/../../etc/passwd", "x");
    finish(tar);
    write_raw(tar);

    expect_rejected_with(extract_error(), "error.archive_malformed_dot_component");
}

/**
 * 正面对照：**`.` 分量必须放行**。
 *
 * `./usr/bin/foo` 是 `tar cf pipe .` 那种打包方式的正常产物（成员名带 `./` 前缀），
 * 中间的 `usr/./bin` 也只会被 `lexically_normal` 无声抹平。把 `.` 一起拒掉会**误伤大量
 * 真实源码 tarball** —— 这条对照用例就是防止将来有人"顺手把 `.` 也拒了"。
 */
TEST_F(ArchiveMalformedGuardTest, SingleDotComponentsAreLegalAndStillExtract)
{
    std::vector<char> tar;
    add_file(tar, "./usr/bin/foo", "a");
    add_file(tar, "usr/./bin/bar", "b");
    finish(tar);
    write_raw(tar);

    EXPECT_EQ(extract_error(), "");
    EXPECT_TRUE(file_exists(out_dir / "usr/bin/foo"));
    EXPECT_TRUE(file_exists(out_dir / "usr/bin/bar"));
}

// ============================================================================
// 3. 反斜杠：**必须放行** —— 真实包里有它（绊线）
// ============================================================================

/**
 * **绊线（tripwire）**：文件名里的**字面反斜杠**必须照常解出来。
 *
 * 通用加固清单里"反斜杠 = 跨平台 zip-slip"排得很前，本模块**曾经**据此拒过它 ——
 * 然后在真实仓库上扫出**误报**：
 *
 *     systemd/262-11.lpkg → content/usr/lib/systemd/system/system-systemd\x2dmute\x2dconsole.slice
 *
 * systemd 用 `\x2d` 转义 unit 名里的 `-`，**文件名里字面就带反斜杠**（全仓 861 个包扫下来
 * 仅此一例，而它恰好是 base 包 —— 拒了它整个发行版都装不上）。POSIX 上 `\` 只是普通字节，
 * lpkg 不跨平台解压，所以这条判据对**本仓库**只有害处。判据当天删掉，用例改成钉住"必须放行"。
 *
 * 这条**红**意味着有人把反斜杠判据加回来了 —— 那时先去扫一遍真实仓库再决定。
 */
TEST_F(ArchiveMalformedGuardTest, LiteralBackslashInMemberNameIsLegalSystemdUsesIt)
{
    std::vector<char> tar;
    add_file(tar, "usr/lib/systemd/system/system-systemd\\x2dmute\\x2dconsole.slice", "unit");
    finish(tar);
    write_raw(tar);

    EXPECT_EQ(extract_error(), "")
        << "systemd 的 unit 名转义会产生字面反斜杠文件名 —— 拒了它 = 拒了 systemd 包";
    EXPECT_TRUE(
        file_exists(out_dir / "usr/lib/systemd/system/system-systemd\\x2dmute\\x2dconsole.slice"));
}

// ============================================================================
// 4. 长度
// ============================================================================

TEST_F(ArchiveMalformedGuardTest, OverlongPathComponentIsRejected)
{
    std::vector<char> tar;
    add_file(tar, "usr/bin/" + std::string(300, 'A'), "x");  // 单分量 > NAME_MAX(255)
    finish(tar);
    write_raw(tar);

    expect_rejected_with(extract_error(), "error.archive_malformed_path_too_long");
}

TEST_F(ArchiveMalformedGuardTest, OverlongWholePathIsRejected)
{
    std::string name;
    while (name.size() < 4200) name += "abcdefgh/";  // 每分量都合法，总长超上限
    std::vector<char> tar;
    add_file(tar, name, "x");
    finish(tar);
    write_raw(tar);

    expect_rejected_with(extract_error(), "error.archive_malformed_path_too_long");
}

// ============================================================================
// 5. 控制字符
// ============================================================================

/**
 * ESC（0x1b）：`member_name_rejection_message` 只拒 `\n\r\t\0` 与字面 `" → "`，
 * 其余的（ESC 是最典型的）由守卫补。它能把包内文件名的内容变成终端 ANSI 指令。
 *
 * 注意：libarchive 自己的警告也可能先触发，故用**两锚点**断言"原因是守卫"。
 */
TEST_F(ArchiveMalformedGuardTest, EscapeCharacterInMemberNameIsRejected)
{
    std::vector<char> tar;
    add_file(tar, std::string("usr/bin/") + '\x1b' + "[31mred", "x");
    finish(tar);
    write_raw(tar);

    expect_rejected_with(extract_error(), "error.archive_malformed_name_encoding");
}

// ============================================================================
// 6. 两个"看着该判、其实不可达"的形态 —— 钉成绊线，不写判据
// ============================================================================

/**
 * **绊线（tripwire）**：`typeflag='0'` 但名字以 `/` 结尾的成员，libarchive **按尾斜杠
 * 判定类型**，读回来就是 `AE_IFDIR` —— 也就是说"尾斜杠与类型不一致"这个冲突
 * **到不了解压代码这一层**，`tar_guard` 里因此**没有**这条判据（写过一版，实测不可达，删了）。
 *
 * 这条用例绿 = 现状成立；**红 = libarchive 改了行为**，那时才需要把判据补回去。
 * 它不声称"我们防住了什么"，只记录一个边界 —— 别把它当成覆盖率。
 */
TEST_F(ArchiveMalformedGuardTest, TrailingSlashIsNormalizedByLibarchive)
{
    std::vector<char> tar;
    add_file(tar, "usr/bin/foo/", "x");  // typeflag '0'（普通文件）却带尾斜杠
    finish(tar);
    write_raw(tar);

    EXPECT_EQ(extract_error(), "")
        << "libarchive 不再按尾斜杠归一化类型了？那时 tar_guard 需要补这条判据";
    EXPECT_TRUE(fs::is_directory(out_dir / "usr/bin/foo"))
        << "尾斜杠被当成目录 —— 这正是判据不可达的原因";
}

/**
 * **绊线（tripwire）**：头声明 4096 字节、实际只给 5 字节。
 *
 * libarchive 在 `archive_read_data_block` 上直接返 **FATAL**（"Truncated tar archive
 * detected"），`extract_tar_zst` 在上层那个 `r < ARCHIVE_WARN` 分支就抛了 —— 到达不了任何
 * "声明 vs 实际"的比较。所以这个向量**由 libarchive 覆盖**，`tar_guard` 里没有第二份实现
 * （写过一版，实测不可达，删了）。
 *
 * 断言落在 `error.extract_failed` 的**字面前缀**上：整包拒绝这件事成立，
 * 只是原因键是 libarchive 那条路径的。
 */
TEST_F(ArchiveMalformedGuardTest, TruncatedMemberIsRejectedByLibarchive)
{
    std::vector<char> tar;
    append_header(tar, "usr/bin/truncated", '0', 4096);  // 声明 4096
    append_padded(tar, "short");                         // 实际 5 字节
    finish(tar);
    write_raw(tar);

    const std::string err = extract_error();
    const std::string head = key_head("error.extract_failed");
    ASSERT_FALSE(head.empty());
    EXPECT_NE(err.find(head), std::string::npos)
        << "截断成员必须整包拒绝（libarchive 的 FATAL 路径）：" << err;
    EXPECT_NE(err.find(pkg.string()), std::string::npos) << "必须点名是哪个归档：" << err;
}

// ============================================================================
// 7. 符号链接目标
// ============================================================================

/// 相对目标上溯**穿出解压根** ⇒ 拒绝。
TEST_F(ArchiveMalformedGuardTest, SymlinkEscapingTheRootIsRejected)
{
    std::vector<char> tar;
    add_file(tar, "usr/bin/foo", "x");
    add_symlink(tar, "usr/bin/evil", "../../../../etc/passwd");
    finish(tar);
    write_raw(tar);

    expect_rejected_with(extract_error(), "error.archive_malformed_link_target");
}

/// 正面对照①：`usr/bin/foo -> ../lib/foo` 归一化后仍在根内 ⇒ **必须放行**。
/// （用"目标里含不含 `..`"当判据会误杀这一例 —— 这是有意的判据选择。）
TEST_F(ArchiveMalformedGuardTest, RelativeSymlinkStayingInsideTheRootIsLegal)
{
    std::vector<char> tar;
    add_file(tar, "usr/lib/foo", "x");
    add_symlink(tar, "usr/bin/foo", "../lib/foo");
    finish(tar);
    write_raw(tar);

    EXPECT_EQ(extract_error(), "");
    EXPECT_TRUE(fs::is_symlink(out_dir / "usr/bin/foo"));
}

/// 正面对照②：**绝对目标放行** —— `--root` 下包发 `<root>/usr/bin/foo -> /etc/foo`
/// 合法且必要（既有的 `AbsoluteSymlinkTargetIsPreservedAsIs` 同样钉着这条）。
TEST_F(ArchiveMalformedGuardTest, AbsoluteSymlinkTargetIsStillLegal)
{
    std::vector<char> tar;
    add_file(tar, "usr/bin/foo", "x");
    add_symlink(tar, "usr/bin/bar", "/etc/bar");
    finish(tar);
    write_raw(tar);

    EXPECT_EQ(extract_error(), "");
    EXPECT_TRUE(fs::is_symlink(out_dir / "usr/bin/bar"));
    EXPECT_EQ(fs::read_symlink(out_dir / "usr/bin/bar").string(), "/etc/bar");
}

// ============================================================================
// 8. 守卫只拒畸形，不拒正常包
// ============================================================================

/**
 * 正面对照总闸：一个结构完全正常的归档（目录 + 普通文件 + 相对符号链接）必须原样解出来。
 *
 * 这条同时是"守卫没有把整条解压路径搞坏"的证据 —— 判据写歪最先红的就是它。
 */
TEST_F(ArchiveMalformedGuardTest, WellFormedArchiveStillExtractsCompletely)
{
    std::vector<char> tar;
    append_header(tar, "usr/", '5', 0);
    append_header(tar, "usr/bin/", '5', 0);
    add_file(tar, "usr/bin/foo", "hello");
    add_file(tar, "usr/share/doc/foo.txt", "docs");
    add_symlink(tar, "usr/bin/foo-link", "foo");
    finish(tar);
    write_raw(tar);

    EXPECT_EQ(extract_error(), "");
    EXPECT_TRUE(fs::is_directory(out_dir / "usr/bin"));
    EXPECT_TRUE(file_exists(out_dir / "usr/bin/foo"));
    EXPECT_TRUE(file_exists(out_dir / "usr/share/doc/foo.txt"));
    EXPECT_TRUE(fs::is_symlink(out_dir / "usr/bin/foo-link"));

    std::ifstream f(out_dir / "usr/bin/foo");
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "hello");
}
