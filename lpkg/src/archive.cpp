#include "archive.hpp"
#include "utils.hpp"
#include "localization.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <memory>

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

bool extract_tar_zst(const std::string& archive_path, const std::string& output_dir) {
    ArchiveReadHandle a(archive_read_new());
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());

    ArchiveWriteHandle ext(archive_write_disk_new());
    archive_write_disk_set_options(ext.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS);
    archive_write_disk_set_standard_lookup(ext.get());

    if (archive_read_open_filename(a.get(), archive_path.c_str(), 10240) != ARCHIVE_OK) {
        return false; // Handles are cleaned up automatically
    }

    struct archive_entry* entry;
    int r = ARCHIVE_OK;
    long long count = 0;
    while ((r = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
        std::string path = output_dir + "/" + archive_entry_pathname(entry);
        archive_entry_set_pathname(entry, path.c_str());
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
            log_sync(string_format("info.extracting", count));
        }
    }

    // Destructors of ArchiveReadHandle and ArchiveWriteHandle will automatically
    // call archive_read_close/free and archive_write_close/free.

    log_sync(string_format("info.extract_complete", count));
    return (r == ARCHIVE_EOF);
}
