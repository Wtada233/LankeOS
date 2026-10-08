/**
 * test_archive_special_files.cpp — 归档里的特殊文件成员（FIFO / 字符设备 / 块设备 / socket）
 * 必须**整包拒绝**。
 *
 * ── 缺口 ──────────────────────────────────────────────────────────────────────
 * `extract_tar_zst` 此前只对 `AE_IFLNK` 特判（修符号链接权限），FIFO/设备/socket 成员照原样
 * 被解出来。后果（都不是"多了个奇怪文件"这么轻）：
 *   · **FIFO**：安装期 `calculate_sha256`（crypto/hash.cpp，用 std::ifstream）打开它会在首次
 *     `read()` 上**永久阻塞**（无 O_NONBLOCK；stdio 也不轮询 SIGINT，Ctrl+C 被吞）→ 安装挂死；
 *   · **字符/块设备**：major/minor 可指向 `/dev/zero` 之类，随后的 `fs::copy`
 *     （pkg/installation_task_copy.cpp）会**无限读直到写满磁盘**；
 *   · **socket**：既非普通文件也非目录，任何下游逻辑都不预期。
 *
 * ── 修法 ──────────────────────────────────────────────────────────────────────
 * 成员循环里若 `filetype ∈ {AE_IFIFO, AE_IFCHR, AE_IFBLK, AE_IFSOCK}` → 抛
 * `error.archive_unsupported_filetype`（参数：归档路径、成员名）。
 * 用**黑名单**而非白名单：本函数同时服务源码 tarball 解压，未知/0 类型必须放行。
 *
 * 反向也要覆盖：普通文件/目录/符号链接照常解出来（守卫过严会误伤合法源码包）。
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
#include "../../main/src/base/exception.hpp"
#include "../../main/src/i18n/localization.hpp"

namespace fs = std::filesystem;

class ArchiveSpecialFilesTest : public ::testing::Test
{
protected:
    fs::path suite_dir;
    fs::path out_dir;
    fs::path pkg;

    void SetUp() override
    {
        init_localization();
        suite_dir = fs::absolute("tmp_archive_special_files_test_" + std::to_string(::getpid()));
        fs::remove_all(suite_dir);
        out_dir = suite_dir / "out";
        fs::create_directories(out_dir);
        pkg = suite_dir / "special.tar.zst";
    }

    void TearDown() override
    {
        fs::remove_all(suite_dir);
    }

    struct Member {
        std::string name;
        int filetype = AE_IFREG;  // 默认普通文件
        std::string data;         // filetype == AE_IFREG 时写入
        std::string symlink;      // 非空则写成符号链接（内容是"目标"）
    };

    /** 用 libarchive 写一个 tar（读取侧 support_filter_all，不压缩也读得动） */
    void write_archive(const std::vector<Member>& members)
    {
        struct archive* a = archive_write_new();
        archive_write_set_format_pax_restricted(a);
        ASSERT_EQ(archive_write_open_filename(a, pkg.c_str()), ARCHIVE_OK);
        for (const auto& m : members) {
            struct archive_entry* e = archive_entry_new();
            archive_entry_set_pathname(e, m.name.c_str());
            archive_entry_set_filetype(e, static_cast<mode_t>(m.filetype));
            archive_entry_set_perm(e, m.filetype == AE_IFDIR ? 0755 : 0644);
            if (!m.symlink.empty()) archive_entry_set_symlink(e, m.symlink.c_str());
            if (m.filetype == AE_IFREG)
                archive_entry_set_size(e, static_cast<la_int64_t>(m.data.size()));
            archive_write_header(a, e);
            if (m.filetype == AE_IFREG && !m.data.empty())
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
     * 某个 l10n 键模板按占位符 `{}` 切出的**字面片段**（语言无关）。
     *
     * 用来断言"拒绝的**原因**是我们的守卫"（每个字面片段都出现在消息里），而不是碰巧因为
     * 别的失败而中止 —— 否则用例可能被 libarchive 自身创建设备节点失败之类的情形蒙混过关。
     */
    static std::vector<std::string> key_literals(const char* key)
    {
        const std::string tmpl = get_string(key);
        std::vector<std::string> parts;
        size_t pos = 0;
        while (true) {
            const size_t next = tmpl.find("{}", pos);
            if (next == std::string::npos) break;
            parts.push_back(tmpl.substr(pos, next - pos));
            pos = next + 2;
        }
        parts.push_back(tmpl.substr(pos));
        return parts;
    }

    /** 断言：消息里逐字面片段地出现了该守卫的模板（顺序不作要求，但都要在）。 */
    void expect_guard_message(const std::string& err, const char* member_name)
    {
        for (const auto& lit : key_literals("error.archive_unsupported_filetype")) {
            if (lit.empty()) continue;
            EXPECT_NE(err.find(lit), std::string::npos)
                << "拒绝原因不是特殊文件类型守卫（缺片段 " << lit << "）：" << err;
        }
        EXPECT_NE(err.find(pkg.string()), std::string::npos)
            << "错误消息必须点名是哪个归档：" << err;
        EXPECT_NE(err.find(member_name), std::string::npos)
            << "错误消息必须点名是哪个成员：" << err;
    }
};

// ============================================================================
// 1. FIFO：会让 calculate_sha256 永久阻塞 → 整包拒绝
// ============================================================================

TEST_F(ArchiveSpecialFilesTest, FifoMemberIsRejectedAndNothingIsExtracted)
{
    // 危险成员排在**第一个**：拒绝必须发生在写任何文件之前（解压是流式的）
    write_archive({
        {"content/usr/share/fifo", AE_IFIFO, "", ""},
        {"content/usr/share/ok.txt", AE_IFREG, "ok", ""},
    });

    const std::string err = extract_error();
    ASSERT_FALSE(err.empty()) << "含 FIFO 成员的归档必须被拒绝（throw），而不是照解不误";
    EXPECT_EQ(count_entries(out_dir), 0u) << "FIFO 是第一个成员：拒绝前不该有任何东西落盘";
    expect_guard_message(err, "content/usr/share/fifo");
}

// ============================================================================
// 2. 字符设备：指向 /dev/zero 之类 → fs::copy 无限读到写满磁盘 → 拒绝
// ============================================================================

TEST_F(ArchiveSpecialFilesTest, CharDeviceMemberIsRejected)
{
    write_archive({
        {"content/dev/zero", AE_IFCHR, "", ""},
    });

    const std::string err = extract_error();
    ASSERT_FALSE(err.empty()) << "含字符设备成员的归档必须被拒绝";
    EXPECT_EQ(count_entries(out_dir), 0u);
    expect_guard_message(err, "content/dev/zero");
}

// ============================================================================
// 3. 块设备 → 拒绝
// ============================================================================

TEST_F(ArchiveSpecialFilesTest, BlockDeviceMemberIsRejected)
{
    write_archive({
        {"content/dev/sda", AE_IFBLK, "", ""},
    });

    const std::string err = extract_error();
    ASSERT_FALSE(err.empty()) << "含块设备成员的归档必须被拒绝";
    EXPECT_EQ(count_entries(out_dir), 0u);
    expect_guard_message(err, "content/dev/sda");
}

// ============================================================================
// 4. socket —— **没有用例**，因为经 tar 不可达
//
// 守卫里的 `AE_IFSOCK` 是**纵深防御**：tar/pax 的类型标志位里根本没有 socket
// （'0'/'1'/'2'/'3'/'4'/'5'/'6' 分别是普通/硬链接/符号链接/字符/块/目录/FIFO），
// 所以 `archive_write_set_format_pax_restricted` 写出的 socket 成员读回来既不是
// `AE_IFSOCK`、也不会被拒（写 socket 成员 → 解压**成功**，err 为空）。而
// `extract_tar_zst` 只处理 tar（+zstd 过滤器），socket 根本进不来。**不为走不到的分支
// 写断言**，故此处只留说明；守卫里的 `AE_IFSOCK` 保留
// （零成本，且将来若支持别的归档格式即自然生效）。
// ============================================================================

// ============================================================================
// 5. 正面对照：普通文件 / 目录 / 符号链接必须照常解出来
//
// 守卫用**黑名单**（只拒那四种）而非白名单 —— 本函数同时服务源码 tarball 解压，
// 过严会误伤合法归档。这条用例防止将来有人"顺手"改成白名单把正路封死。
// ============================================================================

TEST_F(ArchiveSpecialFilesTest, RegularDirAndSymlinkStillExtract)
{
    write_archive({
        {"content/usr/share/dir", AE_IFDIR, "", ""},
        {"content/usr/share/dir/file.txt", AE_IFREG, "hello", ""},
        {"content/usr/share/link", AE_IFLNK, "", "dir/file.txt"},
    });

    EXPECT_NO_THROW(extract_tar_zst(pkg, out_dir, pkg.filename().string()));

    EXPECT_TRUE(fs::is_directory(out_dir / "content/usr/share/dir"));
    const auto content = [](const fs::path& p) {
        std::ifstream f(p);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    EXPECT_EQ(content(out_dir / "content/usr/share/dir/file.txt"), "hello");
    EXPECT_TRUE(fs::is_symlink(out_dir / "content/usr/share/link"));
    EXPECT_EQ(fs::read_symlink(out_dir / "content/usr/share/link").string(), "dir/file.txt");
}

// ============================================================================
// 6. 抛的是 LpkgException（安装/构建调用方按普通失败处理）
// ============================================================================

TEST_F(ArchiveSpecialFilesTest, RejectionIsAnLpkgException)
{
    write_archive({
        {"content/fifo", AE_IFIFO, "", ""},
    });

    EXPECT_THROW(extract_tar_zst(pkg, out_dir, pkg.filename().string()), LpkgException);
}
