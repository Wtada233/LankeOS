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
    // REMOVED: archive_write_disk_set_standard_lookup(ext.get());
    // This function can cause segfaults in static binaries due to NSS issues in chroot.

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

        // SECURITY: Path traversal vulnerability mitigation.
        fs::path dest_path;
        try {
            dest_path = validate_path(current_path, output_dir);
        } catch (const LpkgException& e) {
             throw LpkgException(string_format("error.malicious_path_in_archive", current_path));
        }

        archive_entry_set_pathname(entry, dest_path.c_str());

        // Remap hardlink and symlink targets if they exist
        const char* hardlink = archive_entry_hardlink(entry);
        if (hardlink) {
            try {
                fs::path link_dest = validate_path(hardlink, output_dir);
                archive_entry_set_hardlink(entry, link_dest.c_str());
            } catch (...) {
                archive_entry_set_hardlink(entry, nullptr); // Drop malicious/invalid links
            }
        }

        const char* symlink = archive_entry_symlink(entry);
        if (symlink) {
            try {
                // For symlinks, we only remap if they are absolute. 
                // Relative symlinks are usually package-internal and should be left alone
                // unless they point outside via ../, which validate_path handles.
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
        // Remove leading ./ if present
        if (path.starts_with("./")) path = path.substr(2);

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