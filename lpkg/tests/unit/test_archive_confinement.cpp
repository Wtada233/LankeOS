/**
 * test_archive_confinement.cpp — 解压必须锁在目标目录内（TODO.md X2）
 *
 * 归档成员名是**不可信输入**（未校验的 .lpkg、无校验和的上游源码包）。`fs::path` 语义下
 * `output_dir / "/abs/x"` **等于 "/abs/x"**（绝对右值丢弃左值），所以修复前一个绝对路径
 * 成员就能写到解压根之外：安装期解压根是 /tmp 下的临时目录、构建期是源码树，
 * 两者都会污染/覆盖宿主文件，而且这类写入不进 file_db（query 看不到、remove 删不掉）。
 *
 * 修法：成员名归一化为相对路径 + `ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS` +
 * 硬链接目标必须落在解压根内。符号链接的**目标内容**保持原样（包内绝对链接合法）。
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
#include "../../main/src/pkg/install_common.hpp"

namespace fs = std::filesystem;

class ArchiveConfinementTest : public ::testing::Test
{
protected:
    fs::path suite_dir;
    fs::path out_dir;
    fs::path escape_dir;  // 解压根**之外**的可写目录：用来观察"有没有逃出去"
    fs::path pkg;

    void SetUp() override
    {
        init_localization();
        suite_dir = fs::absolute("tmp_archive_confinement_test");
        fs::remove_all(suite_dir);
        out_dir = suite_dir / "out";
        escape_dir = suite_dir / "outside";  // 与 out_dir 平级 → 逃逸目标落在解压根之外
        fs::create_directories(out_dir);
        fs::create_directories(escape_dir);
        pkg = suite_dir / "evil.lpkg";
    }

    void TearDown() override
    {
        fs::remove_all(suite_dir);
    }

    struct Member {
        std::string name;
        std::string data;      // 普通文件内容（空 = 不写数据）
        std::string hardlink;  // 非空则写成硬链接
        std::string symlink;   // 非空则写成符号链接（内容是"目标"）
        bool is_dir = false;
    };

    /** 用 libarchive 写一个 tar.zst（不压缩也可：读取侧 support_filter_all 全支持） */
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
            } else if (!m.symlink.empty()) {
                archive_entry_set_filetype(e, AE_IFLNK);
                archive_entry_set_symlink(e, m.symlink.c_str());
            } else {
                archive_entry_set_filetype(e, AE_IFREG);
                archive_entry_set_perm(e, 0644);
                archive_entry_set_size(e, static_cast<la_int64_t>(m.data.size()));
                if (!m.hardlink.empty()) archive_entry_set_hardlink(e, m.hardlink.c_str());
            }
            archive_write_header(a, e);
            if (!m.data.empty() && m.hardlink.empty() && m.symlink.empty() && !m.is_dir) {
                archive_write_data(a, m.data.data(), m.data.size());
            }
            archive_entry_free(e);
        }
        archive_write_close(a);
        archive_write_free(a);
    }
};

TEST_F(ArchiveConfinementTest, AbsoluteMemberPathStaysInsideExtractionRoot)
{
    // 成员名是绝对路径，且指向解压根之外的一个**可写**目录（这样旧代码能真的逃出去）
    const std::string escaped = (escape_dir / "ESCAPED.txt").string();
    write_archive({
        {"content/hello.txt", "hello", "", "", false},
        {escaped, "pwned", "", "", false},
    });

    extract_tar_zst(pkg, out_dir);

    EXPECT_TRUE(fs::exists(out_dir / "content/hello.txt")) << "普通成员没解出来";
    EXPECT_FALSE(fs::exists(escape_dir / "ESCAPED.txt"))
        << "归档成员逃出了解压根（写到了 " << escape_dir << "）";
    // 归一化后应落在解压根内部（去掉了前导 '/'）
    EXPECT_TRUE(fs::exists(out_dir / escaped.substr(1))) << "绝对路径成员没有被归一化进解压根";
}

TEST_F(ArchiveConfinementTest, DotDotMemberDoesNotEscape)
{
    write_archive({
        {"content/ok.txt", "ok", "", "", false},
        {"../ESCAPED_DOTDOT.txt", "pwned", "", "", false},
    });

    // `..` 成员由 libarchive 的 SECURE_NODOTDOT 直接拒绝（这是**既有**策略，未改动）：
    // 整次解压失败并抛错，而不是把文件写到解压根的父目录。
    EXPECT_THROW(extract_tar_zst(pkg, out_dir), LpkgException);
    EXPECT_FALSE(fs::exists(suite_dir / "ESCAPED_DOTDOT.txt")) << "`..` 成员逃到了解压根的父目录";
}

TEST_F(ArchiveConfinementTest, HardlinkTargetOutsideRootIsSkipped)
{
    // 先造一个"系统里已有"的目标文件（模拟 /etc/shadow 这类），再让归档给它起别名
    const fs::path secret = escape_dir / "secret.txt";
    {
        std::ofstream(secret) << "sensitive";
    }

    write_archive({
        {"content/evil_link", "", secret.string(), "", false},  // 硬链接指向根外
        {"content/ok.txt", "ok", "", "", false},
    });

    EXPECT_NO_THROW(extract_tar_zst(pkg, out_dir));  // 逃逸成员被跳过，不影响其余成员
    EXPECT_FALSE(fs::exists(out_dir / "content/evil_link")) << "根外硬链接目标未被拒绝";
    EXPECT_TRUE(fs::exists(out_dir / "content/ok.txt"));
}

TEST_F(ArchiveConfinementTest, LegitimateHardlinkInsideRootStillWorks)
{
    // 正向对照：根内的正常硬链接必须照常建立（防止守卫过严把合法归档拒了）
    write_archive({
        {"content/orig.txt", "shared", "", "", false},
        {"content/link.txt", "", "content/orig.txt", "", false},
    });

    extract_tar_zst(pkg, out_dir);

    ASSERT_TRUE(fs::exists(out_dir / "content/orig.txt"));
    ASSERT_TRUE(fs::exists(out_dir / "content/link.txt"));
    std::error_code ec;
    EXPECT_TRUE(fs::equivalent(out_dir / "content/orig.txt", out_dir / "content/link.txt", ec))
        << "根内硬链接应当是同一 inode: " << ec.message();
}

TEST_F(ArchiveConfinementTest, AbsoluteSymlinkTargetIsPreservedAsIs)
{
    // 符号链接的**目标内容**不受归一化影响（包内绝对链接是合法的，指向系统路径）
    write_archive({
        {"content/abslink", "", "", "/usr/lib/libfoo.so.1", false},
    });

    extract_tar_zst(pkg, out_dir);

    const fs::path link = out_dir / "content/abslink";
    ASSERT_TRUE(fs::is_symlink(link));
    EXPECT_EQ(fs::read_symlink(link).string(), "/usr/lib/libfoo.so.1") << "符号链接目标被错误改写";
}

TEST_F(ArchiveConfinementTest, OversizedDeclaredMemberSizeIsRejected)
{
    // X5：归档自报大小是**不可信输入**（GNU base-256 头可以声明任意大小）。
    // 修复前 `content.resize(size)` 会 bad_alloc/abort（调用方只承诺抛 LpkgException）。
    struct archive* a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    archive_write_open_filename(a, pkg.c_str());
    struct archive_entry* e = archive_entry_new();
    archive_entry_set_pathname(e, "metadata.json");
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    // 声明值必须**略高于**实现里的 16 MiB 上限即可覆盖该分支；不能图省事写 1 TiB——
    // libarchive 会把"声明了但没写"的部分补齐成零字节，测试自己会变成磁盘杀手。
    archive_entry_set_size(e, (la_int64_t{16} << 20) + 4096);
    archive_write_header(a, e);
    archive_entry_free(e);
    archive_write_close(a);
    archive_write_free(a);

    EXPECT_THROW(detail::read_archive_metadata(pkg), LpkgException)
        << "超大声明大小应被拒（抛 LpkgException），而不是 bad_alloc/abort";
}
