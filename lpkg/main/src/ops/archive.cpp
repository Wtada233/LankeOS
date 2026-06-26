#include "archive.hpp"
#include "core/exception.hpp"
#include "core/localization.hpp"
#include "core/utils.hpp"
#include "core/constants.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <memory>

namespace fs = std::filesystem;

/** libarchive 读取句柄的自定义删除器 */
struct ArchiveReadDeleter {
    void operator()(struct archive* a) const {
        if (a) {
            archive_read_close(a);
            archive_read_free(a);
        }
    }
};

/** libarchive 写入句柄的自定义删除器 */
struct ArchiveWriteDeleter {
    void operator()(struct archive* a) const {
        if (a) {
            archive_write_close(a);
            archive_write_free(a);
        }
    }
};

using ArchiveReadHandle = std::unique_ptr<struct archive, ArchiveReadDeleter>;
using ArchiveWriteHandle = std::unique_ptr<struct archive, ArchiveWriteDeleter>;

/**
 * 解压 tar.zst 归档文件到目标目录
 * 包含安全检查：路径穿越防护、符号链接权限修复、硬链接/软链接目标重映射
 * 每解压 100 个文件输出一次进度
 */
void extract_tar_zst(const fs::path& archive_path, const fs::path& output_dir) {
    ArchiveReadHandle a(archive_read_new());
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());

    ArchiveWriteHandle ext(archive_write_disk_new());
    archive_write_disk_set_options(ext.get(),
        ARCHIVE_EXTRACT_TIME |
        ARCHIVE_EXTRACT_PERM |
        ARCHIVE_EXTRACT_OWNER |
        ARCHIVE_EXTRACT_ACL |
        ARCHIVE_EXTRACT_FFLAGS |
        ARCHIVE_EXTRACT_SECURE_SYMLINKS |
        ARCHIVE_EXTRACT_SECURE_NODOTDOT |
        ARCHIVE_EXTRACT_UNLINK
    );
    // 已移除：archive_write_disk_set_standard_lookup(ext.get());
    // 该函数在静态链接的 chroot 环境下因 NSS 问题可能导致段错误

    if (archive_read_open_filename(a.get(), archive_path.c_str(), 10240) != ARCHIVE_OK) {
        const char* err = archive_error_string(a.get());
        throw LpkgException(string_format("error.extract_failed", archive_path.string()) + ": " + (err ? err : get_string("error.unknown")));
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
                throw LpkgException(string_format("error.extract_failed", archive_path.string()) + ": " + (err ? err : get_string("error.fatal_read")));
            }
            log_warning(archive_error_string(a.get()));
        }

        const char* current_path = archive_entry_pathname(entry);
        if (!current_path) continue;

        // 安全防护：路径穿越攻击防护，校验路径是否合法
        fs::path dest_path;
        try {
            dest_path = validate_path(current_path, output_dir);
        } catch (const LpkgException& e) {
             throw LpkgException(string_format("error.malicious_path_in_archive", current_path));
        }

        archive_entry_set_pathname(entry, dest_path.c_str());

        // 修复：Linux 没有 lchmod，libarchive 可能会对符号链接使用 chmod，
        // 这会导致跟随链接并破坏目标文件的权限（例如 sudo 变为 777）
        if (archive_entry_filetype(entry) == AE_IFLNK) {
            archive_entry_set_perm(entry, 0);
        }

        // 重映射硬链接和符号链接目标路径
        const char* hardlink = archive_entry_hardlink(entry);
        if (hardlink) {
            try {
                fs::path link_dest = validate_path(hardlink, output_dir);
                archive_entry_set_hardlink(entry, link_dest.c_str());
            } catch (...) {
                archive_entry_set_hardlink(entry, nullptr); // 丢弃恶意或无效的链接
            }
        }

        const char* symlink = archive_entry_symlink(entry);
        if (symlink) {
            try {
                // 对于符号链接，仅当目标为绝对路径时进行重映射。
                // 相对路径的符号链接通常是包内部引用，不应修改，
                // 除非它们通过 ../ 指向外部（validate_path 会处理此情况）
                if (fs::path(symlink).is_absolute()) {
                    fs::path link_dest = validate_path(symlink, output_dir);
                    archive_entry_set_symlink(entry, link_dest.c_str());
                }
            } catch (...) {
                archive_entry_set_symlink(entry, nullptr);
            }
        }

        r = archive_write_header(ext.get(), entry);
        if (r < ARCHIVE_OK) {
            if (r < ARCHIVE_WARN) {
                const char* err = archive_error_string(ext.get());
                throw LpkgException(string_format("error.extract_failed", archive_path.string()) + ": " + (err ? err : get_string("error.fatal_write")));
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
                        throw LpkgException(string_format("error.extract_failed", archive_path.string()) + ": " + (err ? err : get_string("error.data_block_read")));
                    }
                    log_warning(archive_error_string(a.get()));
                    break;
                }

                if (archive_write_data_block(ext.get(), buff, size, offset) < ARCHIVE_OK) {
                    const char* err = archive_error_string(ext.get());
                    throw LpkgException(string_format("error.extract_failed", archive_path.string()) + ": " + (err ? err : get_string("error.data_block_write")));
                }
            }
            archive_write_finish_entry(ext.get());
        }

        if (++count % 100 == 0) {
            log_info(string_format("info.extracting", count));
        }
    }

    log_info(string_format("info.extract_complete", count));
}

/**
 * 从归档文件中提取指定路径的内部文件内容并返回字符串
 * 自动去除路径前缀 "./"，当文件不存在时返回空字符串
 */
std::string extract_file_from_archive(const fs::path& archive_path, const std::string& internal_path) {
    ArchiveReadHandle a(archive_read_new());
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());

    if (archive_read_open_filename(a.get(), archive_path.c_str(), 10240) != ARCHIVE_OK) {
        throw LpkgException(string_format("error.open_file_failed", archive_path.string()) + ": " + archive_error_string(a.get()));
    }

    struct archive_entry* entry;
    while (archive_read_next_header(a.get(), &entry) == ARCHIVE_OK) {
        std::string path = archive_entry_pathname(entry);
        // 去除开头的 ./ 前缀
        if (path.starts_with(constants::CURRENT_DIR_PREFIX)) path = path.substr(constants::CURRENT_DIR_PREFIX.length());

        if (path == internal_path) {
            size_t size = archive_entry_size(entry);
            std::string content;
            content.resize(size);
            archive_read_data(a.get(), content.data(), size);
            return content;
        }
        archive_read_data_skip(a.get());
    }

    return "";
}
