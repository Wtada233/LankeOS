#include "archive.hpp"
#include "exception.hpp"
#include "localization.hpp"
#include "utils.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <memory>

namespace fs = std::filesystem;

// Custom deleters for libarchive handles
struct ArchiveReadDeleter {
    void operator()(struct archive* a) const {
        if (a) {
            archive_read_close(a);
            archive_read_free(a);
        }
    }
};

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

void extract_tar_zst(const fs::path& archive_path, const fs::path& output_dir) {
    ArchiveReadHandle a(archive_read_new());
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());

    ArchiveWriteHandle ext(archive_write_disk_new());
    archive_write_disk_set_options(ext.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS);
    archive_write_disk_set_standard_lookup(ext.get());

    if (archive_read_open_filename(a.get(), archive_path.c_str(), 10240) != ARCHIVE_OK) {
            throw LpkgException(string_format("error.extract_failed", archive_path.string()) + ": " + archive_error_string(a.get()));
        }

    struct archive_entry* entry;
    int r = ARCHIVE_OK;
    long long count = 0;
    while ((r = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
        // SECURITY: Path traversal vulnerability mitigation.
        fs::path entry_path = archive_entry_pathname(entry);

        // Sanitize path: reject absolute paths or paths containing ".." components.
        if (entry_path.is_absolute()) {
            throw LpkgException(string_format("error.malicious_path_in_archive", entry_path.string()));
        }

        fs::path normalized_entry_path = entry_path.lexically_normal();
        for (const auto& component : normalized_entry_path) {
            if (component == "..") {
                throw LpkgException(string_format("error.malicious_path_in_archive", entry_path.string()));
            }
        }

        fs::path dest_path = output_dir / normalized_entry_path;
        archive_entry_set_pathname(entry, dest_path.c_str());

        r = archive_write_header(ext.get(), entry);
        if (r < ARCHIVE_OK) {
            break;
        }

        const void* buff;
        size_t size;
        la_int64_t offset;
        while ((r = archive_read_data_block(a.get(), &buff, &size, &offset)) == ARCHIVE_OK) {
            if (archive_write_data_block(ext.get(), buff, size, offset) < ARCHIVE_OK) {
                r = ARCHIVE_FATAL; // Ensure loop terminates
                break;
            }
        }

        if (r < ARCHIVE_OK) break;
        archive_write_finish_entry(ext.get());

        if (++count % 100 == 0) {
            log_info(string_format("info.extracting", count));
        }
    }

    log_info(string_format("info.extract_complete", count));
    
    if (r != ARCHIVE_EOF) {
        throw LpkgException(string_format("error.extract_failed", archive_path.string()) + ": " + archive_error_string(a.get()));
    }
}
