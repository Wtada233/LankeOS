/**
 * test_content_root_symlink.cpp — `content` 根被做成符号链接的归档必须**整包拒绝**
 *
 * ── 缺口 ──────────────────────────────────────────────────────────────────────
 * `extract_tar_zst` 对**硬链接**目标做了 confinement（根外目标跳过），对**符号链接**目标
 * 没有 —— 那是**有意**的（包内绝对链接合法，见 test_archive_confinement 的
 * `AbsoluteSymlinkTargetIsPreservedAsIs`）。但"成员名消毒"与"硬链接 confinement"都挡不住
 * **把 `content` 这个根本身**做成符号链接：
 *
 *   · 归档成员 `content -> /some/outside/dir` + 一个真 `metadata.json`；
 *   · 解压时 libarchive 把它建成符号链接（`ARCHIVE_EXTRACT_SECURE_SYMLINKS` 只拦"穿过
 *     链接去写"，不拦"创建这条链接"）；
 *   · 随后 `extract_and_validate_package` 的 `fs::exists(tmp/content)`（**跟随**语义）为真、
 *     `scan_content_files` 的 `recursive_directory_iterator(content_dir)` 会**跟随起点目录**，
 *     于是**安装机上**那个目录里的文件被当成"包内容"登记、复制进目标 root
 *     （2026-10-02 端到端实测复现：包 owning 外部目录里的文件）。
 *
 * ── 修法 ──────────────────────────────────────────────────────────────────────
 *   · `extract_and_validate_package`：`content` 必须是**真目录**（lstat），`metadata.json`
 *     必须是**真文件**；
 *   · `scan_content_files`：根不是真目录即抛 `error.content_not_directory`（第二道防线）。
 *
 * 本用例钉"装不上 + 外部文件既没被登记、也没被复制"。
 */

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../../main/src/base/exception.hpp"
#include "../../main/src/db/cache.hpp"
#include "../../main/src/i18n/localization.hpp"
#include "../../main/src/pkg/package_manager.hpp"
#include "../test_base.hpp"

namespace fs = std::filesystem;

class ContentRootSymlinkTest : public IntegrationTestBase
{
protected:
    /** 手工写一个 `.lpkg`：真 `metadata.json` + 一条指向 @p target 的 `content` 符号链接。 */
    static void write_symlinked_content_pkg(const fs::path& pkg, const std::string& target)
    {
        struct archive* a = archive_write_new();
        archive_write_set_format_pax_restricted(a);
        archive_write_open_filename(a, pkg.c_str());

        const std::string meta =
            R"({"name":"evil","version":"1.0","deps":[],"provides":[],"needed_so":[],"man":""})";
        struct archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, "metadata.json");
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_entry_set_size(e, static_cast<la_int64_t>(meta.size()));
        archive_write_header(a, e);
        archive_write_data(a, meta.data(), meta.size());
        archive_entry_free(e);

        e = archive_entry_new();
        archive_entry_set_pathname(e, "content");
        archive_entry_set_filetype(e, AE_IFLNK);
        archive_entry_set_symlink(e, target.c_str());
        archive_write_header(a, e);
        archive_entry_free(e);

        archive_write_close(a);
        archive_write_free(a);
    }
};

TEST_F(ContentRootSymlinkTest, SymlinkedContentRootIsRefusedAndNothingLeaks)
{
    // 解压根之外的"敏感"目录（模拟安装机上的任意可读目录）
    const fs::path outside = suite_work_dir / "outside";
    fs::create_directories(outside);
    {
        std::ofstream(outside / "TOPSECRET") << "sensitive";
    }

    const fs::path pkg = suite_work_dir / "evil-1.0.lpkg";
    write_symlinked_content_pkg(pkg, outside.string());

    EXPECT_THROW(install_packages({pkg.string()}), LpkgException)
        << "content 是符号链接的归档必须被拒绝（否则会枚举解压根之外的文件）";

    Cache::instance().load();
    EXPECT_FALSE(Cache::instance().is_installed("evil"));
    // 外部文件绝不能被登记为本包内容（`/TOPSECRET` 的键来自"相对 content 的路径"）
    EXPECT_TRUE(Cache::instance().get_file_owners("/TOPSECRET").empty());
    // 也不能被复制进目标 root
    EXPECT_FALSE(fs::exists(test_root / "TOPSECRET"));
}
