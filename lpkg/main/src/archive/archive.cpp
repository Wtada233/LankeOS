#include "archive.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <memory>

#include "base/constants.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "i18n/localization.hpp"

namespace fs = std::filesystem;

/** libarchive 读取句柄的自定义删除器 */
struct ArchiveReadDeleter {
    void operator()(struct archive* a) const
    {
        if (a) {
            archive_read_close(a);
            archive_read_free(a);
        }
    }
};

/** libarchive 写入句柄的自定义删除器 */
struct ArchiveWriteDeleter {
    void operator()(struct archive* a) const
    {
        if (a) {
            archive_write_close(a);
            archive_write_free(a);
        }
    }
};

using ArchiveReadHandle = std::unique_ptr<struct archive, ArchiveReadDeleter>;
using ArchiveWriteHandle = std::unique_ptr<struct archive, ArchiveWriteDeleter>;

/**
 * 归档成员名 → 解压根内的相对路径（去掉前导 "./" 与 "/"）。
 *
 * 成员名是**不可信输入**（未校验的 .lpkg、无校验和的上游源码包）。`fs::path` 语义下
 * `output_dir / "/etc/x"` **等于 "/etc/x"**（绝对右值丢弃左值），所以绝对路径成员会写到
 * 解压根之外：安装期解压根是 /tmp 下的临时目录、构建期是源码树，两者都会污染/覆盖宿主
 * 文件，而且这类写入不进 file_db，`query` 看不到、`remove` 删不掉（TODO.md X2）。
 *
 * 只归一化**成员名**；符号链接的**目标内容**保持原样（包内绝对链接是合法的）。
 */
static std::string member_path_relative(const char* raw)
{
    std::string_view sv(raw);
    while (true) {
        if (sv.starts_with("./")) {
            sv.remove_prefix(2);
            continue;
        }
        if (sv.starts_with('/')) {
            sv.remove_prefix(1);
            continue;
        }
        break;
    }
    return std::string(sv);
}

/**
 * 解压 tar.zst 归档文件到目标目录
 * 包含安全检查：路径穿越防护、符号链接权限修复、硬链接/软链接目标重映射
 * 每解压 100 个文件输出一次进度
 */
void extract_tar_zst(const fs::path& archive_path, const fs::path& output_dir)
{
    ArchiveReadHandle a(archive_read_new());
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());

    ArchiveWriteHandle ext(archive_write_disk_new());
    archive_write_disk_set_options(
        ext.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_OWNER |
                       ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS |
                       ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                       ARCHIVE_EXTRACT_UNLINK);
    // 注：这里**不能**加 ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS —— 本实现是先把
    // "解压根 + 成员名"的绝对路径写回 entry 再交给 disk writer，该选项会因此拒绝
    // 每一个成员（实测 "Path is absolute"）。绝对路径的防护由下面的 member_path_relative
    // 归一化承担：成员名先变成相对路径再拼根，绝无逃出 output_dir 的可能。
    // 已移除：archive_write_disk_set_standard_lookup(ext.get());
    // 该函数在静态链接的 chroot 环境下因 NSS 问题可能导致段错误

    if (archive_read_open_filename(a.get(), archive_path.c_str(), constants::ARCHIVE_BUFFER_SIZE) !=
        ARCHIVE_OK) {
        const char* err = archive_error_string(a.get());
        throw LpkgException(string_format("error.extract_failed", archive_path.string()) + ": " +
                            (err ? err : get_string("error.unknown")));
    }

    struct archive_entry* entry;
    int r = ARCHIVE_OK;
    long long count = 0;
    while (true) {
        r = archive_read_next_header(a.get(), &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_OK) {
            if (r < ARCHIVE_WARN) {
                const char* err = archive_error_string(a.get());
                throw LpkgException(string_format("error.extract_failed", archive_path.string()) +
                                    ": " + (err ? err : get_string("error.fatal_read")));
            }
            log_warning(archive_error_string(a.get()));
        }

        const char* current_path = archive_entry_pathname(entry);
        if (!current_path) continue;

        // 成员名归一化为相对路径后再拼解压根（`..` 由 SECURE_NODOTDOT 兜底），
        // 保证任何成员都落在 output_dir 之内 —— 见 member_path_relative 的说明。
        const std::string member = member_path_relative(current_path);
        if (member.empty()) continue;  // "." 之类不产生文件的成员
        fs::path dest_path = output_dir / member;
        archive_entry_set_pathname(entry, dest_path.c_str());

        // 修复：Linux 没有 lchmod，libarchive 可能会对符号链接使用 chmod，
        // 这会导致跟随链接并破坏目标文件的权限（例如 sudo 变为 777）
        if (archive_entry_filetype(entry) == AE_IFLNK) {
            archive_entry_set_perm(entry, 0);
        }

        // 硬链接：目标必须落在解压根内 —— 否则一个成员就能给任意已有文件
        // （如 /etc/shadow）起别名，随后被当作包内容复制进系统。
        // 判据用**原始目标**：绝对路径（指向根外）或 `..` 上溯出根 → 跳过该成员；
        // 根内的相对目标按"归一化后的绝对路径"重写（libarchive 需要绝对路径）。
        const char* hardlink = archive_entry_hardlink(entry);
        if (hardlink) {
            const fs::path root_n = output_dir.lexically_normal();
            const fs::path hl_norm = (root_n / member_path_relative(hardlink)).lexically_normal();
            if (fs::path(hardlink).is_absolute() || !path_within(hl_norm, root_n)) {
                log_warning(string_format("warning.archive_unsafe_member", hardlink));
                continue;
            }
            archive_entry_set_hardlink(entry, hl_norm.c_str());
        }

        r = archive_write_header(ext.get(), entry);
        if (r < ARCHIVE_OK) {
            if (r < ARCHIVE_WARN) {
                const char* err = archive_error_string(ext.get());
                throw LpkgException(string_format("error.extract_failed", archive_path.string()) +
                                    ": " + (err ? err : get_string("error.fatal_write")));
            }
            log_warning(archive_error_string(ext.get()));
        } else {
            const void* buff;
            size_t size;
            la_int64_t offset;
            while (true) {
                r = archive_read_data_block(a.get(), &buff, &size, &offset);
                if (r == ARCHIVE_EOF) break;
                if (r < ARCHIVE_OK) {
                    if (r < ARCHIVE_WARN) {
                        const char* err = archive_error_string(a.get());
                        throw LpkgException(
                            string_format("error.extract_failed", archive_path.string()) + ": " +
                            (err ? err : get_string("error.data_block_read")));
                    }
                    log_warning(archive_error_string(a.get()));
                    break;
                }

                if (archive_write_data_block(ext.get(), buff, size, offset) < ARCHIVE_OK) {
                    const char* err = archive_error_string(ext.get());
                    throw LpkgException(
                        string_format("error.extract_failed", archive_path.string()) + ": " +
                        (err ? err : get_string("error.data_block_write")));
                }
            }
            archive_write_finish_entry(ext.get());
        }

        if (++count % constants::PROGRESS_INTERVAL_FILES == 0) {
            log_info(string_format("info.extracting", count));
        }
    }

    log_info(string_format("info.extract_complete", count));
}

/**
 * 从归档文件中提取指定路径的内部文件内容并返回字符串
 * 自动去除路径前缀 "./"，当文件不存在时返回空字符串
 */
std::string extract_file_from_archive(const fs::path& archive_path,
                                      const std::string& internal_path)
{
    ArchiveReadHandle a(archive_read_new());
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());

    if (archive_read_open_filename(a.get(), archive_path.c_str(), constants::ARCHIVE_BUFFER_SIZE) !=
        ARCHIVE_OK) {
        throw LpkgException(string_format("error.open_file_failed", archive_path.string()) + ": " +
                            archive_error_string(a.get()));
    }

    struct archive_entry* entry;
    while (archive_read_next_header(a.get(), &entry) == ARCHIVE_OK) {
        std::string path = archive_entry_pathname(entry);
        // 去除开头的 ./ 前缀
        if (path.starts_with(constants::CURRENT_DIR_PREFIX))
            path = path.substr(constants::CURRENT_DIR_PREFIX.length());

        if (path == internal_path) {
            // 归档自报大小是**不可信输入**（GNU base-256 头可以声明 1 TiB）：
            // 无上限的 resize 会 bad_alloc/abort 掉整个进程，而调用方只承诺抛
            // LpkgException。要读的只有 metadata.json（几 KB），超过上限即畸形归档。
            constexpr la_int64_t kMaxMemberSize = 16 * 1024 * 1024;
            const la_int64_t declared = archive_entry_size(entry);
            if (declared < 0 || declared > kMaxMemberSize) {
                throw LpkgException(string_format("error.archive_member_too_large",
                                                  std::to_string(declared),
                                                  archive_path.string()));
            }
            size_t size = static_cast<size_t>(declared);
            std::string content;
            content.resize(size);
            ssize_t bytes_read = archive_read_data(a.get(), content.data(), size);
            // archive_read_data 可能返回少于 size 的字节（截断/损坏归档）
            if (bytes_read >= 0 && static_cast<size_t>(bytes_read) < size)
                content.resize(static_cast<size_t>(bytes_read));
            else if (bytes_read < 0)
                content.clear();
            return content;
        }
        archive_read_data_skip(a.get());
    }

    return "";
}
